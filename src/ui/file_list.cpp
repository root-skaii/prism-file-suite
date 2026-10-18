#include "ui/file_list.hpp"

#include "core/path.hpp"
#include "core/shell_ops.hpp"
#include "ui/panel.hpp"

#include <algorithm>
#include <cmath>

namespace prism::ui {

using namespace strata;
using model::SortKey;

namespace {

constexpr float k_pad   = 6.0f;  // inset of a row from the card edge
constexpr float k_min_name = 200.0f;

[[nodiscard]] std::string_view error_text(std::uint32_t code) noexcept
{
    switch (code) {
    case 2:
    case 3:  return "This folder no longer exists";
    case 5:  return "Access is denied";
    case 21: return "The device is not ready";
    case 53:
    case 67: return "The network location cannot be reached";
    default: return "This folder cannot be opened";
    }
}

struct Columns {
    float mod_x{}, type_x{}, size_x{}, end_x{};
};

// x of each column's left edge, from the right: name takes what is left
[[nodiscard]] Columns layout_columns(const std::array<float, 3>& w, float body_w) noexcept
{
    const float sum = w[0] + w[1] + w[2];
    const float room = std::max(0.0f, body_w - k_min_name);
    const float k = sum > room && sum > 0.0f ? room / sum : 1.0f;
    Columns c;
    c.end_x  = body_w;
    c.size_x = body_w - w[2] * k;
    c.type_x = c.size_x - w[1] * k;
    c.mod_x  = c.type_x - w[0] * k;
    return c;
}

} // namespace

void FileList::open_row(model::Tab& tab, std::size_t row)
{
    const core::FileEntry& e = tab.listing().at(row);
    const std::string full = core::path::join(tab.path(), e.name);
    if (e.is_dir()) {
        tab.navigate(full);
    } else {
        (void)core::shell::open(full);
    }
}

void FileList::move_focus(model::Tab& tab, std::size_t row, bool extend)
{
    model::Listing& L = tab.listing();
    if (L.rows() == 0) { return; }
    row = std::min(row, L.rows() - 1);
    if (extend) {
        const std::size_t anchor = L.anchor() == model::Listing::npos ? row : L.anchor();
        L.select_range(anchor, row, false);
        L.set_focus(row, false);
        if (L.anchor() == model::Listing::npos) { L.set_focus(anchor, true); L.set_focus(row, false); }
    } else {
        L.select_only(row);
        L.set_focus(row, true);
    }
    ensure_visible_ = true;
}

void FileList::handle_keys(context& ui, model::Tab& tab, const FrameInput& in, float viewport_h)
{
    model::Listing& L = tab.listing();
    const std::size_t n = L.rows();
    if (ui.want_text_input() || ui.menu_open()) { return; }

    const std::size_t page = std::max<std::size_t>(1, static_cast<std::size_t>(viewport_h / env_.theme.metrics().row_height) - 1);
    const bool        has_focus = L.focus() != model::Listing::npos;
    const std::size_t cur       = has_focus ? L.focus() : 0;

    const auto nav = [&](key k, std::size_t first_target, std::size_t target) {
        for (const bool shift : {false, true}) {
            if (ui.key_pressed(k, false, shift)) {
                keyboard_mode_ = true;
                move_focus(tab, has_focus ? target : first_target, shift);
                return;
            }
        }
    };
    if (n > 0) {
        nav(key::down,      0,     std::min(cur + 1, n - 1));
        nav(key::up,        n - 1, cur > 0 ? cur - 1 : 0);
        nav(key::page_down, 0,     std::min(cur + page, n - 1));
        nav(key::page_up,   0,     cur > page ? cur - page : 0);
        nav(key::home,      0,     0);
        nav(key::end,       n - 1, n - 1);
        if (ui.key_pressed(key::a, true)) { L.select_all(); }
        if (ui.key_pressed(key::enter) && has_focus) { open_row(tab, cur); }
        if (ui.key_pressed(key::escape)) { L.clear_selection(); }
    }

    // type-to-find: letters typed in quick succession build a prefix
    find_age_ += in.dt;
    if (find_age_ > 1.0) { find_buffer_.clear(); }
    bool typed_any = false;
    for (const char c : in.typed) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
            find_buffer_.push_back(c);
            typed_any = true;
        }
    }
    if (typed_any && !ui.ctrl_down() && !ui.alt_down()) {
        find_age_ = 0.0;
        const std::size_t from = find_buffer_.size() == 1 && has_focus ? cur + 1 : cur;
        if (const auto hit = L.find_prefix(find_buffer_, from)) {
            keyboard_mode_ = true;
            move_focus(tab, *hit, false);
        }
    }
}

