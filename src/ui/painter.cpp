#include "ui/painter.hpp"

#include "strata/icons.hpp"

#include <cmath>

namespace prism::ui {

using namespace strata;
using core::FileKind;

std::string_view glyph_utf8(char32_t cp, char (&buf)[5]) noexcept
{
    std::size_t n = 0;
    if (cp < 0x80) {
        buf[n++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
        buf[n++] = static_cast<char>(0xc0 | (cp >> 6));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        buf[n++] = static_cast<char>(0xe0 | (cp >> 12));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        buf[n++] = static_cast<char>(0xf0 | (cp >> 18));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3f));
    }
    buf[n] = '\0';
    return {buf, n};
}

float Painter::text_left(const rect& r, std::string_view s, color c, font_id f, float pad) const
{
    const float y = std::round(r.min.y + (r.height() - line_height(f)) * 0.5f);
    return ui_.label_clipped({r.min.x + pad, y}, std::max(0.0f, r.width() - pad), c, s, f);
}

float Painter::text_right(const rect& r, std::string_view s, color c, font_id f, float pad) const
{
    const vec2  sz = measure(f, s);
    const float w  = std::min(sz.x, std::max(0.0f, r.width() - pad));
    const float y  = std::round(r.min.y + (r.height() - line_height(f)) * 0.5f);
    return ui_.label_clipped({r.max.x - pad - w, y}, w, c, s, f);
}

void Painter::text_center(const rect& r, std::string_view s, color c, font_id f) const
{
    const vec2 sz = measure(f, s);
    const float w = std::min(sz.x, r.width());
    (void)ui_.label_clipped({std::round(r.center().x - w * 0.5f), std::round(r.min.y + (r.height() - line_height(f)) * 0.5f)}, w, c, s, f);
}

void Painter::glyph_center(const rect& r, char32_t g, color c) const
{
    glyph_center(r, g, c, fonts_.icons);
}

void Painter::glyph_center(const rect& r, char32_t g, color c, font_id f) const
{
    char buf[5];
    const std::string_view s = glyph_utf8(g, buf);
    const vec2 sz = measure(f, s);
    dl().text({std::round(r.center().x - sz.x * 0.5f), std::round(r.center().y - line_height(f) * 0.5f)}, c, s, f);
}

void Painter::folder_icon(const rect& box) const
{
    const float s  = std::min(box.width(), box.height());
    const vec2  o  = box.center() - vec2{s * 0.5f, s * 0.5f};
    const auto  at = [&](float x, float y) { return o + vec2{x * s, y * s}; };
    const color front = pal().tint[static_cast<std::size_t>(FileKind::folder)];
    const color back  = pal().folder_back;
    const float r     = std::max(1.5f, s * 0.1f);

    // rear flap with its tab, then the front face with a lighter top edge
    dl().rect_filled({at(0.06f, 0.16f), at(0.44f, 0.40f)}, back, r, corners::top);
    dl().rect_filled({at(0.06f, 0.26f), at(0.94f, 0.84f)}, back, r);
    shape_style face;
    face.radius      = radii(r);
    face.fill_top    = lerp(front, color{255, 255, 255, 255}, 0.28f);
    face.fill_bottom = front;
    dl().shape({at(0.06f, 0.38f), at(0.94f, 0.86f)}, face);
}

void Painter::file_icon(const rect& box, FileKind kind) const
{
    const float s = std::min(box.width(), box.height());
    const rect  tile{box.center() - vec2{s * 0.5f, s * 0.5f} + vec2{s * 0.08f, s * 0.08f}, box.center() + vec2{s * 0.5f, s * 0.5f} - vec2{s * 0.08f, s * 0.08f}};
    const color tint = pal().tint[static_cast<std::size_t>(kind)];

    char32_t g = glyph::document;
    switch (kind) {
    case FileKind::image:        g = glyph::picture; break;
    case FileKind::video:        g = glyph::video; break;
    case FileKind::audio:        g = glyph::music; break;
    case FileKind::archive:      g = glyph::zip; break;
    case FileKind::spreadsheet:  g = glyph::grid; break;
    case FileKind::presentation: g = glyph::desktop; break;
    case FileKind::code:         g = glyph::code; break;
    case FileKind::executable:   g = glyph::app; break;
    case FileKind::library:      g = glyph::gear; break;
    case FileKind::config:       g = glyph::settings; break;
    case FileKind::font:         g = glyph::font; break;
    case FileKind::disk_image:   g = glyph::disc; break;
    case FileKind::shortcut:     g = glyph::link; break;
    case FileKind::drive:        g = glyph::drive; break;
    default: break;
    }
    dl().rect_filled(tile, tint.scaled_alpha(theme_.dark() ? 0.20f : 0.16f), s * 0.24f);
    glyph_center(tile, g, tint, fonts_.tile);
}

void Painter::kind_icon(const rect& box, FileKind kind) const
{
    if (kind == FileKind::folder) {
        folder_icon(box);
    } else {
        file_icon(box, kind);
    }
}

void Painter::sort_arrow(vec2 c, bool ascending, color col) const
{
    const float w = 3.5f;
    const float h = 2.5f;
    if (ascending) {
        dl().triangle_filled({c.x - w, c.y + h}, {c.x + w, c.y + h}, {c.x, c.y - h}, col);
    } else {
        dl().triangle_filled({c.x - w, c.y - h}, {c.x + w, c.y - h}, {c.x, c.y + h}, col);
    }
}

} // namespace prism::ui
