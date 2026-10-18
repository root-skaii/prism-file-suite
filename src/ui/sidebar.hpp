#pragma once

// quick access folders and drives; click navigates the active tab, middle click opens a tab

#include "core/places.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"

#include <vector>

namespace prism::ui {

class Sidebar {
public:
    explicit Sidebar(Env& env);

    void frame(Painter& p, const strata::rect& area, const FrameInput& in);
    void refresh_drives();

private:
    void section_title(Painter& p, std::string_view title);
    // returns true when clicked
    bool place_row(Painter& p, const core::Place& place, char32_t glyph, strata::color tint, bool drive);

    Env&                    env_;
    std::vector<core::Place> quick_;
    std::vector<core::Place> drives_;
    double                  drives_age_{};
};

} // namespace prism::ui
