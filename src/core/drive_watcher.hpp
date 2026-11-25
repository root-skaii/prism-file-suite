#pragma once

// keeps the drive list (names, free space) up to date on a worker thread. asking a volume for its label can take seconds
// (a sleeping disk, a network share that went away), which must never happen on the ui thread

#include "core/places.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
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
    [[nodiscard]] unsigned revision() const noexcept { return state_->revision.load(); }
    void poke(); // look again now

    std::chrono::milliseconds interval{5000};

private:
    struct State {
        std::function<void()>    on_change;
        mutable std::mutex       mutex;
        std::condition_variable  wake;
        std::vector<Place>       drives;
        std::atomic<unsigned>    revision{0};
        bool                     poked{false};
        bool                     stop{false};
        std::chrono::milliseconds interval{5000};
    };

    static void run(std::shared_ptr<State> s);

    std::shared_ptr<State> state_;
};

} // namespace prism::core
