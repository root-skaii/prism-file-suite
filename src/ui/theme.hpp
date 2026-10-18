#pragma once

// the two looks (dark is the default) and the sizes every panel shares. strata only knows its own `style`; the rest of
// the palette (file-type colours, selection, the title bar) is ours and panels read it from here.

#include "core/file_kind.hpp"
#include "strata/context.hpp"

#include <array>
#include <cstdint>

namespace prism::ui {

enum class ThemeMode : std::uint8_t { dark, light };

struct Palette {
    // surfaces, back to front
    strata::color window;       // behind everything, between panels
    strata::color title_bar;
    strata::color sidebar;
    strata::color content;      // the file list
    strata::color raised;       // inputs, popups, buttons
    strata::color border;
    strata::color divider;      // hairlines inside a panel

    strata::color text;
    strata::color text_dim;
    strata::color text_faint;

    strata::color accent;
    strata::color accent_hover;
    strata::color on_accent;

    strata::color hover;        // pointer over a row / button
    strata::color pressed;
    strata::color selection;    // selected row fill
    strata::color selection_inactive;
    strata::color selection_edge;

    strata::color close_hover;
    strata::color success, warning, danger;

    std::array<strata::color, static_cast<std::size_t>(core::FileKind::count_)> tint;
    strata::color folder_back;  // the darker rear flap of a folder icon
};

// logical pixels
struct Metrics {
    float title_bar     = 44.0f;
    float tab_height    = 34.0f;
    float tab_min_width = 120.0f;
    float tab_max_width = 220.0f;
    float toolbar       = 48.0f;
    float status_bar    = 28.0f;
    float sidebar_min   = 180.0f;
    float sidebar_width = 248.0f;
    float row_height    = 32.0f;
    float header_height = 32.0f;
    float side_row      = 32.0f;
    float icon          = 22.0f;
    float gap           = 8.0f;
    float pane_radius   = 10.0f;
    float caption_btn   = 46.0f; // width of minimise / maximise / close
};

// comfortable: touch-friendly rows. compact: ~30 % more rows on screen, tighter chrome
enum class Density : std::uint8_t { comfortable, compact };

[[nodiscard]] constexpr Metrics metrics_for(Density d) noexcept
{
    Metrics m{};
    if (d == Density::compact) {
        m.title_bar     = 38.0f;
        m.tab_height    = 30.0f;
        m.tab_min_width = 104.0f;
        m.tab_max_width = 200.0f;
        m.toolbar       = 40.0f;
        m.status_bar    = 24.0f;
        m.sidebar_width = 220.0f;
        m.row_height    = 24.0f;
        m.header_height = 26.0f;
        m.side_row      = 26.0f;
        m.icon          = 18.0f;
        m.gap           = 6.0f;
        m.pane_radius   = 8.0f;
        m.caption_btn   = 42.0f;
    }
    return m;
}

class Theme {
public:
    explicit Theme(ThemeMode mode = ThemeMode::dark, Density density = Density::comfortable)
        : density_{density}, metrics_{metrics_for(density)}
    {
        set_mode(mode);
    }

    void set_mode(ThemeMode mode);
    void toggle() { set_mode(mode_ == ThemeMode::dark ? ThemeMode::light : ThemeMode::dark); }
    void set_density(Density d) noexcept { density_ = d; metrics_ = metrics_for(d); }
    [[nodiscard]] Density density() const noexcept { return density_; }

    [[nodiscard]] ThemeMode      mode() const noexcept { return mode_; }
    [[nodiscard]] bool           dark() const noexcept { return mode_ == ThemeMode::dark; }
    [[nodiscard]] const Palette& pal() const noexcept { return pal_; }
    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const strata::style& style() const noexcept { return style_; }
    [[nodiscard]] strata::color tint(core::FileKind t) const noexcept { return pal_.tint[static_cast<std::size_t>(t)]; }

private:
    ThemeMode     mode_{ThemeMode::dark};
    Density       density_{Density::comfortable};
    Palette       pal_{};
    Metrics       metrics_{};
    strata::style style_{};
};

} // namespace prism::ui
