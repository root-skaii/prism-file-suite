#include "model/sidebar_layout.hpp"

#include "core/path.hpp"
#include "core/places.hpp"
#include "core/text.hpp"
#include "strata/config.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <format>
#include <iterator>

namespace prism::model {

namespace {

constexpr int k_max_sections = 64;
constexpr int k_max_items    = 512;

[[nodiscard]] bool same_path(std::string_view a, std::string_view b)
{
#ifdef _WIN32
    const std::wstring wa = core::text::widen(core::path::normalize(a));
    const std::wstring wb = core::text::widen(core::path::normalize(b));
    return ::CompareStringOrdinal(wa.c_str(), static_cast<int>(wa.size()), wb.c_str(), static_cast<int>(wb.size()), TRUE) == CSTR_EQUAL;
#else
    return core::path::normalize(a) == core::path::normalize(b);
#endif
}

// an insertion slot into a list of `size` elements
[[nodiscard]] std::size_t clamp_slot(std::size_t slot, std::size_t size) noexcept
{
    return std::min(slot, size);
}

} // namespace

std::string SidebarItem::label() const
{
    if (!name.empty()) { return name; }
    return query.empty() ? core::path::leaf(path) : query;
}

SidebarLayout SidebarLayout::defaults()
{
    SidebarLayout l;
    SidebarSection quick;
    quick.title = "Quick access";
    for (const core::KnownFolder& k : core::known_folders()) {
        std::string p = core::known_path(k.token);
        if (p.empty()) { continue; }
        SidebarItem it;
        it.path  = core::path::normalize(p);
        it.name  = std::string{k.name};
        it.known = std::string{k.token};
        quick.items.push_back(std::move(it));
    }
    l.sections_.push_back(std::move(quick));
    SidebarSection pc;
    pc.title  = "This PC";
    pc.drives = true;
    l.sections_.push_back(std::move(pc));
    return l;
}

std::optional<ItemPos> SidebarLayout::find(std::string_view path) const
{
    for (std::size_t s = 0; s < sections_.size(); ++s) {
        for (std::size_t i = 0; i < sections_[s].items.size(); ++i) {
            if (sections_[s].items[i].query.empty() && same_path(sections_[s].items[i].path, path)) { return ItemPos{s, i}; }
        }
    }
    return std::nullopt;
}

std::size_t SidebarLayout::pin_section()
{
    for (std::size_t s = 0; s < sections_.size(); ++s) {
        if (!sections_[s].dynamic()) { return s; }
    }
    return add_section(0, "Quick access");
}

bool SidebarLayout::has_drives() const noexcept
{
    return std::ranges::any_of(sections_, [](const SidebarSection& s) { return s.drives; });
}

// --- edits ----------------------------------------------------------------------------------------------------------

bool SidebarLayout::pin(std::size_t section, std::size_t slot, std::string path, std::string name)
{
    path = core::path::normalize(path);
    if (path.empty() || section >= sections_.size() || sections_[section].dynamic() || find(path)) { return false; }
    SidebarSection& sec = sections_[section];
    SidebarItem     it;
    it.path = std::move(path);
    it.name = std::move(name);
    sec.items.insert(sec.items.begin() + static_cast<std::ptrdiff_t>(clamp_slot(slot, sec.items.size())), std::move(it));
    sec.collapsed = false; // (what was just pinned should be visible)
    ++revision_;
    return true;
}

bool SidebarLayout::pin_search(std::size_t section, std::string folder, std::string query, std::string name)
{
    folder = core::path::normalize(folder);
    if (folder.empty() || query.empty() || section >= sections_.size() || sections_[section].dynamic()) { return false; }
    for (const SidebarSection& s : sections_) {
        for (const SidebarItem& x : s.items) {
            if (x.query == query && same_path(x.path, folder)) { return false; }
        }
    }
    SidebarItem it;
    it.path  = std::move(folder);
    it.query = std::move(query);
    it.name  = std::move(name);
    sections_[section].items.push_back(std::move(it));
    sections_[section].collapsed = false;
    ++revision_;
    return true;
}

bool SidebarLayout::unpin(std::string_view path)
{
    const auto at = find(path);
    return at && remove_item(*at);
}

bool SidebarLayout::remove_item(ItemPos at)
{
    if (at.section >= sections_.size() || at.index >= sections_[at.section].items.size()) { return false; }
    auto& v = sections_[at.section].items;
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(at.index));
    ++revision_;
    return true;
}

