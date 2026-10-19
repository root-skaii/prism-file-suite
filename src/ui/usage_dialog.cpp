#include "ui/usage_dialog.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "ui/tr.hpp"
#include "core/shell_ops.hpp"
#include "ui/tr.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"
#include "ui/tag_colors.hpp"
#include "ui/tr.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>

namespace prism::ui {

using namespace strata;

namespace {

constexpr float indent_step = 18.0f;
constexpr float size_col_w  = 170.0f;
constexpr float files_col_w = 96.0f;

// one colour per top-level item, shared by its bar and its children
[[nodiscard]] color hue_of(std::size_t top) { return tag_color(static_cast<int>(top)); }

[[nodiscard]] std::string id_of(std::size_t row) { return std::format("##r{}", row); }

[[nodiscard]] std::vector<const core::UsageNode*> sorted(const core::UsageNode& n, int col, bool asc)
{
    std::vector<const core::UsageNode*> v;
    v.reserve(n.children.size());
    for (const core::UsageNode& c : n.children) { v.push_back(&c); }
    std::ranges::stable_sort(v, [&](const core::UsageNode* a, const core::UsageNode* b) {
        if (a->lumped != b->lumped) { return b->lumped; } // ("N smaller files" always last)
        int c = 0;
        switch (col) {
        case 0: c = core::text::compare_natural(a->name, b->name); break;
        case 2: c = a->files < b->files ? -1 : (a->files > b->files ? 1 : 0); break;
        default: c = a->bytes < b->bytes ? -1 : (a->bytes > b->bytes ? 1 : 0); break;
        }
        return asc ? c < 0 : c > 0;
    });
    return v;
}

} // namespace

void UsageDialog::open(std::string folder)
{
    if (folder.empty()) { return; }
    history_.clear();
    rescan(std::move(folder), false, false);
    want_open_ = true;
}

std::string UsageDialog::cache_key(std::string path)
{
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) { path.pop_back(); }
    for (char& c : path) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return path;
}

// remember_view: the view being left stays reachable (Back, the crumbs)
void UsageDialog::rescan(std::string folder, bool force, bool remember_view)
{
    if (remember_view && root_) { history_.push_back({base_, root_, trail_}); }
    base_ = std::move(folder);
    root_.reset();
    trail_.clear();
    open_.clear();
    rows_.clear();
    pick_ = {};
    dirty_ = true;
    ++generation_;
    scan_.cancel();
    if (!force) {
        const auto it = cache_.find(cache_key(base_));
        if (it != cache_.end() && ::GetTickCount64() - it->second.when < cache_ttl_ms) {
            root_  = it->second.root;
            trail_ = {root_.get()};
            return;
        }
    }
    scan_.start(base_, env_.window.waker());
}

void UsageDialog::restore(std::size_t k, std::size_t depth)
{
    if (k >= history_.size()) { return; }
    View v = std::move(history_[k]);
    history_.resize(k);
    base_  = std::move(v.base);
    root_  = std::move(v.root);
    trail_ = std::move(v.trail);
    trail_.resize(std::min(trail_.size(), depth + 1));
    open_.clear();
    pick_  = {};
    dirty_ = true;
    ++generation_;
}

void UsageDialog::back()
{
    if (trail_.size() > 1) { go_to(trail_.size() - 2); }
    else if (!history_.empty()) { restore(history_.size() - 1, history_.back().trail.size() - 1); }
}

std::string UsageDialog::current_path() const
{
    std::string path = base_;
    for (std::size_t i = 1; i < trail_.size(); ++i) { path = core::path::join(path, trail_[i]->name); }
    return path;
}

std::string UsageDialog::path_of(const Pick& pick) const
{
    std::string path = current_path();
    for (const core::UsageNode* n : pick.chain) { path = core::path::join(path, n->name); }
    return path;
}

void UsageDialog::go_to(std::size_t depth)
{
    if (depth + 1 >= trail_.size()) { return; }
    trail_.resize(depth + 1);
    pick_  = {};
    dirty_ = true;
    ++generation_;
}

