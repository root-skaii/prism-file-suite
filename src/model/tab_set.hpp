#pragma once

#include "model/tab.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace prism::model {

class TabSet {
public:
    explicit TabSet(std::function<void()> wake) : wake_{std::move(wake)} {}

    Tab& open(const std::string& path, bool activate = true);
    Tab& attach(std::unique_ptr<Tab> tab, bool activate = true); // adopts an existing tab (history, scroll, listing)
    [[nodiscard]] std::unique_ptr<Tab> detach(std::size_t index); // removes and returns the tab (even if it was the last)
    Tab& duplicate(std::size_t index);
    // false when it was the last tab (the caller closes the window)
    bool close(std::size_t index);
    void move(std::size_t from, std::size_t to); // keeps the active tab active
    void activate(std::size_t index) noexcept;
    void next(int step) noexcept;                // ctrl+tab / ctrl+shift+tab, wraps

    [[nodiscard]] std::size_t size() const noexcept { return tabs_.size(); }
    [[nodiscard]] std::size_t active_index() const noexcept { return active_; }
    [[nodiscard]] Tab&        active() noexcept { return *tabs_[active_]; }
    [[nodiscard]] Tab&        at(std::size_t i) noexcept { return *tabs_[i]; }
    [[nodiscard]] const Tab&  at(std::size_t i) const noexcept { return *tabs_[i]; }

    // every tab keeps loading in the background: poll them all, true if any changed
    bool poll();

private:
    std::function<void()>             wake_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    std::size_t                       active_{};
};

} // namespace prism::model