bool SidebarLayout::move_item(ItemPos from, std::size_t to_section, std::size_t to_slot)
{
    if (from.section >= sections_.size() || from.index >= sections_[from.section].items.size()) { return false; }
    if (to_section >= sections_.size() || sections_[to_section].dynamic()) { return false; }
    auto& src = sections_[from.section].items;
    auto& dst = sections_[to_section].items;
    to_slot   = clamp_slot(to_slot, dst.size());
    if (from.section == to_section) {
        if (to_slot == from.index || to_slot == from.index + 1) { return false; } // already there
        if (to_slot > from.index) { --to_slot; }                                  // (the slot counted the item itself)
    }
    SidebarItem it = std::move(src[from.index]);
    src.erase(src.begin() + static_cast<std::ptrdiff_t>(from.index));
    dst.insert(dst.begin() + static_cast<std::ptrdiff_t>(to_slot), std::move(it));
    sections_[to_section].collapsed = false;
    ++revision_;
    return true;
}

bool SidebarLayout::rename_item(ItemPos at, std::string name)
{
    if (at.section >= sections_.size() || at.index >= sections_[at.section].items.size()) { return false; }
    SidebarItem& it = sections_[at.section].items[at.index];
    if (name == it.label() || name == it.name) { return false; }
    it.name = std::move(name);
    if (it.name == core::path::leaf(it.path)) { it.name.clear(); } // (the folder's own name is the default)
    ++revision_;
    return true;
}

bool SidebarLayout::set_item_style(ItemPos at, char32_t glyph, std::uint32_t color)
{
    if (at.section >= sections_.size() || at.index >= sections_[at.section].items.size()) { return false; }
    SidebarItem& it = sections_[at.section].items[at.index];
    if (it.glyph == glyph && it.color == color) { return false; }
    it.glyph = glyph;
    it.color = color;
    ++revision_;
    return true;
}

std::size_t SidebarLayout::add_section(std::size_t slot, std::string title)
{
    slot = clamp_slot(slot, sections_.size());
    SidebarSection s;
    s.title = std::move(title);
    sections_.insert(sections_.begin() + static_cast<std::ptrdiff_t>(slot), std::move(s));
    ++revision_;
    return slot;
}

bool SidebarLayout::has_neighbour(std::size_t section) const
{
    for (std::size_t s = 0; s < sections_.size(); ++s) {
        if (s != section && !sections_[s].dynamic()) { return true; }
    }
    return false;
}

bool SidebarLayout::remove_section(std::size_t section, bool keep_items)
{
    if (section >= sections_.size()) { return false; }
    std::vector<SidebarItem> orphans = std::move(sections_[section].items);
    sections_.erase(sections_.begin() + static_cast<std::ptrdiff_t>(section));
    if (keep_items && !orphans.empty()) {
        // the nearest folder section above, else below
        std::size_t target = sections_.size();
        for (std::size_t s = std::min(section, sections_.size()); s-- > 0;) {
            if (!sections_[s].dynamic()) { target = s; break; }
        }
        if (target == sections_.size()) {
            for (std::size_t s = section; s < sections_.size(); ++s) {
                if (!sections_[s].dynamic()) { target = s; break; }
            }
        }
        if (target < sections_.size()) {
            auto& dst = sections_[target].items;
            dst.insert(dst.end(), std::make_move_iterator(orphans.begin()), std::make_move_iterator(orphans.end()));
        }
    }
    ++revision_;
    return true;
}

bool SidebarLayout::rename_section(std::size_t section, std::string title)
{
    if (section >= sections_.size() || title.empty() || sections_[section].title == title) { return false; }
    sections_[section].title = std::move(title);
    ++revision_;
    return true;
}