void UsageDialog::look_inside(const Pick& pick)
{
    if (pick.node == nullptr || !pick.node->is_dir) { return; }
    // every folder on the way is stepped into; one the scan did not keep a level of is scanned on its own
    for (const core::UsageNode* n : pick.chain) {
        if (!n->is_dir) { return; }
        if (n->children.empty()) {
            // (the steps before this one are already on the trail, so current_path() holds them)
            rescan(core::path::join(current_path(), n->name), false, true);
            return;
        }
        trail_.push_back(n);
    }
    pick_  = {};
    dirty_ = true;
    ++generation_;
}

UsageDialog::Pick UsageDialog::pick_at(std::size_t row) const
{
    Pick p;
    p.node = rows_[row].node;
    for (int i = static_cast<int>(row); i >= 0; i = rows_[static_cast<std::size_t>(i)].parent) { p.chain.push_back(rows_[static_cast<std::size_t>(i)].node); }
    std::ranges::reverse(p.chain);
    return p;
}

// the visible rows as a flat list (open folders expanded). rebuilt only when something structural changed; drawing then
// touches just the rows in view, so a folder with 100k rows scrolls as smoothly as one with ten
void UsageDialog::rebuild_rows(context& ui, const FontSet& f, const core::UsageNode& cur)
{
    rows_.clear();
    float widest = 0.0f;
    const auto walk = [&](auto&& self, const core::UsageNode& parent, int parent_row, std::size_t top_index, int depth) -> void {
        std::size_t i = 0;
        for (const core::UsageNode* n : sorted(parent, sort_col_, sort_asc_)) {
            const std::size_t top = depth == 0 ? i : top_index;
            ++i;
            const int me = static_cast<int>(rows_.size());
            rows_.push_back({n, parent_row, depth, top});
            widest = std::max(widest, 6.0f + static_cast<float>(depth) * indent_step + 24.0f + ui.font().measure(f.body, n->name).x + 16.0f);
            if (n->is_dir && open_.contains(n)) { self(self, *n, me, top, depth + 1); }
        }
    };
    walk(walk, cur, -1, 0, 0);
    name_w_    = widest;
    rows_root_ = &cur;
    dirty_     = false;
}

