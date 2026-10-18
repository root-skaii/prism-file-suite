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
    const float hk = ui.animate(std::string{id} + "h", hot ? 1.0f : 0.0f, 18.0f);
    const float ok = ui.animate(std::string{id} + "o", on ? 1.0f : 0.0f, 14.0f);
    const rect  face = r.bounds.expanded(-1.0f);
    if (ok > 0.01f) { p.dl().rect_filled(face, pal.accent.scaled_alpha(0.18f * ok), 9.0f); }
    if (hk > 0.01f) { p.dl().rect_filled(face, (r.held ? pal.pressed : pal.hover).scaled_alpha(hk), 9.0f); }
    p.glyph_center(r.bounds, g, !enabled ? pal.text_faint : (on ? pal.accent : pal.text));
    if (r.hovered && !tip.empty()) { ui.tooltip(tip); }
    return hot && ui.mouse_released(mouse_button::left);
}

void Toolbar::field_background(Painter& p, const rect& r, bool invalid)
{
    shape_style s;
    s.radius       = radii(8.0f);
    s.fill_top     = s.fill_bottom = p.pal().raised;
    s.border       = invalid ? p.pal().danger : p.pal().border;
    s.border_width = 1.0f;
    p.dl().shape(r, s);
}

// --- filter / search box -----------------------------------------------------------------------------------------

namespace {

struct HelpRow {
    const char* example;
    const char* what;
};

constexpr HelpRow help_rows[] = {
    {"report 2026",       "all words must match"},
    {"\"annual report\"", "an exact phrase"},
    {"*.cpp",             "wildcards: * any text, ? one character"},
    {"report?.txt",       "a wildcard pattern matches the whole name"},
    {".png | .jpg",       "either one  (| or OR)"},
    {"-draft",            "leave out names containing draft"},
    {"ext:cpp,h",         "by extension  (-ext:obj to exclude)"},
    {"type:image",        "image video audio doc code archive text pdf exe font ..."},
    {"kind:folder",       "only folders  (kind:file: only files)"},
    {"size:>10mb",        "also <, >=, 1kb..1mb  (b kb mb gb)"},
    {"modified:today",    "yesterday week month year 3d 2026-10 >2026-01-01"},
    {"hidden:yes",        "hidden items (they are shown for this query)"},
    {"re:^IMG_\\d+",      "a regular expression on the name"},
    {"src\\*.h",          "a path: only in Subfolders results"},
};

} // namespace

core::QueryOptions Toolbar::query_options() const
{
    core::QueryOptions o;
    o.case_sensitive = env_.settings.search_case;
    o.regex          = env_.settings.search_regex;
    return o;
}

void Toolbar::apply_filter(model::Tab& tab, bool run)
{
    const core::Query q = core::Query::parse(filter_text_, query_options());
    if (!env_.settings.search_subfolders) {
        if (tab.searching()) { tab.clear_search(); } // back to the folder; the filter applies to its listing as it loads
        tab.listing().set_query(q);
        return;
    }
    // (an invalid query only carries its error to the status bar: it filters nothing)
    tab.listing().set_query(q.valid() ? core::Query{} : q);
    if (run) {
        if (q.empty() && q.valid()) { tab.clear_search(); } else { tab.search(q); }
    } else if (tab.searching() && q.empty() && q.valid()) {
        tab.clear_search();
    }
}

