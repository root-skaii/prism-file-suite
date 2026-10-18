#pragma once

// the top strip: tabs (drag to reorder, middle click closes) and the window buttons. it is also the window's caption:
// everything that is not a tab or a button drags the window

#include "ui/env.hpp"
#include "ui/painter.hpp"

namespace prism::ui {

class TitleBar {
public:
    explicit TitleBar(Env& env) : env_{env} {}

    void frame(Painter& p, const strata::rect& area);
    [[nodiscard]] platform::HitZone hit_test(strata::vec2 pt) const;

private:
    Env&         env_;
    strata::rect area_{};
    strata::rect strip_{};     // tabs + the new-tab button
    strata::rect min_{}, max_{}, close_{};
    int          drag_{-1};    // tab being dragged
};

} // namespace prism::ui