void FileList::draw_header(Painter& p, model::Listing& L, float width, float height, float body_w)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;

    const item_result r = ui.custom_item("##hdr", {width, height});
    const rect  box = r.bounds;
    const Columns c = layout_columns(widths_, body_w);
    const vec2  mp  = ui.mouse_pos();
    p.dl().rect_filled({{box.min.x + k_pad, box.max.y - 1.0f}, {box.max.x - k_pad, box.max.y}}, pal.divider);

    struct Cell { SortKey key; std::string_view label; float x0, x1; bool right; };
    const float name_x = box.min.x + k_pad + 10.0f + env_.theme.metrics().icon + 10.0f - 10.0f;
    const Cell cells[] = {
        {SortKey::name,     "Name",          box.min.x + k_pad,       box.min.x + c.mod_x,  false},
        {SortKey::modified, "Date modified", box.min.x + c.mod_x,     box.min.x + c.type_x, false},
        {SortKey::type,     "Type",          box.min.x + c.type_x,    box.min.x + c.size_x, false},
        {SortKey::size,     "Size",          box.min.x + c.size_x,    box.min.x + c.end_x,  true},
    };

    // resize handles sit on the left edge of the three right columns
    const float handles[col_count] = {box.min.x + c.mod_x, box.min.x + c.type_x, box.min.x + c.size_x};
    int hot_handle = -1;
    if (r.hovered) {
        for (int i = 0; i < col_count; ++i) {
            if (std::abs(mp.x - handles[i]) <= 4.0f) { hot_handle = i; }
        }
    }
    if (hot_handle >= 0 && ui.mouse_clicked(mouse_button::left)) {
        resize_col_     = hot_handle;
        resize_start_x_ = mp.x;
        resize_start_w_ = widths_[static_cast<std::size_t>(hot_handle)];
    }
    if (resize_col_ >= 0) {
        if (!ui.mouse_down(mouse_button::left)) {
            resize_col_ = -1;
        } else {
            widths_[static_cast<std::size_t>(resize_col_)] = std::clamp(resize_start_w_ - (mp.x - resize_start_x_), 60.0f, 420.0f);
        }
    }

    for (const Cell& cell : cells) {
        const rect cr{{cell.x0, box.min.y + 3.0f}, {cell.x1 - 2.0f, box.max.y - 4.0f}};
        const bool hot = r.hovered && cr.contains(mp) && hot_handle < 0 && resize_col_ < 0;
        const bool is_sorted = L.sort().key == cell.key;
        if (hot) { p.dl().rect_filled(cr, pal.hover, 6.0f); }

        const float text_pad = cell.key == SortKey::name ? 10.0f + env_.theme.metrics().icon + 10.0f : 10.0f;
        const color col = is_sorted ? pal.text : pal.text_dim;
        float label_w = 0.0f;
        if (cell.right) {
            label_w = p.text_right(cr, cell.label, col, f.small, 22.0f);
        } else {
            label_w = p.text_left(cr, cell.label, col, f.small, text_pad);
        }
        if (is_sorted) {
            const float ax = cell.right ? cr.max.x - 22.0f - label_w - 10.0f : cr.min.x + text_pad + label_w + 10.0f;
            p.sort_arrow({ax, cr.center().y}, L.sort().ascending, pal.accent);
        }
        if (hot && ui.mouse_clicked(mouse_button::left)) {
            model::SortSpec s = L.sort();
            if (s.key == cell.key) {
                s.ascending = !s.ascending;
            } else {
                s.key = cell.key;
                // sizes and dates are most useful biggest / newest first
                s.ascending = cell.key == SortKey::name || cell.key == SortKey::type;
            }
            L.set_sort(s);
        }
    }
    for (int i = 0; i < col_count; ++i) {
        const bool active = hot_handle == i || resize_col_ == i;
        const color lc = active ? pal.accent : pal.divider;
        p.dl().rect_filled({{handles[i], box.min.y + 8.0f}, {handles[i] + 1.0f, box.max.y - 9.0f}}, lc);
    }
    (void)name_x;
}

void FileList::draw_empty_state(Painter& p, model::Tab& tab, const rect& vp)
{
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    std::string line;
    color col = pal.text_dim;
    if (tab.listing().finished()) {
        if (tab.error() != 0) {
            line = std::string{error_text(tab.error())};
            col  = pal.danger;
        } else if (tab.listing().total() > 0) {
            line = "No matching items";
        } else {
            line = "This folder is empty";
        }
    } else {
        line = "Loading...";
    }
    p.text_center(rect{{vp.min.x, vp.min.y + vp.height() * 0.30f}, {vp.max.x, vp.min.y + vp.height() * 0.30f + 28.0f}}, line, col, f.body);
}

