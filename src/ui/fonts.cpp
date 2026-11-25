#include "ui/fonts.hpp"

#include <windows.h>

#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace prism::ui {

namespace {

[[nodiscard]] bool system_font_exists(const char* file_name)
{
    wchar_t dir[MAX_PATH]{};
    if (::GetWindowsDirectoryW(dir, MAX_PATH) == 0) { return false; }
    std::wstring path = std::wstring{dir} + L"\\Fonts\\";
    for (const char* c = file_name; *c != '\0'; ++c) { path.push_back(static_cast<wchar_t>(c[0])); }
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::expected<strata::context, strata::font_error> create_context(const strata::style& style, float scale)
{
    using namespace strata;

    // faces that supply the glyphs the main face lacks, same size and baseline
    static constexpr std::array<std::string_view, 6> fallbacks{
        "Microsoft YaHei UI", "Yu Gothic UI", "Malgun Gothic", "Segoe UI Symbol", "Segoe UI Emoji", "Leelawadee UI",
    };

    // windows 11 ships a variable "Segoe UI Variable"; windows 10 has Segoe UI
    const bool             variable  = system_font_exists("SegUIVar.ttf");
    const std::string_view text_face = variable ? "Segoe UI Variable Text" : "Segoe UI";
    const std::string_view icon_face = system_font_exists("SegoeIcons.ttf") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets";

    // only the icons the app draws (painter.hpp's glyph::, the sidebar's icon picker). a new icon = add it here
    static constexpr std::array<std::uint32_t, 64> icon_codepoints{
        0xe700, 0xe706, 0xe70d, 0xe710, 0xe711, 0xe712, 0xe713, 0xe714, 0xe715, 0xe718, 0xe719, 0xe71b, 0xe71d, 0xe721, 0xe722,
        0xe72a, 0xe72b, 0xe72c, 0xe72e, 0xe734, 0xe73e, 0xe745, 0xe74a, 0xe74d, 0xe753, 0xe76c, 0xe774, 0xe787, 0xe7b3, 0xe7c1,
        0xe7f4, 0xe7fc, 0xe80a, 0xe80f, 0xe821, 0xe838, 0xe88e, 0xe896, 0xe8a1, 0xe8a5, 0xe8a7, 0xe8a9, 0xe8b7, 0xe8bb, 0xe8c8,
        0xe768, 0xe769, 0xe8d2, 0xe8d6, 0xe8ea, 0xe8ec, 0xe8f1, 0xe8fd, 0xe91b, 0xe921, 0xe922, 0xe923, 0xe943, 0xe946, 0xe958, 0xea18, 0xeb51, 0xeda2, 0xf012,
    };
    static const std::array<codepoint_range, icon_codepoints.size()> icon_ranges = [] {
        std::array<codepoint_range, icon_codepoints.size()> r{};
        for (std::size_t i = 0; i < r.size(); ++i) { r[i] = {icon_codepoints[i], icon_codepoints[i]}; }
        return r;
    }();
    // printable ascii only at start (fast, zero fallback misses); every other script (latin-1, cyrillic, cjk, arabic, emoji ...) bakes on demand
    static constexpr std::array<codepoint_range, 1> base_ranges{
        codepoint_range{0x0020, 0x007e},
    };

    font_config body{};
    body.face           = text_face;
    body.pixel_height   = 14.0f;
    body.ranges         = base_ranges;
    body.kerning        = false; // integer-pixel kerning pairs give uneven gaps at ui sizes
    body.fallback_faces = fallbacks;

    std::array<font_config, 7> extra_fonts{};
    extra_fonts[0].face           = icon_face; // id 1
    extra_fonts[0].pixel_height   = 16.0f;
    extra_fonts[0].ranges         = icon_ranges;
    extra_fonts[1].face           = text_face; // id 2 (bold)
    extra_fonts[1].pixel_height   = 14.0f;
    extra_fonts[1].bold           = true;
    extra_fonts[1].kerning        = false;
    extra_fonts[1].ranges         = base_ranges;
    extra_fonts[1].fallback_faces = fallbacks;
    extra_fonts[2].face           = text_face; // id 3 (small)
    extra_fonts[2].pixel_height   = 12.0f;
    extra_fonts[2].kerning        = false;
    extra_fonts[2].ranges         = base_ranges;
    extra_fonts[2].fallback_faces = fallbacks;
    extra_fonts[3].face           = icon_face; // id 4 (tile)
    extra_fonts[3].pixel_height   = 12.0f;
    extra_fonts[3].ranges         = icon_ranges;
    extra_fonts[4].face           = icon_face; // id 5 (big)
    extra_fonts[4].pixel_height   = 30.0f;
    extra_fonts[4].ranges         = icon_ranges;
    extra_fonts[5].face           = text_face; // id 6 (title)
    extra_fonts[5].pixel_height   = 20.0f;
    extra_fonts[5].bold           = true;
    extra_fonts[5].kerning        = false;
    extra_fonts[5].ranges         = base_ranges;
    extra_fonts[5].fallback_faces = fallbacks;
    extra_fonts[6].face           = "Consolas"; // id 7 (mono)
    extra_fonts[6].pixel_height   = 12.5f;
    extra_fonts[6].kerning        = false;
    extra_fonts[6].ranges         = base_ranges;

    context_config cfg{};
    cfg.font           = body;
    cfg.extra_fonts    = extra_fonts;
    cfg.theme          = style;
    cfg.max_atlas_size = 4096;
    cfg.dynamic_glyphs = true;
    // a file list shows thousands of rows; the defaults (64 windows / tables) are far more than the app needs
    cfg.capacity.windows  = 24;
    cfg.capacity.tables   = 8;
    cfg.capacity.children = 24;
    cfg.capacity.cards    = 8;
    cfg.scale             = std::clamp(scale, 0.5f, 4.0f);

    auto ui = context::create(cfg);
    core::log::info("font atlas: dynamic ({}x{})", ui ? ui->font().width() : 0u, ui ? ui->font().height() : 0u);
    return ui;
}

} // namespace prism::ui
