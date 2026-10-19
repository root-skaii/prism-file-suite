#pragma once

// zip / 7z / tar.* without a library: Windows 10+ ships bsdtar (System32\tar.exe), which reads zip, 7z, tar, tar.gz ...
// and writes zip. each job runs on its own thread; the ui polls results

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace prism::core {

// whether `name` looks like an archive tar.exe can open (by extension)
[[nodiscard]] bool is_archive_name(std::string_view name);
// "photos.tar.gz" -> "photos", "a.zip" -> "a"
[[nodiscard]] std::string archive_stem(std::string_view name);

// browsing an archive in place: it is unpacked into a temporary folder (a read-only copy as far as the archive goes) and
// shown as an ordinary folder. ArchiveViews remembers which folders are such copies so the ui can show "photos.zip > sub"
// instead of the temp path and step out of the archive with Up
struct ArchiveView {
    std::string archive;   // the archive file
    std::string inner;     // the path below the unpacked root ("" at the root)
};

class ArchiveViews {
public:
    void add(std::string root, std::string archive);
    [[nodiscard]] bool has_root(std::string_view root) const;
    // the view `path` is (or is inside), if any
    [[nodiscard]] std::optional<ArchiveView> find(std::string_view path) const;
    // `path` is the root of a view: where Up leads (the folder holding the archive)
    [[nodiscard]] std::optional<std::string> leave_target(std::string_view path) const;
    // the unpacked root of `archive`'s view, if it has one
    [[nodiscard]] std::optional<std::string> root_for(std::string_view archive) const;

private:
    std::vector<std::pair<std::string, std::string>> views_; // root, archive
};

// %TEMP%\PrismFS\archive-view\<hash of the archive's path, size and time>
// (`create`: makes the folder, and the ones above it, too)
[[nodiscard]] std::string archive_cache_dir(const std::string& archive, bool create = false);
// removes every unpacked copy of earlier sessions
void clear_archive_cache();

class ArchiveOps {
public:
    struct Result {
        bool        ok{};
        std::string message;  // for a toast
        std::string dir;      // the folder whose listing changed
        std::string made;     // the new file or folder's name (to select it)
        std::string tag;      // what extract() was given
    };

    explicit ArchiveOps(std::function<void()> wake) : wake_{std::move(wake)} {}
    ~ArchiveOps();
    ArchiveOps(const ArchiveOps&)            = delete;
    ArchiveOps& operator=(const ArchiveOps&) = delete;

    // unpacks into `dest_dir`, or (make_folder) into a new folder there named after the archive
    // `tag` comes back in the Result (the caller's way to tell its own job from a menu action)
    void extract(std::string archive, std::string dest_dir, bool make_folder, std::string tag = {});
    // packs `names` (inside `dir`) into a new zip in `dir`
    void compress(std::string dir, std::vector<std::string> names, std::string zip_name);

    [[nodiscard]] bool poll(Result& out);
    [[nodiscard]] bool busy() const noexcept { return running_.load() > 0; }

private:
    void spawn(std::function<Result()> job);

    std::function<void()>    wake_;
    std::atomic<bool>        quit_{};
    std::atomic<int>         running_{};
    std::mutex               lock_;
    std::vector<std::thread> threads_;
    std::vector<Result>      results_;
};

} // namespace prism::core
