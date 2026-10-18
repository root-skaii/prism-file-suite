#include "core/bitmap.hpp"

#include <windows.h>

#include <algorithm>

namespace prism::core {

bool bitmap_to_bgra(void* hbitmap, int& w, int& h, std::vector<unsigned char>& out)
{
    const auto bmp = static_cast<HBITMAP>(hbitmap);
    BITMAP bm{};
    if (::GetObjectW(bmp, sizeof bm, &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) { return false; }
    w = bm.bmWidth;
    h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    HDC dc = ::GetDC(nullptr);
    const int rows = ::GetDIBits(dc, bmp, 0, static_cast<UINT>(h), out.data(), &bi, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    if (rows == 0) { return false; }

    bool any_alpha = false;
    for (std::size_t i = 3; i < out.size(); i += 4) {
        if (out[i] != 0) { any_alpha = true; break; }
    }
    for (std::size_t i = 0; i < out.size(); i += 4) {
        if (!any_alpha) {
            out[i + 3] = 255;
        } else if (const unsigned a = out[i + 3]; a != 0 && a != 255) {
            for (int c = 0; c < 3; ++c) { out[i + static_cast<std::size_t>(c)] = static_cast<unsigned char>(std::min(255u, out[i + static_cast<std::size_t>(c)] * 255u / a)); }
        }
    }
    return true;
}

} // namespace prism::core
