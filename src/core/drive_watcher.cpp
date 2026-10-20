#include "core/drive_watcher.hpp"

#include "core/i18n.hpp"
#include "core/recycle.hpp"

namespace prism::core {

namespace {

[[nodiscard]] bool same(const std::vector<Place>& a, const std::vector<Place>& b)
{
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].path != b[i].path || a[i].name != b[i].name || a[i].total != b[i].total || a[i].free != b[i].free || a[i].removable != b[i].removable || a[i].optical != b[i].optical || a[i].bin != b[i].bin) { return false; }
    }
    return true;
}

} // namespace

DriveWatcher::DriveWatcher(std::function<void()> on_change) : on_change_{std::move(on_change)}
{
    drives_ = core::drives(); // (the first answer is there before the first frame; later ones come from the thread)
    revision_.store(1);
    poked_ = true;            // (devices are only looked for on the thread: its first pass runs at once)
    thread_ = std::jthread{[this](std::stop_token st) { run(st); }};
}

DriveWatcher::~DriveWatcher()
{
    thread_.request_stop();
    wake_.notify_all();
}

std::vector<Place> DriveWatcher::list() const
{
    std::lock_guard lock{mutex_};
    return drives_;
}

void DriveWatcher::poke()
{
    {
        std::lock_guard lock{mutex_};
        poked_ = true;
    }
    wake_.notify_all();
}

void DriveWatcher::run(std::stop_token st)
{
    while (!st.stop_requested()) {
        {
            std::unique_lock lock{mutex_};
            wake_.wait_for(lock, st, interval, [this] { return poked_; });
            poked_ = false;
        }
        if (st.stop_requested()) { return; }
        std::vector<Place> now = core::drives(); // (may take seconds: no lock held)
        if (!recycle::folders().empty()) { // (one row for every drive's bin)
            Place p;
            p.name    = std::string{i18n::tr("Recycle Bin")};
            p.path    = std::string{recycle::all};
            p.recycle = true;
            now.push_back(std::move(p));
        }
        for (Place& d : core::portable_devices()) { now.push_back(std::move(d)); }
        bool changed = false;
        {
            std::lock_guard lock{mutex_};
            changed = !same(drives_, now);
            if (changed) {
                drives_ = std::move(now);
                revision_.fetch_add(1);
            }
        }
        if (changed && on_change_) { on_change_(); }
    }
}

} // namespace prism::core
