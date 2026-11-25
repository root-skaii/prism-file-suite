#include "ui/title_bar.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "core/shell_ops.hpp"
#include "ui/tr.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace prism::ui {

using namespace strata;

std::string TitleBar::drop_target_at(vec2 p) const
{
    for (std::size_t i = 0; i < tab_rects_.size() && i < env_.tabs.size(); ++i) {
        if (tab_rects_[i].contains(p)) { return env_.tabs.at(i).path(); }
    }
    return {};
}

platform::HitZone TitleBar::hit_test(vec2 pt) const
{
    if (max_.contains(pt)) { return platform::HitZone::maximize_button; }
    if (!area_.contains(pt) || strip_.contains(pt) || admin_.contains(pt) || min_.contains(pt) || close_.contains(pt)) { return platform::HitZone::client; }
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
    // the tabs get what is left beside the window buttons (and a stretch of bare title bar to drag the window by). past
    // that they keep a readable width and scroll sideways instead of running under the buttons (the stretch of bare
    // title bar left over is what you drag the window by: with many tabs it must not shrink to nothing)
    const float    drag_gap = std::clamp(area.width() * 0.12f, 80.0f, 180.0f); // bare title bar, kept free to grab the window by
    const float    avail = std::max(80.0f, area.width() - lead - plus - btn * 3.0f - drag_gap);
    const float    tw    = std::clamp(avail / n, m.tab_min_width, m.tab_max_width);
    const float    vw    = std::min(n * tw, avail);          // the visible part of the strip
    const float    max_scroll = std::max(0.0f, n * tw - vw);
    const float    strip_w = lead + vw + plus;

    // --- tab strip: one canvas, tabs and buttons hit-tested by hand
    const item_result strip = ui.custom_item("##tabstrip", {strip_w, h});
    strip_ = strip.bounds;
    ui.same_line();
    // administrator: a shield button starts an elevated window; in an elevated one a badge says so
    const bool  elevated = core::shell::is_elevated();
    constexpr float admin_w = 28.0f; // square, like the new-tab button
    const float rest = std::max(0.0f, area.width() - strip_w - btn * 3.0f - admin_w - 6.0f);
    (void)ui.custom_item("##titlegap", {rest, h});
    ui.same_line();
    const item_result radmin = ui.custom_item("##admin", {admin_w + 6.0f, h});
    admin_ = rect::from_size({radmin.bounds.min.x, area.min.y + (h - admin_w) * 0.5f}, {admin_w, admin_w});
    ui.same_line();

    const vec2  origin = strip.bounds.min;
    const float y0     = origin.y + h - m.tab_height;
    const float y1     = y0 + m.tab_height;
    const vec2  mp     = ui.mouse_pos();
    const bool  in_band = strip.hovered && mp.y >= y0 && mp.y < y1;

    // scrolling: the wheel over the strip, the active tab kept in view when it changes (new tab, Ctrl+Tab, reopened)
    if (max_scroll > 0.0f && strip.hovered) {
        if (const float wheel = ui.take_wheel(); wheel != 0.0f) { scroll_x_ -= wheel * 90.0f; }
    }
    if (&tabs.active() != last_active_ || tabs.size() != last_count_) {
        const float ax = static_cast<float>(tabs.active_index()) * tw;
        if (ax < scroll_x_) { scroll_x_ = ax; }
        else if (ax + tw > scroll_x_ + vw) { scroll_x_ = ax + tw - vw; }
        last_active_ = &tabs.active();
        last_count_  = tabs.size();
    }
    const float view_l = origin.x + lead; // the visible part of the strip, left and right edge
    const float view_r = view_l + vw;
    if (drag_ >= 0 && ui.mouse_down(mouse_button::left)) { // (dragging a tab to the edge scrolls on)
        if (ui.mouse_pos().x < view_l + 24.0f) { scroll_x_ -= 10.0f; ui.invalidate(); }
        else if (ui.mouse_pos().x > view_r - 24.0f) { scroll_x_ += 10.0f; ui.invalidate(); }
    }
    scroll_x_ = std::clamp(scroll_x_, 0.0f, max_scroll);

    int hover_tab = -1;
    if (in_band && mp.x >= view_l && mp.x < view_r) {
        const int i = static_cast<int>(std::floor((mp.x - view_l + scroll_x_) / tw));
        if (i >= 0 && i < static_cast<int>(tabs.size())) { hover_tab = i; }
    }
    const rect plus_rect = rect::from_size({view_r + 2.0f, y0 + (m.tab_height - 28.0f) * 0.5f}, {28.0f, 28.0f});
    const bool hover_plus = strip.hovered && plus_rect.contains(mp);

    const auto close_rect_of = [&](float x) {
        return rect::from_size({x + tw - 2.0f - 26.0f, y0 + (m.tab_height - 20.0f) * 0.5f}, {20.0f, 20.0f});
    };

    // --- input
    if (strip.hovered && ui.mouse_clicked(mouse_button::left)) {
        if (hover_plus) {
            env_.cmd.open_tab(tabs.active().path(), true);
        } else if (hover_tab >= 0) {
            const float x = view_l - scroll_x_ + static_cast<float>(hover_tab) * tw;
            const bool  show_close = hover_tab == static_cast<int>(tabs.active_index()) || tw > 90.0f;
            if (show_close && close_rect_of(x).contains(mp)) {
                env_.cmd.close_tab(static_cast<std::size_t>(hover_tab));
            } else {
                tabs.activate(static_cast<std::size_t>(hover_tab));
                drag_ = hover_tab;
                drag_origin_ = mp;
            }
        }
    }
    if (strip.hovered && hover_tab >= 0 && ui.mouse_clicked(mouse_button::middle)) {
        env_.cmd.close_tab(static_cast<std::size_t>(hover_tab));
    }
    if (drag_ >= 0) {
        if (!ui.mouse_down(mouse_button::left)) {
            POINT cursor{};
            env_.window.hide_drag_ghost();
            env_.window.clear_window_hint();
            if (::GetCursorPos(&cursor) != FALSE && !env_.window.contains_screen_point(cursor.x, cursor.y)) {
                env_.cmd.move_tab_out(static_cast<std::size_t>(drag_), cursor.x, cursor.y);
            }
            drag_ = -1;
        } else {
            // outside the window the tab becomes a card on the pointer; a PrismFS window under it outlines itself
            POINT cursor{};
            if (::GetCursorPos(&cursor) != FALSE && !env_.window.contains_screen_point(cursor.x, cursor.y) && drag_ < static_cast<int>(tabs.size())) {
                const std::string path = tabs.at(static_cast<std::size_t>(drag_)).path();
                const bool other = env_.window.hint_window_at(cursor.x, cursor.y);
                const std::string leaf = core::path::leaf(path);
                env_.window.show_drag_ghost(leaf.empty() ? path : leaf,
                                            other ? tr("Release to move it to this window") : (tabs.size() > 1 ? tr("Release to open it in a new window") : std::string_view{}),
                                            cursor.x, cursor.y, env_.settings.accent, other || tabs.size() > 1);
                ui.invalidate();
            } else {
                env_.window.hide_drag_ghost();
                env_.window.clear_window_hint();
            }
            const int target = std::clamp(static_cast<int>(std::floor((mp.x - view_l + scroll_x_) / tw)), 0, static_cast<int>(tabs.size()) - 1);
            if (target != drag_ && drag_ < static_cast<int>(tabs.size())) {
                tabs.move(static_cast<std::size_t>(drag_), static_cast<std::size_t>(target));
                drag_ = target;
            }
        }
    }

    // --- tabs
    const bool lifted = drag_ >= 0 && ui.mouse_down(mouse_button::left) && std::abs(mp.x - drag_origin_.x) + std::abs(mp.y - drag_origin_.y) > 6.0f;
    p.dl().push_clip(rect{{view_l - 2.0f, area.min.y}, {view_r + 2.0f, area.max.y}});
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        model::Tab& tab    = tabs.at(i);
        const bool  active = i == tabs.active_index();
        const bool  hover  = static_cast<int>(i) == hover_tab;
        const std::string key = std::format("##tab{}", reinterpret_cast<std::uintptr_t>(&tab));
        const float x      = ui.animate(key + "x", view_l - scroll_x_ + static_cast<float>(i) * tw, 26.0f);
        const float hover_k = ui.animate(key + "h", hover ? 1.0f : 0.0f, 18.0f);
        auto& seen = born_[&tab];
        ++seen;
        const float appear = ui.animate(key + "b", seen > 1 ? 1.0f : 0.0f, 12.0f);
        p.dl().push_alpha(appear * (lifted && static_cast<int>(i) == drag_ ? 0.3f : 1.0f)); // (its slot stays, faded, while the card is lifted)
        const rect  body{{x, y0}, {x + tw - 2.0f, y1}};
        if (i == 0) { tab_rects_.clear(); }
        // (only the part inside the strip takes drops: a tab scrolled out of sight is not a target)
        const rect shown{{std::max(body.min.x, view_l), body.min.y}, {std::max(std::min(body.max.x, view_r), std::max(body.min.x, view_l)), body.max.y}};
        tab_rects_.push_back(shown);
        if (drop_ && shown.contains(*drop_)) { p.dl().rect_outline(body, pal.accent, m.pane_radius - 2.0f, 2.0f); }

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
        std::string title = tab.title();
        if (const auto view = env_.views.find(tab.path())) { // (an archive's unpacked copy: name the archive, not the temp folder)
            title = core::path::leaf(view->archive);
            if (!view->inner.empty()) { title = core::path::leaf(view->inner) + " - " + title; }
        }
        p.text_left(text_box, title, active ? pal.text : pal.text_dim, env_.fonts.body);

        if (close_visible) {
            const rect cr = close_rect_of(x);
            if (strip.hovered && cr.contains(mp)) { p.dl().rect_filled(cr, pal.pressed, 5.0f); }
            p.glyph_center(cr, glyph::close, active || hover ? pal.text_dim : pal.text_faint, env_.fonts.tile);
        }
        p.dl().pop_alpha();
    }
    p.dl().pop_clip();

    // the tab being dragged follows the pointer as a raised card (outside the window a native card takes over)
    if (lifted && drag_ < static_cast<int>(tabs.size()) && ui.display_size().x > mp.x && mp.x >= 0.0f && mp.y >= 0.0f && mp.y < ui.display_size().y) {
        auto fg = ui.layer(layer::foreground);
        model::Tab& tab = tabs.at(static_cast<std::size_t>(drag_));
        const float cw  = std::min(tw - 2.0f, 220.0f);
        const rect  card = rect::from_size({mp.x - cw * 0.5f, mp.y - m.tab_height * 0.5f}, {cw, m.tab_height});
        p.dl().rect_filled(rect::from_size({card.min.x - 2.0f, card.min.y + 1.0f}, {card.width() + 4.0f, card.height() + 4.0f}), color{0, 0, 0, 80}, m.pane_radius + 2.0f);
        p.dl().rect_filled(card, pal.raised, m.pane_radius - 2.0f);
        p.dl().rect_outline(card, pal.accent, m.pane_radius - 2.0f, 1.5f);
        p.folder_icon(rect::from_size({card.min.x + 10.0f, card.min.y + (m.tab_height - 16.0f) * 0.5f}, {16.0f, 16.0f}));
        p.text_left(rect{{card.min.x + 34.0f, card.min.y}, {card.max.x - 10.0f, card.max.y}}, tab.title(), pal.text, env_.fonts.body);
        env_.cmd.set_cursor(cursor_kind::hand);
        ui.invalidate();
    }

    if (hover_plus) { p.dl().rect_filled(plus_rect, pal.hover, 7.0f); }
    p.glyph_center(plus_rect, glyph::add, pal.text_dim, env_.fonts.tile);

    if (elevated) {
        const bool over = radmin.hovered && admin_.contains(mp);
        p.dl().rect_filled(admin_, pal.warning.scaled_alpha(over ? 0.30f : 0.18f), 7.0f);
        p.glyph_center(admin_, glyph::shield, pal.warning, env_.fonts.tile);
        if (over) {
            ui.tooltip(tr("Running as administrator. Click to reopen without administrator rights"));
            if (ui.mouse_clicked(mouse_button::left)) { env_.cmd.restart_unelevated(); }
        }
    } else {
        const bool over = radmin.hovered && admin_.contains(mp);
        if (over) { p.dl().rect_filled(admin_, pal.hover, 7.0f); }
        p.glyph_center(admin_, glyph::shield, over ? pal.text : pal.text_dim, env_.fonts.tile);
        if (over) {
            ui.tooltip(tr("Reopen as administrator"));
            if (ui.mouse_clicked(mouse_button::left)) { env_.cmd.restart_elevated(); }
        }
    }

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
