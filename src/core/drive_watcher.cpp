#include "core/drive_watcher.hpp"

#include "core/i18n.hpp"
#include "core/path.hpp"
#include "core/rclone.hpp"
#include "core/recycle.hpp"
#include "core/text.hpp"

namespace prism::core {

namespace {

[[nodiscard]] bool same(const std::vector<Place>& a, const std::vector<Place>& b)
{
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].path != b[i].path || a[i].name != b[i].name || a[i].total != b[i].total || a[i].free != b[i].free ||
            a[i].removable != b[i].removable || a[i].optical != b[i].optical || a[i].network != b[i].network ||
            a[i].wsl != b[i].wsl || a[i].cloud != b[i].cloud || a[i].bin != b[i].bin) {
            return false;
        }
    }
    return true;
}

} // namespace

DriveWatcher::DriveWatcher(std::function<void()> on_change) : on_change_{std::move(on_change)}
{
    // local drives only on startup so an unreachable mapped network share never stalls the first frame;
    // the worker immediately fills in network shares, Recycle Bin and portable devices
    drives_ = core::drives(false);
    revision_.store(1);
    poked_ = true;
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
        for (Place& w : core::wsl_distros()) { now.push_back(std::move(w)); }
        for (Place& g : core::google_drive_places()) {
            const std::string gl = text::lower_ascii(path::normalize(g.path));
            const bool dup = std::ranges::any_of(now, [&](const Place& e) {
                return text::lower_ascii(path::normalize(e.path)) == gl;
            });
            if (!dup) { now.push_back(std::move(g)); }
        }
        for (const rclone::Remote& r : rclone::list_remotes()) {
            Place p;
            p.path  = rclone::make_root(r.name);
            p.cloud = true;
            if (r.type == "drive") {
                p.name = text::lower_ascii(r.name) == "gdrive" ? std::string{"Google Drive"} : (r.name + " (Google Drive)");
            } else if (!r.type.empty()) {
                p.name = r.name + " (" + r.type + ")";
            } else {
                p.name = r.name;
            }
            (void)rclone::about(r.name, p.total, p.free);
            now.push_back(std::move(p));
        }
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