// header (sticky, follows the sideways scroll) and a clipped list of rows
void UsageDialog::draw_tree(context& ui, const FontSet& f, const Palette& pal, const core::UsageNode& cur, float row_h, float height)
{
    if (dirty_ || rows_root_ != &cur) { rebuild_rows(ui, f, cur); }

    const float view_w = ui.content_width();
    const float row_w  = std::max(view_w - 14.0f, name_w_ + size_col_w + files_col_w);
    const float name_w = row_w - size_col_w - files_col_w;
    const double total = cur.bytes > 0 ? static_cast<double>(cur.bytes) : 1.0;

    // header
    {
        const float hh = row_h + 8.0f;
        const item_result r = ui.custom_item("##du_hdr", {view_w, hh});
        const float x0 = r.bounds.min.x - scroll_x_;
        const std::array<std::pair<std::string_view, int>, 3> cols{{{tr("Name"), 0}, {tr("Size"), 1}, {tr("Files"), 2}}};
        const std::array<float, 3> start{x0, x0 + name_w, x0 + name_w + size_col_w};
        const std::array<float, 3> width{name_w, size_col_w, files_col_w};
        ui.draw().push_clip(r.bounds);
        for (std::size_t i = 0; i < 3; ++i) {
            const rect cell = rect::from_size({start[i], r.bounds.min.y}, {width[i], hh});
            const bool hot = r.hovered && cell.contains(ui.mouse_pos());
            if (hot) { ui.draw().rect_filled(cell.expanded(-1.0f), pal.hover, 5.0f); }
            const bool active = sort_col_ == cols[i].second;
            const std::string label{cols[i].first};
            const float tw = ui.font().measure(f.body, label).x;
            const float tx = i == 0 ? cell.min.x + 8.0f : cell.max.x - tw - 22.0f;
            ui.draw().text({tx, cell.min.y + (hh - ui.font().line_height(f.body)) * 0.5f}, active ? pal.text : pal.text_dim, label, f.body);
            if (active) {
                const vec2 c{tx + tw + 9.0f, cell.center().y};
                if (sort_asc_) { ui.draw().triangle_filled({c.x - 4.0f, c.y + 2.0f}, {c.x + 4.0f, c.y + 2.0f}, {c.x, c.y - 3.0f}, pal.accent); }
                else { ui.draw().triangle_filled({c.x - 4.0f, c.y - 2.0f}, {c.x + 4.0f, c.y - 2.0f}, {c.x, c.y + 3.0f}, pal.accent); }
            }
            if (hot && ui.mouse_clicked(mouse_button::left)) {
                if (sort_col_ == cols[i].second) { sort_asc_ = !sort_asc_; } else { sort_col_ = cols[i].second; sort_asc_ = sort_col_ == 0; }
                dirty_ = true;
            }
        }
        ui.draw().pop_clip();
    }

    Pick activated;
    Pick selected;
    int  toggled = -1; // a row whose folder opens / closes
    const float list_h = std::max(60.0f, height - (row_h + 8.0f));
    auto list = ui.child("##du_rows", {view_w, list_h}, child_flags::no_padding | child_flags::horizontal);
    if (!list) { return; }
    scroll_x_ = ui.scroll_x();

    if (rows_.empty()) {
        const item_result r = ui.custom_item("##du_none", {view_w - 14.0f, 80.0f});
        const std::string msg = std::string{cur.bytes == 0 ? tr("Nothing here takes any space") : tr("Nothing to show")};
        ui.draw().text({r.bounds.center().x - ui.font().measure(f.body, msg).x * 0.5f, r.bounds.center().y - ui.font().line_height(f.body) * 0.5f}, pal.text_dim, msg, f.body);
    }

    list_clipper clip(ui, rows_.size(), row_h);
    while (clip.step()) {
        for (int ri = clip.begin(); ri < clip.end(); ++ri) {
            const std::size_t i = static_cast<std::size_t>(ri);
            const Row& rw = rows_[i];
            const core::UsageNode& n = *rw.node;
            const bool has_kids = n.is_dir && !n.children.empty();
            const bool is_open  = has_kids && open_.contains(&n);
            const bool deeper   = n.is_dir && !has_kids && n.bytes > 0; // the scan stopped above it: opening it scans it

            const item_result r = ui.custom_item(id_of(i), {row_w, row_h});
            const rect row = r.bounds;
            const bool sel = pick_.node == &n;
            if (sel) { ui.draw().rect_filled(row, pal.selection, 5.0f); }
            else if (r.hovered) { ui.draw().rect_filled(row, pal.hover, 5.0f); }

            const float indent = 6.0f + static_cast<float>(rw.depth) * indent_step;
            const rect  arrow  = rect::from_size({row.min.x + indent, row.min.y}, {18.0f, row_h});
            const bool  on_arrow = r.hovered && arrow.contains(ui.mouse_pos());
            if (has_kids || deeper) {
                const vec2 c = arrow.center();
                const color ac = on_arrow ? pal.text : pal.text_dim;
                if (is_open) { ui.draw().triangle_filled({c.x - 4.0f, c.y - 2.0f}, {c.x + 4.0f, c.y - 2.0f}, {c.x, c.y + 3.0f}, ac); }
                else { ui.draw().triangle_filled({c.x - 2.0f, c.y - 4.0f}, {c.x - 2.0f, c.y + 4.0f}, {c.x + 3.0f, c.y}, ac); }
            }
            const float ty = row.min.y + (row_h - ui.font().line_height(f.body)) * 0.5f;
            ui.draw().text({row.min.x + indent + 24.0f, ty}, n.lumped ? pal.text_dim : pal.text, n.name, f.body);

            // size: a bar as long as the share of the folder on screen, behind the number
            const float bx = row.min.x + name_w;
            const double share = std::clamp(static_cast<double>(n.bytes) / total, 0.0, 1.0);
            ui.draw().rect_filled(rect::from_size({bx + 4.0f, row.min.y + 4.0f}, {(size_col_w - 12.0f) * static_cast<float>(share), row_h - 8.0f}),
                                  (n.lumped ? pal.border : hue_of(rw.top)).scaled_alpha(0.38f), 4.0f);
            const std::string size = core::path::format_size(n.bytes);
            ui.draw().text({bx + size_col_w - ui.font().measure(f.body, size).x - 10.0f, ty}, pal.text, size, f.body);
            const std::string files = std::format("{}", n.files);
            ui.draw().text({bx + size_col_w + files_col_w - ui.font().measure(f.small, files).x - 10.0f, row.min.y + (row_h - ui.font().line_height(f.small)) * 0.5f}, pal.text_dim, files, f.small);

            if (r.hovered && ui.mouse_clicked(mouse_button::left)) {
                if (on_arrow && has_kids) { toggled = ri; }
                else if (on_arrow && deeper) { activated = pick_at(i); }
                else { selected = pick_at(i); }
            }
            if (r.hovered && !on_arrow && ui.item_double_clicked() && n.is_dir) { // a double click does what the arrow does
                if (has_kids) { toggled = ri; } else if (deeper) { activated = pick_at(i); }
            }
            if (r.hovered && ui.mouse_clicked(mouse_button::right)) {
                ctx_ = pick_at(i);
                selected = ctx_;
                ui.open_popup_menu("##du_ctx", ui.mouse_pos());
            }
        }
    }

    // keep the selected row in view after the keyboard moved it
    if (follow_) {
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].node != pick_.node) { continue; }
            const float top = static_cast<float>(i) * row_h;
            if (top < ui.scroll_y()) { ui.set_scroll_y(top); }
            else if (top + row_h > ui.scroll_y() + list_h) { ui.set_scroll_y(top + row_h - list_h); }
            break;
        }
        follow_ = false;
    }

    if (toggled >= 0) {
        const core::UsageNode* n = rows_[static_cast<std::size_t>(toggled)].node;
        if (open_.contains(n)) { open_.erase(n); } else { open_.insert(n); }
        dirty_ = true;
    }
    if (selected.node != nullptr) { pick_ = selected; }
    if (activated.node != nullptr) { look_inside(activated); }
}

