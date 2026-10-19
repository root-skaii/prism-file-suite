#pragma once

// how big the folders in a listing are, counted on a worker thread: the size column asks for what it draws, gets the
// last known answer (or nothing yet) and the worker wakes the ui when a count lands. nothing here ever blocks the ui

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace prism::core {

class FolderSizes {
public:
    explicit FolderSizes(std::function<void()> wake);
    ~FolderSizes();
    FolderSizes(const FolderSizes&)            = delete;
    FolderSizes& operator=(const FolderSizes&) = delete;

    // the total size of the folder's files, if counted. Starts a count when there is none (or the answer is older than
    // `max_age`, in which case the old number keeps being returned until the new one lands)
    [[nodiscard]] std::optional<unsigned long long> lookup(const std::string& path);

    std::chrono::seconds max_age{60};

private:
    struct Entry {
        unsigned long long          bytes{};
        std::chrono::steady_clock::time_point at{};
        bool                        have{};
        bool                        queued{};
    };
    void run(std::stop_token st);

    std::function<void()>       wake_;
    std::mutex                  mutex_;
    std::condition_variable_any cv_;
    std::map<std::string, Entry> entries_;
    std::deque<std::string>     queue_;
    std::jthread                thread_;
};

// total size of the files below `dir` (not following links); stops early and returns what it has when `stop` fires
[[nodiscard]] unsigned long long measure_folder(const std::string& dir, const std::stop_token& stop);

} // namespace prism::core
