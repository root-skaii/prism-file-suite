#pragma once

#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

namespace prism::ui {

// a modal page built from the widget kit: appearance (theme, dark style, density, scale, accent), behaviour and the
// keyboard shortcuts, each section a card. edits Settings in place
class SettingsDialog {
public:
    explicit SettingsDialog(Env& env) : env_{env} {}

    void open() noexcept { want_open_ = true; }
    // true on a frame where something changed (the shell applies and saves)
    [[nodiscard]] bool frame(Painter& p);

private:
    Env& env_;
    bool want_open_{};
    bool shown_{};
    float keys_h_{900.0f};    // the shortcut table's height, measured last frame
    float content_h_{900.0f}; // the body's content height, measured last frame
};

} // namespace prism::ui
