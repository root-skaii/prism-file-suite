#pragma once

// asks "is this folder still there?" for a set of paths on a worker thread, so a vanished network share or an ejected
// drive cannot stall the ui while windows times out. answers are as of the last check (a few seconds old)

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace prism::core {

class PathProbe {
public:
    PathProbe();
    ~PathProbe();
    PathProbe(const PathProbe&)            = delete;
    PathProbe& operator=(const PathProbe&) = delete;

    // the folders to keep an eye on; a changed set is checked right away
    void watch(const std::vector<std::string>& paths);
    // true only once a check has found it gone (unknown counts as there)
    [[nodiscard]] bool missing(const std::string& path) const;
    // look again now (after the user did something that may have fixed it)
    void poke();

    std::chrono::milliseconds interval{3000};

private:
    void run(std::stop_token st);

    mutable std::mutex           mutex_;
    std::condition_variable_any  wake_;
    std::vector<std::string>     paths_;
    std::map<std::string, bool>  gone_;
    bool                         poked_{};
    std::jthread                 thread_;
};

} // namespace prism::core
