#pragma once

// the colour of each label (bit i of FileEntry::tags, see core::tag::names)

#include "strata/context.hpp"

namespace prism::ui {

[[nodiscard]] inline strata::color tag_color(int i) noexcept
{
    using strata::color;
    static constexpr color colors[8] = {{235, 87, 87, 255},  {242, 153, 74, 255}, {242, 201, 76, 255}, {111, 207, 151, 255},
                                        {45, 212, 191, 255}, {86, 162, 255, 255}, {155, 125, 255, 255}, {242, 120, 190, 255}};
    return colors[i & 7];
}

} // namespace prism::ui
