#include "ui/theme.hpp"

#include <algorithm>

namespace prism::ui {

namespace {

using strata::color;

[[nodiscard]] constexpr color rgb(std::uint32_t hex, std::uint8_t a = 255) noexcept
{
    return color{static_cast<std::uint8_t>(hex >> 16), static_cast<std::uint8_t>(hex >> 8), static_cast<std::uint8_t>(hex), a};
}

[[nodiscard]] Palette dark_palette()
{
    Palette p{};
    p.window             = rgb(0x0b0d11);
    p.title_bar          = rgb(0x0b0d11);
    p.sidebar            = rgb(0x11141a);
    p.content            = rgb(0x151921);
    p.raised             = rgb(0x1d222c);
    p.border             = rgb(0x262c38);
    p.divider            = rgb(0xffffff, 14);
    p.text               = rgb(0xe8ebf2);
    p.text_dim           = rgb(0x9099ab);
    p.text_faint         = rgb(0x5d6677);
    p.accent             = rgb(0x6d8cff);
    p.accent_hover       = rgb(0x8aa3ff);
    p.on_accent          = rgb(0x0b0d11);
    p.hover              = rgb(0xffffff, 12);
    p.pressed            = rgb(0xffffff, 20);
    p.selection          = rgb(0x6d8cff, 52);
    p.selection_inactive = rgb(0xffffff, 22);
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
    p.window             = rgb(0xe9ecf2);
    p.title_bar          = rgb(0xe9ecf2);
    p.sidebar            = rgb(0xf0f2f7);
    p.content            = rgb(0xffffff);
    p.raised             = rgb(0xf5f6fa);
    p.border             = rgb(0xd5d9e3);
    p.divider            = rgb(0x000000, 16);
    p.text               = rgb(0x1a1e27);
    p.text_dim           = rgb(0x5f687a);
    p.text_faint         = rgb(0x9aa2b3);
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

void Theme::set_mode(ThemeMode mode)
{
    mode_ = mode;
    pal_  = mode == ThemeMode::dark ? dark_palette() : light_palette();

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
