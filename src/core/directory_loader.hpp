#pragma once

// lists one folder on a worker thread and hands the entries to the ui thread in batches, so a 200k-file folder (or a
// slow network share) shows its first rows at once and the ui never blocks. starting a new listing abandons the old one.

#include "core/file_entry.hpp"
#include "core/query.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace prism::core {

// one find result as an entry; `rel_dir` (utf-8, "" or "sub\dir\\") is prepended to the name for recursive search results
[[nodiscard]] FileEntry entry_from_find_data(const void* win32_find_data_w, const std::string& rel_dir = {});

class DirectoryLoader {
public:
    // `wake` runs on the worker thread whenever there is something to poll: post a message to the window
    explicit DirectoryLoader(std::function<void()> wake) : wake_{std::move(wake)} {}
    ~DirectoryLoader() { cancel(); }
    DirectoryLoader(const DirectoryLoader&)            = delete;
    DirectoryLoader& operator=(const DirectoryLoader&) = delete;

    void set_waker(std::function<void()> wake) { wake_ = std::move(wake); }
    void start(const std::string& utf8_path);
    // every file and folder below `utf8_root` that matches `query`; names are relative paths. capped at 200k results
    void start_search(const std::string& utf8_root, Query query);
    void cancel() noexcept;

    struct Progress {
        bool          any{};      // `out` got entries, or the state changed
        bool          done{};     // the listing is complete (or failed)
        std::uint32_t error{};    // win32 error code when it failed, 0 otherwise
    };
    // moves what the worker produced since the last call into `out` (appended)
    [[nodiscard]] Progress poll(std::vector<FileEntry>& out);

    [[nodiscard]] bool running() const noexcept { return job_ != nullptr && !job_done_; }

private:
    struct Job {
        std::atomic<bool>      cancelled{false};
        std::mutex             lock;
        std::vector<FileEntry> pending;
        bool                   done{};
        std::uint32_t          error{};
    };

    static void work(std::shared_ptr<Job> job, std::wstring path, std::function<void()> wake);
    static void work_search(std::shared_ptr<Job> job, std::wstring root, Query query, std::function<void()> wake);

    std::function<void()> wake_;
    std::shared_ptr<Job>  job_;
    bool                  job_done_{true};
};

} // namespace prism::core
