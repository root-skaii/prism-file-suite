#pragma once

// everything the Properties dialog shows: metadata, NTFS details, version resources, folder totals, hashes

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace prism::core {

struct MetaGroup {
    std::string title; // e.g. "Image", "Camera", "Audio", "Video", "Document", "Shortcut"
    std::vector<std::pair<std::string, std::string>> rows;
};

struct FileInfo {
    std::string   path, name, type, owner, volume, volume_label, filesystem, link_target;
    bool          exists{};
    bool          is_dir{};
    bool          is_drive{};
    std::uint64_t size{};
    std::uint64_t size_on_disk{};
    std::uint64_t drive_total{}, drive_free{}; // populated when `is_drive`
    std::uint64_t created{}, modified{}, accessed{}; // FILETIME ticks
    std::uint32_t attributes{};
    std::uint32_t hard_links{};
    std::uint64_t file_id{};

    // quick-access highlights (also reflected in `meta_groups` for Details)
    std::uint32_t img_width{}, img_height{};
    std::uint32_t img_bpp{}, img_frames{};
    double        img_dpi_x{}, img_dpi_y{};
    std::string   media_duration;   // e.g. "03:42"
    std::string   media_bitrate;    // e.g. "320 kbps"
    std::string   media_summary;    // short header pill/subtitle e.g. "1920 × 1080 · 2.1 MP" or "03:42 · 320 kbps"

    // Mark-of-the-Web (Zone.Identifier ADS)
    bool          motw_blocked{};
    int           motw_zone{};      // 3 = Internet, 4 = Restricted
    std::string   motw_host;        // HostUrl or ReferrerUrl when present

    std::vector<MetaGroup>                                meta_groups; // format-specific cards (Image, Camera, Video, Audio, Document, Shortcut)
    std::vector<std::pair<std::string, std::uint64_t>>    streams;     // alternate data streams
    std::vector<std::pair<std::string, std::string>>      version;     // from the exe / dll resource
};

[[nodiscard]] FileInfo query_file_info(const std::string& path);
// flips one FILE_ATTRIBUTE_* bit
[[nodiscard]] bool set_file_attribute(const std::string& path, std::uint32_t flag, bool on);
// removes the Mark-of-the-Web (:Zone.Identifier) alternate data stream
[[nodiscard]] bool unblock_file(const std::string& path);

// runs query_file_info for one or more paths off the ui thread, waking the ui when ready
class FileInfoJob {
public:
    FileInfoJob() = default;
    ~FileInfoJob() { cancel(); }
    FileInfoJob(const FileInfoJob&)            = delete;
    FileInfoJob& operator=(const FileInfoJob&) = delete;

    void start(std::vector<std::string> paths, std::function<void()> wake = {});
    void cancel() noexcept;

    [[nodiscard]] bool ready() const noexcept { return s_ != nullptr && s_->done.load(std::memory_order_acquire); }
    [[nodiscard]] bool running() const noexcept { return s_ != nullptr && !s_->done.load(std::memory_order_acquire); }
    [[nodiscard]] std::vector<FileInfo> take();

private:
    struct State {
        std::atomic<bool>     done{}, cancel{};
        std::mutex            lock;
        std::vector<FileInfo> results;
    };
    std::shared_ptr<State> s_;
};

// totals of a folder tree, counted on a worker thread; read the counters while it runs
class FolderScan {
public:
    FolderScan() = default;
    ~FolderScan() { cancel(); }
    FolderScan(const FolderScan&)            = delete;
    FolderScan& operator=(const FolderScan&) = delete;

    void start(const std::string& path);
    void cancel() noexcept;

    [[nodiscard]] std::uint64_t files() const noexcept { return s_ ? s_->files.load() : 0; }
    [[nodiscard]] std::uint64_t folders() const noexcept { return s_ ? s_->folders.load() : 0; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return s_ ? s_->bytes.load() : 0; }
    [[nodiscard]] std::uint64_t on_disk() const noexcept { return s_ ? s_->disk.load() : 0; }
    [[nodiscard]] bool          done() const noexcept { return s_ && s_->done.load(); }

private:
    struct State {
        std::atomic<std::uint64_t> files{}, folders{}, bytes{}, disk{};
        std::atomic<bool>          done{}, cancel{};
    };
    std::shared_ptr<State> s_;
};

// MD5, SHA-1 and SHA-256 of a file in one pass (windows CNG)
class HashJob {
public:
    HashJob() = default;
    ~HashJob() { cancel(); }
    HashJob(const HashJob&)            = delete;
    HashJob& operator=(const HashJob&) = delete;

    void start(const std::string& path);
    void cancel() noexcept;

    [[nodiscard]] bool  running() const noexcept { return s_ != nullptr && !s_->done.load(); }
    [[nodiscard]] bool  done() const noexcept { return s_ != nullptr && s_->done.load(); }
    [[nodiscard]] bool  failed() const noexcept { return s_ != nullptr && s_->failed.load(); }
    [[nodiscard]] float progress() const noexcept { return s_ ? static_cast<float>(s_->progress.load()) / 1000.0f : 0.0f; }
    [[nodiscard]] std::string md5() const;
    [[nodiscard]] std::string sha1() const;
    [[nodiscard]] std::string sha256() const;

private:
    struct State {
        std::atomic<int>  progress{};  // per mille
        std::atomic<bool> done{}, cancel{}, failed{};
        std::mutex        lock;
        std::string       md5, sha1, sha256;
    };
    std::shared_ptr<State> s_;
};

} // namespace prism::core
