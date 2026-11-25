#include "ui/sidebar.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "core/rclone.hpp"
#include "ui/tr.hpp"
#include "core/recycle.hpp"
#include "core/shell_ops.hpp"
#include "ui/tr.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"
#include "ui/menu_parts.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>

namespace prism::ui {

using namespace strata;

namespace {

// the names the sidebar starts with; they are stored as typed, so they are translated only when shown
[[maybe_unused]] constexpr const char* k_default_names[] = {N_("Quick access"), N_("This PC"), N_("Recent"), N_("Frequent"), N_("Home"), N_("Desktop"),
                                                            N_("Downloads"), N_("Documents"), N_("Pictures"), N_("Music"), N_("Videos")};

constexpr std::string_view k_pin_prefix = "\x01pin:";
constexpr std::size_t      k_end        = std::string::npos; // (an insertion slot past the last element)
constexpr std::size_t      k_auto_rows  = 6;                 // folders an automatic section lists

[[nodiscard]] std::string upper(std::string_view s)
{
    std::string r{s};
    for (char& c : r) {
        if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 32); }
    }
    return r;
}

[[nodiscard]] std::string trimmed(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.remove_suffix(1); }
    return std::string{s};
}

[[nodiscard]] color from_argb(std::uint32_t c)
{
    return color{static_cast<std::uint8_t>((c >> 16) & 0xff), static_cast<std::uint8_t>((c >> 8) & 0xff),
                 static_cast<std::uint8_t>(c & 0xff), static_cast<std::uint8_t>(c >> 24)};
}

[[nodiscard]] rect inset(const rect& cell) { return rect{{cell.min.x + 10.0f, cell.min.y + 2.0f}, {cell.max.x - 10.0f, cell.max.y - 2.0f}}; }

void insertion_line(Painter& p, const rect& line)
{
    p.dl().rect_filled(rect{{line.min.x, line.center().y - 1.0f}, {line.max.x, line.center().y + 1.0f}}, p.pal().accent, 1.0f);
    p.dl().rect_filled(rect::from_size({line.min.x - 3.0f, line.center().y - 3.5f}, {7.0f, 7.0f}), p.pal().accent, 3.5f);
}

} // namespace

Sidebar::Sidebar(Env& env) : env_{env}, drive_watch_{env.window.waker()}, shell_menu_{env.window.waker()}
{
    drives_     = drive_watch_.list();
    drives_rev_ = drive_watch_.revision();
}

void Sidebar::refresh_drives()
{
    drive_watch_.poke();
}

bool Sidebar::is_pin_target(std::string_view dest) noexcept
{
    return dest.starts_with(k_pin_prefix);
}

// --- dropping -------------------------------------------------------------------------------------------------------

// pointer over a row: its middle is "into this folder", its top / bottom quarter is the gap before / after it (a pin).
// a section title pins at the top of its section, an empty section takes the first one
Sidebar::Spot Sidebar::spot_at(vec2 p, bool into_ok) const
{
    Spot spot;
    if (!area_.contains(p)) { return spot; }
    const auto& secs = env_.layout.sections();
    for (const Row& r : rows_) {
        if (!r.cell.contains(p)) { continue; }
        const float x0 = r.cell.min.x + 12.0f;
        const float x1 = r.cell.max.x - 12.0f;
        switch (r.kind) {
        case Kind::header: {
            if (r.section >= secs.size() || secs[r.section].dynamic()) { return spot; }
            spot.type    = Spot::Type::between;
            spot.section = r.section;
            spot.slot    = secs[r.section].collapsed ? secs[r.section].items.size() : 0;
            spot.line    = rect{{x0, r.cell.max.y}, {x1, r.cell.max.y}};
            return spot;
        }
        case Kind::empty:
            spot.type    = Spot::Type::between;
            spot.section = r.section;
            spot.slot    = 0;
            spot.line    = rect{{x0, r.cell.center().y}, {x1, r.cell.center().y}};
            return spot;
        case Kind::item: {
            const float f = (p.y - r.cell.min.y) / std::max(1.0f, r.cell.height());
            if (into_ok && f >= 0.25f && f <= 0.75f) {
                if (probe_.missing(r.path)) { return spot; }
                spot.type = Spot::Type::into;
                spot.path = r.path;
                return spot;
            }
            const bool before = f < 0.5f;
            spot.type    = Spot::Type::between;
            spot.section = r.section;
            spot.slot    = before ? r.index : r.index + 1;
            const float y = before ? r.cell.min.y : r.cell.max.y;
            spot.line    = rect{{x0, y}, {x1, y}};
            return spot;
        }
        case Kind::drive:
        case Kind::visit:
            if (into_ok && !core::is_device_path(r.path) && !core::recycle::inside(r.path)) {
                spot.type = Spot::Type::into;
                spot.path = r.path;
            }
            return spot;
        case Kind::note:
        case Kind::none:
            return spot;
        }
    }
    return spot;
}

std::string Sidebar::drop_target_at(vec2 p) const
{
    const Spot s = spot_at(p, true);
    switch (s.type) {
    case Spot::Type::into:    return s.path;
    case Spot::Type::between: return std::string{k_pin_prefix} + std::to_string(s.section) + ":" + std::to_string(s.slot);
    case Spot::Type::none:    break;
    }
    return {};
}

void Sidebar::pin_dropped(const std::vector<std::string>& paths, std::string_view dest)
{
    dest.remove_prefix(k_pin_prefix.size());
    const std::size_t colon = dest.find(':');
    if (colon == std::string_view::npos) { return; }
    std::size_t section = 0, slot = 0;
    std::from_chars(dest.data(), dest.data() + colon, section);
    std::from_chars(dest.data() + colon + 1, dest.data() + dest.size(), slot);

    std::vector<std::string> folders;
    for (const std::string& s : paths) {
        if (core::path::is_directory(s)) { folders.push_back(s); }
    }
    if (folders.empty()) {
        env_.cmd.toast(tr("Only folders can be pinned to the sidebar"), toast_kind::info);
        return;
    }
    std::size_t pinned = 0;
    const std::string label = folders.size() == 1 ? trf("Pin {}", core::path::leaf(folders.front())) : std::string{tr("Pin folders")};
    (void)env_.cmd.edit_sidebar(label, [&](model::SidebarLayout& l) {
        for (const std::string& f : folders) {
            if (l.pin(section, slot, f)) {
                ++pinned;
                if (slot != k_end) { ++slot; }
            }
        }
    });
    if (pinned == 0) { env_.cmd.toast(tr("Already in the sidebar"), toast_kind::info); }
}

// --- rows -----------------------------------------------------------------------------------------------------------

