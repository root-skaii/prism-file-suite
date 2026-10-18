#pragma once

// the top strip: tabs (drag to reorder, middle click closes) and the window buttons. it is also the window's caption:
// everything that is not a tab or a button drags the window

#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace prism::ui {

class TitleBar {
public:
    explicit TitleBar(Env& env) : env_{env} {}

    void frame(Painter& p, const strata::rect& area);
    [[nodiscard]] platform::HitZone hit_test(strata::vec2 pt) const;

    // dropping files on a tab moves / copies them into that tab's folder
    [[nodiscard]] std::string drop_target_at(strata::vec2 p) const;
    void set_drop_point(std::optional<strata::vec2> p) { drop_ = p; }

private:
    Env&         env_;
    strata::rect area_{};
    strata::rect strip_{};     // tabs + the new-tab button
    strata::rect min_{}, max_{}, close_{};
    int          drag_{-1};    // tab being dragged
    std::vector<strata::rect> tab_rects_; // last frame's tab bodies (drop targets)
    std::optional<strata::vec2> drop_;
    std::unordered_map<const void*, int> born_; // frames each tab has been seen: new tabs fade in
};

} // namespace prism::ui
