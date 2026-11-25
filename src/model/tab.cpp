#include "model/tab.hpp"

#include "core/path.hpp"
#include "core/rclone.hpp"
#include "core/recycle.hpp"
#include "core/text.hpp"

namespace prism::model {

Tab::Tab(std::function<void()> wake) : loader_{wake}, quiet_loader_{wake}, watcher_{wake} {}

void Tab::set_waker(std::function<void()> wake)
{
    loader_.set_waker(wake);
    quiet_loader_.set_waker(wake);
    watcher_.set_waker(std::move(wake));
}

const std::string& Tab::path() const noexcept
{
    static const std::string none;
    return history_.empty() ? none : history_[index_].path;
}

void Tab::remember_view()
{
    if (history_.empty()) { return; }
    Place& p = history_[index_];
    p.scroll = scroll_y;
    const std::size_t f = listing_.focus();
    p.focus_name = f < listing_.rows() ? listing_.at(f).name : std::string{};
}

void Tab::navigate(const std::string& raw)
{
    const std::string target = core::path::normalize(raw);
    if (target.empty()) { return; }
    if (!history_.empty()) {
        if (target == path()) { refresh(); return; }
        remember_view();
        history_.resize(index_ + 1); // going somewhere new drops the forward history
    }
    history_.push_back({target, 0.0f, {}});
    index_ = history_.size() - 1;
    reveal_name.clear();
    load(false);
}

bool Tab::back()
{
    if (!can_back()) { return false; }
    remember_view();
    const std::string left = history_[index_].path;
    --index_;
    reveal_name = core::path::parent(left) == history_[index_].path ? core::path::leaf(left) : std::string{};
    load(true);
    return true;
}

bool Tab::forward()
{
    if (!can_forward()) { return false; }
    remember_view();
    ++index_;
    reveal_name.clear();
    load(true);
    return true;
}

std::vector<std::pair<std::size_t, std::string>> Tab::back_history() const
{
    std::vector<std::pair<std::size_t, std::string>> out;
    for (std::size_t i = index_; i-- > 0;) { out.emplace_back(i, history_[i].path); }
    return out;
}

std::vector<std::pair<std::size_t, std::string>> Tab::forward_history() const
{
    std::vector<std::pair<std::size_t, std::string>> out;
    for (std::size_t i = index_ + 1; i < history_.size(); ++i) { out.emplace_back(i, history_[i].path); }
    return out;
}

bool Tab::jump_history(std::size_t history_index)
{
    if (history_index >= history_.size() || history_index == index_) { return false; }
    remember_view();
    index_ = history_index;
    reveal_name.clear();
    load(true);
    return true;
}

bool Tab::can_up() const
{
    return !core::path::parent(path()).empty();
}

bool Tab::up()
{
    const std::string parent = core::path::parent(path());
    if (parent.empty()) { return false; }
    const std::string left = core::path::leaf(path());
    navigate(parent);
    reveal_name = left;
    return true;
}

void Tab::refresh()
{
    if (path().empty()) { return; }
    if (core::rclone::is_path(path())) { core::rclone::invalidate_cache(path()); }
    if (searching()) { search(search_); return; }
    remember_view();
    load(true);
}

void Tab::refresh_quiet()
{
    if (path().empty() || searching()) { refresh(); return; }
    quiet_dirty_ = true;
}

void Tab::forget(const std::vector<std::string>& paths)
{
    if (path().empty() || searching()) { return; }
    std::unordered_set<std::string> names;
    for (const std::string& p : paths) {
        if (core::text::lower_ascii(core::path::parent(p)) == core::text::lower_ascii(path())) { names.insert(core::path::leaf(p)); }
    }
    listing_.remove_names(names);
}

void Tab::search(core::Query query)
{
    if (path().empty() || query.empty() || !query.valid()) { return; }
    if (!searching()) { remember_view(); }
    search_    = query;
    searching_ = true;
    error_     = 0;
    ++generation;
    listing_.set_query({});
    listing_.clear();
    reveal_name.clear();
    restore_pending_ = true;
    scroll_y         = 0.0f;
    loader_.start_search(path(), std::move(query));
}

void Tab::clear_search()
{
    if (!searching()) { return; }
    load(true);
}

void Tab::load(bool keep_scroll)
{
    searching_ = false;
    search_    = {};
    restore_scroll_  = keep_scroll ? history_[index_].scroll : 0.0f;
    restore_pending_ = true;
    scroll_y         = restore_scroll_;
    error_           = 0;
    ++generation;
    listing_.set_query({}); // (a filter belongs to the folder it was typed in)
    listing_.clear();
    if (reveal_name.empty() && keep_scroll) { reveal_name = history_[index_].focus_name; }
    quiet_loader_.cancel();
    quiet_dirty_   = false;
    watch_pending_ = false;
    watcher_.watch(core::rclone::is_path(path()) ? std::string{} : path());
    loader_.start(path());
}

double Tab::next_wake_seconds() const noexcept
{
    if (!watch_pending_) { return 3600.0; }
    const double s = std::chrono::duration<double>(watch_due_ - std::chrono::steady_clock::now()).count();
    return s > 0.0 ? s : 0.0;
}

bool Tab::poll()
{
    bool changed = false;
    std::vector<core::FileEntry> batch;
    const core::DirectoryLoader::Progress p = loader_.poll(batch);
    if (p.any) {
        changed = true;
        if (!batch.empty()) { listing_.append(std::move(batch)); }
        if (p.done) {
            error_ = p.error;
            listing_.finish();
        }
    }

    if (searching_) { return changed; }
    // incremental patching for small bursts of external changes (avoids full re-enumeration on huge folders);
    // overflows and Recycle Bin fall back to debounced quiet_loader_
    const auto now = std::chrono::steady_clock::now();
    if (core::FolderChanges fc = watcher_.take_changes(); fc.any) {
        if (!fc.overflow && !fc.deltas.empty() && listing_.finished() && !loader_.running() && !core::recycle::inside(path())) {
            for (core::FolderDelta& d : fc.deltas) {
                if (d.kind == core::FolderDelta::Kind::remove) {
                    changed |= listing_.remove_name(d.name);
                } else {
                    changed |= listing_.upsert_entry(std::move(d.entry));
                }
            }
        } else {
            if (!watch_pending_) {
                watch_pending_ = true;
                watch_first_   = now;
            }
            watch_due_ = std::min(now + std::chrono::milliseconds{150}, watch_first_ + std::chrono::milliseconds{500});
        }
    }
    if (watch_pending_ && now >= watch_due_) {
        watch_pending_ = false;
        quiet_dirty_   = true;
    }
    if (!loader_.running()) {
        if (quiet_dirty_ && !quiet_loader_.running()) {
            quiet_dirty_ = false;
            quiet_.clear();
            quiet_loader_.start(path());
        }
        // (a finished job keeps reporting `done`: only a running one is polled, or the next call would swap in an empty list)
        std::vector<core::FileEntry> more;
        const core::DirectoryLoader::Progress q = quiet_loader_.running() ? quiet_loader_.poll(more) : core::DirectoryLoader::Progress{};
        if (q.any) {
            quiet_.insert(quiet_.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
            if (q.done && q.error == 0 && listing_.finished()) {
                listing_.replace_entries(std::move(quiet_));
                quiet_.clear();
                changed = true;
            }
        }
    }
    return changed;
}

std::string Tab::title() const
{
    const std::string& p = path();
    if (p.empty()) { return "New tab"; }
    // (the bin's folder is named after your SID)
    if (p == core::recycle::all) { return "Recycle Bin"; }
    if (core::recycle::inside(p) && core::text::lower_ascii(core::path::leaf(core::path::parent(p))) == "$recycle.bin") { return "Recycle Bin"; }
    return core::path::leaf(p);
}

} // namespace prism::model