void Sidebar::look(const model::SidebarItem& it, char32_t& g, color& c, const Palette& pal) const
{
    g = glyph::folder_open;
    c = pal.tint[static_cast<std::size_t>(core::FileKind::folder)];
    const std::string_view k = it.known;
    if (k == "home")           { g = glyph::home;     c = pal.accent; }
    else if (k == "desktop")   { g = glyph::desktop;  c = color{90, 160, 255, 255}; }
    else if (k == "downloads") { g = glyph::download; c = pal.success; }
    else if (k == "documents") { g = glyph::document; c = pal.tint[static_cast<std::size_t>(core::FileKind::document)]; }
    else if (k == "pictures")  { g = glyph::picture;  c = pal.tint[static_cast<std::size_t>(core::FileKind::image)]; }
    else if (k == "music")     { g = glyph::music;    c = pal.tint[static_cast<std::size_t>(core::FileKind::audio)]; }
    else if (k == "videos")    { g = glyph::video;    c = pal.tint[static_cast<std::size_t>(core::FileKind::video)]; }
    if (!it.query.empty()) { g = glyph::search; c = pal.accent; }
    if (it.glyph != 0) { g = it.glyph; }
    if (it.color != 0) { c = from_argb(it.color); }
}

void Sidebar::row_background(Painter& p, const std::string& key, const item_result& r, const rect& row, bool active)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const bool     hot = r.hovered && !press_.active && !drop_;
    const float hover = ui.animate(key + "#h", hot ? 1.0f : 0.0f, 16.0f);
    const float act   = ui.animate(key + "#a", active ? 1.0f : 0.0f, 14.0f);
    const float press = ui.animate(key + "#p", r.held && hot ? 1.0f : 0.0f, 30.0f);

    if (hover > 0.01f) { p.dl().rect_filled(row, (press > 0.5f ? pal.pressed : pal.hover).scaled_alpha(hover), 8.0f); }
    if (act > 0.01f) {
        p.dl().rect_filled(row, pal.selection.scaled_alpha(act), 8.0f);
        const float bar_h = 6.0f + 12.0f * act;
        p.dl().rect_filled(rect::from_size({row.min.x, row.center().y - bar_h * 0.5f}, {3.0f, bar_h}), pal.accent.scaled_alpha(act), 1.5f);
    }
}

void Sidebar::header(Painter& p, std::size_t si, float top_space)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    const model::SidebarSection& sec = env_.layout.sections()[si];
    core::text::note_codepoints(sec.title);

    const item_result r = ui.custom_item("##sec" + std::to_string(si), {ui.content_width(), top_space + 26.0f});
    const rect  hdr{{r.bounds.min.x, r.bounds.min.y + top_space}, r.bounds.max};
    const vec2  mp    = ui.mouse_pos();
    const bool  quiet = press_.active || drop_.has_value();
    const bool  over  = r.hovered && hdr.contains(mp) && !quiet;
    const rect  chev  = rect::from_size({hdr.max.x - 30.0f, hdr.min.y}, {22.0f, hdr.height()});
    const rect  plus  = rect::from_size({chev.min.x - 24.0f, hdr.min.y + 2.0f}, {22.0f, hdr.height() - 4.0f});
    const bool  over_plus = over && !sec.dynamic() && plus.contains(mp);

    rows_.push_back({hdr, rect{{hdr.min.x + 12.0f, hdr.min.y + 2.0f}, {plus.min.x - 4.0f, hdr.max.y - 2.0f}}, Kind::header, si, 0, {}});

    const color c = over ? pal.text_dim : pal.text_faint;
    const bool  renaming = rename_.active && rename_.target.kind == Kind::header && rename_.target.section == si;
    if (!renaming) { p.text_left(rect{hdr.min, {plus.min.x, hdr.max.y}}, upper(tr(sec.title)), c, f.small, 20.0f); }
    p.glyph_center(chev, sec.collapsed ? glyph::chevron_r : glyph::chevron_d, c);
    if (over && !sec.dynamic()) {
        if (over_plus) { p.dl().rect_filled(plus, pal.hover, 6.0f); }
        p.glyph_center(plus, glyph::add, over_plus ? pal.text : pal.text_dim);
    }

    if (r.hovered && hdr.contains(mp)) {
        if (ui.mouse_clicked(mouse_button::right)) { menu_ = Target{Kind::header, si, 0, {}}; }
        if (over) {
            if (ui.mouse_clicked(mouse_button::left) && !rename_.active) {
                if (over_plus) { pin_current(si); }
                else { press_ = Press{true, false, Target{Kind::header, si, 0, {}}, mp}; }
            }
            if (over_plus) { ui.tooltip(tr("Pin the current folder here")); }
        }
    }
}

// "Devices" title under the drives while a phone / camera is connected: collapses like a section, but is not part of the layout
void Sidebar::devices_header(Painter& p, std::size_t si)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    constexpr float top = 14.0f;

    const item_result r   = ui.custom_item("##devices", {ui.content_width(), top + 26.0f});
    const rect        hdr{{r.bounds.min.x, r.bounds.min.y + top}, r.bounds.max};
    rows_.push_back({hdr, hdr, Kind::note, si, 0, {}});
    const bool  over = r.hovered && hdr.contains(ui.mouse_pos()) && !press_.active && !drop_.has_value();
    const color c    = over ? pal.text_dim : pal.text_faint;
    p.text_left(hdr, upper(tr("Devices")), c, env_.fonts.small, 20.0f);
    p.glyph_center(rect::from_size({hdr.max.x - 30.0f, hdr.min.y}, {22.0f, hdr.height()}), devices_collapsed_ ? glyph::chevron_r : glyph::chevron_d, c);
    if (over && ui.mouse_clicked(mouse_button::left)) { devices_collapsed_ = !devices_collapsed_; }
}

void Sidebar::item_row(Painter& p, std::size_t si, std::size_t ii)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    const FontSet& f   = env_.fonts;
    const model::SidebarItem& it = env_.layout.sections()[si].items[ii];
    // (the built-in names are translated; a name the user typed stays as it is)
    const std::string label = it.known.empty() ? it.label() : std::string{tr(it.label())};
    core::text::note_codepoints(label);

    const std::string key = it.query.empty() ? it.path : it.path + "?" + it.query; // (a folder and a search in it are two rows)
    const item_result r   = ui.custom_item(key, {ui.content_width(), m.side_row + 4.0f});
    const rect        row = inset(r.bounds);
    const float       tx  = row.min.x + 12.0f + 24.0f + 12.0f;
    rows_.push_back({r.bounds, rect{{tx - 8.0f, row.min.y + 1.0f}, {row.max.x - 4.0f, row.max.y - 1.0f}}, Kind::item, si, ii, it.path});

    const bool missing = probe_.missing(it.path);
    const model::Tab& cur = env_.tab();
    const bool active  = core::path::normalize(cur.path()) == it.path
                      && (it.query.empty() ? !cur.searching() : cur.searching() && cur.search_query() == it.query);
    row_background(p, key, r, row, active);
    if (hint_.type == Spot::Type::into && hint_.path == it.path) {
        p.dl().rect_outline(row, pal.accent, 8.0f, 2.0f);
    }

    char32_t g{};
    color    tint{};
    look(it, g, tint, pal);
    if (missing) { p.dl().push_alpha(0.45f); }
    const rect ibox = rect::from_size({row.min.x + 12.0f, row.center().y - 12.0f}, {24.0f, 24.0f});
    if (it.glyph == 0 && it.color == 0 && it.known.empty() && it.query.empty()) { p.folder_icon(ibox); } // (a plain folder looks like one everywhere)
    else { p.glyph_center(ibox, g, tint); }
    const bool renaming = rename_.active && rename_.target.kind == Kind::item && rename_.target.section == si && rename_.target.index == ii;
    if (!renaming) { p.text_left(rect{{tx, row.min.y}, {row.max.x - 12.0f, row.max.y}}, label, pal.text, f.body); }
    if (missing) { p.dl().pop_alpha(); }

    if (r.hovered && !press_.active && !drop_) {
        const Target t{Kind::item, si, ii, it.path};
        if (ui.mouse_clicked(mouse_button::left) && !rename_.active) { press_ = Press{true, false, t, ui.mouse_pos()}; }
        if (ui.mouse_clicked(mouse_button::right)) { menu_ = t; }
        if (ui.mouse_clicked(mouse_button::middle) && !missing) { env_.cmd.open_tab(it.path, false); }
        ui.tooltip(missing ? trf("{}\nFolder not found", it.path) : (it.query.empty() ? it.path : trf("Search \"{}\" in {}", it.query, it.path)));
    }
}

