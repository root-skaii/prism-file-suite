#include "ui/toolbar.hpp"

#include "core/path.hpp"
#include "ui/panel.hpp"

#include <algorithm>
#include <cmath>

namespace prism::ui {

using namespace strata;

bool Toolbar::icon_button(Painter& p, std::string_view id, char32_t g, bool enabled, bool on, std::string_view tip, float size)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const item_result r = ui.custom_item(id, {size, size});
    const bool hot = enabled && r.hovered;
    if (on) { p.dl().rect_filled(r.bounds, pal.accent.scaled_alpha(0.18f), 8.0f); }
    if (hot) { p.dl().rect_filled(r.bounds, r.held ? pal.pressed : pal.hover, 8.0f); }
    p.glyph_center(r.bounds, g, !enabled ? pal.text_faint : (on ? pal.accent : pal.text));
    if (r.hovered && !tip.empty()) { ui.tooltip(tip); }
    return hot && ui.mouse_released(mouse_button::left);
}

void Toolbar::field_background(Painter& p, const rect& r)
{
    shape_style s;
    s.radius       = radii(8.0f);
    s.fill_top     = s.fill_bottom = p.pal().raised;
    s.border       = p.pal().border;
    s.border_width = 1.0f;
    p.dl().shape(r, s);
}

void Toolbar::address(Painter& p, float area_x, float area_y, float width, float height)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    model::Tab&    tab = env_.tabs.active();

    if (want_edit_) {
        want_edit_ = false;
        editing_   = true;
        edit_age_  = 0;
        edit_text_ = tab.path();
    }

    if (editing_) {
        // the field is a strata widget: sized to the row by its padding
        ui.request_text_focus("##address");
        field_background(p, rect::from_size({area_x, area_y}, {width, height}));
        ui.set_next_item_width(width);
        ui.push_var(style_var::frame_padding_y, (height - p.line_height(f.body)) * 0.5f);
        ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
        (void)ui.input_text("##address", edit_text_, "Type a path");
        ui.pop_color();
        ui.pop_var();
        ++edit_age_;
        if (ui.key_pressed(key::enter)) {
            const std::string target = core::path::normalize(edit_text_);
            if (core::path::is_directory(target)) {
                tab.navigate(target);
                editing_ = false;
            }
        } else if (ui.key_pressed(key::escape) || (edit_age_ > 3 && !ui.field_focused("##address"))) {
            editing_ = false;
        }
        return;
    }

    const item_result r = ui.custom_item("##crumbs", {width, height});
    const rect box = r.bounds;
    shape_style bg;
    bg.radius       = radii(8.0f);
    bg.fill_top     = bg.fill_bottom = pal.raised;
    bg.border       = r.hovered ? pal.text_faint.scaled_alpha(0.6f) : pal.border;
    bg.border_width = 1.0f;
    p.dl().shape(box, bg);

    struct Seg { std::string name; std::string path; float w; };
    std::vector<Seg> segs;
    for (auto& s : core::path::segments(tab.path())) {
        const float w = p.measure(f.body, s.name).x + 16.0f;
        segs.push_back({std::move(s.name), std::move(s.path), w});
    }
    const float chev = 16.0f;
    const float avail = width - 16.0f;
    // drop leading segments until the rest fits; a "..." takes their place
    std::size_t first = 0;
    const auto total = [&](std::size_t from) {
        float t = from > 0 ? 28.0f : 0.0f;
        for (std::size_t i = from; i < segs.size(); ++i) { t += segs[i].w + (i + 1 < segs.size() ? chev : 0.0f); }
        return t;
    };
    while (first + 1 < segs.size() && total(first) > avail) { ++first; }

    p.dl().push_clip(box);
    float x = box.min.x + 8.0f;
    const vec2 mp = ui.mouse_pos();
    bool over_segment = false;
    if (first > 0) {
        p.text_center(rect{{x, box.min.y}, {x + 28.0f, box.max.y}}, "...", pal.text_dim, f.body);
        x += 28.0f;
    }
    for (std::size_t i = first; i < segs.size(); ++i) {
        const rect cell{{x, box.min.y + 4.0f}, {x + segs[i].w, box.max.y - 4.0f}};
        const bool hot = r.hovered && cell.contains(mp);
        if (hot) {
            over_segment = true;
            p.dl().rect_filled(cell, r.held ? pal.pressed : pal.hover, 6.0f);
            if (ui.mouse_clicked(mouse_button::left)) { tab.navigate(segs[i].path); }
            if (ui.mouse_clicked(mouse_button::middle)) { env_.cmd.open_tab(segs[i].path, false); }
        }
        p.text_center(cell, segs[i].name, i + 1 == segs.size() ? pal.text : pal.text_dim, f.body);
        x += segs[i].w;
        if (i + 1 < segs.size()) {
            p.glyph_center(rect{{x, box.min.y}, {x + chev, box.max.y}}, glyph::chevron_r, pal.text_faint, f.tile);
            x += chev;
        }
    }
    p.dl().pop_clip();

    if (r.hovered && !over_segment && ui.mouse_clicked(mouse_button::left)) { want_edit_ = true; }
}

