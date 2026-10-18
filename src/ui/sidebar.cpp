#include "ui/sidebar.hpp"

#include "core/path.hpp"
#include "ui/panel.hpp"

#include <algorithm>
#include <cmath>

namespace prism::ui {

using namespace strata;

Sidebar::Sidebar(Env& env) : env_{env}
{
    quick_ = core::quick_access();
    refresh_drives();
}

void Sidebar::refresh_drives()
{
    drives_ = core::drives();
}

std::string Sidebar::drop_target_at(vec2 p) const
{
    if (!area_.contains(p)) { return {}; }
    for (const auto& [r, path] : rows_) {
        if (r.contains(p)) { return path; }
    }
    return {};
}

void Sidebar::section_title(Painter& p, std::string_view title, float top_space)
{
    context& ui = p.ui();
    const float h = top_space + 26.0f;
    const item_result r = ui.custom_item(title, {ui.content_width(), h});
    p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + top_space}, r.bounds.max}, title, p.pal().text_faint, env_.fonts.small, 20.0f);
}

bool Sidebar::place_row(Painter& p, const core::Place& place, char32_t g, color tint, bool drive)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    const FontSet& f   = env_.fonts;
    const bool     compact = env_.theme.density() == Density::compact;
    const float    h   = drive ? (compact ? 54.0f : 62.0f) : m.side_row + 4.0f;

    const item_result r = ui.custom_item(place.path, {ui.content_width(), h});
    const rect row{{r.bounds.min.x + 10.0f, r.bounds.min.y + 2.0f}, {r.bounds.max.x - 10.0f, r.bounds.max.y - 2.0f}};

    rows_.emplace_back(row, place.path);
    if (drop_ && row.contains(*drop_)) { p.dl().rect_outline(row, pal.accent, 8.0f, 2.0f); }

    const std::string& current = env_.tab().path();
    const bool active = drive ? (current.size() >= 2 && std::toupper(static_cast<unsigned char>(current[0])) == std::toupper(static_cast<unsigned char>(place.path[0])) && current[1] == ':')
                              : core::path::normalize(current) == core::path::normalize(place.path);
    const float hover  = ui.animate(place.path + "#h", r.hovered ? 1.0f : 0.0f, 16.0f);
    const float act    = ui.animate(place.path + "#a", active ? 1.0f : 0.0f, 14.0f);
    const float press  = ui.animate(place.path + "#p", r.held ? 1.0f : 0.0f, 30.0f);

    if (hover > 0.01f) { p.dl().rect_filled(row, (press > 0.5f ? pal.pressed : pal.hover).scaled_alpha(hover), 8.0f); }
    if (act > 0.01f) {
        p.dl().rect_filled(row, pal.selection.scaled_alpha(act), 8.0f);
        const float bar_h = 6.0f + 12.0f * act;
        p.dl().rect_filled(rect::from_size({row.min.x, row.center().y - bar_h * 0.5f}, {3.0f, bar_h}), pal.accent.scaled_alpha(act), 1.5f);
    }

    const float tile = drive ? (compact ? 30.0f : 34.0f) : 24.0f;
    const float ty   = row.center().y - tile * 0.5f;
    const rect  ibox = rect::from_size({row.min.x + 12.0f, ty}, {tile, tile});
    if (drive) { p.dl().rect_filled(ibox, tint.scaled_alpha(0.16f), 9.0f); }
    p.glyph_center(ibox, g, tint);

    const float tx = ibox.max.x + 12.0f;
    const float right = row.max.x - 12.0f;
    if (!drive) {
        p.text_left(rect{{tx, row.min.y}, {right, row.max.y}}, place.name, pal.text, f.body);
    } else {
        // name, bar and free-space line, centred in the row (tighter in compact mode)
        const float block = compact ? 42.0f : 47.0f;
        const float y0    = row.center().y - block * 0.5f - 1.0f;
        const float bar_y = compact ? 22.0f : 25.0f;
        const float free_y = compact ? 27.0f : 31.0f;
        p.text_left(rect{{tx, y0}, {right, y0 + (compact ? 18.0f : 20.0f)}}, place.name, pal.text, f.body);
        if (place.total > 0) {
            const float used = std::clamp(1.0f - static_cast<float>(place.free) / static_cast<float>(place.total), 0.0f, 1.0f);
            const rect  bar{{tx, y0 + bar_y}, {right, y0 + bar_y + 4.0f}};
            p.dl().rect_filled(bar, pal.text_faint.scaled_alpha(0.30f), 2.0f);
            const float filled = ui.animate(place.path + "#u", used, 6.0f);
            p.dl().rect_filled({bar.min, {bar.min.x + bar.width() * filled, bar.max.y}}, used > 0.9f ? pal.danger : pal.accent, 2.0f);
            const std::string free = core::path::format_size(place.free) + " free of " + core::path::format_size(place.total);
            p.text_left(rect{{tx, y0 + free_y}, {right, y0 + free_y + 18.0f}}, free, pal.text_dim, f.small);
        }
    }

    if (r.hovered && ui.mouse_clicked(mouse_button::middle)) { env_.cmd.open_tab(place.path, false); }
    return r.hovered && ui.mouse_clicked(mouse_button::left);
}

void Sidebar::frame(Painter& p, const rect& area, const FrameInput& in)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();

    area_ = area;
    rows_.clear();
    PanelScope panel{ui, "##sidebar", area};
    p.dl().rect_filled(area, pal.window);

    // (the drive list is cheap but not free: it asks every volume for its label)
    drives_age_ += in.dt;
    if (drives_age_ > 5.0) {
        drives_age_ = 0.0;
        refresh_drives();
    }

    auto scroll = ui.child("##side_scroll", {area.width(), area.height()}, child_flags::no_padding);
    (void)ui.custom_item("##side_top", {1.0f, 2.0f});

    section_title(p, "QUICK ACCESS", 6.0f);
    for (const core::Place& pl : quick_) {
        char32_t g = glyph::pin;
        color    c = pal.tint[static_cast<std::size_t>(core::FileKind::folder)];
        if (pl.name == "Home")           { g = glyph::home;     c = pal.accent; }
        else if (pl.name == "Desktop")   { g = glyph::desktop;  c = color{90, 160, 255, 255}; }
        else if (pl.name == "Downloads") { g = glyph::download; c = pal.success; }
        else if (pl.name == "Documents") { g = glyph::document; c = pal.tint[static_cast<std::size_t>(core::FileKind::document)]; }
        else if (pl.name == "Pictures")  { g = glyph::picture;  c = pal.tint[static_cast<std::size_t>(core::FileKind::image)]; }
        else if (pl.name == "Music")     { g = glyph::music;    c = pal.tint[static_cast<std::size_t>(core::FileKind::audio)]; }
        else if (pl.name == "Videos")    { g = glyph::video;    c = pal.tint[static_cast<std::size_t>(core::FileKind::video)]; }
        if (place_row(p, pl, g, c, false)) { env_.tab().navigate(pl.path); }
    }

    section_title(p, "THIS PC", 14.0f);
    for (const core::Place& pl : drives_) {
        if (place_row(p, pl, pl.removable ? glyph::usb : glyph::drive, pal.tint[static_cast<std::size_t>(core::FileKind::drive)], true)) {
            env_.tab().navigate(pl.path);
        }
    }
    (void)ui.custom_item("##side_bottom", {1.0f, 12.0f});
}

} // namespace prism::ui