void Sidebar::drive_row(Painter& p, const core::Place& place)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    const FontSet& f   = env_.fonts;
    (void)m;
    const bool  compact = env_.theme.density() == Density::compact;
    // compact height when there is no capacity bar (Recycle Bin, WSL, mirror cloud folders)
    const bool  short_row = place.recycle || (place.total <= 0 && !place.device);
    const float h       = short_row ? (compact ? 40.0f : 46.0f) : compact ? 54.0f : 62.0f;
    const std::string key = "##drive:" + place.path;

    const item_result r   = ui.custom_item(key, {ui.content_width(), h});
    const rect        row = inset(r.bounds);
    rows_.push_back({r.bounds, row, Kind::drive, 0, 0, place.path});

    const std::string& current = env_.tab().path();
    // (a drive lights up for anything on it, except the bin that lives on it: that has a row of its own)
    const bool in_bin = core::recycle::inside(current);
    bool active = false;
    if (place.recycle) {
        active = in_bin;
    } else if (!in_bin && !current.empty()) {
        if (place.wsl || place.cloud) {
            const std::string cn = core::text::lower_ascii(core::path::normalize(current));
            const std::string pn = core::text::lower_ascii(core::path::normalize(place.path));
            active = !pn.empty() && (cn == pn || (cn.size() > pn.size() && cn.starts_with(pn) && (pn.back() == '\\' || cn[pn.size()] == '\\')));
        } else {
            active = current.size() >= 2 && place.path.size() >= 2 &&
                     std::toupper(static_cast<unsigned char>(current[0])) == std::toupper(static_cast<unsigned char>(place.path[0])) && current[1] == ':';
        }
    }
    row_background(p, key, r, row, active);
    if (hint_.type == Spot::Type::into && hint_.path == place.path) {
        p.dl().rect_outline(row, pal.accent, 8.0f, 2.0f);
    }

    const color tint = place.cloud   ? color{66, 133, 244, 255}
                     : place.wsl     ? color{233, 84, 32, 255}
                     : place.network ? pal.accent
                                     : pal.tint[static_cast<std::size_t>(core::FileKind::drive)];
    const float tile = short_row ? (compact ? 26.0f : 30.0f) : compact ? 30.0f : 34.0f;
    const rect  ibox = rect::from_size({row.min.x + 12.0f, row.center().y - tile * 0.5f}, {tile, tile});
    p.dl().rect_filled(ibox, tint.scaled_alpha(0.16f), 9.0f);
    const char32_t icon_glyph = place.device    ? glyph::phone
                              : place.recycle   ? glyph::delete_
                              : place.cloud     ? glyph::cloud
                              : place.wsl       ? glyph::code
                              : place.network   ? glyph::network
                              : place.optical   ? glyph::disc
                              : place.removable ? glyph::usb
                                                : glyph::drive;
    p.glyph_center(ibox, icon_glyph, tint);

    // name, bar and free-space line, centred in the row (tighter in compact mode)
    const float tx    = ibox.max.x + 12.0f;
    const float right = row.max.x - 12.0f;
    const float block = compact ? 42.0f : 47.0f;
    const float y0    = row.center().y - block * 0.5f - 1.0f;
    const float bar_y = compact ? 22.0f : 25.0f;
    const float free_y = compact ? 27.0f : 31.0f;
    if (place.total <= 0) {
        // no bar: the name (and a device's second line) sits centred in the row instead of at the top of the bar layout
        const float cy  = row.center().y;
        const float gap = compact ? 8.0f : 9.0f;
        const bool  two = place.device;
        p.text_left(rect{{tx, cy - (two ? gap : 0.0f) - 9.0f}, {right, cy - (two ? gap : 0.0f) + 9.0f}}, place.name, pal.text, f.body);
        if (two) { p.text_left(rect{{tx, cy + gap - 9.0f}, {right, cy + gap + 9.0f}}, tr("Opens in Explorer"), pal.text_dim, f.small); }
    } else {
        p.text_left(rect{{tx, y0}, {right, y0 + (compact ? 18.0f : 20.0f)}}, place.name, pal.text, f.body);
        const float used = std::clamp(1.0f - static_cast<float>(place.free) / static_cast<float>(place.total), 0.0f, 1.0f);
        const rect  bar{{tx, y0 + bar_y}, {right, y0 + bar_y + 4.0f}};
        p.dl().rect_filled(bar, pal.text_faint.scaled_alpha(0.30f), 2.0f);
        const float filled = ui.animate(key + "#u", used, 6.0f);
        p.dl().rect_filled({bar.min, {bar.min.x + bar.width() * filled, bar.max.y}}, used > 0.9f ? pal.danger : pal.accent, 2.0f);
        const std::string free = trf("{} free of {}", core::path::format_size(place.free), core::path::format_size(place.total));
        p.text_left(rect{{tx, y0 + free_y}, {right, y0 + free_y + 18.0f}}, free, pal.text_dim, f.small);
    }

    if (r.hovered && !press_.active && !drop_) {
        const Target t{Kind::drive, 0, 0, place.path};
        if (ui.mouse_clicked(mouse_button::left)) { press_ = Press{true, false, t, ui.mouse_pos()}; }
        if (ui.mouse_clicked(mouse_button::right)) { menu_ = t; }
        if (ui.mouse_clicked(mouse_button::middle)) {
            if (!place.device) { env_.cmd.open_tab(place.path, false); }
            else if (!core::shell::open_device(place.path)) { env_.cmd.toast(tr("Could not open File Explorer"), toast_kind::error); }
        }
        if (place.device) { ui.tooltip(tr("Phones and cameras open in Windows Explorer")); }
    }
}

