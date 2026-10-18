#pragma once

// shell icons and thumbnails: decoded on a worker thread (IShellItemImageFactory, so every installed codec / handler
// works), uploaded as gpu textures on the ui thread. nothing here ever blocks a frame

#include "strata/backend/d3d11.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace prism::platform {

enum class ImageKind { icon, thumbnail };

struct Image {
    strata::texture_id id{};   // 0: not ready (queued, loading or unavailable)
    int                w{}, h{};
};

class ImageCache {
public:
    // `wake` runs on the worker when images are ready
    explicit ImageCache(std::function<void()> wake);
    ~ImageCache();
    ImageCache(const ImageCache&)            = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // ui thread. queues a load the first time; `px` is the requested edge in physical pixels
    [[nodiscard]] Image get(const std::string& path, int px, ImageKind kind);

    // uploads what the worker finished (a few per frame); true if any texture is new
    bool poll(strata::d3d11_renderer& renderer);
    // the device was lost: every texture is gone with it
    void forget();

private:
    struct Entry {
        Image         image;
        bool          loading{true};
        unsigned long used{};
    };
    struct Request { std::string key, path; int px; ImageKind kind; };
    struct Result  { std::string key; int w, h; std::vector<unsigned char> bgra; };

    void worker();

    std::function<void()>                  wake_;
    std::unordered_map<std::string, Entry> entries_;   // ui thread only
    unsigned long                          tick_{};

    std::mutex              lock_;
    std::condition_variable cv_;
    std::deque<Request>     queue_;     // newest first: what just scrolled into view loads first
    std::vector<Result>     results_;
    bool                    quit_{};
    std::thread             thread_;
};

} // namespace prism::platform
