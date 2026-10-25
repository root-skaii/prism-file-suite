#include "ui/usage_dialog.hpp"

#include "core/path.hpp"
#include "core/shell_ops.hpp"
#include "ui/menu_parts.hpp"
#include "core/text.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>

namespace prism::ui {

using namespace strata;

namespace {

constexpr float indent_step  = 18.0f;
constexpr float share_col_w  = 210.0f;
constexpr float size_col_w   = 110.0f;
constexpr float files_col_w  = 90.0f;
constexpr float overview_h   = 184.0f;
constexpr float ring_r       = 62.0f;
constexpr float ring_w       = 18.0f;
constexpr float two_pi       = 2.0f * std::numbers::pi_v<float>;

// the ring's colours, biggest item first: cool to warm, so neighbours stay apart
constexpr std::array<color, 7> k_hues{{{99, 140, 255, 255}, {167, 123, 255, 255}, {236, 104, 170, 255}, {255, 138, 92, 255},
                                       {247, 190, 72, 255}, {72, 199, 142, 255}, {56, 189, 214, 255}}};

[[nodiscard]] color hue_of(std::size_t rank, const Palette& pal) { return rank < k_hues.size() ? k_hues[rank] : pal.text_faint; }

[[nodiscard]] std::string id_of(std::size_t row) { return std::format("##r{}", row); }

[[nodiscard]] std::string percent(double share)
{
    const double v = share * 100.0;
    if (v <= 0.0) { return "0%"; }
    if (v < 0.1) { return "<0.1%"; }
    return v < 10.0 ? std::format("{:.1f}%", v) : std::format("{:.0f}%", v);
}

[[nodiscard]] core::FileKind kind_of(const core::UsageNode& n)
{
    if (n.is_dir) { return core::FileKind::folder; }
    const std::size_t dot = n.name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= n.name.size()) { return core::FileKind::other; }
    std::string ext = n.name.substr(dot + 1);
    for (char& c : ext) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return core::classify_extension(ext);
}

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
    ranked_root_ = nullptr;
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

// the biggest children by size, whatever the list is sorted by: they keep their colour when the sort changes
void UsageDialog::rank(const core::UsageNode& cur)
{
    if (ranked_root_ == &cur && ranked_gen_ == generation_) { return; }
    ranked_gen_ = generation_;
    ranked_.clear();
    for (const core::UsageNode& c : cur.children) {
        if (!c.lumped && c.bytes > 0) { ranked_.push_back(&c); }
    }
    const std::size_t n = std::min(ranked_.size(), ring_items);
    std::ranges::partial_sort(ranked_, ranked_.begin() + static_cast<std::ptrdiff_t>(n),
                              [](const core::UsageNode* a, const core::UsageNode* b) { return a->bytes > b->bytes; });
    ranked_.resize(n);
    ranked_root_ = &cur;
}

std::size_t UsageDialog::rank_of(const core::UsageNode* top) const noexcept
{
    const auto it = std::ranges::find(ranked_, top);
    return it == ranked_.end() ? ring_items : static_cast<std::size_t>(it - ranked_.begin());
}

void UsageDialog::select_top(const core::UsageNode* top)
{
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].depth == 0 && rows_[i].node == top) {
            pick_   = pick_at(i);
            follow_ = true;
            return;
        }
    }
}

// the visible rows as a flat list (open folders expanded). rebuilt only when something structural changed; drawing then
// touches just the rows in view, so a folder with 100k rows scrolls as smoothly as one with ten
void UsageDialog::rebuild_rows(context& ui, const FontSet& f, const core::UsageNode& cur)
{
    rank(cur);
    rows_.clear();
    float widest = 0.0f;
    const auto walk = [&](auto&& self, const core::UsageNode& parent, int parent_row, std::size_t top_rank, int depth) -> void {
        for (const core::UsageNode* n : sorted(parent, sort_col_, sort_asc_)) {
            const std::size_t top = depth == 0 ? rank_of(n) : top_rank;
            const int me = static_cast<int>(rows_.size());
            rows_.push_back({n, parent_row, depth, top, kind_of(*n)});
            widest = std::max(widest, 6.0f + static_cast<float>(depth) * indent_step + 44.0f + ui.font().measure(f.body, n->name).x + 16.0f);
            if (n->is_dir && open_.contains(n)) { self(self, *n, me, top, depth + 1); }
        }
    };
    walk(walk, cur, -1, 0, 0);
    name_w_    = std::min(widest, 320.0f); // (longer names are cut with ".."; the number columns must stay in view)
    rows_root_ = &cur;
    dirty_     = false;
}