// a section with nothing in it yet: says where to drop
void Sidebar::empty_row(Painter& p, std::size_t si)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const item_result r   = ui.custom_item("##empty" + std::to_string(si), {ui.content_width(), 38.0f});
    const rect        row = inset(r.bounds);
    rows_.push_back({r.bounds, row, Kind::empty, si, 0, {}});
    const bool hot = hint_.type == Spot::Type::between && hint_.section == si;
    p.dl().rect_outline(row, hot ? pal.accent : pal.text_faint.scaled_alpha(0.35f), 8.0f, hot ? 2.0f : 1.0f);
    p.text_center(row, tr("Drop folders here"), pal.text_faint, env_.fonts.small);
    if (r.hovered && ui.mouse_clicked(mouse_button::right)) { menu_ = Target{Kind::header, si, 0, {}}; }
}

// a folder the Recent / Frequent list offers: same look as a pin, but it comes and goes by itself and cannot be dragged
void Sidebar::visit_row(Painter& p, std::size_t si, const std::string& path)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    const std::string label = core::path::leaf(path);
    core::text::note_codepoints(label);
    const std::string key = "##visit" + std::to_string(si) + ":" + path;

    const item_result r   = ui.custom_item(key, {ui.content_width(), m.side_row + 4.0f});
    const rect        row = inset(r.bounds);
    const float       tx  = row.min.x + 12.0f + 24.0f + 12.0f;
    rows_.push_back({r.bounds, row, Kind::visit, si, 0, path});

    row_background(p, key, r, row, core::path::normalize(env_.tab().path()) == path);
    if (hint_.type == Spot::Type::into && hint_.path == path) { p.dl().rect_outline(row, pal.accent, 8.0f, 2.0f); }
    p.folder_icon(rect::from_size({row.min.x + 12.0f, row.center().y - 12.0f}, {24.0f, 24.0f}));
    p.text_left(rect{{tx, row.min.y}, {row.max.x - 12.0f, row.max.y}}, label, pal.text, env_.fonts.body);

    if (r.hovered && !press_.active && !drop_) {
        const Target t{Kind::visit, si, 0, path};
        if (ui.mouse_clicked(mouse_button::left)) { press_ = Press{true, false, t, ui.mouse_pos()}; }
        if (ui.mouse_clicked(mouse_button::right)) { menu_ = t; }
        if (ui.mouse_clicked(mouse_button::middle)) { env_.cmd.open_tab(path, false); }
        ui.tooltip(path);
    }
}

// a line of small print in a section (an empty Recent list says so)
void Sidebar::note_row(Painter& p, std::size_t si, std::string_view text)
{
    context& ui = p.ui();
    const item_result r = ui.custom_item("##note" + std::to_string(si), {ui.content_width(), 30.0f});
    rows_.push_back({r.bounds, inset(r.bounds), Kind::note, si, 0, {}});
    p.text_left(inset(r.bounds), text, p.pal().text_faint, env_.fonts.small, 14.0f);
    if (r.hovered && ui.mouse_clicked(mouse_button::right)) { menu_ = Target{Kind::header, si, 0, {}}; }
}

// --- frame ----------------------------------------------------------------------------------------------------------

void Sidebar::frame(Painter& p, const rect& area, const FrameInput& in)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();

    hint_ = drop_ ? spot_at(*drop_, true) : Spot{}; // (before the rows are rebuilt)
    area_ = area;
    rows_.clear();
    blocks_.clear();
    PanelScope panel{ui, "##sidebar", area};
    p.dl().rect_filled(area, pal.window);

    // (the drive list comes from DriveWatcher: asking a volume for its label can take seconds)
    if (const unsigned rev = drive_watch_.revision(); rev != drives_rev_) {
        drives_     = drive_watch_.list();
        drives_rev_ = rev;
    }
    if (drive_op_ && drive_op_->done.load(std::memory_order_acquire)) {
        env_.cmd.toast(drive_op_->msg, drive_op_->ok ? toast_kind::success : toast_kind::error);
        drive_op_.reset();
        drive_watch_.poke();
    }
    (void)in;
    const std::size_t n = env_.layout.sections().size();
    {   // what the automatic sections would list (a few spare: vanished folders are left out below)
        std::vector<std::string> watch;
        auto_rows_.assign(n, {});
        for (std::size_t si = 0; si < n; ++si) {
            const model::SidebarSection& s = env_.layout.sections()[si];
            for (const model::SidebarItem& it : s.items) { watch.push_back(it.path); }
            if (s.auto_kind == model::SidebarSection::Auto::none) { continue; }
            auto_rows_[si] = s.auto_kind == model::SidebarSection::Auto::recent ? env_.visits.recent(k_auto_rows * 3) : env_.visits.frequent(k_auto_rows * 3);
            watch.insert(watch.end(), auto_rows_[si].begin(), auto_rows_[si].end());
        }
        probe_.watch(watch);
        for (auto& list : auto_rows_) {
            std::erase_if(list, [&](const std::string& path) { return probe_.missing(path); });
            if (list.size() > k_auto_rows) { list.resize(k_auto_rows); }
        }
    }
    if (ui.mouse_clicked(mouse_button::right) && area.contains(ui.mouse_pos())) {
        menu_     = Target{}; // (rows below claim it)
        paste_ok_ = core::shell::clipboard_text_contains(L"[sidebar]");
    }

    {
        auto scroll = ui.child("##side_scroll", {area.width(), area.height()}, child_flags::no_padding);
        (void)ui.custom_item("##side_top", {1.0f, 2.0f});

        for (std::size_t si = 0; si < n; ++si) {
            const model::SidebarSection& sec = env_.layout.sections()[si];
            header(p, si, si == 0 ? 6.0f : 14.0f);
            if (!sec.collapsed) {
                if (sec.drives) {
                    for (const core::Place& pl : drives_) {
                        if (!pl.device && !env_.layout.drive_hidden(pl.path)) { drive_row(p, pl); }
                    }
                    if (std::ranges::any_of(drives_, [](const core::Place& pl) { return pl.device; })) {
                        devices_header(p, si);
                        if (!devices_collapsed_) {
                            for (const core::Place& pl : drives_) {
                                if (pl.device) { drive_row(p, pl); }
                            }
                        }
                    }
                } else if (sec.auto_kind != model::SidebarSection::Auto::none) {
                    if (auto_rows_[si].empty()) {
                        note_row(p, si, !env_.settings.track_history ? tr("History is off (Settings)") : sec.auto_kind == model::SidebarSection::Auto::recent ? tr("Folders you open show up here") : tr("Folders you keep coming back to"));
                    }
                    for (const std::string& path : auto_rows_[si]) { visit_row(p, si, path); }
                } else if (sec.items.empty()) {
                    empty_row(p, si);
                } else {
                    for (std::size_t ii = 0; ii < sec.items.size(); ++ii) { item_row(p, si, ii); }
                }
            }
            // (the header is the first row recorded for this section)
            const auto first = std::ranges::find_if(rows_, [&](const Row& r) { return r.kind == Kind::header && r.section == si; });
            blocks_.push_back(rect{{area.min.x, first->cell.min.y}, {area.max.x, rows_.back().cell.max.y}});
        }
        (void)ui.custom_item("##side_bottom", {1.0f, 12.0f});
    }

    settle_press(p);
    draw_drop_hint(p);
    draw_rename(p);
    context_menu(p);
    icon_popup(p);
}

