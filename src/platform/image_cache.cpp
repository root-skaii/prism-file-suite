#include "platform/image_cache.hpp"

#include "core/text.hpp"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wrl/client.h>

#include <algorithm>

namespace prism::platform {

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::size_t k_max_entries = 700;
constexpr int         k_uploads_per_poll = 12;

// HBITMAP -> straight-alpha BGRA, top-down
bool read_bitmap(HBITMAP bmp, int& w, int& h, std::vector<unsigned char>& out)
{
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

    // the shell hands out premultiplied alpha (or none at all: alpha 0 everywhere)
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

} // namespace

ImageCache::ImageCache(std::function<void()> wake) : wake_{std::move(wake)}, thread_{[this] { worker(); }} {}

ImageCache::~ImageCache()
{
    {
        std::lock_guard g{lock_};
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) { thread_.join(); }
}

Image ImageCache::get(const std::string& path, int px, ImageKind kind)
{
    std::string key = path;
    key.push_back('|');
    key += std::to_string(px);
    key.push_back(kind == ImageKind::icon ? 'i' : 't');

    auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.used = tick_;
        return it->second.image;
    }
    entries_[key] = Entry{{}, true, tick_};
    {
        std::lock_guard g{lock_};
        queue_.push_front({key, path, px, kind});
        if (queue_.size() > 400) { queue_.pop_back(); }
    }
    cv_.notify_one();
    return {};
}

bool ImageCache::poll(strata::d3d11_renderer& renderer)
{
    ++tick_;
    std::vector<Result> batch;
    {
        std::lock_guard g{lock_};
        const std::size_t n = std::min<std::size_t>(results_.size(), k_uploads_per_poll);
        batch.assign(std::make_move_iterator(results_.begin()), std::make_move_iterator(results_.begin() + static_cast<std::ptrdiff_t>(n)));
        results_.erase(results_.begin(), results_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    bool any = false;
    for (Result& r : batch) {
        const auto it = entries_.find(r.key);
        if (it == entries_.end()) { continue; }
        it->second.loading = false;
        if (r.w > 0) {
            strata::texture_desc d{};
            d.width  = static_cast<strata::u32>(r.w);
            d.height = static_cast<strata::u32>(r.h);
            d.format = strata::texture_format::bgra8;
            it->second.image = {renderer.create_texture(d, r.bgra), r.w, r.h};
            any = true;
        }
    }

    // keep the cache bounded: the least recently used finished entries go
    if (entries_.size() > k_max_entries) {
        std::vector<std::pair<unsigned long, std::string>> by_age;
        for (const auto& [k, e] : entries_) {
            if (!e.loading) { by_age.emplace_back(e.used, k); }
        }
        std::sort(by_age.begin(), by_age.end());
        const std::size_t drop = std::min(by_age.size(), entries_.size() - k_max_entries + 100);
        for (std::size_t i = 0; i < drop; ++i) {
            const auto it = entries_.find(by_age[i].second);
            if (it->second.image.id != 0) { renderer.destroy_texture(it->second.image.id); }
            entries_.erase(it);
        }
    }
    if (!batch.empty() && !results_.empty()) { if (wake_) { wake_(); } } // (more waiting: another frame)
    return any;
}

void ImageCache::forget()
{
    entries_.clear();
    std::lock_guard g{lock_};
    queue_.clear();
    results_.clear();
}

void ImageCache::worker()
{
    const bool com = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    for (;;) {
        Request req;
        {
            std::unique_lock g{lock_};
            cv_.wait(g, [this] { return quit_ || !queue_.empty(); });
            if (quit_) { break; }
            req = std::move(queue_.front());
            queue_.pop_front();
        }

        Result res{req.key, 0, 0, {}};
        ComPtr<IShellItem> item;
        if (SUCCEEDED(::SHCreateItemFromParsingName(core::text::widen(req.path).c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            ComPtr<IShellItemImageFactory> factory;
            if (SUCCEEDED(item.As(&factory))) {
                const SIZE size{req.px, req.px};
                HBITMAP bmp = nullptr;
                HRESULT hr = E_FAIL;
                if (req.kind == ImageKind::thumbnail) { hr = factory->GetImage(size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp); }
                if (FAILED(hr) || bmp == nullptr) { hr = factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp); }
                if (SUCCEEDED(hr) && bmp != nullptr) {
                    (void)read_bitmap(bmp, res.w, res.h, res.bgra);
                    ::DeleteObject(bmp);
                }
            }
        }
        {
            std::lock_guard g{lock_};
            results_.push_back(std::move(res));
        }
        if (wake_) { wake_(); }
    }
    if (com) { ::CoUninitialize(); }
}

} // namespace prism::platform
