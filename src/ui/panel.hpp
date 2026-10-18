#pragma once

// a chrome-less strata window placed at a rectangle every frame: how the app lays its panels out. padding and spacing
// are zero inside, so the panel decides where everything goes.

#include "strata/context.hpp"

#include <string_view>

namespace prism::ui {

class PanelScope {
public:
    PanelScope(strata::context& ui, std::string_view id, const strata::rect& r) : ui_{ui}
    {
        ui_.push_var(strata::style_var::padding, 0.0f);
        ui_.push_var(strata::style_var::item_spacing, 0.0f);
        using strata::window_flags;
        open_ = ui_.begin_window(id, r.min, r.size(),
                                 window_flags::placed | window_flags::no_title_bar | window_flags::no_background | window_flags::no_collapse);
    }
    ~PanelScope()
    {
        ui_.end_window();
        ui_.pop_var(2);
    }
    PanelScope(const PanelScope&)            = delete;
    PanelScope& operator=(const PanelScope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return open_; }

private:
    strata::context& ui_;
    bool             open_{};
};

} // namespace prism::ui
