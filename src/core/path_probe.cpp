#include "core/path_probe.hpp"

#include "core/path.hpp"

namespace prism::core {

PathProbe::PathProbe() : thread_{[this](std::stop_token st) { run(st); }} {}

PathProbe::~PathProbe()
{
    thread_.request_stop();
    wake_.notify_all();
}

void PathProbe::watch(const std::vector<std::string>& paths)
{
    {
        std::lock_guard lock{mutex_};
        if (paths == paths_) { return; }
        paths_ = paths;
        poked_ = true;
    }
    wake_.notify_all();
}

bool PathProbe::missing(const std::string& path) const
{
    std::lock_guard lock{mutex_};
    const auto it = gone_.find(path);
    return it != gone_.end() && it->second;
}

void PathProbe::poke()
{
    {
        std::lock_guard lock{mutex_};
        poked_ = true;
    }
    wake_.notify_all();
}

void PathProbe::run(std::stop_token st)
{
    while (!st.stop_requested()) {
        std::vector<std::string> todo;
        {
            std::lock_guard lock{mutex_};
            todo   = paths_;
            poked_ = false;
        }
        std::map<std::string, bool> found;
        for (const std::string& p : todo) {
            if (st.stop_requested()) { return; }
            found[p] = !path::is_directory(p);
        }
        std::unique_lock lock{mutex_};
        gone_ = std::move(found);
        wake_.wait_for(lock, st, interval, [this] { return poked_; });
    }
}

} // namespace prism::core