void FileList::draw_rows(Painter& p, model::Tab& tab, const item_result& cv, float body_w, float viewport_top, float viewport_bottom, int hover_row)
{
    context&        ui  = p.ui();
    const Palette&  pal = p.pal();
    const Metrics&  m   = p.metrics();
    const FontSet&  f   = env_.fonts;
    model::Listing& L   = tab.listing();
    const Columns   c   = layout_columns(widths_, body_w);

    const float rh   = m.row_height;
    const std::size_t rows = L.rows();
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor((viewport_top - cv.bounds.min.y) / rh)));
    const auto last  = std::min(rows, static_cast<std::size_t>(std::max(0.0f, std::ceil((viewport_bottom - cv.bounds.min.y) / rh))) + 1);

    const float x0 = cv.bounds.min.x;
    for (std::size_t i = first; i < last; ++i) {
        const float y = cv.bounds.min.y + static_cast<float>(i) * rh;
        const rect  row{{x0 + k_pad, y + 1.0f}, {x0 + body_w - k_pad, y + rh - 1.0f}};
        const core::FileEntry& e = L.at(i);
        const bool sel   = L.selected(i);
        const bool hover = static_cast<int>(i) == hover_row;

        if (sel) {
            p.dl().rect_filled(row, pal.selection, 7.0f);
            if (hover) { p.dl().rect_filled(row, pal.hover, 7.0f); }
        } else if (hover) {
            p.dl().rect_filled(row, pal.hover, 7.0f);
        }
        if (keyboard_mode_ && L.focus() == i) { p.dl().rect_outline(row, pal.selection_edge, 7.0f, 1.5f); }

        const float icon = m.icon;
        p.kind_icon(rect::from_size({row.min.x + 10.0f, y + (rh - icon) * 0.5f}, {icon, icon}), e.kind);

        const float tx = row.min.x + 10.0f + icon + 10.0f;
        const color name_col = e.is_hidden() ? pal.text_dim : pal.text;
        p.text_left(rect{{tx, y}, {x0 + c.mod_x - 8.0f, y + rh}}, e.name, name_col, f.body);

        const color dim = pal.text_dim;
        p.text_left(rect{{x0 + c.mod_x, y}, {x0 + c.type_x - 8.0f, y + rh}}, core::path::format_time(e.modified), dim, f.body, 10.0f);
        p.text_left(rect{{x0 + c.type_x, y}, {x0 + c.size_x - 8.0f, y + rh}}, core::type_label(e.kind, e.ext()), dim, f.body, 10.0f);
        if (!e.is_dir()) {
            p.text_right(rect{{x0 + c.size_x, y}, {x0 + c.end_x, y + rh}}, core::path::format_size(e.size), dim, f.body, 22.0f);
        }
    }
    (void)ui;
}

void FileList::context_menu(Painter& p, model::Tab& tab, const rect& area)
{
    context&        ui = p.ui();
    model::Listing& L  = tab.listing();
    auto menu = ui.context_menu("##file_ctx", area);
    if (!menu) { return; }

    const std::vector<std::size_t> picked = L.selected_rows();
    if (picked.empty()) {
        if (ui.menu_item("Refresh", "F5")) { tab.refresh(); }
        if (ui.menu_item("Copy folder path")) { (void)core::shell::set_clipboard_text(tab.path()); }
        if (ui.menu_item("Open in new tab")) { env_.cmd.open_tab(tab.path(), true); }
        return;
    }
    const std::size_t row = picked.front();
    const core::FileEntry& e = L.at(row);
    const std::string full = core::path::join(tab.path(), e.name);

    if (ui.menu_item("Open", "Enter")) { open_row(tab, row); }
    if (e.is_dir() && ui.menu_item("Open in new tab")) { env_.cmd.open_tab(full, false); }
    ui.menu_separator();
    if (ui.menu_item(picked.size() > 1 ? "Copy paths" : "Copy path")) {
        std::string all;
        for (const std::size_t r : picked) {
            if (!all.empty()) { all += "\r\n"; }
            all += core::path::join(tab.path(), L.at(r).name);
        }
        (void)core::shell::set_clipboard_text(all);
    }
    if (ui.menu_item("Show in File Explorer")) { (void)core::shell::reveal_in_explorer(full); }
    if (ui.menu_item("Properties")) { (void)core::shell::show_properties(full); }
    ui.menu_separator();
    if (ui.menu_item("Refresh", "F5")) { tab.refresh(); }
}

