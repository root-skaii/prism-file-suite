#pragma once

// quick image jobs through Windows' imaging component (WIC): convert to JPEG / PNG, resize, rotate. the result is a new file
// next to the original ("photo - 50%.jpg"); the original is never touched. jobs run on their own threads; the ui polls

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace prism::core {

enum class ImageOp : unsigned char { to_jpeg, to_png, half, quarter, fit_1920, rotate_left, rotate_right };

// whether `name` is an image these jobs can read (by extension)
[[nodiscard]] bool is_image_name(std::string_view name);

struct ImageResult {
    bool        ok{};
    std::string path;     // the new file
    std::string error;
};

// blocking: call it from a worker thread (it sets up COM for itself)
[[nodiscard]] ImageResult transform_image(const std::string& source, ImageOp op);

class ImageJobs {
public:
    struct Result {
        bool        ok{};
        std::string message;  // for a toast
        std::string dir;      // the folder whose listing changed
        std::string made;     // the (last) new file's name, to select it
        std::size_t done{};
    };

    explicit ImageJobs(std::function<void()> wake) : wake_{std::move(wake)} {}
    ~ImageJobs();
    ImageJobs(const ImageJobs&)            = delete;
    ImageJobs& operator=(const ImageJobs&) = delete;

    void start(ImageOp op, std::vector<std::string> sources);
    [[nodiscard]] bool poll(Result& out);
    [[nodiscard]] bool busy() const noexcept { return running_.load() > 0; }

private:
    std::function<void()>    wake_;
    std::atomic<int>         running_{};
    std::atomic<bool>        quit_{};
    std::mutex               lock_;
    std::vector<std::thread> threads_;
    std::vector<Result>      results_;
};

} // namespace prism::core
