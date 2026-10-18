#pragma once

// the font set: ids are what ui.push_font() and draw().text() take

#include "strata/context.hpp"

#include <expected>

namespace prism::ui {

struct FontSet {
    strata::font_id body  = 0; // ui text
    strata::font_id icons = 1; // glyphs (segoe fluent icons, segoe mdl2 assets on windows 10)
    strata::font_id bold  = 2; // headings, selected tab
    strata::font_id small = 3; // captions, status bar
    strata::font_id tile  = 4; // icon glyphs inside the file-type tiles
};

// builds the context with the fonts above at `style`'s sizes
[[nodiscard]] std::expected<strata::context, strata::font_error> create_context(const strata::style& style, float scale);

} // namespace prism::ui