// a ring of the biggest items with a legend beside it; hovering one lights it in the ring, the legend and the list
void UsageDialog::draw_overview(Painter& p, const core::UsageNode& cur)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet  f   = p.fonts();
    rank(cur);

    if (anim_gen_ != generation_) { anim_gen_ = generation_; anim_start_ = ui.time(); }
    const float t    = std::clamp(static_cast<float>(ui.time() - anim_start_) / 0.6f, 0.0f, 1.0f);
    const float grow = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    if (t < 1.0f) { ui.invalidate(); }

    const item_result r = ui.custom_item("##du_over", {ui.content_width(), overview_h});
    {
        shape_style st;
        st.radius       = radii(12.0f);
        st.fill_top     = st.fill_bottom = pal.content;
        st.border       = pal.divider;
        st.border_width = 1.0f;
        p.dl().shape(r.bounds, st);
    }

    struct Seg { const core::UsageNode* node; std::uint64_t bytes; std::string_view name; int id; };
    std::array<Seg, ring_items + 1> segs{};
    std::size_t count = 0;
    std::uint64_t ranked_bytes = 0;
    for (std::size_t i = 0; i < ranked_.size(); ++i) {
        segs[count++] = {ranked_[i], ranked_[i]->bytes, ranked_[i]->name, static_cast<int>(i)};
        ranked_bytes += ranked_[i]->bytes;
    }
    if (cur.bytes > ranked_bytes) { segs[count++] = {nullptr, cur.bytes - ranked_bytes, tr("Everything else"), static_cast<int>(ring_items)}; }
    const double total = cur.bytes > 0 ? static_cast<double>(cur.bytes) : 1.0;
    const vec2   m     = ui.mouse_pos();
    int hot = -1;
    const core::UsageNode* hot_node = nullptr;

    // the ring
    const vec2 c{r.bounds.min.x + 28.0f + ring_r + ring_w * 0.5f, r.bounds.center().y};
    if (r.hovered) {
        const float dx = m.x - c.x, dy = m.y - c.y;
        if (std::abs(std::sqrt(dx * dx + dy * dy) - ring_r) <= ring_w * 0.5f + 5.0f) {
            float a = std::atan2(dy, dx) + two_pi * 0.25f;
            if (a < 0.0f) { a += two_pi; }
            float acc = 0.0f;
            for (std::size_t i = 0; i < count; ++i) {
                acc += two_pi * static_cast<float>(static_cast<double>(segs[i].bytes) / total);
                if (a < acc) { hot = segs[i].id; hot_node = segs[i].node; break; }
            }
        }
    }
    p.dl().circle(c, ring_r, pal.raised, ring_w);
    {
        float a0 = -two_pi * 0.25f;
        for (std::size_t i = 0; i < count; ++i) {
            const float span = two_pi * static_cast<float>(static_cast<double>(segs[i].bytes) / total) * grow;
            const float gap  = count > 1 && span > 0.08f ? 0.03f : 0.0f;
            const bool  lit  = hot_ < 0 || hot_ == segs[i].id;
            const color col  = hue_of(static_cast<std::size_t>(segs[i].id), pal);
            p.dl().arc(c, ring_r, a0 + gap * 0.5f, a0 + span - gap * 0.5f, lit ? col : col.scaled_alpha(0.3f), hot_ == segs[i].id ? ring_w + 6.0f : ring_w);
            a0 += span;
        }
    }
    {   // the middle: the folder's total, or the share of the item under the pointer
        const Seg* s = nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            if (segs[i].id == hot_) { s = &segs[i]; }
        }
        const float inner = (ring_r - ring_w * 0.5f) * 2.0f - 14.0f;
        const float lt = p.line_height(f.title), ls = p.line_height(f.small);
        const float y  = c.y - (lt + ls) * 0.5f;
        const std::string value = s != nullptr ? percent(static_cast<double>(s->bytes) / total) : core::path::format_size(cur.bytes);
        const std::string label = s != nullptr ? std::string{s->name} : trf_n("{} file", "{} files", static_cast<long long>(cur.files), cur.files);
        const rect vr{{c.x - inner * 0.5f, y}, {c.x + inner * 0.5f, y + lt}};
        const rect lr{{c.x - inner * 0.5f, y + lt}, {c.x + inner * 0.5f, y + lt + ls}};
        p.text_center(vr, value, pal.text, f.title);
        if (p.measure(f.small, label).x <= inner) { p.text_center(lr, label, pal.text_dim, f.small); }
        else { p.text_left(lr, label, pal.text_dim, f.small); }
    }

    // the legend: two columns, biggest first down the left one
    const float x0 = c.x + ring_r + ring_w + 36.0f;
    const float x1 = r.bounds.max.x - 16.0f;
    const float top = r.bounds.min.y + 16.0f;
    {
        const std::size_t folders = std::ranges::count_if(cur.children, [](const core::UsageNode& n) { return n.is_dir; });
        const rect hr{{x0, top}, {x1, top + 24.0f}};
        p.text_left(hr, tr("Biggest here"), pal.text, f.bold, 8.0f);
        p.text_right(hr, trf_n("{} file", "{} files", static_cast<long long>(cur.files), cur.files) + "  \xC2\xB7  "
                         + trf_n("{} folder", "{} folders", static_cast<long long>(folders), folders), pal.text_dim, f.small, 8.0f);
    }
    const float ly = top + 24.0f + 10.0f;
    const float cell_h = 28.0f, col_gap = 16.0f;
    const float cw = (x1 - x0 - col_gap) * 0.5f;
    if (count == 0) {
        p.text_left(rect{{x0, ly}, {x1, ly + cell_h}}, tr("Nothing here takes any space"), pal.text_dim, f.body, 8.0f);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float x = x0 + static_cast<float>(i / 4) * (cw + col_gap);
        const rect cell = rect::from_size({x, ly + static_cast<float>(i % 4) * cell_h}, {cw, cell_h});
        const Seg& s = segs[i];
        if (r.hovered && cell.contains(m)) { hot = s.id; hot_node = s.node; }
        if (hot_ == s.id) { p.dl().rect_filled(cell, pal.hover, 6.0f); }
        const float cy = cell.center().y;
        p.dl().rect_filled(rect::from_size({cell.min.x + 9.0f, cy - 5.0f}, {10.0f, 10.0f}), hue_of(static_cast<std::size_t>(s.id), pal), 3.0f);
        // a narrow dialog drops the percent so the names keep their room (the ring and the list still show it)
        const float pct_w = cw >= 330.0f ? 54.0f : 0.0f;
        p.text_left(rect{{cell.min.x + 28.0f, cell.min.y}, {cell.max.x - 82.0f - pct_w, cell.max.y}}, s.name, s.node != nullptr ? pal.text : pal.text_dim, f.body);
        p.text_right(rect{{cell.max.x - 82.0f - pct_w, cell.min.y}, {cell.max.x - pct_w, cell.max.y}}, core::path::format_size(s.bytes), pal.text, f.body, pct_w > 0.0f ? 0.0f : 8.0f);
        if (pct_w > 0.0f) { p.text_right(rect{{cell.max.x - pct_w, cell.min.y}, cell.max}, percent(static_cast<double>(s.bytes) / total), pal.text_dim, f.small, 8.0f); }
    }

    if (hot >= 0) { hot_next_ = hot; }
    if (hot_node != nullptr) {
        env_.cmd.set_cursor(cursor_kind::hand);
        if (ui.mouse_clicked(mouse_button::left)) { select_top(hot_node); }
        if (ui.item_double_clicked() && hot_node->is_dir && pick_.node == hot_node) { look_inside(pick_); }
    }
}

