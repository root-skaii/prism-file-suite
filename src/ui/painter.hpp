#pragma once

// drawing helpers over strata's draw list: aligned text, icon glyphs, the coloured file / folder icons. one per frame.

#include "core/file_kind.hpp"
#include "platform/image_cache.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

#include <string_view>

namespace prism::ui {

// segoe mdl2 / fluent code points used by the app
namespace glyph {
inline constexpr char32_t back       = 0xe72b;
inline constexpr char32_t forward    = 0xe72a;
inline constexpr char32_t up         = 0xe74a;
inline constexpr char32_t refresh    = 0xe72c;
inline constexpr char32_t search     = 0xe721;
inline constexpr char32_t add        = 0xe710;
inline constexpr char32_t close      = 0xe711;
inline constexpr char32_t minimize   = 0xe921;
inline constexpr char32_t maximize   = 0xe922;
inline constexpr char32_t restore    = 0xe923;
inline constexpr char32_t window_x   = 0xe8bb;
inline constexpr char32_t chevron_r  = 0xe76c;
inline constexpr char32_t chevron_d  = 0xe70d;
inline constexpr char32_t more       = 0xe712;
inline constexpr char32_t settings   = 0xe713;
inline constexpr char32_t sun        = 0xe706;
inline constexpr char32_t home       = 0xe80f;
inline constexpr char32_t desktop    = 0xe7f4;
inline constexpr char32_t download   = 0xe896;
inline constexpr char32_t document   = 0xe8a5;
inline constexpr char32_t picture    = 0xe91b;
inline constexpr char32_t music      = 0xe8d6;
inline constexpr char32_t video      = 0xe714;
inline constexpr char32_t drive      = 0xeda2;
inline constexpr char32_t usb        = 0xe88e;
inline constexpr char32_t code       = 0xe943;
inline constexpr char32_t zip        = 0xf012;
inline constexpr char32_t font       = 0xe8d2;
inline constexpr char32_t gear       = 0xe713;
inline constexpr char32_t app        = 0xe71d;
inline constexpr char32_t link       = 0xe71b;
inline constexpr char32_t grid       = 0xe80a;
inline constexpr char32_t list       = 0xe8fd;
inline constexpr char32_t eye        = 0xe7b3;
inline constexpr char32_t pin        = 0xe718;
inline constexpr char32_t disc       = 0xe958;
inline constexpr char32_t check      = 0xe73e;
inline constexpr char32_t folder_open = 0xe838;
inline constexpr char32_t folder     = 0xe8b7;
inline constexpr char32_t copy       = 0xe8c8;
inline constexpr char32_t info       = 0xe946;
inline constexpr char32_t open_new   = 0xe8a7;
inline constexpr char32_t split      = 0xe745;   // two columns
inline constexpr char32_t preview    = 0xe8a1;   // side panel
inline constexpr char32_t lock       = 0xe72e;
inline constexpr char32_t delete_    = 0xe74d;
inline constexpr char32_t view       = 0xe8a9;   // the view options button
inline constexpr char32_t rows       = 0xe700;   // compact rows
} // namespace glyph

class Painter {
public:
    Painter(strata::context& ui, const Theme& theme, const FontSet& fonts) : ui_{ui}, theme_{theme}, fonts_{fonts} {}

    [[nodiscard]] strata::draw_list& dl() const noexcept { return ui_.draw(); }
    [[nodiscard]] const Palette&     pal() const noexcept { return theme_.pal(); }
    [[nodiscard]] const Metrics&     metrics() const noexcept { return theme_.metrics(); }
    [[nodiscard]] const FontSet&     fonts() const noexcept { return fonts_; }
    [[nodiscard]] strata::context&   ui() const noexcept { return ui_; }

    [[nodiscard]] strata::vec2 measure(strata::font_id f, std::string_view s) const noexcept { return ui_.font().measure(f, s); }
    [[nodiscard]] float        line_height(strata::font_id f) const noexcept { return ui_.font().line_height(f); }

    // text vertically centred in `r`, left aligned at r.min.x + pad, cut with ".." past the right edge. returns width drawn
    float text_left(const strata::rect& r, std::string_view s, strata::color c, strata::font_id f, float pad = 0.0f) const;
    float text_right(const strata::rect& r, std::string_view s, strata::color c, strata::font_id f, float pad = 0.0f) const;
    void  text_center(const strata::rect& r, std::string_view s, strata::color c, strata::font_id f) const;

    void glyph_center(const strata::rect& r, char32_t g, strata::color c) const;
    void glyph_center(const strata::rect& r, char32_t g, strata::color c, strata::font_id f) const;

    // coloured icons, `box` is a square
    void folder_icon(const strata::rect& box) const;
    void file_icon(const strata::rect& box, core::FileKind kind) const;
    void kind_icon(const strata::rect& box, core::FileKind kind) const; // folder or file tile

    // a decoded image scaled to fit `box`, centred, aspect kept
    void image_fit(const strata::rect& box, const platform::Image& img, float rounding = 6.0f) const;

    // a small triangle pointing up / down (sort direction)
    void sort_arrow(strata::vec2 center, bool ascending, strata::color c) const;

private:
    strata::context& ui_;
    const Theme&     theme_;
    FontSet          fonts_;
};

[[nodiscard]] std::string_view glyph_utf8(char32_t cp, char (&buf)[5]) noexcept;

} // namespace prism::ui
