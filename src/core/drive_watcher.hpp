#pragma once

// keeps the drive list (names, free space) up to date on a worker thread. asking a volume for its label can take seconds
// (a sleeping disk, a network share that went away), which must never happen on the ui thread

#include "core/places.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace prism::core {

class DriveWatcher {
public:
    // `on_change` runs on the worker when the list differs from the last one (wake the ui there)
    explicit DriveWatcher(std::function<void()> on_change = {});
    ~DriveWatcher();
    DriveWatcher(const DriveWatcher&)            = delete;
    DriveWatcher& operator=(const DriveWatcher&) = delete;

    [[nodiscard]] std::vector<Place> list() const;
    // changes whenever the list does: compare it to what you last copied
    [[nodiscard]] unsigned revision() const noexcept { return revision_.load(); }
    void poke(); // look again now

    std::chrono::milliseconds interval{5000};

private:
    void run(std::stop_token st);

    std::function<void()>       on_change_;
    mutable std::mutex          mutex_;
    std::condition_variable_any wake_;
    std::vector<Place>          drives_;
    std::atomic<unsigned>       revision_{0};
    bool                        poked_{};
    std::jthread                thread_;
};

} // namespace prism::core