// header (sticky, follows the sideways scroll) and a clipped list of rows, on a card
void UsageDialog::draw_tree(Painter& p, const core::UsageNode& cur, float row_h, float height)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet  f   = p.fonts();
    if (dirty_ || rows_root_ != &cur) { rebuild_rows(ui, f, cur); }

    const float view_w = ui.content_width();
    const float row_w  = std::max(view_w - 24.0f, name_w_ + share_col_w + size_col_w + files_col_w);
    const float name_w = row_w - share_col_w - size_col_w - files_col_w;
    const double total = cur.bytes > 0 ? static_cast<double>(cur.bytes) : 1.0;
    const float hh     = row_h + 8.0f;
    const float list_h = std::max(60.0f, height - hh - 6.0f);

    // header, with the card behind it and the list
    {
        const item_result r = ui.custom_item("##du_hdr", {view_w, hh});
        shape_style st;
        st.radius       = radii(12.0f);
        st.fill_top     = st.fill_bottom = pal.content;
        st.border       = pal.divider;
        st.border_width = 1.0f;
        p.dl().shape(rect{r.bounds.min, {r.bounds.max.x, r.bounds.min.y + hh + list_h + 6.0f}}, st);
        p.dl().line({r.bounds.min.x + 1.0f, r.bounds.max.y - 0.5f}, {r.bounds.max.x - 1.0f, r.bounds.max.y - 0.5f}, pal.divider);

        const float x0 = r.bounds.min.x - scroll_x_;
        struct Col { std::string_view label; int sort; float start, width; bool right; };
        const std::array<Col, 4> cols{{{tr("Name"), 0, x0, name_w, false},
                                       {tr("Share"), -1, x0 + name_w, share_col_w, false},
                                       {tr("Size"), 1, x0 + name_w + share_col_w, size_col_w, true},
                                       {tr("Files"), 2, x0 + name_w + share_col_w + size_col_w, files_col_w, true}}};
        ui.draw().push_clip(r.bounds);
        for (const Col& col : cols) {
            const rect cell = rect::from_size({col.start, r.bounds.min.y}, {col.width, hh});
            const bool sortable = col.sort >= 0;
            const bool hot = sortable && r.hovered && cell.contains(ui.mouse_pos());
            const bool active = sort_col_ == col.sort;
            const color tc = active || hot ? pal.text : pal.text_dim;
            const std::string label{col.label};
            const float tw = p.measure(f.small, label).x;
            const float tx = col.right ? cell.max.x - tw - (active ? 26.0f : 12.0f) : cell.min.x + (col.sort == 0 ? 14.0f : 10.0f);
            p.text_left(rect{{tx, cell.min.y}, {tx + tw + 2.0f, cell.max.y}}, label, tc, f.small);
            if (active) { p.sort_arrow({tx + tw + 9.0f, cell.center().y}, sort_asc_, pal.accent); }
            if (hot) {
                env_.cmd.set_cursor(cursor_kind::hand);
                if (ui.mouse_clicked(mouse_button::left)) {
                    if (sort_col_ == col.sort) { sort_asc_ = !sort_asc_; } else { sort_col_ = col.sort; sort_asc_ = sort_col_ == 0; }
                    dirty_ = true;
                }
            }
        }
        ui.draw().pop_clip();
    }

    Pick activated;
    Pick selected;
    int  toggled = -1; // a row whose folder opens / closes
    auto list = ui.child("##du_rows", {view_w, list_h}, child_flags::no_padding | child_flags::horizontal);
    if (!list) { return; }
    scroll_x_ = ui.scroll_x();

    if (rows_.empty()) {
        const item_result r = ui.custom_item("##du_none", {view_w - 14.0f, 80.0f});
        const std::string msg = std::string{cur.bytes == 0 ? tr("Nothing here takes any space") : tr("Nothing to show")};
        p.text_center(r.bounds, msg, pal.text_dim, f.body);
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
            const color hue     = n.lumped ? pal.text_faint : hue_of(rw.top, pal);

            const item_result r = ui.custom_item(id_of(i), {row_w, row_h});
            const rect row = r.bounds;
            const rect fill{{row.min.x + 4.0f, row.min.y + 1.0f}, {row.max.x - 2.0f, row.max.y - 1.0f}};
            const bool sel = pick_.node == &n;
            if (sel) { p.dl().rect_filled(fill, pal.selection, 6.0f); }
            else if (r.hovered) { p.dl().rect_filled(fill, pal.hover, 6.0f); }
            else if (rw.depth == 0 && hot_ >= 0 && static_cast<std::size_t>(hot_) == rw.top) { p.dl().rect_filled(fill, hue.scaled_alpha(0.10f), 6.0f); }
            if (r.hovered) { hot_next_ = static_cast<int>(rw.top); }

            // the tree's lines: one per level above, under that level's arrow
            for (int k = 0; k < rw.depth; ++k) {
                const float gx = std::floor(row.min.x + 6.0f + static_cast<float>(k) * indent_step + 9.0f) + 0.5f;
                p.dl().line({gx, row.min.y}, {gx, row.max.y}, pal.divider);
            }
            const float indent = 6.0f + static_cast<float>(rw.depth) * indent_step;
            const rect  arrow  = rect::from_size({row.min.x + indent, row.min.y}, {18.0f, row_h});
            const bool  on_arrow = r.hovered && arrow.contains(ui.mouse_pos());
            if (has_kids || deeper) {
                p.glyph_center(arrow, is_open ? glyph::chevron_d : glyph::chevron_r, on_arrow ? pal.text : pal.text_dim, f.tile);
            }
            const rect icon = rect::from_size({arrow.max.x + 3.0f, row.center().y - 9.0f}, {18.0f, 18.0f});
            if (n.lumped) { p.glyph_center(icon, glyph::more, pal.text_faint); }
            else { p.kind_icon(icon, rw.kind); }
            p.text_left(rect{{icon.max.x + 8.0f, row.min.y}, {row.min.x + name_w - 8.0f, row.max.y}}, n.name, n.lumped ? pal.text_dim : pal.text, f.body);

            // share: a bar as long as the part of the folder on screen, then the percent
            const float  sx    = row.min.x + name_w;
            const double share = std::clamp(static_cast<double>(n.bytes) / total, 0.0, 1.0);
            const rect   track = rect::from_size({sx + 10.0f, row.center().y - 3.0f}, {share_col_w - 76.0f, 6.0f});
            p.dl().rect_filled(track, pal.raised, 3.0f);
            if (n.bytes > 0) {
                const float w = std::max(6.0f, track.width() * static_cast<float>(share));
                p.dl().rect_filled(rect::from_size(track.min, {w, track.height()}), rw.depth == 0 ? hue : hue.scaled_alpha(0.65f), 3.0f);
            }
            p.text_right(rect{{track.max.x, row.min.y}, {sx + share_col_w, row.max.y}}, percent(share), pal.text_dim, f.small, 4.0f);
            p.text_right(rect{{sx + share_col_w, row.min.y}, {sx + share_col_w + size_col_w, row.max.y}}, core::path::format_size(n.bytes), pal.text, f.body, 12.0f);
            if (n.is_dir || n.lumped) {
                p.text_right(rect{{sx + share_col_w + size_col_w, row.min.y}, {sx + share_col_w + size_col_w + files_col_w, row.max.y}}, std::format("{}", n.files), pal.text_dim, f.small, 12.0f);
            }

            if (r.hovered && ui.mouse_clicked(mouse_button::left)) {
                if (on_arrow && has_kids) { toggled = ri; }
                else if (on_arrow && deeper) { activated = pick_at(i); }
                else { selected = pick_at(i); }
            }
            if (r.hovered && !on_arrow && ui.item_double_clicked()) { // a double click does what the arrow does; a file opens
                if (n.is_dir) {
                    if (has_kids) { toggled = ri; } else if (deeper) { activated = pick_at(i); }
                } else if (!n.lumped) {
                    menus::open_file(env_, path_of(pick_at(i)));
                }
            }
            if (r.hovered && ui.mouse_clicked(mouse_button::right)) {
                ctx_ = pick_at(i);
                selected = ctx_;
                ctx_pos_  = ui.mouse_pos();
                ctx_open_ = true; // (opened outside the list's id scope, where the menu is built)
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
    } else if (ui.key_pressed(key::enter, false, false, true)) { // Alt+Enter
        if (!n->lumped) { env_.cmd.open_properties({path_of(pick_)}); ui.close_modal(); }
    } else if (ui.key_pressed(key::enter)) {
        if (has_kids) { toggle(); } else if (deeper) { look_inside(pick_); }
        else if (!n->is_dir && !n->lumped) { menus::open_file(env_, path_of(pick_)); }
    } else if (ui.key_pressed(key::del) && !n->lumped) {
        cache_.erase(cache_key(base_));
        env_.cmd.request_delete({path_of(pick_)}, false);
        ui.close_modal();
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
    const float width = std::min(1000.0f, ui.display_size().x - 60.0f);
    auto modal = ui.modal("Disk usage", {width, 0.0f}, modal_flags::esc_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        scan_.cancel();
        root_.reset();
        trail_.clear();
        history_.clear();
        rows_.clear();
        open_.clear();
        ranked_.clear();
        ranked_root_ = nullptr;
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
    {   // an icon tile, the title and the folder
        const item_result r = ui.custom_item("##du_head", {ui.content_width() - 36.0f, 48.0f});
        const rect tile = rect::from_size({r.bounds.min.x, r.bounds.min.y + 4.0f}, {40.0f, 40.0f});
        p.dl().rect_filled(tile, pal.accent.scaled_alpha(0.16f), 10.0f);
        p.glyph_center(tile, glyph::drive, pal.accent);
        const float tx = tile.max.x + 14.0f;
        p.text_left(rect{{tx, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 28.0f}}, tr("Disk usage"), pal.text, f.title);
        p.text_left(rect{{tx, r.bounds.min.y + 27.0f}, {r.bounds.max.x, r.bounds.max.y}}, root_ ? current_path() : base_, pal.text_dim, f.small);
        ui.same_line();
        if (kit.icon_button("##du_close", glyph::close, tr("Close  (Esc)"), true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(12.0f);

    if (!root_) {   // no way to know the total up front: a turning arc says it is working
        const item_result r = ui.custom_item("##du_run", {ui.content_width(), 96.0f});
        shape_style st;
        st.radius       = radii(12.0f);
        st.fill_top     = st.fill_bottom = pal.content;
        st.border       = pal.divider;
        st.border_width = 1.0f;
        p.dl().shape(r.bounds, st);
        const vec2  c{r.bounds.min.x + 52.0f, r.bounds.center().y};
        const float a = static_cast<float>(std::fmod(ui.time() * 4.0, static_cast<double>(two_pi)));
        p.dl().circle(c, 20.0f, pal.raised, 6.0f);
        p.dl().arc(c, 20.0f, a, a + 1.7f, pal.accent, 6.0f);
        const float tx = c.x + 44.0f;
        const float lb = p.line_height(f.bold);
        p.text_left(rect{{tx, c.y - lb - 1.0f}, {r.bounds.max.x - 16.0f, c.y - 1.0f}}, core::path::leaf(base_).empty() ? base_ : core::path::leaf(base_), pal.text, f.bold);
        p.text_left(rect{{tx, c.y + 1.0f}, {r.bounds.max.x - 16.0f, c.y + 1.0f + p.line_height(f.body)}},
                    trf("Measuring ... {} files, {} so far", scan_.files(), core::path::format_size(scan_.bytes())), pal.text_dim, f.body);
        ui.invalidate();
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
    kit.vgap(10.0f);

    draw_overview(p, cur);
    kit.vgap(10.0f);

    // the tree takes what is left under the fixed parts (header, crumbs, ring, footer, padding) and a margin round the dialog
    const float chrome = 60.0f + (kit.control_h() + 10.0f) + (overview_h + 10.0f) + (12.0f + kit.control_h()) + 36.0f + 6.0f;
    const float body_h = std::clamp(ui.display_size().y - chrome - 60.0f, 150.0f, 520.0f);
    draw_tree(p, cur, kit.dense() ? 26.0f : 30.0f, body_h);
    if (hot_next_ != hot_) { hot_ = hot_next_; ui.invalidate(); }
    hot_next_ = -1;
    // keyboard: arrows walk the tree, Enter opens / closes, Backspace goes back out
    tree_keys(ui);
    if (ui.key_pressed(key::backspace) && (trail_.size() > 1 || !history_.empty())) { back(); return; }

    if (ctx_open_) {
        ctx_open_ = false;
        ui.open_popup_menu("##du_ctx", ctx_pos_);
    }
    if (auto menu = ui.popup_menu("##du_ctx")) {
        if (ctx_.node != nullptr) {
            const std::string path = path_of(ctx_);
            const bool        dir    = ctx_.node->is_dir;
            const bool        real   = !ctx_.node->lumped; // ("N smaller files" is no single item)
            const std::string folder = dir ? path : core::path::parent(path);
            const bool kids  = dir && !ctx_.node->children.empty();
            const bool deeper = dir && !kids && ctx_.node->bytes > 0;
            if (kids && ui.menu_item(open_.contains(ctx_.node) ? tr("Collapse") : tr("Expand"), "Enter")) {
                if (open_.contains(ctx_.node)) { open_.erase(ctx_.node); } else { open_.insert(ctx_.node); }
                dirty_ = true;
            }
            if ((kids || deeper) && ui.menu_item(tr("Look inside"))) { look_inside(ctx_); }
            if (real && !dir && ui.menu_item(tr("Open"))) { menus::open_file(env_, path); }
            if (ui.menu_item(dir ? tr("Show in the file list") : tr("Show its folder in the file list"))) {
                env_.tab().navigate(folder);
                ui.close_modal();
            }
            menus::open_in(ui, env_, folder); // (the dialog stays: it is the user's place)
            ui.menu_separator();
            menus::terminal(ui, env_, folder);
            ui.menu_separator();
            if (real) { menus::path_items(ui, env_, {path}); }
            if (real && ui.menu_item(tr("Copy name"))) { (void)core::shell::set_clipboard_text(ctx_.node->name); }
            if (real) {
                ui.menu_separator();
                // (their dialogs show from the shell after this one is closed)
                if (ui.menu_item(tr("Properties"), "Alt+Enter")) { env_.cmd.open_properties({path}); ui.close_modal(); }
                if (ui.menu_item(tr("Move to Recycle Bin"), "Del")) {
                    cache_.erase(cache_key(base_)); // (its numbers are stale now)
                    env_.cmd.request_delete({path}, false);
                    ui.close_modal();
                }
            }
        }
    }

    // the line at the bottom
    kit.vgap(12.0f);
    {
        const bool can_back = trail_.size() > 1 || !history_.empty();
        const float bw_back = kit.button_width(tr("Back"), true);
        const float bw_scan = kit.button_width(tr("Scan again"), true);
        const float bw_show = kit.button_width(tr("Show in list"), true, Kit::ButtonKind::primary);
        const item_result hint = ui.custom_item("##du_hint", {ui.content_width() - bw_back - bw_scan - bw_show - 40.0f, kit.control_h()});
        if (pick_.node != nullptr) {
            const double share = cur.bytes > 0 ? static_cast<double>(pick_.node->bytes) / static_cast<double>(cur.bytes) : 0.0;
            // the numbers keep their room; a long name (or a wide language) is what gets cut
            const std::string info = core::path::format_size(pick_.node->bytes) + "  -  " + percent(share) + "  -  "
                                   + trf_n("{} file", "{} files", static_cast<long long>(pick_.node->files), pick_.node->files);
            const float iw = p.measure(f.small, info).x;
            const float nw = std::min(p.measure(f.small, pick_.node->name).x + 8.0f, std::max(0.0f, hint.bounds.width() - iw - 24.0f));
            const rect  nr{{hint.bounds.min.x, hint.bounds.min.y}, {hint.bounds.min.x + nw, hint.bounds.max.y}};
            if (nw > 24.0f) { p.text_left(nr, pick_.node->name, pal.text, f.small, 4.0f); }
            p.text_left(rect{{hint.bounds.min.x + (nw > 24.0f ? nw + 8.0f : 0.0f), hint.bounds.min.y}, hint.bounds.max}, info, pal.text_dim, f.small, 4.0f);
        } else {
            p.text_left(hint.bounds, tr("Double click to open, right click for more"), pal.text_dim, f.small, 4.0f);
        }
        if (can_back) {
            ui.same_line();
            if (kit.button({"##du_back", tr("Back"), glyph::back})) { back(); return; }
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