void FileList::frame(Painter& p, const rect& area, const FrameInput& in)
{
    context&        ui  = p.ui();
    const Palette&  pal = p.pal();
    const Metrics&  m   = p.metrics();
    model::Tab&     tab = env_.tabs.active();
    model::Listing& L   = tab.listing();

    PanelScope panel{ui, "##files", area};

    shape_style card;
    card.radius       = radii(m.pane_radius);
    card.fill_top     = card.fill_bottom = pal.content;
    card.border       = pal.border;
    card.border_width = 1.0f;
    p.dl().shape(area, card);

    if (last_tab_ != &tab || applied_generation_ != tab.generation) {
        last_tab_           = &tab;
        applied_generation_ = tab.generation;
        restore_pending_    = true;
        keyboard_mode_      = false;
        find_buffer_.clear();
    }

    const float hh     = m.header_height;
    const float body_w = area.width() - context::scrollbar_width();
    draw_header(p, L, area.width(), hh, body_w);

    auto body = ui.child("##rows", {area.width(), area.height() - hh - k_pad}, child_flags::no_padding);
    const rect vp = ui.content_rect();

    const float rh      = m.row_height;
    const float total_h = static_cast<float>(L.rows()) * rh;
    const item_result cv = ui.custom_item("##canvas", {body_w, std::max(total_h, vp.height() - 1.0f)});
    const bool dbl = ui.item_double_clicked();

    // scroll: restore after navigation, keep the focused row in view
    {
        const float scroll = ui.scroll_y();
        if (restore_pending_ && L.finished()) {
            restore_pending_ = false;
            float target = tab.scroll_y;
            if (!tab.reveal_name.empty()) {
                if (const auto row = L.find_name(tab.reveal_name)) {
                    L.select_only(*row);
                    L.set_focus(*row, true);
                    target = std::max(0.0f, static_cast<float>(*row) * rh - vp.height() * 0.35f);
                }
                tab.reveal_name.clear();
            }
            ui.set_scroll_y(target);
            tab.scroll_y = target;
        } else if (!restore_pending_) {
            tab.scroll_y = scroll;
        }
        if (ensure_visible_ && L.focus() != model::Listing::npos) {
            const float top = static_cast<float>(L.focus()) * rh;
            if (top < scroll) {
                ui.set_scroll_y(top);
            } else if (top + rh > scroll + vp.height()) {
                ui.set_scroll_y(top + rh - vp.height());
            }
        }
        ensure_visible_ = false;
    }

    // pointer
    const vec2 mp = ui.mouse_pos();
    int hover_row = -1;
    if (cv.hovered && vp.contains(mp)) {
        const int r = static_cast<int>(std::floor((mp.y - cv.bounds.min.y) / rh));
        if (r >= 0 && static_cast<std::size_t>(r) < L.rows()) { hover_row = r; }
    }
    if (cv.hovered && vp.contains(mp)) {
        if (ui.mouse_clicked(mouse_button::left)) {
            keyboard_mode_ = false;
            if (hover_row >= 0) {
                const auto row = static_cast<std::size_t>(hover_row);
                if (ui.ctrl_down()) {
                    L.toggle(row);
                    L.set_focus(row, true);
                } else if (ui.shift_down()) {
                    const std::size_t anchor = L.anchor() == model::Listing::npos ? row : L.anchor();
                    L.select_range(anchor, row, false);
                    L.set_focus(row, false);
                } else {
                    L.select_only(row);
                    L.set_focus(row, true);
                }
            } else if (!ui.ctrl_down()) {
                L.clear_selection();
            }
        }
        if (dbl && hover_row >= 0 && !ui.ctrl_down() && !ui.shift_down()) { open_row(tab, static_cast<std::size_t>(hover_row)); }
        if (ui.mouse_clicked(mouse_button::right)) {
            if (hover_row >= 0 && !L.selected(static_cast<std::size_t>(hover_row))) {
                L.select_only(static_cast<std::size_t>(hover_row));
                L.set_focus(static_cast<std::size_t>(hover_row), true);
            } else if (hover_row < 0) {
                L.clear_selection();
            }
        }
        if (ui.mouse_clicked(mouse_button::middle) && hover_row >= 0) {
            const core::FileEntry& e = L.at(static_cast<std::size_t>(hover_row));
            if (e.is_dir()) { env_.cmd.open_tab(core::path::join(tab.path(), e.name), false); }
        }
    }
    handle_keys(ui, tab, in, vp.height());

    draw_rows(p, tab, cv, body_w, vp.min.y, vp.max.y, hover_row);
    if (L.rows() == 0) { draw_empty_state(p, tab, vp); }
    context_menu(p, tab, vp);
}

} // namespace prism::ui
