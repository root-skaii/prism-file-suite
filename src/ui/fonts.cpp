#include "ui/fonts.hpp"

#include <windows.h>

#include <array>
#include <string>

namespace prism::ui {

namespace {

[[nodiscard]] bool system_font_exists(const char* file_name)
{
    wchar_t dir[MAX_PATH]{};
    if (::GetWindowsDirectoryW(dir, MAX_PATH) == 0) { return false; }
    std::wstring path = std::wstring{dir} + L"\\Fonts\\";
    for (const char* c = file_name; *c != '\0'; ++c) { path.push_back(static_cast<wchar_t>(*c)); }
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::expected<strata::context, strata::font_error> create_context(const strata::style& style, float scale)
{
    using namespace strata;

    // windows 11 ships a variable "Segoe UI Variable"; windows 10 has Segoe UI
    const bool variable = system_font_exists("SegUIVar.ttf");
    const std::string_view text_face = variable ? "Segoe UI Variable Text" : "Segoe UI";
    const std::string_view icon_face = system_font_exists("SegoeIcons.ttf") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets";

    static constexpr std::array icon_ranges{glyph_ranges::private_use};
    static constexpr std::array text_ranges{glyph_ranges::latin, glyph_ranges::greek, glyph_ranges::cyrillic,
                                            glyph_ranges::punctuation, glyph_ranges::currency, glyph_ranges::arrows,
                                            glyph_ranges::math_operators, glyph_ranges::box_and_shapes};

    font_config body{};
    body.face         = text_face;
    body.pixel_height = 14.0f;
    body.ranges       = text_ranges;

    std::array<font_config, 4> extra{};
    extra[3].face         = icon_face; // id 4
    extra[3].pixel_height = 12.0f;
    extra[3].ranges       = icon_ranges;
    extra[0].face         = icon_face; // id 1
    extra[0].pixel_height = 16.0f;
    extra[0].ranges       = icon_ranges;
    extra[1].face         = text_face; // id 2
    extra[1].pixel_height = 14.0f;
    extra[1].bold         = true;
    extra[1].ranges       = text_ranges;
    extra[2].face         = text_face; // id 3
    extra[2].pixel_height = 12.0f;
    extra[2].ranges       = text_ranges;

    context_config cfg{};
    cfg.font         = body;
    cfg.extra_fonts  = extra;
    cfg.theme        = style;
    cfg.max_atlas_size = 4096;
    // a file list shows thousands of rows; the defaults (64 windows / tables) are far more than the app needs
    cfg.capacity.windows  = 24;
    cfg.capacity.tables   = 8;
    cfg.capacity.children = 24;
    cfg.capacity.cards    = 8;

    auto ui = context::create(cfg);
    if (ui && scale != 1.0f) { (void)ui->set_scale(scale); }
    return ui;
}

} // namespace prism::ui
