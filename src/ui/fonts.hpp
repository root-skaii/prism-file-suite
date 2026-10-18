#pragma once

// the font set: ids are what ui.push_font() and draw().text() take

#include "strata/context.hpp"

#include <expected>
#include <vector>

namespace prism::ui {

struct FontSet {
    strata::font_id body  = 0; // ui text
    strata::font_id icons = 1; // glyphs (segoe fluent icons, segoe mdl2 assets on windows 10)
    strata::font_id bold  = 2; // headings, selected tab
    strata::font_id small = 3; // captions, status bar
    strata::font_id tile  = 4; // icon glyphs inside the file-type tiles
    strata::font_id big   = 5; // icon glyphs inside large tiles (thumbnail view, preview)
    strata::font_id title = 6; // dialog headings, big numbers
    strata::font_id mono  = 7; // checksums, ids
};

// builds the context with the fonts above at `style`'s sizes
// `extra`: characters beyond the defaults (latin, greek, cyrillic, symbols) the names on screen use; sorted, unique
[[nodiscard]] std::expected<strata::context, strata::font_error> create_context(const strata::style& style, float scale, const std::vector<char32_t>& extra = {});

} // namespace prism::ui
