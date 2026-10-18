#pragma once

// quick access folders and drives; click navigates the active tab, middle click opens a tab

#include "core/places.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prism::ui {

class Sidebar {
public:
    explicit Sidebar(Env& env);

    void frame(Painter& p, const strata::rect& area, const FrameInput& in);
    void refresh_drives();

    // dropping files on a place moves / copies them there
    [[nodiscard]] std::string drop_target_at(strata::vec2 p) const;
    void set_drop_point(std::optional<strata::vec2> p) { drop_ = p; }

private:
    void section_title(Painter& p, std::string_view title, float top_space);
    // returns true when clicked
    bool place_row(Painter& p, const core::Place& place, char32_t glyph, strata::color tint, bool drive);

    Env&                    env_;
    std::vector<core::Place> quick_;
    std::vector<core::Place> drives_;
    double                  drives_age_{};
    strata::rect            area_{};
    std::vector<std::pair<strata::rect, std::string>> rows_; // last frame's rows (screen space) for drop targets
    std::optional<strata::vec2> drop_;
};

} // namespace prism::ui
