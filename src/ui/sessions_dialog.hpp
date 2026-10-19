#pragma once

// saved sessions: name the tabs you have open now (and the second pane) and bring them back later, or switch to another set

#include "model/sessions.hpp"
#include "ui/env.hpp"
#include "ui/painter.hpp"
#include "ui/widgets.hpp"

#include <functional>
#include <string>

namespace prism::ui {

class SessionsDialog {
public:
    // `capture` builds a session of what is open now under a name; `restore` replaces what is open with a session
    SessionsDialog(Env& env, model::Sessions& store, std::function<model::Session(std::string)> capture, std::function<void(const model::Session&)> restore)
        : env_{env}, store_{store}, capture_{std::move(capture)}, restore_{std::move(restore)} {}

    void open() { want_open_ = true; age_ = 0; }
    void frame(Painter& p);

private:
    Env&                                         env_;
    model::Sessions&                             store_;
    std::function<model::Session(std::string)>   capture_;
    std::function<void(const model::Session&)>   restore_;
    bool                                         want_open_{};
    bool                                         shown_{};
    int                                          age_{};
    std::string                                  name_;
};

} // namespace prism::ui