// --- click and drag -------------------------------------------------------------------------------------------------

bool Sidebar::valid(const Target& t) const
{
    const auto& secs = env_.layout.sections();
    switch (t.kind) {
    case Kind::header: return t.section < secs.size();
    case Kind::item:   return t.section < secs.size() && t.index < secs[t.section].items.size() && secs[t.section].items[t.index].path == t.path;
    case Kind::drive:
    case Kind::visit:  return true;
    case Kind::empty:
    case Kind::note:
    case Kind::none:   return false;
    }
    return false;
}

const Sidebar::Row* Sidebar::row_for(const Target& t) const
{
    for (const Row& r : rows_) {
        if (r.kind != t.kind) { continue; }
        if (t.kind == Kind::drive || t.kind == Kind::visit ? r.path == t.path : (r.section == t.section && r.index == t.index)) { return &r; }
    }
    return nullptr;
}

void Sidebar::click(const Target& t)
{
    if (!valid(t)) { return; }
    switch (t.kind) {
    case Kind::header:
        (void)env_.layout.set_collapsed(t.section, !env_.layout.sections()[t.section].collapsed);
        break;
    case Kind::item:
        if (probe_.missing(t.path)) {
            env_.cmd.toast(trf("This folder is gone: {}", t.path), toast_kind::warning);
            probe_.poke();
        } else if (const std::string& q = env_.layout.sections()[t.section].items[t.index].query; !q.empty()) {
            // a saved search: the folder, then the query below it (the filter box follows the tab)
            env_.settings.search_subfolders = true;
            env_.cmd.settings_changed();
            model::Tab& tab = env_.tab();
            tab.navigate(t.path);
            core::QueryOptions qo;
            qo.case_sensitive = env_.settings.search_case;
            qo.regex          = env_.settings.search_regex;
            tab.search(core::Query::parse(q, qo));
        } else {
            env_.tab().navigate(t.path);
        }
        break;
    case Kind::drive:
    case Kind::visit:
        if (core::is_device_path(t.path)) { (void)core::shell::open_device(t.path); } else { env_.tab().navigate(t.path); }
        break;
    case Kind::empty:
    case Kind::note:
    case Kind::none:
        break;
    }
}

// a press becomes a click on release, or, once the pointer has moved, a drag that reorders on release
void Sidebar::settle_press(Painter& p)
{
    if (!press_.armed) { return; }
    context&   ui = p.ui();
    const vec2 mp = ui.mouse_pos();

    if (!press_.active && ui.mouse_down(mouse_button::left) && press_.target.kind != Kind::drive && press_.target.kind != Kind::visit) {
        const vec2 d = mp - press_.origin;
        if (d.x * d.x + d.y * d.y > 25.0f) { press_.active = true; }
    }
    if (press_.active && ui.key_pressed(key::escape)) {
        press_ = {};
        return;
    }
    if (ui.mouse_down(mouse_button::left)) {
        if (press_.active) { draw_drag(p); }
        return;
    }

    const Press pr = std::exchange(press_, {});
    if (!pr.active) {
        click(pr.target);
        return;
    }
    if (!valid(pr.target)) { return; }
    const auto& secs = env_.layout.sections();
    if (pr.target.kind == Kind::item) {
        const Spot s = spot_at(mp, false);
        if (s.type == Spot::Type::between) {
            const model::ItemPos from{pr.target.section, pr.target.index};
            (void)env_.cmd.edit_sidebar(trf("Move {}", secs[from.section].items[from.index].label()),
                                        [&](model::SidebarLayout& l) { (void)l.move_item(from, s.section, s.slot); });
        }
    } else if (pr.target.kind == Kind::header) {
        const std::size_t slot = section_slot_at(mp.y).first;
        (void)env_.cmd.edit_sidebar(trf("Move {}", secs[pr.target.section].title),
                                    [&](model::SidebarLayout& l) { (void)l.move_section(pr.target.section, slot); });
    }
}

// the gap between sections the pointer is nearest to: the slot and a line across the sidebar there
std::pair<std::size_t, rect> Sidebar::section_slot_at(float y) const
{
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (y < (blocks_[i].min.y + blocks_[i].max.y) * 0.5f) {
            const float ly = blocks_[i].min.y - 2.0f;
            return {i, rect{{area_.min.x + 12.0f, ly}, {area_.max.x - 12.0f, ly}}};
        }
    }
    const float ly = blocks_.empty() ? area_.min.y : blocks_.back().max.y + 2.0f;
    return {blocks_.size(), rect{{area_.min.x + 12.0f, ly}, {area_.max.x - 12.0f, ly}}};
}

void Sidebar::ghost(Painter& p, const rect& at, char32_t g, color tint, std::string_view label)
{
    const Palette& pal = p.pal();
    p.dl().push_alpha(0.94f);
    p.dl().rect_filled(at.expanded(2.0f), color{0, 0, 0, 60}, 10.0f);
    p.dl().rect_filled(at, pal.raised, 8.0f);
    p.dl().rect_outline(at, pal.accent, 8.0f, 1.5f);
    if (g != 0) { p.glyph_center(rect::from_size({at.min.x + 12.0f, at.center().y - 12.0f}, {24.0f, 24.0f}), g, tint); }
    p.text_left(rect{{at.min.x + (g != 0 ? 48.0f : 12.0f), at.min.y}, {at.max.x - 12.0f, at.max.y}}, label, pal.text, env_.fonts.body);
    p.dl().pop_alpha();
}

void Sidebar::draw_drag(Painter& p)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Target&  t   = press_.target;
    if (!valid(t)) { return; }
    const auto& secs = env_.layout.sections();
    const vec2  mp   = ui.mouse_pos();
    const float h    = p.metrics().side_row;
    const float cy   = std::clamp(mp.y, area_.min.y + h * 0.5f, area_.max.y - h * 0.5f);
    const rect  at{{area_.min.x + 10.0f, cy - h * 0.5f}, {area_.max.x - 10.0f, cy + h * 0.5f}};

    auto fg = ui.layer(layer::foreground);
    std::optional<rect> line; // (drawn last: the ghost sits right on top of it)
    if (t.kind == Kind::item) {
        if (const Row* r = row_for(t)) { p.dl().rect_filled(inset(r->cell), pal.window.scaled_alpha(0.65f), 8.0f); }
        if (const Spot s = spot_at(mp, false); s.type == Spot::Type::between) { line = s.line; }
        char32_t g{};
        color    tint{};
        look(secs[t.section].items[t.index], g, tint, pal);
        ghost(p, at, g, tint, secs[t.section].items[t.index].label());
    } else if (t.kind == Kind::header) {
        if (t.section < blocks_.size()) { p.dl().rect_filled(blocks_[t.section], pal.window.scaled_alpha(0.65f), 8.0f); }
        line = section_slot_at(mp.y).second;
        ghost(p, at, 0, pal.text, upper(secs[t.section].title));
    }
    if (line && line->center().y >= area_.min.y && line->center().y <= area_.max.y) { insertion_line(p, *line); }
}