bool Toolbar::scope_control(Painter& p, float half, float height)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const bool     sub = env_.settings.search_subfolders;

    const item_result r = ui.custom_item("##scope", {half * 2.0f + 6.0f, height});
    const rect box = r.bounds;
    shape_style track;
    track.radius       = radii(8.0f);
    track.fill_top     = track.fill_bottom = pal.raised;
    track.border       = pal.border;
    track.border_width = 1.0f;
    p.dl().shape(box, track);

    const float t     = ui.animate("scope_thumb", sub ? 1.0f : 0.0f, 16.0f);
    const float thumb_x = box.min.x + 3.0f + t * half;
    const rect  thumb{{thumb_x, box.min.y + 3.0f}, {thumb_x + half, box.max.y - 3.0f}};
    p.dl().rect_filled(thumb, pal.accent.scaled_alpha(0.22f), 6.0f);
    p.dl().rect_outline(thumb, pal.accent.scaled_alpha(0.55f), 6.0f, 1.0f);

    const int over = r.hovered ? (ui.mouse_pos().x < box.min.x + 3.0f + half ? 0 : 1) : -1;
    const rect left{{box.min.x + 3.0f, box.min.y}, {box.min.x + 3.0f + half, box.max.y}};
    const rect right{{box.min.x + 3.0f + half, box.min.y}, {box.min.x + 3.0f + 2.0f * half, box.max.y}};
    const color on_col  = pal.text;
    const color off_col = pal.text_dim;
    p.glyph_center(left, glyph::folder, !sub ? on_col : (over == 0 ? on_col : off_col));
    // subfolders: a small tree (a parent line, two indented children joined by a stem)
    {
        const color c = sub ? on_col : (over == 1 ? on_col : off_col);
        const vec2  m = right.center();
        p.dl().rect_filled(rect{{m.x - 8.0f, m.y - 6.0f}, {m.x + 8.0f, m.y - 4.5f}}, c, 0.75f);
        p.dl().rect_filled(rect{{m.x - 4.0f, m.y - 6.0f}, {m.x - 2.5f, m.y + 5.0f}}, c, 0.75f);
        p.dl().rect_filled(rect{{m.x - 4.0f, m.y - 0.75f}, {m.x + 8.0f, m.y + 0.75f}}, c, 0.75f);
        p.dl().rect_filled(rect{{m.x - 4.0f, m.y + 4.0f}, {m.x + 8.0f, m.y + 5.5f}}, c, 0.75f);
    }
    if (over >= 0) {
        ui.tooltip(over == 0 ? "This folder: filter what is shown, as you type" : "Subfolders: search everything below this folder  (Enter)");
        env_.cmd.set_cursor(cursor_kind::hand);
    }
    if (over >= 0 && ui.mouse_released(mouse_button::left) && (over == 1) != sub) {
        env_.settings.search_subfolders = over == 1;
        return true;
    }
    return false;
}

bool Toolbar::option_chip(Painter& p, std::string_view id, std::string_view label, bool on, std::string_view tip, float w, float h)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const item_result r = ui.custom_item(id, {w, h});
    const float hk = ui.animate(std::string{id} + "h", r.hovered ? 1.0f : 0.0f, 18.0f);
    const rect  pill = rect{{r.bounds.min.x + 1.0f, r.bounds.min.y + 6.0f}, {r.bounds.max.x - 1.0f, r.bounds.max.y - 6.0f}};
    if (on) { p.dl().rect_filled(pill, pal.accent.scaled_alpha(0.24f), 6.0f); }
    if (hk > 0.01f) { p.dl().rect_filled(pill, (r.held ? pal.pressed : pal.hover).scaled_alpha(hk), 6.0f); }
    p.text_center(pill, label, on ? pal.accent : pal.text_dim, env_.fonts.bold);
    if (r.hovered && !tip.empty()) { ui.tooltip(tip); }
    return r.hovered && ui.mouse_released(mouse_button::left);
}

