#pragma once

#include "ui/env.hpp"
#include "ui/painter.hpp"

namespace prism::ui {

// item count, selection summary, loading state
class StatusBar {
public:
    explicit StatusBar(Env& env) : env_{env} {}
    void frame(Painter& p, const strata::rect& area);

private:
    Env& env_;
};

} // namespace prism::ui
