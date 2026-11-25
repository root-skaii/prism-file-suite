#include "platform/image_cache.hpp"

#include "core/bitmap.hpp"
#include "core/crash.hpp"
#include "core/text.hpp"
#include "core/thread_priority.hpp"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wrl/client.h>
#endif

#include <algorithm>

namespace prism::platform {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif

namespace {

constexpr std::size_t k_max_entries    = 700;
constexpr int         k_uploads_per_poll = 16;
constexpr std::size_t k_worker_count   = 4;

#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif

} // namespace

ImageCache::ImageCache(std::function<void()> wake) : wake_{std::move(wake)}
{
    workers_.reserve(k_worker_count);
    for (std::size_t i = 0; i < k_worker_count; ++i) { workers_.emplace_back([this] { worker(); }); }
}

ImageCache::~ImageCache()
{
    {
        std::lock_guard g{lock_};
        quit_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) { t.join(); }
    }
}

Image ImageCache::get(const std::string& path, int px, ImageKind kind)
{
    std::string key;
    key.reserve(path.size() + 12);
    key += path;
    key.push_back('|');
    key += std::to_string(px);
    key.push_back(kind == ImageKind::icon ? 'i' : 't');

    auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.used = tick_;
        return it->second.image;
    }
    entries_[key] = Entry{{}, true, tick_};
    std::string dropped_key;
    {
        std::lock_guard g{lock_};
        queue_.push_front({key, path, px, kind});
        if (queue_.size() > 400) {
            dropped_key = std::move(queue_.back().key);
            queue_.pop_back();
        }
    }
    // dropped requests must leave entries_, else they stay loading=true forever and leak past LRU eviction
    if (!dropped_key.empty()) {
        if (const auto dit = entries_.find(dropped_key); dit != entries_.end() && dit->second.loading) { entries_.erase(dit); }
    }
    cv_.notify_one();
    return {};
}

Image ImageCache::raw(const std::string& key, int w, int h, const std::vector<unsigned char>& bgra)
{
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.used = tick_;
        return it->second.image;
    }
    entries_[key] = Entry{{}, true, tick_};
    {
        std::lock_guard g{lock_};
        results_.push_back({key, w, h, bgra});
    }
    if (wake_) { wake_(); }
    return {};
}

bool ImageCache::poll(Renderer& renderer)
{
    ++tick_;
    std::vector<Result> batch;
    bool more = false;
    {
        std::lock_guard g{lock_};
        const std::size_t n = std::min<std::size_t>(results_.size(), k_uploads_per_poll);
        batch.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            batch.push_back(std::move(results_.front()));
            results_.pop_front();
        }
        more = !results_.empty();
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
        by_age.reserve(entries_.size());
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
    if (!batch.empty() && more) { if (wake_) { wake_(); } } // (more waiting: another frame)
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
    core::lower_thread_priority();
#ifdef _WIN32
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
        // third-party thumbnail handlers run in here: a fault in one costs this picture, not the window
        (void)core::crash::guard("thumbnail / icon handler", [&] {
        ComPtr<IShellItem> item;
        const std::wstring wpath = core::text::widen(req.path);
        const DWORD attrs = ::GetFileAttributesW(wpath.c_str());
        const bool cloud_only = attrs != INVALID_FILE_ATTRIBUTES &&
                                (attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) != 0;
        if (SUCCEEDED(::SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            ComPtr<IShellItemImageFactory> factory;
            if (SUCCEEDED(item.As(&factory))) {
                const SIZE size{req.px, req.px};
                HBITMAP bmp = nullptr;
                HRESULT hr = E_FAIL;
                if (req.kind == ImageKind::thumbnail) {
                    // never hydrate cloud-only placeholders just to render a thumbnail
                    const int flags = SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK | (cloud_only ? SIIGBF_INCACHEONLY : 0);
                    hr = factory->GetImage(size, flags, &bmp);
                }
                if (FAILED(hr) || bmp == nullptr) { hr = factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp); }
                if (SUCCEEDED(hr) && bmp != nullptr) {
                    (void)core::bitmap_to_bgra(bmp, res.w, res.h, res.bgra);
                    ::DeleteObject(bmp);
                }
            }
        }
        });
        {
            std::lock_guard g{lock_};
            results_.push_back(std::move(res));
        }
        if (wake_) { wake_(); }
    }
    if (com) { ::CoUninitialize(); }
#else
    for (;;) {
        Request req;
        {
            std::unique_lock g{lock_};
            cv_.wait(g, [this] { return quit_ || !pending_.empty(); });
            if (quit_) { break; }
            req = std::move(pending_.front());
            pending_.pop_front();
        }
        Result res;
        res.key = std::move(req.key);
        {
            std::lock_guard g{lock_};
            results_.push_back(std::move(res));
        }
        if (wake_) { wake_(); }
    }
#endif
}

} // namespace prism::platform
