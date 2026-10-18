#include "model/tab.hpp"

#include "core/path.hpp"

namespace prism::model {

Tab::Tab(std::function<void()> wake) : loader_{std::move(wake)} {}

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
    remember_view();
    load(true);
}

void Tab::load(bool keep_scroll)
{
    restore_scroll_  = keep_scroll ? history_[index_].scroll : 0.0f;
    restore_pending_ = true;
    scroll_y         = restore_scroll_;
    error_           = 0;
    ++generation;
    listing_.set_filter({}); // (a filter belongs to the folder it was typed in)
    listing_.clear();
    if (reveal_name.empty() && keep_scroll) { reveal_name = history_[index_].focus_name; }
    loader_.start(path());
}

bool Tab::poll()
{
    std::vector<core::FileEntry> batch;
    const core::DirectoryLoader::Progress p = loader_.poll(batch);
    if (!p.any) { return false; }
    if (!batch.empty()) { listing_.append(std::move(batch)); }
    if (p.done) {
        error_ = p.error;
        listing_.finish();
    }
    return true;
}

std::string Tab::title() const
{
    const std::string& p = path();
    if (p.empty()) { return "New tab"; }
    return core::path::leaf(p);
}

} // namespace prism::model