// Up / Down walk the rows, Right opens a folder (or steps into it), Left closes it (or steps out), Enter opens / closes
void UsageDialog::tree_keys(context& ui)
{
    if (rows_.empty() || ui.want_text_input()) { return; }
    int at = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == pick_.node) { at = static_cast<int>(i); break; }
    }
    const auto go = [&](int to) {
        to = std::clamp(to, 0, static_cast<int>(rows_.size()) - 1);
        pick_ = pick_at(static_cast<std::size_t>(to));
        follow_ = true;
    };
    if (ui.key_pressed(key::down)) { go(at + 1); }
    else if (ui.key_pressed(key::up)) { go(at < 0 ? 0 : at - 1); }
    else if (ui.key_pressed(key::home)) { go(0); }
    else if (ui.key_pressed(key::end)) { go(static_cast<int>(rows_.size()) - 1); }
    else if (ui.key_pressed(key::page_down)) { go(at + 12); }
    else if (ui.key_pressed(key::page_up)) { go(at - 12); }
    if (pick_.node == nullptr) { return; }
    at = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].node == pick_.node) { at = static_cast<int>(i); break; }
    }
    if (at < 0) { return; }
    const core::UsageNode* n = pick_.node;
    const bool has_kids = n->is_dir && !n->children.empty();
    const bool deeper   = n->is_dir && !has_kids && n->bytes > 0;
    const auto toggle = [&] {
        if (open_.contains(n)) { open_.erase(n); } else { open_.insert(n); }
        dirty_ = true;
    };
    if (ui.key_pressed(key::right)) {
        if (has_kids && !open_.contains(n)) { toggle(); }
        else if (has_kids) { go(at + 1); }
        else if (deeper) { look_inside(pick_); }
    } else if (ui.key_pressed(key::left)) {
        if (has_kids && open_.contains(n)) { toggle(); }
        else if (rows_[static_cast<std::size_t>(at)].parent >= 0) { // out to the folder it is in
            pick_ = pick_at(static_cast<std::size_t>(rows_[static_cast<std::size_t>(at)].parent));
            follow_ = true;
        }
    } else if (ui.key_pressed(key::enter)) {
        if (has_kids) { toggle(); } else if (deeper) { look_inside(pick_); }
    }
}