// files dragged in from outside (or from the list): outline the folder they would go into, or the gap they would pin to
void Sidebar::draw_drop_hint(Painter& p)
{
    if (!drop_ || hint_.type != Spot::Type::between) { return; }
    // (the empty section's placeholder outlines itself instead)
    const auto first = std::ranges::find_if(rows_, [&](const Row& r) { return r.kind == Kind::empty && r.section == hint_.section; });
    if (first != rows_.end()) { return; }
    if (hint_.line.center().y < area_.min.y || hint_.line.center().y > area_.max.y) { return; }
    auto fg = p.ui().layer(layer::foreground);
    insertion_line(p, hint_.line);
}

void Sidebar::pin_current(std::size_t section)
{
    const std::string path = env_.tab().path();
    if (path.empty() || !core::path::is_directory(path)) {
        env_.cmd.toast(tr("Open a folder first"), toast_kind::info);
        return;
    }
    if (env_.layout.find(path)) {
        env_.cmd.toast(tr("Already in the sidebar"), toast_kind::info);
        return;
    }
    const std::string name = core::path::leaf(path);
    if (env_.cmd.edit_sidebar(trf("Pin {}", name), [&](model::SidebarLayout& l) { (void)l.pin(section, k_end, path); })) {
        env_.cmd.toast(trf("Pinned {}", name), toast_kind::info);
    }
}

// --- rename ---------------------------------------------------------------------------------------------------------

void Sidebar::begin_rename(const Target& t)
{
    if (!valid(t)) { return; }
    const int gen = rename_.gen + 1; // (a fresh window each time: it opens on top of everything)
    rename_       = Rename{};
    rename_.gen   = gen;
    rename_.active = true;
    rename_.target = t;
    const auto& secs = env_.layout.sections();
    rename_.text = t.kind == Kind::item ? secs[t.section].items[t.index].label() : secs[t.section].title;
}

// the label turns into a text field right where it is
void Sidebar::draw_rename(Painter& p)
{
    if (!rename_.active) { return; }
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    if (!valid(rename_.target)) { rename_.active = false; return; }
    const Row* row = row_for(rename_.target);
    if (row == nullptr) { // (a section that was just made or expanded: its row shows up next frame)
        if (++rename_.miss > 30) { rename_.active = false; }
        return;
    }
    const rect box = row->label;

    bool done = false, commit = false;
    {
        PanelScope panel{ui, "##side_rename" + std::to_string(rename_.gen), box};
        shape_style s;
        s.radius       = radii(7.0f);
        s.fill_top     = s.fill_bottom = pal.raised;
        s.border       = pal.accent;
        s.border_width = 1.5f;
        p.dl().shape(box, s);
        ui.push_var(style_var::frame_padding_y, std::max(0.0f, (box.height() - p.line_height(env_.fonts.body)) * 0.5f));
        ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
        ui.push_color(style_color::widget_border, color{0, 0, 0, 0});
        ui.set_next_item_width(box.width());
        if (rename_.age < 3) { ui.request_text_focus("##srn"); }
        (void)ui.input_text("##srn", rename_.text);
        ui.pop_color(2);
        ui.pop_var();
        ++rename_.age;
        if (ui.key_pressed(key::enter)) { done = commit = true; }
        else if (ui.key_pressed(key::escape)) { done = true; }
        else if (rename_.age >= 4 && !ui.field_focused("##srn")) { done = commit = true; } // clicking elsewhere keeps the name
    }
    if (!done) { return; }
    rename_.active = false;
    if (!commit) { return; }

    const Target t    = rename_.target;
    const std::string text = trimmed(rename_.text);
    core::text::note_codepoints(text);
    if (t.kind == Kind::item) {
        (void)env_.cmd.edit_sidebar(trf("Rename {}", env_.layout.sections()[t.section].items[t.index].label()),
                                    [&](model::SidebarLayout& l) { (void)l.rename_item({t.section, t.index}, text); });
    } else if (!text.empty()) {
        (void)env_.cmd.edit_sidebar(tr("Rename section"), [&](model::SidebarLayout& l) { (void)l.rename_section(t.section, text); });
    }
}

// --- icon and colour picker -----------------------------------------------------------------------------------------

namespace {

constexpr char32_t k_icons[] = {
    glyph::folder_open, glyph::home, glyph::desktop, glyph::download, glyph::document, glyph::picture, glyph::music, glyph::video,
    glyph::drive, glyph::usb, glyph::code, glyph::zip, glyph::app, glyph::link, glyph::pin, glyph::lock,
    0xe734, 0xeb51, 0xe753, 0xe7fc, 0xe821, 0xe8f1, 0xe774, 0xe715,   // star heart cloud game work library globe mail
    0xe722, 0xe8ec, 0xe787, 0xe719, 0xe7c1, glyph::gear, glyph::info, glyph::disc,
};
constexpr std::uint32_t k_colors[] = {
    0, 0xffe5484d, 0xfff5a524, 0xffe9c46a, 0xff3ecf8e, 0xff2dd4bf, 0xff5b8def, 0xffa78bfa, 0xfff472b6,
};

} // namespace

