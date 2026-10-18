#pragma once

// a gdi bitmap (shell icons, menu icons) as pixels

#include <vector>

namespace prism::core {

// HBITMAP -> straight-alpha BGRA, top-down. the shell hands out premultiplied alpha, or none at all (alpha 0 everywhere,
// which becomes opaque). false if it is not a readable bitmap
bool bitmap_to_bgra(void* hbitmap, int& w, int& h, std::vector<unsigned char>& out);

} // namespace prism::core
