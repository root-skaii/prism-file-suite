#pragma once

// squarified treemap layout (Bruls, Huizing, van Wijk): areas proportional to the values, tiles as square as can be.
// pure maths, no ui

#include <vector>

namespace prism::core {

struct TreemapRect {
    float x{}, y{}, w{}, h{};
};

// `values` must be sorted largest first; zero or negative values get an empty rect. the result has one rect per value, in
// the same order, all inside (x, y, w, h)
[[nodiscard]] std::vector<TreemapRect> squarify(const std::vector<double>& values, float x, float y, float w, float h);

} // namespace prism::core
