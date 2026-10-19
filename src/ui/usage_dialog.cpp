#include "ui/usage_dialog.hpp"

#include "core/path.hpp"
#include "core/shell_ops.hpp"
#include "core/text.hpp"
#include "core/treemap.hpp"
#include "ui/panel.hpp"
#include "ui/tag_colors.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace prism::ui {

using namespace strata;

namespace {

// one colour per top-level item, shared by its tile and its row, so the two sides read as one picture
[[nodiscard]] color hue_of(std::size_t top) { return tag_color(static_cast<int>(top)); }

[[nodiscard]] std::string id_of(const core::UsageNode* n)
{
    return std::format("{:x}", reinterpret_cast<std::uintptr_t>(n));
}

// the children in the order the table is sorted by
std::vector<const core::UsageNode*> sorted(const core::UsageNode& n, int col, bool asc)
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
    rescan(std::move(folder));
    want_open_ = true;
}

void UsageDialog::rescan(std::string folder)
{
    base_ = std::move(folder);
    root_.reset();
    trail_.clear();
    open_.clear();
    flat_.clear();
    pick_ = {};
    hover_ = {};
    ++generation_;
    scan_.start(base_, env_.window.waker());
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
    pick_ = {};
    ++generation_;
}

void UsageDialog::look_inside(const Pick& pick)
{
    if (pick.node == nullptr || !pick.node->is_dir) { return; }
    std::vector<const core::UsageNode*> steps = pick.chain;
    // every folder on the way is stepped into; one the scan did not keep a level of is scanned on its own
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const core::UsageNode* n = steps[i];
        if (!n->is_dir) { return; }
        if (n->children.empty()) {
            std::string path = current_path();
            for (std::size_t k = 0; k <= i; ++k) { path = core::path::join(path, steps[k]->name); }
            rescan(std::move(path));
            return;
        }
        trail_.push_back(n);
    }
    pick_ = {};
    ++generation_;
}

