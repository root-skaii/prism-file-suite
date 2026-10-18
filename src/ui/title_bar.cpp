#include "ui/title_bar.hpp"

#include "ui/panel.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace prism::ui {

using namespace strata;

platform::HitZone TitleBar::hit_test(vec2 pt) const
{
    if (max_.contains(pt)) { return platform::HitZone::maximize_button; }
    if (!area_.contains(pt) || strip_.contains(pt) || min_.contains(pt) || close_.contains(pt)) { return platform::HitZone::client; }
    return platform::HitZone::caption;
}

void TitleBar::frame(Painter& p, const rect& area)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    area_ = area;

    PanelScope panel{ui, "##titlebar", area};
    p.dl().rect_filled(area, pal.title_bar);

    model::TabSet& tabs = env_.tabs;
    const float    h    = area.height();
    const auto     n    = static_cast<float>(tabs.size());
    const float    btn  = m.caption_btn;
    const float    lead = 12.0f;
    const float    plus = 34.0f;
    const float    avail = area.width() - lead - plus - btn * 3.0f - 48.0f;
    const float    tw    = std::clamp(avail / n, 56.0f, m.tab_max_width);
    const float    strip_w = lead + n * tw + plus;

    // --- tab strip: one canvas, tabs and buttons hit-tested by hand
    const item_result strip = ui.custom_item("##tabstrip", {strip_w, h});
    strip_ = strip.bounds;
    ui.same_line();
    const float rest = std::max(0.0f, area.width() - strip_w - btn * 3.0f);
    (void)ui.custom_item("##titlegap", {rest, h});
    ui.same_line();

    const vec2  origin = strip.bounds.min;
    const float y0     = origin.y + h - m.tab_height - 4.0f;
    const float y1     = y0 + m.tab_height;
    const vec2  mp     = ui.mouse_pos();
    const bool  in_band = strip.hovered && mp.y >= y0 && mp.y < y1;

    int hover_tab = -1;
    if (in_band) {
        const int i = static_cast<int>(std::floor((mp.x - origin.x - lead) / tw));
        if (i >= 0 && i < static_cast<int>(tabs.size()) && mp.x >= origin.x + lead) { hover_tab = i; }
    }
    const rect plus_rect = rect::from_size({origin.x + lead + n * tw + 2.0f, y0 + (m.tab_height - 28.0f) * 0.5f}, {28.0f, 28.0f});
    const bool hover_plus = strip.hovered && plus_rect.contains(mp);

    const auto close_rect_of = [&](float x) {
        return rect::from_size({x + tw - 2.0f - 26.0f, y0 + (m.tab_height - 20.0f) * 0.5f}, {20.0f, 20.0f});
    };

    // --- input
    if (strip.hovered && ui.mouse_clicked(mouse_button::left)) {
        if (hover_plus) {
            env_.cmd.open_tab(tabs.active().path(), true);
        } else if (hover_tab >= 0) {
            const float x = origin.x + lead + static_cast<float>(hover_tab) * tw;
            const bool  show_close = hover_tab == static_cast<int>(tabs.active_index()) || tw > 90.0f;
            if (show_close && close_rect_of(x).contains(mp)) {
                env_.cmd.close_tab(static_cast<std::size_t>(hover_tab));
            } else {
                tabs.activate(static_cast<std::size_t>(hover_tab));
                drag_ = hover_tab;
            }
        }
    }
    if (strip.hovered && hover_tab >= 0 && ui.mouse_clicked(mouse_button::middle)) {
        env_.cmd.close_tab(static_cast<std::size_t>(hover_tab));
    }
    if (drag_ >= 0) {
        if (!ui.mouse_down(mouse_button::left)) {
            drag_ = -1;
        } else {
            const int target = std::clamp(static_cast<int>(std::floor((mp.x - origin.x - lead) / tw)), 0, static_cast<int>(tabs.size()) - 1);
            if (target != drag_ && drag_ < static_cast<int>(tabs.size())) {
                tabs.move(static_cast<std::size_t>(drag_), static_cast<std::size_t>(target));
                drag_ = target;
            }
        }
    }

    // --- tabs
    p.dl().push_clip(rect{{origin.x, area.min.y}, {origin.x + strip_w + 4.0f, area.max.y}});
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        model::Tab& tab    = tabs.at(i);
        const bool  active = i == tabs.active_index();
        const bool  hover  = static_cast<int>(i) == hover_tab;
        const std::string key = std::format("##tab{}", reinterpret_cast<std::uintptr_t>(&tab));
        const float x      = ui.animate(key + "x", origin.x + lead + static_cast<float>(i) * tw, 26.0f);
        const float hover_k = ui.animate(key + "h", hover ? 1.0f : 0.0f, 18.0f);
        const rect  body{{x, y0}, {x + tw - 2.0f, y1}};

        if (active) {
            shape_style s;
            s.radius      = radii(m.pane_radius - 2.0f);
            s.fill_top    = s.fill_bottom = pal.raised;
            s.border      = pal.border;
            s.border_width = 1.0f;
            p.dl().shape(body, s);
        } else if (hover_k > 0.01f) {
            p.dl().rect_filled(body, pal.hover.scaled_alpha(hover_k), m.pane_radius - 2.0f);
        }

        const float icon = 16.0f;
        p.folder_icon(rect::from_size({x + 10.0f, y0 + (m.tab_height - icon) * 0.5f}, {icon, icon}));
        const bool close_visible = active || hover || tw > 90.0f;
        const float text_right = close_visible ? 30.0f : 10.0f;
        const rect  text_box{{x + 34.0f, y0}, {x + tw - 2.0f - text_right, y1}};
        p.text_left(text_box, tab.title(), active ? pal.text : pal.text_dim, env_.fonts.body);

        if (close_visible) {
            const rect cr = close_rect_of(x);
            if (strip.hovered && cr.contains(mp)) { p.dl().rect_filled(cr, pal.pressed, 5.0f); }
            p.glyph_center(cr, glyph::close, active || hover ? pal.text_dim : pal.text_faint, env_.fonts.tile);
        }
    }
    p.dl().pop_clip();

    if (hover_plus) { p.dl().rect_filled(plus_rect, pal.hover, 7.0f); }
    p.glyph_center(plus_rect, glyph::add, pal.text_dim, env_.fonts.tile);

    // --- window buttons
    const float bx = area.max.x - btn * 3.0f;
    min_   = rect::from_size({bx, area.min.y}, {btn, h});
    max_   = rect::from_size({bx + btn, area.min.y}, {btn, h});
    close_ = rect::from_size({bx + btn * 2.0f, area.min.y}, {btn, h});

    const item_result rmin   = ui.custom_item("##btn_min", {btn, h});
    ui.same_line();
    (void)ui.custom_item("##btn_max", {btn, h});
    ui.same_line();
    const item_result rclose = ui.custom_item("##btn_close", {btn, h});

    const bool max_hot = env_.window.maximize_hovered();
    if (rmin.hovered) { p.dl().rect_filled(min_, pal.hover); }
    if (max_hot) { p.dl().rect_filled(max_, env_.window.maximize_pressed() ? pal.pressed : pal.hover); }
    if (rclose.hovered) { p.dl().rect_filled(close_, pal.close_hover); }
    p.glyph_center(min_, glyph::minimize, pal.text, env_.fonts.tile);
    p.glyph_center(max_, env_.window.maximized() ? glyph::restore : glyph::maximize, pal.text, env_.fonts.tile);
    p.glyph_center(close_, glyph::window_x, rclose.hovered ? color{255, 255, 255, 255} : pal.text, env_.fonts.tile);

    if (rmin.hovered && ui.mouse_released(mouse_button::left)) { env_.window.minimize(); }
    if (rclose.hovered && ui.mouse_released(mouse_button::left)) { env_.window.request_close(); }
}

} // namespace prism::ui
