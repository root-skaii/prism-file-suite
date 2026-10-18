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

void Sidebar::section_title(Painter& p, std::string_view title)
{
    context& ui = p.ui();
    const item_result r = ui.custom_item(title, {ui.content_width(), 30.0f});
    p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 6.0f}, r.bounds.max}, title, p.pal().text_faint, env_.fonts.small, 16.0f);
}

bool Sidebar::place_row(Painter& p, const core::Place& place, char32_t g, color tint, bool drive)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    const FontSet& f   = env_.fonts;
    const float    h   = drive ? m.side_row + 18.0f : m.side_row;

    const item_result r = ui.custom_item(place.path, {ui.content_width(), h});
    const rect row{{r.bounds.min.x + 8.0f, r.bounds.min.y + 1.0f}, {r.bounds.max.x - 8.0f, r.bounds.max.y - 1.0f}};

    const std::string& current = env_.tabs.active().path();
    const bool active = drive ? (!current.empty() && current.size() >= 2 && std::toupper(static_cast<unsigned char>(current[0])) == std::toupper(static_cast<unsigned char>(place.path[0])) && current[1] == ':')
                              : core::path::normalize(current) == core::path::normalize(place.path);
    const float k = ui.animate(place.path + "#h", r.hovered ? 1.0f : 0.0f, 18.0f);
    if (active) {
        p.dl().rect_filled(row, pal.selection, 7.0f);
        p.dl().rect_filled(rect::from_size({row.min.x, row.min.y + row.height() * 0.5f - 8.0f}, {3.0f, 16.0f}), pal.accent, 1.5f);
    } else if (k > 0.01f) {
        p.dl().rect_filled(row, pal.hover.scaled_alpha(k), 7.0f);
    }

    const float icon = 20.0f;
    const rect  ibox = rect::from_size({row.min.x + 10.0f, row.min.y + (drive ? 6.0f : (row.height() - icon) * 0.5f)}, {icon, icon});
    p.glyph_center(ibox, g, tint);
    const float tx = row.min.x + 10.0f + icon + 10.0f;
    if (!drive) {
        p.text_left(rect{{tx, row.min.y}, row.max}, place.name, active ? pal.text : pal.text, f.body);
    } else {
        p.text_left(rect{{tx, row.min.y + 3.0f}, {row.max.x, row.min.y + 24.0f}}, place.name, pal.text, f.body);
        if (place.total > 0) {
            const float used = 1.0f - static_cast<float>(place.free) / static_cast<float>(place.total);
            const rect  bar{{tx, row.max.y - 17.0f}, {row.max.x - 10.0f, row.max.y - 12.0f}};
            p.dl().rect_filled(bar, pal.text_faint.scaled_alpha(0.35f), 2.5f);
            const color fill = used > 0.9f ? pal.danger : pal.accent;
            p.dl().rect_filled({bar.min, {bar.min.x + bar.width() * std::clamp(used, 0.0f, 1.0f), bar.max.y}}, fill, 2.5f);
            const std::string free = core::path::format_size(place.free) + " free of " + core::path::format_size(place.total);
            p.text_left(rect{{tx, row.max.y - 12.0f}, {row.max.x, row.max.y + 6.0f}}, free, pal.text_dim, f.small);
        }
    }

    if (r.hovered && ui.mouse_clicked(mouse_button::middle)) { env_.cmd.open_tab(place.path, false); }
    return r.hovered && ui.mouse_clicked(mouse_button::left);
}

void Sidebar::frame(Painter& p, const rect& area, const FrameInput& in)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();

    PanelScope panel{ui, "##sidebar", area};
    p.dl().rect_filled(area, pal.window);

    // (the drive list is cheap but not free: it asks every volume for its label)
    drives_age_ += in.dt;
    if (drives_age_ > 5.0) {
        drives_age_ = 0.0;
        refresh_drives();
    }

    auto scroll = ui.child("##side_scroll", {area.width(), area.height()}, child_flags::no_padding);
    (void)ui.custom_item("##side_top", {1.0f, 4.0f});

    section_title(p, "QUICK ACCESS");
    for (const core::Place& pl : quick_) {
        char32_t g = glyph::pin;
        color    c = p.pal().tint[static_cast<std::size_t>(core::FileKind::folder)];
        if (pl.name == "Home")      { g = glyph::home; c = pal.accent; }
        else if (pl.name == "Desktop")   { g = glyph::desktop; c = color{90, 160, 255, 255}; }
        else if (pl.name == "Downloads") { g = glyph::download; c = pal.success; }
        else if (pl.name == "Documents") { g = glyph::document; c = pal.tint[static_cast<std::size_t>(core::FileKind::document)]; }
        else if (pl.name == "Pictures")  { g = glyph::picture; c = pal.tint[static_cast<std::size_t>(core::FileKind::image)]; }
        else if (pl.name == "Music")     { g = glyph::music; c = pal.tint[static_cast<std::size_t>(core::FileKind::audio)]; }
        else if (pl.name == "Videos")    { g = glyph::video; c = pal.tint[static_cast<std::size_t>(core::FileKind::video)]; }
        if (place_row(p, pl, g, c, false)) { env_.tabs.active().navigate(pl.path); }
    }

    (void)ui.custom_item("##side_gap", {1.0f, 10.0f});
    section_title(p, "THIS PC");
    for (const core::Place& pl : drives_) {
        const bool usb = pl.removable;
        if (place_row(p, pl, usb ? glyph::usb : glyph::drive, pal.tint[static_cast<std::size_t>(core::FileKind::drive)], true)) {
            env_.tabs.active().navigate(pl.path);
        }
    }
}

} // namespace prism::ui