// the table: a tree of the folder on screen, as deep as the scan kept it
void UsageDialog::draw_tree(context& ui, const FontSet& f, const Palette& pal, const core::UsageNode& cur, float row_h)
{
    std::vector<const core::UsageNode*> chain;
    Pick activated;     // a double click this frame
    Pick selected;      // a click this frame
    const double total = cur.bytes > 0 ? static_cast<double>(cur.bytes) : 1.0;

    const auto cells = [&](const core::UsageNode& n, std::size_t top) {
        ui.table_next_column();
        const float w = std::max(40.0f, ui.content_width());
        const item_result r = ui.custom_item("##sz" + id_of(&n), {w, row_h});
        const rect box = rect::from_size({r.bounds.min.x, r.bounds.min.y}, {r.bounds.width(), row_h});
        const double share = static_cast<double>(n.bytes) / total;
        const float  bw = std::max(0.0f, box.width() - 8.0f);
        // a bar in the item's own colour behind the size, as long as its share of the folder
        ui.draw().rect_filled(rect::from_size({box.min.x, box.min.y + 3.0f}, {bw * static_cast<float>(std::clamp(share, 0.0, 1.0)), row_h - 6.0f}),
                              (n.lumped ? pal.border : hue_of(top)).scaled_alpha(0.35f), 4.0f);
        const std::string size = core::path::format_size(n.bytes);
        const float tw = ui.font().measure(f.body, size).x;
        ui.draw().text({box.max.x - tw - 8.0f, box.min.y + (row_h - ui.font().line_height(f.body)) * 0.5f}, pal.text, size, f.body);
        ui.table_next_column();
        const std::string files = std::format("{}", n.files);
        const item_result r2 = ui.custom_item("##fl" + id_of(&n), {std::max(30.0f, ui.content_width()), row_h});
        const float fw = ui.font().measure(f.small, files).x;
        ui.draw().text({r2.bounds.max.x - fw - 8.0f, r2.bounds.min.y + (row_h - ui.font().line_height(f.small)) * 0.5f}, pal.text_dim, files, f.small);
    };

    flat_next_.clear();
    bool saw_selected = false;
    const auto rows = [&](auto&& self, const core::UsageNode& parent, std::size_t top_index, bool at_top, int depth) -> void {
        std::size_t i = 0;
        for (const core::UsageNode* n : sorted(parent, sort_col_, sort_asc_)) {
            const std::size_t top = at_top ? i : top_index;
            ++i;
            chain.push_back(n);
            const bool has_kids = n->is_dir && !n->children.empty();
            const bool is_open  = has_kids && open_.contains(n);
            flat_next_.push_back({n, chain}); // (every row, in view or not: the keyboard walks this list)
            // a row scrolled out of view returns false and its cells are skipped; its children below are still walked, so
            // what follows keeps its place
            if (ui.table_next_row()) {
                ui.table_next_column();
                const item_result r = ui.custom_item("##nm" + id_of(n), {std::max(60.0f, ui.content_width()), row_h});
                const rect  row = r.bounds;
                const bool  sel = pick_.node == n;
                const float indent = 4.0f + static_cast<float>(depth) * 16.0f;
                const rect  arrow = rect::from_size({row.min.x + indent, row.min.y}, {16.0f, row_h});
                const bool  on_arrow = r.hovered && arrow.contains(ui.mouse_pos());
                if (sel) { ui.draw().rect_filled(row, pal.selection, 5.0f); }
                else if (r.hovered) { ui.draw().rect_filled(row, pal.hover, 5.0f); }
                if (has_kids) {
                    const vec2 c = arrow.center();
                    const color ac = on_arrow ? pal.text : pal.text_dim;
                    if (is_open) { ui.draw().triangle_filled({c.x - 4.0f, c.y - 2.0f}, {c.x + 4.0f, c.y - 2.0f}, {c.x, c.y + 3.0f}, ac); }
                    else { ui.draw().triangle_filled({c.x - 2.0f, c.y - 4.0f}, {c.x - 2.0f, c.y + 4.0f}, {c.x + 3.0f, c.y}, ac); }
                }
                ui.draw().push_clip(row);
                ui.draw().text({row.min.x + indent + 20.0f, row.min.y + (row_h - ui.font().line_height(f.body)) * 0.5f}, n->lumped ? pal.text_dim : pal.text, n->name, f.body);
                ui.draw().pop_clip();
                if (r.hovered) { hover_ = {n, chain}; }
                if (r.hovered && ui.mouse_clicked(mouse_button::left)) {
                    if (on_arrow && has_kids) { if (is_open) { open_.erase(n); } else { open_.insert(n); } }
                    else { selected = {n, chain}; }
                }
                if (r.hovered && !on_arrow && ui.item_double_clicked() && n->is_dir) { activated = {n, chain}; }
                if (r.hovered && ui.mouse_clicked(mouse_button::right)) {
                    ctx_ = {n, chain};
                    selected = ctx_;
                    ui.open_popup_menu("##du_ctx", ui.mouse_pos());
                }
                if (sel && follow_) { ui.ensure_item_visible(); saw_selected = true; }
                cells(*n, top);
            }
            if (is_open) { self(self, *n, top, false, depth + 1); }
            chain.pop_back();
        }
    };
    rows(rows, cur, 0, true, 0);
    if (saw_selected) { follow_ = false; }
    flat_.swap(flat_next_);

    if (selected.node != nullptr) { pick_ = selected; }
    if (activated.node != nullptr) { look_inside(activated); }
}

// Up / Down walk the rows, Right opens a folder (or steps into it), Left closes it (or steps out), Enter opens / closes
void UsageDialog::tree_keys(context& ui)
{
    if (flat_.empty() || ui.want_text_input()) { return; }
    int at = -1;
    for (std::size_t i = 0; i < flat_.size(); ++i) {
        if (flat_[i].node == pick_.node) { at = static_cast<int>(i); break; }
    }
    const auto go = [&](int to) {
        to = std::clamp(to, 0, static_cast<int>(flat_.size()) - 1);
        pick_ = flat_[static_cast<std::size_t>(to)];
        follow_ = true;
    };
    if (ui.key_pressed(key::down)) { go(at + 1); }
    else if (ui.key_pressed(key::up)) { go(at < 0 ? 0 : at - 1); }
    else if (ui.key_pressed(key::home)) { go(0); }
    else if (ui.key_pressed(key::end)) { go(static_cast<int>(flat_.size()) - 1); }
    if (pick_.node == nullptr) { return; }
    const core::UsageNode* n = pick_.node;
    const bool has_kids = n->is_dir && !n->children.empty();
    if (ui.key_pressed(key::right) && has_kids) {
        if (!open_.contains(n)) { open_.insert(n); } else if (at >= 0) { go(at + 1); }
    } else if (ui.key_pressed(key::left)) {
        if (has_kids && open_.contains(n)) {
            open_.erase(n);
        } else if (pick_.chain.size() > 1) { // out to the folder it is in
            Pick parent = pick_;
            parent.chain.pop_back();
            parent.node = parent.chain.back();
            pick_ = parent;
            follow_ = true;
        }
    } else if (ui.key_pressed(key::enter) && has_kids) {
        if (open_.contains(n)) { open_.erase(n); } else { open_.insert(n); }
    }
}