bool SidebarLayout::set_collapsed(std::size_t section, bool collapsed)
{
    if (section >= sections_.size() || sections_[section].collapsed == collapsed) { return false; }
    sections_[section].collapsed = collapsed;
    ++revision_;
    return true;
}

bool SidebarLayout::move_section(std::size_t from, std::size_t slot)
{
    if (from >= sections_.size()) { return false; }
    slot = clamp_slot(slot, sections_.size());
    if (slot == from || slot == from + 1) { return false; }
    if (slot > from) { --slot; }
    SidebarSection s = std::move(sections_[from]);
    sections_.erase(sections_.begin() + static_cast<std::ptrdiff_t>(from));
    sections_.insert(sections_.begin() + static_cast<std::ptrdiff_t>(slot), std::move(s));
    ++revision_;
    return true;
}

bool SidebarLayout::add_drives_section(std::size_t slot)
{
    if (has_drives()) { return false; }
    slot = clamp_slot(slot, sections_.size());
    SidebarSection s;
    s.title  = "This PC";
    s.drives = true;
    sections_.insert(sections_.begin() + static_cast<std::ptrdiff_t>(slot), std::move(s));
    ++revision_;
    return true;
}

void SidebarLayout::reset()
{
    sections_ = defaults().sections_;
    hidden_drives_.clear();
    ++revision_;
}

bool SidebarLayout::add_auto_section(std::size_t slot, SidebarSection::Auto kind)
{
    if (kind == SidebarSection::Auto::none || has_auto(kind)) { return false; }
    slot = clamp_slot(slot, sections_.size());
    SidebarSection s;
    s.title     = kind == SidebarSection::Auto::recent ? "Recent" : "Frequent";
    s.auto_kind = kind;
    sections_.insert(sections_.begin() + static_cast<std::ptrdiff_t>(slot), std::move(s));
    ++revision_;
    return true;
}

bool SidebarLayout::has_auto(SidebarSection::Auto kind) const noexcept
{
    return std::ranges::any_of(sections_, [&](const SidebarSection& s) { return s.auto_kind == kind; });
}

bool SidebarLayout::drive_hidden(std::string_view root) const
{
    return std::ranges::any_of(hidden_drives_, [&](const std::string& h) { return same_path(h, root); });
}

bool SidebarLayout::hide_drive(std::string_view root)
{
    if (root.empty() || drive_hidden(root)) { return false; }
    hidden_drives_.push_back(core::path::normalize(root));
    ++revision_;
    return true;
}

bool SidebarLayout::show_drive(std::string_view root)
{
    const auto it = std::ranges::find_if(hidden_drives_, [&](const std::string& h) { return same_path(h, root); });
    if (it == hidden_drives_.end()) { return false; }
    hidden_drives_.erase(it);
    ++revision_;
    return true;
}

void SidebarLayout::replace(const SidebarLayout& other)
{
    sections_      = other.sections_;
    hidden_drives_ = other.hidden_drives_;
    ++revision_;
}

void SidebarLayout::restore(const SidebarLayout& snapshot)
{
    std::vector<SidebarSection> now = std::move(sections_);
    sections_                       = snapshot.sections_;
    hidden_drives_                  = snapshot.hidden_drives_;
    for (SidebarSection& s : sections_) {
        for (const SidebarSection& k : now) {
            if (k.title == s.title && k.drives == s.drives && k.auto_kind == s.auto_kind) { s.collapsed = k.collapsed; break; }
        }
    }
    ++revision_;
}

// --- persistence: [sidebar] sections = N, then [sidebar.0] title / collapsed / kind / items / i0.path ... ----------

