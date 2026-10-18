#include "ui/theme.hpp"

#include <algorithm>

namespace prism::ui {

namespace {

using strata::color;

[[nodiscard]] constexpr color rgb(std::uint32_t hex, std::uint8_t a = 255) noexcept
{
    return color{static_cast<std::uint8_t>(hex >> 16), static_cast<std::uint8_t>(hex >> 8), static_cast<std::uint8_t>(hex), a};
}

// the dark surfaces, back to front: each layer is ~5-8 % lighter than the one behind it so panels read as separate
// planes, and the border is a step above `raised` so edges are visible without being loud
struct DarkLayers {
    std::uint32_t window, sidebar, content, raised, border, text, text_dim, text_faint;
    std::uint8_t  hover, pressed; // white overlay alpha
};

// Windows 11 dark: pure neutral grey, the clearest separation (the default)
constexpr DarkLayers dark_neutral{0x1c1c1c, 0x242424, 0x2d2d2d, 0x383838, 0x4c4c4c, 0xf2f2f2, 0xb0b0b0, 0x80807f, 15, 26};
// neutral with a slightly cool tint, a little darker
constexpr DarkLayers dark_graphite{0x17181b, 0x1e1f23, 0x27282d, 0x32343a, 0x474a52, 0xeceef2, 0xa6aab5, 0x7a7e8a, 15, 26};
// Linear / VS Code feel: darkest, a hint of blue
constexpr DarkLayers dark_slate{0x0f1013, 0x16181d, 0x1f2229, 0x2b2f38, 0x3e4350, 0xe9ecf3, 0x9fa6b6, 0x727a8c, 14, 24};

[[nodiscard]] Palette dark_palette(const DarkLayers& l)
{
    Palette p{};
    p.window             = rgb(l.window);
    p.title_bar          = rgb(l.window);
    p.sidebar            = rgb(l.sidebar);
    p.content            = rgb(l.content);
    p.raised             = rgb(l.raised);
    p.border             = rgb(l.border);
    p.divider            = rgb(0xffffff, 18);
    p.text               = rgb(l.text);
    p.text_dim           = rgb(l.text_dim);
    p.text_faint         = rgb(l.text_faint);
    p.accent             = rgb(0x6d8cff);
    p.accent_hover       = rgb(0x8aa3ff);
    p.on_accent          = rgb(0x0b0d11);
    p.hover              = rgb(0xffffff, l.hover);
    p.pressed            = rgb(0xffffff, l.pressed);
    p.selection          = rgb(0x6d8cff, 52);
    p.selection_inactive = rgb(0xffffff, 26);
    p.selection_edge     = rgb(0x6d8cff, 120);
    p.close_hover        = rgb(0xe5484d);
    p.success            = rgb(0x4ade80);
    p.warning            = rgb(0xffb454);
    p.danger             = rgb(0xff5d6c);

    auto set = [&](core::FileKind t, std::uint32_t hex) { p.tint[static_cast<std::size_t>(t)] = rgb(hex); };
    set(core::FileKind::folder,       0xf6b93b);
    set(core::FileKind::image,        0xb78bff);
    set(core::FileKind::video,        0xff6b8e);
    set(core::FileKind::audio,        0x2dd4bf);
    set(core::FileKind::archive,      0xe39b4a);
    set(core::FileKind::document,     0x5b9dff);
    set(core::FileKind::spreadsheet,  0x3fc380);
    set(core::FileKind::presentation, 0xff8a4c);
    set(core::FileKind::pdf,          0xf25c5c);
    set(core::FileKind::code,         0x4fd1e8);
    set(core::FileKind::text,         0xa3adbf);
    set(core::FileKind::executable,   0x8d95ff);
    set(core::FileKind::library,      0x8590a3);
    set(core::FileKind::config,       0x7fb2e6);
    set(core::FileKind::font,         0xe49be4);
    set(core::FileKind::disk_image,   0xc9b36f);
    set(core::FileKind::shortcut,     0x6d8cff);
    set(core::FileKind::drive,        0x9aa6bd);
    set(core::FileKind::other,        0x8a93a5);
    p.folder_back = rgb(0xd49a22);
    return p;
}

[[nodiscard]] Palette light_palette()
{
    Palette p{};
    p.window             = rgb(0xdfe2e8);
    p.title_bar          = rgb(0xdfe2e8);
    p.sidebar            = rgb(0xe9ebf0);
    p.content            = rgb(0xffffff);
    p.raised             = rgb(0xf4f5f8);
    p.border             = rgb(0xc4c9d4);
    p.divider            = rgb(0x000000, 18);
    p.text               = rgb(0x1a1e27);
    p.text_dim           = rgb(0x566073);
    p.text_faint         = rgb(0x7d8596);
    p.accent             = rgb(0x3b63f0);
    p.accent_hover       = rgb(0x2a4fd6);
    p.on_accent          = rgb(0xffffff);
    p.hover              = rgb(0x000000, 12);
    p.pressed            = rgb(0x000000, 22);
    p.selection          = rgb(0x3b63f0, 36);
    p.selection_inactive = rgb(0x000000, 18);
    p.selection_edge     = rgb(0x3b63f0, 110);
    p.close_hover        = rgb(0xe5484d);
    p.success            = rgb(0x1f9d55);
    p.warning            = rgb(0xc77700);
    p.danger             = rgb(0xd6293b);

    auto set = [&](core::FileKind t, std::uint32_t hex) { p.tint[static_cast<std::size_t>(t)] = rgb(hex); };
    set(core::FileKind::folder,       0xf2a900);
    set(core::FileKind::image,        0x8f5cf0);
    set(core::FileKind::video,        0xe83e6c);
    set(core::FileKind::audio,        0x0fae9b);
    set(core::FileKind::archive,      0xc9791c);
    set(core::FileKind::document,     0x2f7bf0);
    set(core::FileKind::spreadsheet,  0x1f9d55);
    set(core::FileKind::presentation, 0xf0661d);
    set(core::FileKind::pdf,          0xdc3a3a);
    set(core::FileKind::code,         0x0ea5c4);
    set(core::FileKind::text,         0x6c7689);
    set(core::FileKind::executable,   0x5a63e6);
    set(core::FileKind::library,      0x6a7488);
    set(core::FileKind::config,       0x4a8fd6);
    set(core::FileKind::font,         0xc45ec4);
    set(core::FileKind::disk_image,   0xa08a3a);
    set(core::FileKind::shortcut,     0x3b63f0);
    set(core::FileKind::drive,        0x6f7b93);
    set(core::FileKind::other,        0x707a8d);
    p.folder_back = rgb(0xd68f00);
    return p;
}

} // namespace

void Theme::set_accent(color c)
{
    accent_ = color{c.r, c.g, c.b, 255};
    set_mode(mode_);
}

void Theme::set_mode(ThemeMode mode)
{
    mode_ = mode;
    const DarkLayers& layers = dark_style_ == DarkStyle::graphite ? dark_graphite : (dark_style_ == DarkStyle::slate ? dark_slate : dark_neutral);
    pal_  = mode == ThemeMode::dark ? dark_palette(layers) : light_palette();
    {   // the accent colours: one preset drives the highlight, selection and links
        const bool d = mode == ThemeMode::dark;
        pal_.accent       = accent_;
        pal_.accent_hover = d ? lerp(accent_, color{255, 255, 255, 255}, 0.22f) : lerp(accent_, color{0, 0, 0, 255}, 0.18f);
        // text on an accent fill: black or white, whichever the colour can carry
        const int lum = (accent_.r * 299 + accent_.g * 587 + accent_.b * 114) / 1000;
        pal_.on_accent = lum > 150 ? color{11, 13, 17, 255} : color{255, 255, 255, 255};
        pal_.selection      = pal_.accent.scaled_alpha(d ? 0.22f : 0.15f);
        pal_.selection_edge = pal_.accent.scaled_alpha(d ? 0.50f : 0.45f);
        pal_.tint[static_cast<std::size_t>(core::FileKind::shortcut)] = pal_.accent;
    }

    // strata's own widgets (popups, menus, text fields, scrollbars, tooltips) wear the same colours
    strata::style s{};
    s.padding        = 10.0f;
    s.item_spacing   = 6.0f;
    s.rounding       = 8.0f;
    s.border_width   = 1.0f;
    s.shadow_blur    = 20.0f;
    s.gradient       = 0.0f;
    s.frame_padding  = {10.0f, 6.0f};
    s.window_bg      = pal_.raised;
    s.title_bg       = pal_.raised;
    s.border         = pal_.border;
    s.widget_bg      = pal_.raised;
    s.widget_border  = pal_.border;
    s.accent         = pal_.accent;
    s.accent_hover   = pal_.accent_hover;
    s.text           = pal_.text;
    s.text_dim       = pal_.text_dim;
    s.shadow         = color{0, 0, 0, static_cast<std::uint8_t>(mode == ThemeMode::dark ? 150 : 60)};
    s.modal_dim      = color{0, 0, 0, static_cast<std::uint8_t>(mode == ThemeMode::dark ? 140 : 70)};
    s.success        = pal_.success;
    s.warning        = pal_.warning;
    s.error          = pal_.danger;
    s.text_contrast  = mode == ThemeMode::dark ? 0.15f : 0.0f; // light-on-dark text reads thin otherwise
    // hover / pressed fills sit on top of the raised surface: lighten (dark) or darken (light) it
    const auto shift = [&](int d) {
        const auto c = [&](std::uint8_t v) { return static_cast<std::uint8_t>(std::clamp(int{v} + d, 0, 255)); };
        return color{c(pal_.raised.r), c(pal_.raised.g), c(pal_.raised.b), 255};
    };
    s.widget_hover  = shift(mode == ThemeMode::dark ? 14 : -10);
    s.widget_active = shift(mode == ThemeMode::dark ? 26 : -20);
    style_ = s;
}

} // namespace prism::ui
