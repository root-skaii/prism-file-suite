#pragma once

// the built-in copy / move queue as the user sees it: a small progress card in the corner while something is copying
// (speed, time left, pause, cancel), and the "this file already exists" prompt

#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

namespace prism::ui {

class TransfersUi {
public:
    explicit TransfersUi(Env& env) : env_{env} {}

    // the card, bottom right of `area`
    void frame_card(Painter& p, const strata::rect& area);
    // the conflict prompt (a modal: call it outside every window, at the end of the frame)
    void frame_conflict(Painter& p);

private:
    Env&          env_;
    bool          open_{};
    bool          for_all_{};
    std::uint64_t answered_{};   // the last question this ui answered (the worker may still show it for a moment)
};

} // namespace prism::ui