// a small popover: pick a glyph and a colour for a pinned folder (both optional: "automatic" is the folder's own look)
void Sidebar::icon_popup(Painter& p)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    if (icon_want_) {
        icon_want_ = false;
        ui.open_popup("##side_icon", icon_pos_);
    }
    auto pop = ui.popup("##side_icon", 304.0f);
    if (!pop) { return; }
    if (!valid(icon_target_) || icon_target_.kind != Kind::item) {
        ui.close_popup();
        return;
    }
    const model::ItemPos at{icon_target_.section, icon_target_.index};
    const model::SidebarItem it = env_.layout.sections()[at.section].items[at.index];
    char32_t glyph_now = it.glyph;
    std::uint32_t color_now = it.color;
    char32_t auto_glyph{};
    color    auto_tint{};
    look(model::SidebarItem{it.path, {}, it.known, 0, 0}, auto_glyph, auto_tint, pal);
    const color tint = color_now != 0 ? from_argb(color_now) : auto_tint;

    const item_result title = ui.custom_item("##ic_title", {288.0f, 28.0f});
    p.text_left(title.bounds, tr("Icon and colour"), pal.text, env_.fonts.bold, 6.0f);

    constexpr int   cols = 8;
    constexpr float cell = 34.0f;
    const int       rows = (static_cast<int>(std::size(k_icons)) + cols - 1) / cols;
    const item_result grid = ui.custom_item("##ic_grid", {288.0f, static_cast<float>(rows) * cell + 4.0f});
    const vec2 mp = ui.mouse_pos();
    bool changed = false;
    for (std::size_t i = 0; i < std::size(k_icons); ++i) {
        const rect c = rect::from_size({grid.bounds.min.x + 4.0f + static_cast<float>(i % cols) * cell, grid.bounds.min.y + 2.0f + static_cast<float>(i / cols) * cell}, {cell - 2.0f, cell - 2.0f});
        const bool hot = grid.hovered && c.contains(mp);
        const bool sel = glyph_now == k_icons[i];
        if (sel) { p.dl().rect_filled(c, pal.accent.scaled_alpha(0.22f), 8.0f); p.dl().rect_outline(c, pal.accent.scaled_alpha(0.7f), 8.0f, 1.0f); }
        else if (hot) { p.dl().rect_filled(c, pal.hover, 8.0f); }
        p.glyph_center(c, k_icons[i], tint);
        if (hot && ui.mouse_released(mouse_button::left)) { glyph_now = k_icons[i]; changed = true; }
    }

    const item_result sw = ui.custom_item("##ic_colors", {288.0f, 40.0f});
    for (std::size_t i = 0; i < std::size(k_colors); ++i) {
        const rect c = rect::from_size({sw.bounds.min.x + 8.0f + static_cast<float>(i) * 30.0f, sw.bounds.min.y + 8.0f}, {24.0f, 24.0f});
        const bool hot = sw.hovered && c.contains(mp);
        const bool sel = color_now == k_colors[i];
        const float r = c.width() * 0.5f;
        if (k_colors[i] == 0) {
            p.dl().rect_outline(c, pal.text_faint, r, 1.5f); // automatic: an empty ring
        } else {
            p.dl().rect_filled(c, from_argb(k_colors[i]), r);
        }
        if (sel) { p.dl().rect_outline(c.expanded(3.0f), pal.text, r + 3.0f, 1.5f); }
        else if (hot) { p.dl().rect_outline(c.expanded(2.0f), pal.text_dim, r + 2.0f, 1.0f); }
        if (hot) { ui.tooltip(k_colors[i] == 0 ? tr("Automatic") : tr("Colour")); }
        if (hot && ui.mouse_released(mouse_button::left)) { color_now = k_colors[i]; changed = true; }
    }

    const item_result reset = ui.custom_item("##ic_reset", {288.0f, 30.0f});
    const rect rb = rect{{reset.bounds.min.x + 4.0f, reset.bounds.min.y + 2.0f}, {reset.bounds.max.x - 4.0f, reset.bounds.max.y - 2.0f}};
    if (reset.hovered) { p.dl().rect_filled(rb, pal.hover, 7.0f); }
    p.text_center(rb, tr("Back to automatic"), pal.text_dim, env_.fonts.small);
    if (reset.hovered && ui.mouse_released(mouse_button::left)) {
        glyph_now = 0;
        color_now = 0;
        changed   = true;
    }

    if (changed) {
        (void)env_.cmd.edit_sidebar(trf("Change icon of {}", it.label()),
                                    [&](model::SidebarLayout& l) { (void)l.set_item_style(at, glyph_now, color_now); });
    }
}

// --- context menu ---------------------------------------------------------------------------------------------------

