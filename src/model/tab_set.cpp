#include "model/tab_set.hpp"

#include <algorithm>

namespace prism::model {

Tab& TabSet::open(const std::string& path, bool activate)
{
    // a new tab opens right of the active one, like a browser
    const std::size_t at = tabs_.empty() ? 0 : active_ + 1;
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::make_unique<Tab>(wake_));
    Tab& t = *tabs_[at];
    t.navigate(path);
    if (activate || tabs_.size() == 1) { active_ = at; }
    return t;
}

Tab& TabSet::duplicate(std::size_t index)
{
    return open(tabs_[index]->path());
}

bool TabSet::close(std::size_t index)
{
    if (tabs_.size() <= 1) { return false; }
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (active_ > index || active_ >= tabs_.size()) { active_ = active_ > 0 ? active_ - 1 : 0; }
    return true;
}

void TabSet::move(std::size_t from, std::size_t to)
{
    if (from == to || from >= tabs_.size() || to >= tabs_.size()) { return; }
    const Tab* active = tabs_[active_].get();
    auto moved = std::move(tabs_[from]);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].get() == active) { active_ = i; }
    }
}

void TabSet::activate(std::size_t index) noexcept
{
    if (index < tabs_.size()) { active_ = index; }
}

void TabSet::next(int step) noexcept
{
    const auto n = static_cast<int>(tabs_.size());
    if (n == 0) { return; }
    active_ = static_cast<std::size_t>(((static_cast<int>(active_) + step) % n + n) % n);
}

bool TabSet::poll()
{
    bool changed = false;
    for (auto& t : tabs_) { changed |= t->poll(); }
    return changed;
}

} // namespace prism::model
