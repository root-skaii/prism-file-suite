#pragma once

// the widget kit the dialogs are built from: buttons, pill tabs, switches, chips and badges, cards, key / value rows,
// stat tiles, bars. every widget is a strata custom_item drawn with the Painter: animated hover and press fades, a
// keyboard focus ring (Tab / Shift+Tab move, Enter / Space activate), a disabled state, and the pointer turns to a hand.
//
// layout is plain strata flow with item spacing 0 (push_var(item_spacing, 0) around a dialog body): rows have fixed
// heights, so a card knows how tall it is before its content is submitted and can draw its own background first.

#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prism::ui {

enum class Tone : std::uint8_t { neutral, accent, success, warning, danger };

// the look every dialog wears: a panel-coloured background (cards sit lighter on it), roomy padding and no item spacing
// (rows have exact heights). pushed before ui.modal(), popped when the dialog's frame function returns
class DialogStyle {
public:
    DialogStyle(strata::context& ui, const Palette& pal) : ui_{ui}
    {
        ui_.push_color(strata::style_color::window_bg, pal.sidebar);
        ui_.push_var(strata::style_var::padding, 18.0f);
        ui_.push_var(strata::style_var::item_spacing, 0.0f);
    }
    ~DialogStyle()
    {
        ui_.pop_var(2);
        ui_.pop_color();
    }
    DialogStyle(const DialogStyle&)            = delete;
    DialogStyle& operator=(const DialogStyle&) = delete;

private:
    strata::context& ui_;
};

class Kit {
public:
    Kit(Painter& p, Commands& cmd) noexcept : p_{p}, cmd_{cmd} {}

    // once per frame before the first widget: keyboard focus bookkeeping. `reset_focus()` when a dialog closes
    void begin_frame();
    static void reset_focus() noexcept;

    // metrics (comfortable / compact)
    [[nodiscard]] bool  dense() const noexcept { return p_.metrics().row_height < 28.0f; }
    [[nodiscard]] float row_h() const noexcept { return dense() ? 28.0f : 34.0f; }
    [[nodiscard]] float control_h() const noexcept { return dense() ? 28.0f : 34.0f; }
    [[nodiscard]] static constexpr float pad() noexcept { return 16.0f; }

    // spacing ----------------------------------------------------------------
    void vgap(float h);   // vertical space
    void hgap(float w);   // between two widgets on one line (call between them; it handles same_line)

    // buttons ----------------------------------------------------------------
    enum class ButtonKind : std::uint8_t { primary, secondary, ghost };
    struct ButtonSpec {
        std::string_view id;
        std::string_view label;
        char32_t         icon{};
        ButtonKind       kind{ButtonKind::secondary};
        bool             enabled{true};
        std::string_view tip;
        float            width{}; // 0: fits the label
    };
    [[nodiscard]] bool button(const ButtonSpec& s);
    [[nodiscard]] bool icon_button(std::string_view id, char32_t glyph, std::string_view tip, bool enabled = true, bool danger = false, float size = 0.0f);

    // pill tabs / segmented control: true when the selection changed
    // `line_h` > 0 centres the control vertically in an item of that height (to sit on a taller row)
    [[nodiscard]] bool pill_tabs(std::string_view id, std::span<const std::string_view> labels, int& selected, float width = 0.0f, float line_h = 0.0f);
    // a round colour swatch (accent presets); a ring marks the selected one
    [[nodiscard]] bool swatch(std::string_view id, strata::color c, bool selected, float line_h);
    [[nodiscard]] bool toggle_switch(std::string_view id, bool& on, bool enabled = true);

    // chips and badges -------------------------------------------------------
    // a chip is a small rounded label with an optional icon; interactive ones react to hover and report clicks
    // `line_h` > 0 centres the chip vertically in an item of that height (to sit on a taller row)
    [[nodiscard]] bool chip(std::string_view id, std::string_view label, char32_t icon = 0, Tone tone = Tone::neutral, bool interactive = false, std::string_view tip = {}, float line_h = 0.0f);
    void badge(std::string_view id, std::string_view label, Tone tone, char32_t icon = 0, float line_h = 0.0f);
    [[nodiscard]] float chip_width(std::string_view label, bool icon) const noexcept;
    [[nodiscard]] float button_width(std::string_view label, bool icon, ButtonKind kind = ButtonKind::secondary) const noexcept;

    // cards ------------------------------------------------------------------
    // a rounded section with a subtle border. `body_h` is the summed height of the rows that follow (the card draws its
    // background up front); end it with card_end()
    void card_begin(std::string_view id, std::string_view title, float body_h);
    void card_end(std::string_view id);
    // the summed height of the cards finished so far this frame (a dialog sizes its scrolling body to it)
    [[nodiscard]] float cards_height() const noexcept { return cards_h_; }
    // a hint wrapped to `width` (small font), one entry per line, views into `hint`
    [[nodiscard]] std::vector<std::string_view> hint_lines(std::string_view hint, float width) const;
    // height a hint of `lines` lines needs beyond the one line a row has; the open card grows by it (from the next frame:
    // the card box is drawn before its rows, so the amount is remembered per card)
    [[nodiscard]] float hint_extra(std::size_t lines) const;
    void add_extra(float h) noexcept { cur_extra_ += h; }

    struct KeyValue {
        std::string_view id;
        std::string_view key;
        std::string_view value;
        bool  mono{};
        bool  copyable{true};
        bool  separator{true};
        float key_w{140.0f};
    };
    // a fixed-height row: dim key, value cut with ".." (the tooltip has all of it), a click copies the value
    void key_value(const KeyValue& kv);
    // a row with a label (and an optional hint line) and a switch; the whole row toggles. true when changed
    [[nodiscard]] bool switch_row(std::string_view id, std::string_view label, std::string_view hint, bool& on, bool separator = true);
    // big numbers in rounded tiles side by side
    struct Stat {
        std::string_view label;
        std::string value;
        std::string_view sub;
    };
    void stat_tiles(std::string_view id, std::span<const Stat> stats);
    [[nodiscard]] float stat_tiles_height() const noexcept; // what stat_tiles() takes: size a card for it with this
    // a bar filled to `fraction` (0..1, animated) over a track; `tone` colours the fill
    void bar(std::string_view id, float fraction, Tone tone, float height = 8.0f, float inset = 0.0f);

    [[nodiscard]] strata::color tone_color(Tone t) const noexcept;
    [[nodiscard]] Painter& painter() const noexcept { return p_; }

private:
    struct Hit {
        strata::item_result r;
        bool hot{};
        bool clicked{};
        bool focused{};
    };
    [[nodiscard]] Hit hit(std::string_view id, strata::vec2 size, bool enabled);
    void focus_ring(const strata::rect& r, float radius) const;
    [[nodiscard]] std::string uid(const char* prefix);

    Painter&  p_;
    Commands& cmd_;
    int       counter_{};
    float     card_h_{};   // the open card's full flow height
    float     cards_h_{};
    float     cur_extra_{};      // wrapped hint lines added inside the open card this frame
    std::string cur_card_;
};

} // namespace prism::ui