void UsageDialog::frame(Painter& p)
{
    context& ui = p.ui();
    if (want_open_) {
        want_open_ = false;
        shown_     = true;
        Kit::reset_focus();
        ui.open_modal("Disk usage");
    }
    if (!shown_) { return; }
    const Palette&    pal = p.pal();
    const FontSet     f   = p.fonts();
    const DialogStyle style{ui, pal};
    const float width = std::min(1100.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Disk usage", {width, 0.0f}, modal_flags::esc_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        scan_.cancel();
        root_.reset();
        trail_.clear();
        history_.clear();
        rows_.clear();
        open_.clear();
        pick_ = ctx_ = {};
        Kit::reset_focus();
        return;
    }
    if (!root_) {
        if (auto r = scan_.result()) {
            if (cache_.size() >= 8) { cache_.clear(); }
            cache_[cache_key(base_)] = {r, ::GetTickCount64()};
            root_  = std::move(r);
            trail_ = {root_.get()};
            dirty_ = true;
            ++generation_;
        }
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    {
        const item_result r = ui.custom_item("##du_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, tr("Disk usage"), pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, root_ ? current_path() : base_, pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##du_close", glyph::close, tr("Close  (Esc)"), true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(8.0f);

    if (!root_) {
        const item_result r = ui.custom_item("##du_run", {ui.content_width(), 34.0f});
        p.text_left(r.bounds, trf("Measuring ... {} files, {} so far", scan_.files(), core::path::format_size(scan_.bytes())), pal.text_dim, f.body, 4.0f);
        {   // no way to know the total up front: a pulse that keeps moving says it is working
            const item_result bar = ui.custom_item("##du_pulse", {ui.content_width(), 10.0f});
            const rect track = rect::from_size({bar.bounds.min.x + 4.0f, bar.bounds.min.y + 2.0f}, {bar.bounds.width() - 8.0f, 6.0f});
            p.dl().rect_filled(track, pal.raised, 3.0f);
            const float x = std::fmod(static_cast<float>(ui.time()) * 0.7f, 1.0f);
            const float len = track.width() * 0.22f;
            p.dl().push_clip(track);
            p.dl().rect_filled(rect::from_size({track.min.x + (track.width() + len) * x - len, track.min.y}, {len, track.height()}), pal.accent, 3.0f);
            p.dl().pop_clip();
            ui.invalidate();
        }
        kit.vgap(14.0f);
        if (kit.button({"##du_cancel", tr("Cancel")})) { ui.close_modal(); }
        return;
    }
    const core::UsageNode& cur = *trail_.back();

    // where you are: every folder from the first scan on, each one clickable (the last is this folder)
    {
        struct Crumb { std::string label; std::size_t view; std::size_t depth; };
        std::vector<Crumb> crumbs;
        const auto add_view = [&](const std::string& base, const std::vector<const core::UsageNode*>& trail, std::size_t view) {
            for (std::size_t i = 0; i < trail.size(); ++i) {
                crumbs.push_back({i == 0 ? (core::path::leaf(base).empty() ? base : core::path::leaf(base)) : trail[i]->name, view, i});
            }
        };
        for (std::size_t k = 0; k < history_.size(); ++k) { add_view(history_[k].base, history_[k].trail, k); }
        add_view(base_, trail_, static_cast<std::size_t>(-1));

        const float line = kit.control_h();
        (void)ui.custom_item("##du_crumb_lead", {2.0f, line});
        const std::size_t first = crumbs.size() > 6 ? crumbs.size() - 6 : 0; // (a very deep path keeps its end in view)
        for (std::size_t i = first; i < crumbs.size(); ++i) {
            ui.same_line();
            const bool last = i + 1 == crumbs.size();
            if (kit.chip("##du_crumb" + std::to_string(i), crumbs[i].label, i == 0 ? glyph::folder : 0, last ? Tone::accent : Tone::neutral, !last, last ? std::string_view{} : "Back to here", line) && !last) {
                if (crumbs[i].view == static_cast<std::size_t>(-1)) { go_to(crumbs[i].depth); } else { restore(crumbs[i].view, crumbs[i].depth); }
                return;
            }
            if (!last) {
                ui.same_line();
                const item_result sep = ui.custom_item("##du_sep" + std::to_string(i), {16.0f, line});
                p.glyph_center(sep.bounds, glyph::chevron_r, pal.text_faint, f.tile);
            }
        }
    }
    kit.vgap(6.0f);

    // the numbers
    {
        std::string biggest = "-", biggest_sub;
        for (const core::UsageNode& c : cur.children) {
            if (c.lumped) { continue; }
            biggest = c.name;
            biggest_sub = core::path::format_size(c.bytes);
            break;
        }
        const std::size_t folders = std::ranges::count_if(cur.children, [](const core::UsageNode& c) { return c.is_dir; });
        const std::string folders_sub = trf_n("{} folder at the top", "{} folders at the top", static_cast<long long>(folders), folders);
        const std::array<Kit::Stat, 3> tiles{{
            {tr("In this folder"), core::path::format_size(cur.bytes), tr("all the files below it")},
            {tr("Files"), std::format("{}", cur.files), folders_sub},
            {tr("Biggest item"), biggest.size() > 22 ? biggest.substr(0, 20) + ".." : biggest, biggest_sub},
        }};
        kit.stat_tiles("##du_tiles", tiles);
    }
    kit.vgap(8.0f);

    const float body_h = std::clamp(ui.display_size().y - 400.0f, 260.0f, 760.0f);
    draw_tree(ui, f, pal, cur, 24.0f, body_h);
    // keyboard: arrows walk the tree, Enter opens / closes, Backspace goes back out
    tree_keys(ui);
    if (ui.key_pressed(key::backspace) && (trail_.size() > 1 || !history_.empty())) { back(); return; }

    if (auto menu = ui.popup_menu("##du_ctx")) {
        if (ctx_.node != nullptr) {
            const std::string path = path_of(ctx_);
            if (ctx_.node->is_dir && ui.menu_item(tr("Look inside"), "")) { look_inside(ctx_); }
            if (ui.menu_item(ctx_.node->is_dir ? tr("Show in the file list") : tr("Show its folder in the file list"))) {
                env_.tab().navigate(ctx_.node->is_dir ? path : core::path::parent(path));
                ui.close_modal();
            }
            if (ui.menu_item(tr("Open in a new tab"))) { env_.cmd.open_tab(ctx_.node->is_dir ? path : core::path::parent(path), true); ui.close_modal(); }
            if (ui.menu_item(tr("Open in a new window"))) { env_.cmd.open_window(ctx_.node->is_dir ? path : core::path::parent(path)); ui.close_modal(); }
            if (ui.menu_item(tr("Copy path"))) { (void)core::shell::set_clipboard_text(path); }
        }
    }

    // the line at the bottom
    kit.vgap(10.0f);
    {
        const bool can_back = trail_.size() > 1 || !history_.empty();
        const float bw_back = kit.button_width(tr("Back"), true);
        const float bw_scan = kit.button_width(tr("Scan again"), true);
        const float bw_show = kit.button_width(tr("Show in list"), true, Kit::ButtonKind::primary);
        const item_result hint = ui.custom_item("##du_hint", {ui.content_width() - bw_back - bw_scan - bw_show - 40.0f, kit.control_h()});
        std::string line{tr("Double click or the arrow opens a folder. Click a column to sort.")};
        if (pick_.node != nullptr) {
            line = trf("{}  -  {}  -  ", pick_.node->name, core::path::format_size(pick_.node->bytes))
                 + trf_n("{} file", "{} files", static_cast<long long>(pick_.node->files), pick_.node->files);
        }
        p.text_left(hint.bounds, line, pick_.node != nullptr ? pal.text : pal.text_dim, f.small, 4.0f);
        if (can_back) {
            ui.same_line();
            if (kit.button({"##du_back", tr("Back")})) { back(); return; }
            ui.same_line();
            (void)ui.custom_item("##du_gap1", {8.0f, kit.control_h()});
        } else {
            ui.same_line();
            (void)ui.custom_item("##du_gap0", {bw_back + 8.0f, kit.control_h()});
        }
        ui.same_line();
        if (kit.button({"##du_again", tr("Scan again"), glyph::refresh})) { rescan(current_path(), true, false); return; }
        ui.same_line();
        (void)ui.custom_item("##du_gap2", {8.0f, kit.control_h()});
        ui.same_line();
        if (kit.button({"##du_show", tr("Show in list"), glyph::folder_open, Kit::ButtonKind::primary})) {
            env_.tab().navigate(current_path());
            ui.close_modal();
            return;
        }
    }
}

} // namespace prism::ui