void Toolbar::frame(Painter& p, const rect& area, bool show_hidden)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    const FontSet& f   = env_.fonts;
    model::TabSet& tabs = env_.tabs;
    model::Tab&    tab  = tabs.active();

    // the filter follows the tab: another tab, or this one navigated
    if (last_tab_ != &tab || last_generation_ != tab.generation) {
        last_tab_        = &tab;
        last_generation_ = tab.generation;
        filter_text_     = tab.listing().filter();
        editing_         = false;
    }

    PanelScope panel{ui, "##toolbar", area};
    p.dl().rect_filled(area, pal.title_bar);

    const float rh  = m.toolbar - 12.0f;
    const float gap = m.gap;
    const float filter_w = std::clamp(area.width() * 0.2f, 160.0f, 280.0f);
    const float right_w  = rh * 3.0f + gap * 3.0f + filter_w;
    const float left_w   = 10.0f + rh * 4.0f + gap * 2.0f;
    const float addr_w   = std::max(120.0f, area.width() - left_w - right_w - 10.0f);

    (void)ui.custom_item("##tb_vpad", {1.0f, 6.0f});
    (void)ui.custom_item("##tb_lpad", {10.0f, rh});
    ui.same_line();
    if (icon_button(p, "##nav_back", glyph::back, tab.can_back(), false, "Back  (Alt+Left)", rh)) { (void)tab.back(); }
    ui.same_line();
    if (icon_button(p, "##nav_fwd", glyph::forward, tab.can_forward(), false, "Forward  (Alt+Right)", rh)) { (void)tab.forward(); }
    ui.same_line();
    if (icon_button(p, "##nav_up", glyph::up, tab.can_up(), false, "Up  (Alt+Up)", rh)) { (void)tab.up(); }
    ui.same_line();
    if (icon_button(p, "##nav_refresh", glyph::refresh, true, false, "Refresh  (F5)", rh)) { tab.refresh(); }
    ui.same_line();
    (void)ui.custom_item("##tb_gap1", {gap * 2.0f, rh});
    ui.same_line();

    address(p, area.min.x + left_w, area.min.y + 6.0f, addr_w, rh);
    ui.same_line();
    (void)ui.custom_item("##tb_gap2", {gap, rh});
    ui.same_line();

    // filter field (a strata widget, sized to the row). strata darkens a field's fill by a fixed amount, which turns grey
    // on the light theme: the fill is made transparent and the background drawn here, where the layout puts the field
    const float filter_x = area.min.x + left_w + addr_w + gap;
    field_background(p, rect::from_size({filter_x, area.min.y + 6.0f}, {filter_w, rh}));
    ui.set_next_item_width(filter_w);
    ui.push_var(style_var::frame_padding_y, (rh - p.line_height(f.body)) * 0.5f);
    ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
    if (want_filter_focus_) {
        want_filter_focus_ = false;
        ui.request_text_focus("##filter");
    }
    if (ui.input_text("##filter", filter_text_, "Filter this folder")) { tab.listing().set_filter(filter_text_); }
    ui.pop_color();
    ui.pop_var();
    ui.same_line();
    (void)ui.custom_item("##tb_gap3", {gap, rh});
    ui.same_line();
    if (icon_button(p, "##tb_hidden", glyph::eye, true, show_hidden, "Show hidden files", rh)) { env_.cmd.toggle_hidden(); }
    ui.same_line();
    if (icon_button(p, "##tb_density", glyph::list, true, env_.theme.density() == Density::compact, "Compact layout", rh)) { env_.cmd.toggle_density(); }
    ui.same_line();
    if (icon_button(p, "##tb_theme", glyph::sun, true, false, env_.theme.dark() ? "Light theme" : "Dark theme", rh)) { env_.cmd.toggle_theme(); }
}

} // namespace prism::ui