// the treemap: the folder's items as tiles, and inside each folder tile the biggest things in it
void UsageDialog::draw_map(Painter& p, const item_result& mr, const core::UsageNode& cur)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    const rect     area = mr.bounds;

    p.dl().rect_filled(area, pal.raised, 10.0f);
    if (cur.children.empty() || cur.bytes == 0) {
        p.text_center(area, cur.bytes == 0 ? "Nothing here takes any space" : "Nothing smaller to show", pal.text_dim, f.body);
        return;
    }
    const float enter = ui.animate("du_enter" + std::to_string(generation_), 1.0f, 9.0f); // the new view fades and settles in
    p.dl().push_alpha(0.25f + 0.75f * enter);

    std::vector<double> values;
    for (const core::UsageNode& c : cur.children) { values.push_back(static_cast<double>(c.bytes)); }
    const rect inner = area.expanded(-5.0f);
    const std::vector<core::TreemapRect> tiles = core::squarify(values, inner.min.x, inner.min.y, inner.width(), inner.height());

    const vec2 mp = ui.mouse_pos();
    const core::UsageNode* lit = hover_.node; // the row of the table under the pointer
    Pick under;
    p.dl().push_clip(area);
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const core::TreemapRect& t = tiles[i];
        if (t.w < 2.0f || t.h < 2.0f) { continue; }
        const core::UsageNode& c = cur.children[i];
        const rect box = rect::from_size({t.x, t.y}, {t.w, t.h}).expanded(-1.5f);
        const color hue = c.lumped ? pal.text_faint : (c.is_dir ? hue_of(i) : hue_of(i).scaled_alpha(0.7f));
        const bool picked = pick_.node == &c;
        const bool over_top = (mr.hovered && box.contains(mp)) || lit == &c;
        if (mr.hovered && box.contains(mp)) { under = {&c, {&c}}; }

        // the tile: a soft vertical gradient in the item's colour
        shape_style st;
        st.radius       = radii(7.0f);
        st.fill_top     = hue.scaled_alpha(c.lumped ? 0.20f : (over_top ? 0.80f : 0.62f));
        st.fill_bottom  = hue.scaled_alpha(c.lumped ? 0.14f : (over_top ? 0.58f : 0.42f));
        st.border       = hue.scaled_alpha(0.85f);
        st.border_width = 1.0f;
        p.dl().shape(box, st);

        const bool nest = c.is_dir && !c.children.empty() && box.width() > 110.0f && box.height() > 70.0f;
        if (nest) {
            const float head = 24.0f;
            p.dl().rect_filled(rect{box.min, {box.max.x, box.min.y + head}}, color{0, 0, 0, 55}, 7.0f, corners::top);
            p.dl().push_clip(rect{box.min, {box.max.x - 4.0f, box.min.y + head}});
            p.text_left(rect{{box.min.x + 8.0f, box.min.y}, {box.max.x - 4.0f, box.min.y + head}}, c.name, pal.text, f.bold);
            p.dl().pop_clip();
            const rect body{{box.min.x + 4.0f, box.min.y + head + 3.0f}, {box.max.x - 4.0f, box.max.y - 4.0f}};
            std::vector<double> sub;
            for (const core::UsageNode& g : c.children) { sub.push_back(static_cast<double>(g.bytes)); }
            const std::vector<core::TreemapRect> inside = core::squarify(sub, body.min.x, body.min.y, body.width(), body.height());
            for (std::size_t k = 0; k < inside.size(); ++k) {
                const core::TreemapRect& s = inside[k];
                if (s.w < 3.0f || s.h < 3.0f) { continue; }
                const core::UsageNode& g = c.children[k];
                const rect sb = rect::from_size({s.x, s.y}, {s.w, s.h}).expanded(-1.0f);
                const bool over_sub = mr.hovered && sb.contains(mp);
                if (over_sub) { under = {&g, {&c, &g}}; }
                const bool sub_picked = pick_.node == &g;
                p.dl().rect_filled(sb, (g.lumped ? pal.border : (g.is_dir ? hue : hue.scaled_alpha(0.75f))).scaled_alpha(sub_picked ? 0.85f : (over_sub ? 0.60f : 0.38f)), 4.0f);
                if (sub_picked) { p.dl().rect_outline(sb, pal.text, 4.0f, 1.5f); }
                if (sb.width() > 56.0f && sb.height() > 20.0f) {
                    p.dl().push_clip(sb.expanded(-2.0f));
                    p.text_left(rect{{sb.min.x + 5.0f, sb.min.y}, {sb.max.x - 2.0f, sb.min.y + 20.0f}}, g.name, pal.text, f.small);
                    p.dl().pop_clip();
                }
            }
            // (the header is part of the folder tile: hovering it picks the folder itself)
            if (mr.hovered && rect{box.min, {box.max.x, box.min.y + head}}.contains(mp)) { under = {&c, {&c}}; }
        } else if (box.width() > 64.0f && box.height() > 36.0f) {
            p.dl().push_clip(box.expanded(-4.0f));
            p.text_left(rect{{box.min.x + 8.0f, box.min.y + 4.0f}, {box.max.x - 4.0f, box.min.y + 24.0f}}, c.name, pal.text, f.bold);
            p.text_left(rect{{box.min.x + 8.0f, box.min.y + 22.0f}, {box.max.x - 4.0f, box.min.y + 40.0f}}, core::path::format_size(c.bytes), pal.text_dim, f.small);
            p.dl().pop_clip();
        }
        if (picked) { p.dl().rect_outline(box, pal.text, 7.0f, 2.0f); }
    }
    p.dl().pop_clip();
    p.dl().pop_alpha();

    // what the pointer is on lights up on the map and in the table, and says what it is
    if (under.node != nullptr) { hover_ = under; }
    if (under.node != nullptr && mr.hovered) {
        const core::UsageNode& n = *under.node;
        const double pct = cur.bytes > 0 ? 100.0 * static_cast<double>(n.bytes) / static_cast<double>(cur.bytes) : 0.0;
        ui.tooltip(std::format("{}\n{}  -  {:.1f} % of this folder\n{} {}", n.name, core::path::format_size(n.bytes), pct, n.files, n.files == 1 ? "file" : "files"));
        env_.cmd.set_cursor(n.is_dir ? cursor_kind::hand : cursor_kind::arrow);
        if (ui.mouse_released(mouse_button::left)) { pick_ = under; }
        if (ui.item_double_clicked() && n.is_dir) { look_inside(under); }
        if (ui.mouse_clicked(mouse_button::right)) {
            ctx_ = under;
            pick_ = under;
            ui.open_popup_menu("##du_ctx", mp);
        }
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
    const float width = std::min(1240.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Disk usage", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        scan_.cancel();
        root_.reset();
        trail_.clear();
        pick_ = hover_ = ctx_ = {};
        Kit::reset_focus();
        return;
    }
    if (!root_) {
        if (auto r = scan_.result()) {
            root_  = std::move(r);
            trail_ = {root_.get()};
            ++generation_;
        }
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    {
        const item_result r = ui.custom_item("##du_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, "Disk usage", pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, root_ ? current_path() : base_, pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##du_close", glyph::close, "Close  (Esc)", true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(8.0f);

    if (!root_) {
        const item_result r = ui.custom_item("##du_run", {ui.content_width(), 34.0f});
        p.text_left(r.bounds, std::format("Measuring ... {} files, {} so far", scan_.files(), core::path::format_size(scan_.bytes())), pal.text_dim, f.body, 4.0f);
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
        if (kit.button({"##du_cancel", "Cancel"})) { ui.close_modal(); }
        return;
    }
    const core::UsageNode& cur = *trail_.back();
    hover_ = {};

    // where you are: clickable breadcrumbs (the last one is this folder)
    {
        const float line = kit.control_h();
        const item_result lead = ui.custom_item("##du_crumb_lead", {2.0f, line});
        (void)lead;
        for (std::size_t i = 0; i < trail_.size(); ++i) {
            ui.same_line();
            std::string label = i == 0 ? (core::path::leaf(base_).empty() ? base_ : core::path::leaf(base_)) : trail_[i]->name;
            const bool last = i + 1 == trail_.size();
            if (kit.chip("##du_crumb" + std::to_string(i), label, i == 0 ? glyph::folder : 0, last ? Tone::accent : Tone::neutral, !last, last ? std::string_view{} : "Back to here", line) && !last) {
                go_to(i);
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
        const std::string folders_text = std::format("{} folders at the top", folders); // (the tile keeps a view of it)
        const std::array<Kit::Stat, 3> tiles{{
            {"In this folder", core::path::format_size(cur.bytes), "all the files below it"},
            {"Files", std::format("{}", cur.files), folders_text},
            {"Biggest item", biggest.size() > 22 ? biggest.substr(0, 20) + ".." : biggest, biggest_sub},
        }};
        kit.stat_tiles("##du_tiles", tiles);
    }
    kit.vgap(8.0f);

    // the body: the tree on the left, the map on the right
    const float body_h = std::clamp(ui.display_size().y - 430.0f, 240.0f, 640.0f);
    const float gap    = 10.0f;
    const float left_w = std::floor((ui.content_width() - gap) * 0.42f);
    const float right_w = ui.content_width() - gap - left_w;
    const float row_h = 22.0f;
    {
        auto left = ui.child("##du_left", {left_w, body_h}, child_flags::no_padding);
        if (left) {
            if (auto t = ui.table("##du_table", 3, table_flags::row_hover | table_flags::resizable, body_h)) {
                ui.table_setup_column("Name", 0.0f, 2.2f);
                ui.table_setup_column("Size", 0.0f, 1.5f);
                ui.table_setup_column("Files", 74.0f);
                const int clicked = ui.table_headers_row(sort_col_, sort_asc_);
                if (clicked >= 0) {
                    if (clicked == sort_col_) { sort_asc_ = !sort_asc_; } else { sort_col_ = clicked; sort_asc_ = clicked == 0; }
                }
                draw_tree(ui, f, pal, cur, row_h);
            }
        }
    }
    ui.same_line();
    (void)ui.custom_item("##du_gap", {gap, body_h});
    ui.same_line();
    const item_result mr = ui.custom_item("##du_map", {right_w, body_h});
    draw_map(p, mr, cur);
    // keyboard: arrows walk the tree, Ctrl+Enter looks inside the selected folder (plain Enter opens / closes it), Backspace goes back out
    tree_keys(ui);
    if (ui.key_pressed(key::enter, true) && pick_.node != nullptr && pick_.node->is_dir) { look_inside(pick_); return; }
    if (ui.key_pressed(key::backspace) && trail_.size() > 1) { go_to(trail_.size() - 2); return; }

    // the right-click menu (the same for a row and a tile)
    if (auto menu = ui.popup_menu("##du_ctx")) {
        if (ctx_.node != nullptr) {
            const std::string path = path_of(ctx_);
            if (ctx_.node->is_dir && ui.menu_item("Look inside", "Ctrl+Enter")) { look_inside(ctx_); }
            if (ui.menu_item(ctx_.node->is_dir ? "Show in the file list" : "Show its folder in the file list")) {
                env_.tab().navigate(ctx_.node->is_dir ? path : core::path::parent(path));
                ui.close_modal();
            }
            if (ui.menu_item("Open in a new tab")) { env_.cmd.open_tab(ctx_.node->is_dir ? path : core::path::parent(path), true); ui.close_modal(); }
            if (ui.menu_item("Copy path")) { (void)core::shell::set_clipboard_text(path); }
        }
    }

    // the line at the bottom
    kit.vgap(10.0f);
    {
        const float bw_scan = kit.button_width("Scan again", true);
        const float bw_show = kit.button_width("Show in list", true, Kit::ButtonKind::primary);
        const item_result hint = ui.custom_item("##du_hint", {ui.content_width() - bw_scan - bw_show - 24.0f, kit.control_h()});
        std::string line = "Click selects, double click looks inside, Backspace goes back. Click a column to sort.";
        if (pick_.node != nullptr) {
            line = std::format("{}  -  {}  -  {} {}", pick_.node->name, core::path::format_size(pick_.node->bytes), pick_.node->files, pick_.node->files == 1 ? "file" : "files");
        }
        p.text_left(hint.bounds, line, pick_.node != nullptr ? pal.text : pal.text_dim, f.small, 4.0f);
        ui.same_line();
        if (kit.button({"##du_again", "Scan again", glyph::refresh})) { rescan(current_path()); return; }
        ui.same_line();
        (void)ui.custom_item("##du_gap2", {8.0f, kit.control_h()});
        ui.same_line();
        if (kit.button({"##du_show", "Show in list", glyph::folder_open, Kit::ButtonKind::primary})) {
            env_.tab().navigate(current_path());
            ui.close_modal();
            return;
        }
    }
}

} // namespace prism::ui
