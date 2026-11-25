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

DriveWatcher::DriveWatcher(std::function<void()> on_change) : state_{std::make_shared<State>()}
{
    state_->on_change = std::move(on_change);
    state_->interval  = interval;
    // local drives only on startup so an unreachable mapped network share never stalls the first frame;
    // the worker immediately fills in network shares, Recycle Bin, portable devices, WSL, and cloud drives
    state_->drives = core::drives(false);
    state_->revision.store(1);
    state_->poked = true;
    // detached: never block window close on a slow network drive / cloud RPC
    std::thread{&DriveWatcher::run, state_}.detach();
}

DriveWatcher::~DriveWatcher()
{
    {
        std::lock_guard lock{state_->mutex};
        state_->stop = true;
        state_->on_change = {};
    }
    state_->wake.notify_all();
}

std::vector<Place> DriveWatcher::list() const
{
    std::lock_guard lock{state_->mutex};
    return state_->drives;
}

void DriveWatcher::poke()
{
    {
        std::lock_guard lock{state_->mutex};
        state_->poked    = true;
        state_->interval = interval;
    }
    state_->wake.notify_all();
}

void DriveWatcher::run(std::shared_ptr<State> s)
{
    for (;;) {
        {
            std::unique_lock lock{s->mutex};
            s->wake.wait_for(lock, s->interval, [&] { return s->stop || s->poked; });
            if (s->stop) { return; }
            s->poked = false;
        }
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
        std::function<void()> cb;
        {
            std::lock_guard lock{s->mutex};
            if (s->stop) { return; }
            changed = !same(s->drives, now);
            if (changed) {
                s->drives = std::move(now);
                s->revision.fetch_add(1);
                cb = s->on_change;
            }
        }
        if (changed && cb) { cb(); }
    }
}

} // namespace prism::core