void SidebarLayout::store(strata::config& c) const
{
    c.set_int("sidebar", "sections", static_cast<int>(sections_.size()));
    std::string hidden;
    for (const std::string& h : hidden_drives_) { hidden += (hidden.empty() ? "" : "|") + h; } // ('|' cannot be in a path)
    if (!hidden.empty()) { c.set("sidebar", "hidden_drives", hidden); }
    for (std::size_t s = 0; s < sections_.size(); ++s) {
        const SidebarSection& sec = sections_[s];
        const std::string     id  = std::format("sidebar.{}", s);
        c.set(id, "title", sec.title);
        c.set(id, "kind", sec.drives ? "drives" : sec.auto_kind == SidebarSection::Auto::recent ? "recent" : sec.auto_kind == SidebarSection::Auto::frequent ? "frequent" : "folders");
        c.set_bool(id, "collapsed", sec.collapsed);
        c.set_int(id, "items", static_cast<int>(sec.items.size()));
        for (std::size_t i = 0; i < sec.items.size(); ++i) {
            const SidebarItem& it = sec.items[i];
            c.set(id, std::format("i{}.path", i), it.path);
            if (!it.name.empty())  { c.set(id, std::format("i{}.name", i), it.name); }
            if (!it.known.empty()) { c.set(id, std::format("i{}.known", i), it.known); }
            if (!it.query.empty()) { c.set(id, std::format("i{}.query", i), it.query); }
            if (it.glyph != 0)     { c.set_int(id, std::format("i{}.glyph", i), static_cast<int>(it.glyph)); }
            if (it.color != 0)     { c.set_int(id, std::format("i{}.color", i), static_cast<int>(it.color)); }
        }
    }
}

bool SidebarLayout::load(const strata::config& c)
{
    if (!c.has("sidebar", "sections")) { return false; }
    const int n = std::clamp(c.get_int("sidebar", "sections", 0), 0, k_max_sections);
    std::vector<SidebarSection> out;
    for (int s = 0; s < n; ++s) {
        const std::string id = std::format("sidebar.{}", s);
        SidebarSection    sec;
        sec.title     = std::string{c.get(id, "title", "Section")};
        sec.drives    = c.get(id, "kind") == "drives";
        sec.auto_kind = c.get(id, "kind") == "recent" ? SidebarSection::Auto::recent : c.get(id, "kind") == "frequent" ? SidebarSection::Auto::frequent : SidebarSection::Auto::none;
        sec.collapsed = c.get_bool(id, "collapsed", false);
        if (sec.title.empty()) { sec.title = "Section"; }
        const int count = sec.dynamic() ? 0 : std::clamp(c.get_int(id, "items", 0), 0, k_max_items);
        for (int i = 0; i < count; ++i) {
            SidebarItem it;
            it.path  = core::path::normalize(c.get(id, std::format("i{}.path", i)));
            it.name  = std::string{c.get(id, std::format("i{}.name", i))};
            it.known = std::string{c.get(id, std::format("i{}.known", i))};
            it.query = std::string{c.get(id, std::format("i{}.query", i))};
            it.glyph = static_cast<char32_t>(c.get_int(id, std::format("i{}.glyph", i), 0));
            it.color = static_cast<std::uint32_t>(c.get_int(id, std::format("i{}.color", i), 0));
            if (!it.known.empty()) {
                if (const std::string p = core::known_path(it.known); !p.empty()) { it.path = core::path::normalize(p); }
            }
            if (it.path.empty()) { continue; }
            const auto same = [&](const SidebarItem& x) { return x.query == it.query && same_path(x.path, it.path); };
            const bool dup = std::ranges::any_of(out, [&](const SidebarSection& o) { return std::ranges::any_of(o.items, same); })
                          || std::ranges::any_of(sec.items, same);
            if (!dup) { sec.items.push_back(std::move(it)); }
        }
        out.push_back(std::move(sec));
    }
    sections_ = std::move(out);
    hidden_drives_.clear();
    std::string_view rest = c.get("sidebar", "hidden_drives");
    while (!rest.empty()) {
        const std::size_t bar = rest.find('|');
        if (const std::string_view one = rest.substr(0, bar); !one.empty()) { hidden_drives_.push_back(core::path::normalize(one)); }
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
    }
    ++revision_;
    return true;
}

bool SidebarLayout::load_file(const std::string& path)
{
    strata::config c;
    return c.load_file(path) && load(c);
}

bool SidebarLayout::save_file(const std::string& path) const
{
    strata::config c;
    store(c);
    return c.save_file(path);
}

} // namespace prism::model
