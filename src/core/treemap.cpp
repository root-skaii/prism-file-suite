#include "core/treemap.hpp"

#include <algorithm>
#include <cmath>

namespace prism::core {

namespace {

// the worst aspect ratio of a row of `areas` laid along a side of length `side`
double worst(const std::vector<double>& areas, std::size_t from, std::size_t to, double sum, double side)
{
    if (sum <= 0.0 || side <= 0.0) { return 1e30; }
    double lo = 1e300, hi = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        lo = std::min(lo, areas[i]);
        hi = std::max(hi, areas[i]);
    }
    const double s2 = side * side;
    return std::max(s2 * hi / (sum * sum), sum * sum / (s2 * lo));
}

} // namespace

std::vector<TreemapRect> squarify(const std::vector<double>& values, float x, float y, float w, float h)
{
    std::vector<TreemapRect> out(values.size());
    double total = 0.0;
    for (const double v : values) { total += std::max(v, 0.0); }
    if (total <= 0.0 || w <= 0.0f || h <= 0.0f) { return out; }

    // values scaled to areas
    std::vector<double> area(values.size());
    const double scale = static_cast<double>(w) * static_cast<double>(h) / total;
    for (std::size_t i = 0; i < values.size(); ++i) { area[i] = std::max(values[i], 0.0) * scale; }

    double rx = x, ry = y, rw = w, rh = h;
    std::size_t i = 0;
    while (i < values.size() && area[i] > 0.0) {
        const bool   wide = rw >= rh;          // lay the row along the shorter side
        const double side = wide ? rh : rw;
        std::size_t  j    = i + 1;
        double       sum  = area[i];
        double       cur  = worst(area, i, j, sum, side);
        while (j < values.size() && area[j] > 0.0) {
            const double next = worst(area, i, j + 1, sum + area[j], side);
            if (next > cur) { break; }
            sum += area[j];
            cur  = next;
            ++j;
        }
        // the row occupies a strip `thick` wide across the whole short side
        const double thick = sum / side;
        double pos = wide ? ry : rx;
        for (std::size_t k = i; k < j; ++k) {
            const double len = area[k] / thick;
            if (wide) {
                out[k] = {static_cast<float>(rx), static_cast<float>(pos), static_cast<float>(thick), static_cast<float>(len)};
            } else {
                out[k] = {static_cast<float>(pos), static_cast<float>(ry), static_cast<float>(len), static_cast<float>(thick)};
            }
            pos += len;
        }
        if (wide) { rx += thick; rw -= thick; } else { ry += thick; rh -= thick; }
        i = j;
    }
    return out;
}

} // namespace prism::core
