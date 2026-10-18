#pragma once

// lists one folder on a worker thread and hands the entries to the ui thread in batches, so a 200k-file folder (or a
// slow network share) shows its first rows at once and the ui never blocks. starting a new listing abandons the old one.

#include "core/file_entry.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace prism::core {

class DirectoryLoader {
public:
    // `wake` runs on the worker thread whenever there is something to poll: post a message to the window
    explicit DirectoryLoader(std::function<void()> wake) : wake_{std::move(wake)} {}
    ~DirectoryLoader() { cancel(); }
    DirectoryLoader(const DirectoryLoader&)            = delete;
    DirectoryLoader& operator=(const DirectoryLoader&) = delete;

    void start(const std::string& utf8_path);
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

    std::function<void()> wake_;
    std::shared_ptr<Job>  job_;
    bool                  job_done_{true};
};

} // namespace prism::core