void Sidebar::context_menu(Painter& p)
{
    context& ui = p.ui();
    const MenuDensity density{ui, env_.settings.compact};
    shell_page_.begin_frame();
    auto menu = ui.context_menu("##side_ctx", area_);
    if (!menu) { return; }

    const Target t = valid(menu_) ? menu_ : Target{};
    const auto&  secs = env_.layout.sections();
    const auto   edit = [&](std::string_view label, const std::function<void(model::SidebarLayout&)>& fn) { (void)env_.cmd.edit_sidebar(label, fn); };
    // what every folder menu ends with (terminal, path, properties, Explorer's entries)
    const auto folder_tail = [&](const std::string& folder) {
        ui.menu_separator();
        menus::terminal(ui, env_, folder);
        ui.menu_separator();
        menus::path_items(ui, env_, {folder});
        menus::pin_item(ui, env_, folder);
        ui.menu_separator();
        menus::properties_item(ui, env_, {folder});
        menus::more_options(ui, env_, shell_page_, shell_menu_, {folder}, false);
        menus::windows_items(ui, env_, {folder});
    };

    if (t.kind == Kind::item) {
        // (copies: the edits below change the layout under these references)
        const model::SidebarItem it = secs[t.section].items[t.index];
        const std::string label     = it.label();
        const bool gone             = probe_.missing(it.path);
        const bool first            = t.index == 0;
        const bool last             = t.index + 1 >= secs[t.section].items.size();
        std::vector<std::pair<std::size_t, std::string>> others;
        for (std::size_t s = 0; s < secs.size(); ++s) {
            if (s != t.section && !secs[s].dynamic()) { others.emplace_back(s, secs[s].title); }
        }
        const model::ItemPos at{t.section, t.index};

        if (ui.menu_item(tr("Open"), "", false, !gone)) { env_.tab().navigate(it.path); }
        menus::open_in(ui, env_, it.path, !gone);
        ui.menu_separator();
        if (ui.menu_item(tr("Rename"))) { begin_rename(t); }
        if (ui.menu_item(tr("Change icon and colour"))) {
            icon_target_ = t;
            icon_want_   = true;
            icon_pos_    = ui.mouse_pos();
        }
        if (ui.menu_item(tr("Move up"), "", false, !first)) { edit("Move " + label, [=](model::SidebarLayout& l) { (void)l.move_item(at, at.section, at.index - 1); }); }
        if (ui.menu_item(tr("Move down"), "", false, !last)) { edit("Move " + label, [=](model::SidebarLayout& l) { (void)l.move_item(at, at.section, at.index + 2); }); }
        if (auto sub = ui.menu(tr("Move to section"), !others.empty())) {
            for (const auto& other : others) {
                const std::size_t to = other.first;
                if (ui.menu_item(other.second)) { edit(trf("Move {}", label), [=](model::SidebarLayout& l) { (void)l.move_item(at, to, k_end); }); }
            }
        }
        ui.menu_separator();
        if (ui.menu_item(tr("Remove from sidebar"))) { edit("Unpin " + label, [=](model::SidebarLayout& l) { (void)l.remove_item(at); }); }
        if (!gone) { folder_tail(it.path); }
        return;
    }

    if (t.kind == Kind::drive && core::is_device_path(t.path)) {
        const std::string id = t.path;
        if (ui.menu_item(tr("Open in File Explorer"))) { (void)core::shell::open_device(id); }
        return;
    }

    if (t.kind == Kind::drive && core::recycle::inside(t.path)) { // the Recycle Bin row
        const std::string bin = t.path;
        if (ui.menu_item(tr("Open"))) { env_.tab().navigate(bin); }
        menus::open_in(ui, env_, bin);
        ui.menu_separator();
        if (ui.menu_item(tr("Empty Recycle Bin"))) { env_.cmd.empty_recycle_bin(core::recycle::drive_of(bin)); }
        return;
    }

    if (t.kind == Kind::drive) {
        const std::string root      = t.path;
        const auto        found     = std::ranges::find(drives_, root, &core::Place::path);
        const std::string bin       = found != drives_.end() ? found->bin : std::string{};
        const bool        removable = found != drives_.end() && found->removable;
        const bool        network   = found != drives_.end() && found->network;
        if (ui.menu_item(tr("Open"))) { env_.tab().navigate(root); }
        menus::open_in(ui, env_, root);
        ui.menu_separator();
        if (removable && ui.menu_item(tr("Eject"))) {
            auto op = std::make_shared<DriveOp>();
            drive_op_ = op;
            std::thread{[op, root, wake = env_.window.waker()] {
                std::string err;
                op->ok  = core::eject_drive(root, err);
                op->msg = op->ok ? trf("Ejected {}", root) : err;
                op->done.store(true, std::memory_order_release);
                if (wake) { wake(); }
            }}.detach();
        }
        if (network && ui.menu_item(tr("Disconnect"))) {
            auto op = std::make_shared<DriveOp>();
            drive_op_ = op;
            std::thread{[op, root, wake = env_.window.waker()] {
                std::string err;
                op->ok  = core::disconnect_network_drive(root, err);
                op->msg = op->ok ? trf("Disconnected {}", root) : err;
                op->done.store(true, std::memory_order_release);
                if (wake) { wake(); }
            }}.detach();
        }
        if (!bin.empty()) {
            if (ui.menu_item(tr("Open Recycle Bin"))) { env_.tab().navigate(bin); }
            if (ui.menu_item(tr("Empty Recycle Bin"))) { env_.cmd.empty_recycle_bin(root); }
        }
        if (ui.menu_item(tr("Hide from sidebar"))) { edit("Hide " + root, [=](model::SidebarLayout& l) { (void)l.hide_drive(root); }); }
        folder_tail(root);
        return;
    }

    if (t.kind == Kind::visit) {
        const std::string path = t.path;
        if (ui.menu_item(tr("Open"))) { env_.tab().navigate(path); }
        menus::open_in(ui, env_, path);
        ui.menu_separator();
        if (ui.menu_item(tr("Remove from this list"))) { env_.visits.forget(path); }
        folder_tail(path);
        return;
    }

    if (t.kind == Kind::header) {
        const model::SidebarSection sec = secs[t.section];
        const std::size_t s = t.section;
        if (!sec.dynamic() && ui.menu_item(tr("Pin current folder here"))) { pin_current(s); }
        if (ui.menu_item(tr("Rename"))) { begin_rename(t); }
        if (ui.menu_item(sec.collapsed ? tr("Expand") : tr("Collapse"))) { (void)env_.layout.set_collapsed(s, !sec.collapsed); }
        if (ui.menu_item(tr("Move up"), "", false, s > 0)) { edit("Move " + sec.title, [=](model::SidebarLayout& l) { (void)l.move_section(s, s - 1); }); }
        if (ui.menu_item(tr("Move down"), "", false, s + 1 < secs.size())) { edit("Move " + sec.title, [=](model::SidebarLayout& l) { (void)l.move_section(s, s + 2); }); }
        ui.menu_separator();
        if (sec.auto_kind != model::SidebarSection::Auto::none && ui.menu_item(tr("Clear history"))) { env_.visits.clear(); }
        if (ui.menu_item(tr("Remove section"))) { env_.cmd.request_remove_section(s); }
        return;
    }

    // empty space
    if (ui.menu_item(tr("New section"))) {
        // after the last folder section, so it does not land below the drives
        std::size_t slot = 0;
        for (std::size_t s = 0; s < secs.size(); ++s) {
            if (!secs[s].dynamic()) { slot = s + 1; }
        }
        if (env_.cmd.edit_sidebar(tr("New section"), [=](model::SidebarLayout& l) { (void)l.add_section(slot, "New section"); })) {
            begin_rename(Target{Kind::header, slot, 0, {}});
        }
    }
    std::size_t auto_slot = 0; // (an automatic list goes where a new section would: below the pins, above the drives)
    for (std::size_t s = 0; s < secs.size(); ++s) {
        if (!secs[s].drives) { auto_slot = s + 1; }
    }
    if (!env_.layout.has_drives() && ui.menu_item(tr("Show This PC"))) {
        edit(tr("Show This PC"), [](model::SidebarLayout& l) { (void)l.add_drives_section(k_end); });
    }
    if (!env_.layout.has_auto(model::SidebarSection::Auto::recent) && ui.menu_item(tr("Show recent folders"))) {
        edit(tr("Show recent folders"), [=](model::SidebarLayout& l) { (void)l.add_auto_section(auto_slot, model::SidebarSection::Auto::recent); });
    }
    if (!env_.layout.has_auto(model::SidebarSection::Auto::frequent) && ui.menu_item(tr("Show frequent folders"))) {
        edit(tr("Show frequent folders"), [=](model::SidebarLayout& l) { (void)l.add_auto_section(auto_slot, model::SidebarSection::Auto::frequent); });
    }
    ui.menu_separator();
    if (ui.menu_item(tr("Connect Google Drive (rclone)"))) {
        auto op = std::make_shared<DriveOp>();
        drive_op_ = op;
        std::thread{[op, wake = env_.window.waker()] {
            std::string msg;
            op->ok  = core::rclone::connect_google_drive(msg);
            op->msg = msg;
            op->done.store(true, std::memory_order_release);
            if (wake) { wake(); }
        }}.detach();
    }
    if (const auto& hidden = env_.layout.hidden_drives(); !hidden.empty()) {
        const std::vector<std::string> roots = hidden; // (a copy: showing one changes the list)
        if (auto sub = ui.menu(tr("Hidden drives"))) {
            for (const std::string& root : roots) {
                std::string name = root;
                for (const core::Place& d : drives_) {
                    if (d.path == root) { name = d.name; }
                }
                if (ui.menu_item(trf("Show {}", name))) { edit(trf("Show {}", root), [=](model::SidebarLayout& l) { (void)l.show_drive(root); }); }
            }
        }
    }
    ui.menu_separator();
    if (ui.menu_item(tr("Copy layout"))) { env_.cmd.copy_sidebar(); }
    if (ui.menu_item(tr("Paste layout"), "", false, paste_ok_)) { env_.cmd.paste_sidebar(); }
    ui.menu_separator();
    if (ui.menu_item(tr("Reset sidebar to defaults"))) { edit(tr("Reset sidebar"), [](model::SidebarLayout& l) { l.reset(); }); }
}

} // namespace prism::ui