void Toolbar::help_popup(Painter& p, model::Tab& tab)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    auto pop = ui.popup("##search_help", 560.0f);
    if (!pop) { return; }

    const float row_h = 28.0f;
    const item_result title = ui.custom_item("##help_title", {540.0f, 26.0f});
    p.text_left(title.bounds, "Search and filter", pal.text, f.bold, 8.0f);
    for (std::size_t i = 0; i < std::size(help_rows); ++i) {
        const item_result r = ui.custom_item("##help" + std::to_string(i), {540.0f, row_h});
        if (r.hovered) { p.dl().rect_filled(r.bounds, pal.hover, 6.0f); }
        p.text_left(rect{r.bounds.min, {r.bounds.min.x + 190.0f, r.bounds.max.y}}, help_rows[i].example, pal.accent, f.body, 8.0f);
        p.text_left(rect{{r.bounds.min.x + 192.0f, r.bounds.min.y}, r.bounds.max}, help_rows[i].what, pal.text_dim, f.small, 4.0f);
        if (r.hovered && ui.mouse_released(mouse_button::left)) {
            filter_text_ = help_rows[i].example;
            apply_filter(tab, env_.settings.search_subfolders);
            ui.close_popup();
        }
    }
    const rect foot = ui.custom_item("##help_foot", {540.0f, 56.0f}).bounds;
    p.text_left(rect{foot.min, {foot.max.x, foot.min.y + 18.0f}}, "This folder filters the list as you type.", pal.text_dim, f.small, 8.0f);
    p.text_left(rect{{foot.min.x, foot.min.y + 18.0f}, {foot.max.x, foot.min.y + 36.0f}}, "Subfolders searches everything below it: press Enter  (Ctrl+Enter from This folder).", pal.text_dim, f.small, 8.0f);
    p.text_left(rect{{foot.min.x, foot.min.y + 36.0f}, {foot.max.x, foot.max.y}}, "Aa matches case. .* reads plain words as regular expressions. Esc clears.", pal.text_dim, f.small, 8.0f);
}

void Toolbar::address(Painter& p, float area_x, float area_y, float width, float height)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet& f   = env_.fonts;
    model::Tab&    tab = env_.tab();

    if (want_edit_) {
        want_edit_ = false;
        editing_   = true;
        edit_age_  = 0;
        edit_text_ = tab.path();
    }

    if (editing_) {
        // the field is a strata widget: sized to the row by its padding
        if (edit_age_ < 3) { ui.request_text_focus("##address"); } // (only at the start: asking every frame steals the focus back from whatever you click next)
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
        } else if (ui.key_pressed(key::escape) || (edit_age_ >= 3 && !ui.field_focused("##address"))) {
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
    (void)tabs;
    model::Tab&    tab  = env_.tab();

    // the filter follows the tab: another tab, or this one navigated
    if (last_tab_ != &tab || last_generation_ != tab.generation) {
        last_tab_        = &tab;
        last_generation_ = tab.generation;
        filter_text_     = tab.searching() ? tab.search_query() : tab.listing().query().text();
        editing_         = false;
    }

    PanelScope panel{ui, "##toolbar", area};
    p.dl().rect_filled(area, pal.title_bar);

    const float rh  = m.toolbar - 12.0f;
    const float gap = m.gap;
    const float chip_w   = 26.0f;
    const float half     = rh * 0.85f + 6.0f; // two icon buttons: This folder / Subfolders (tooltips say which)
    const float scope_w  = half * 2.0f + 6.0f;
    const float filter_w = std::clamp(area.width() * 0.13f, 110.0f, 190.0f) + chip_w * 3.0f; // (the chips sit inside the box)
    const float right_w  = rh * 6.0f + gap * 4.0f + 6.0f + scope_w + filter_w;
    const float left_w   = 12.0f + rh * 4.0f + 6.0f + gap * 2.0f;
    const float addr_w   = std::max(120.0f, area.width() - left_w - right_w - 10.0f);

    (void)ui.custom_item("##tb_vpad", {1.0f, 6.0f});
    (void)ui.custom_item("##tb_lpad", {12.0f, rh});
    ui.same_line();
    const auto sp = [&](const char* id) { ui.same_line(); (void)ui.custom_item(id, {2.0f, rh}); ui.same_line(); };
    if (icon_button(p, "##nav_back", glyph::back, tab.can_back(), false, "Back  (Alt+Left)", rh)) { (void)tab.back(); }
    sp("##sp1");
    if (icon_button(p, "##nav_fwd", glyph::forward, tab.can_forward(), false, "Forward  (Alt+Right)", rh)) { (void)tab.forward(); }
    sp("##sp2");
    if (icon_button(p, "##nav_up", glyph::up, tab.can_up(), false, "Up  (Alt+Up)", rh)) { (void)tab.up(); }
    sp("##sp3");
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
    Settings& st = env_.settings;
    if (scope_control(p, half, rh)) {
        env_.cmd.settings_changed();
        apply_filter(tab, !filter_text_.empty()); // (to Subfolders: search at once; back to This folder: filter live)
    }
    ui.same_line();
    (void)ui.custom_item("##tb_gap2b", {gap, rh});
    ui.same_line();

    const float filter_x = area.min.x + left_w + addr_w + gap + scope_w + gap;
    const bool  invalid  = !tab.listing().query().valid();
    field_background(p, rect::from_size({filter_x, area.min.y + 6.0f}, {filter_w, rh}), invalid);
    ui.set_next_item_width(filter_w - chip_w * 3.0f);
    ui.push_var(style_var::frame_padding_y, (rh - p.line_height(f.body)) * 0.5f);
    ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
    if (want_filter_focus_) {
        want_filter_focus_ = false;
        ui.request_text_focus("##filter");
    }
    const bool typed_in_filter = ui.input_text("##filter", filter_text_, st.search_subfolders ? "Search subfolders  (Enter)" : "Filter this folder");
    const bool filter_focus = ui.field_focused("##filter");
    ui.pop_color();
    ui.pop_var();
    if (typed_in_filter) { apply_filter(tab, false); }
    if (filter_focus && ui.key_pressed(key::enter, true)) {
        if (!st.search_subfolders) {
            st.search_subfolders = true;
            env_.cmd.settings_changed();
        }
        apply_filter(tab, true);
    } else if (filter_focus && ui.key_pressed(key::enter) && st.search_subfolders) {
        apply_filter(tab, true);
    }
    if (filter_focus && ui.key_pressed(key::escape)) {
        filter_text_.clear();
        apply_filter(tab, false);
    }
    ui.same_line();
    bool options_changed = false;
    if (option_chip(p, "##opt_case", "Aa", st.search_case, "Match case", chip_w, rh)) { st.search_case = !st.search_case; options_changed = true; }
    ui.same_line();
    if (option_chip(p, "##opt_regex", ".*", st.search_regex, "Regular expressions", chip_w, rh)) { st.search_regex = !st.search_regex; options_changed = true; }
    ui.same_line();
    if (option_chip(p, "##opt_help", "?", ui.popup_open("##search_help"), "Search syntax", chip_w, rh)) { ui.toggle_popup("##search_help"); }
    if (options_changed) {
        env_.cmd.settings_changed();
        apply_filter(tab, st.search_subfolders && tab.searching());
    }
    help_popup(p, tab);
    ui.same_line();
    (void)ui.custom_item("##tb_gap3", {gap, rh});
    ui.same_line();
    if (icon_button(p, "##tb_hidden", glyph::eye, true, show_hidden, "Show hidden files", rh)) { env_.cmd.toggle_hidden(); }
    ui.same_line();
    if (icon_button(p, "##tb_density", glyph::list, true, env_.theme.density() == Density::compact, "Compact layout", rh)) { env_.cmd.toggle_density(); }
    ui.same_line();
    if (icon_button(p, "##tb_view", glyph::grid, true, env_.settings.grid_view, "Thumbnails", rh)) { env_.cmd.toggle_view(); }
    ui.same_line();
    if (icon_button(p, "##tb_split", glyph::split, true, env_.settings.split, "Two panes  (F3)", rh)) { env_.cmd.toggle_split(); }
    ui.same_line();
    if (icon_button(p, "##tb_preview", glyph::preview, true, env_.settings.preview, "Preview pane  (Alt+P)", rh)) { env_.cmd.toggle_preview(); }
    ui.same_line();
    if (icon_button(p, "##tb_settings", glyph::settings, true, false, "Settings  (Ctrl+,)", rh)) { env_.cmd.open_settings(); }
}

} // namespace prism::ui
