#pragma once

// what every panel can reach: the window, the look, the tabs, and the app-level commands

#include "model/tab_set.hpp"
#include "platform/window.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

#include <string>
#include <string_view>

namespace prism::ui {

// app-level actions a panel can ask for (the shell implements them)
class Commands {
public:
    virtual ~Commands() = default;
    virtual void open_tab(const std::string& path, bool activate) = 0;
    virtual void close_tab(std::size_t index) = 0;
    virtual void toggle_theme() = 0;
    virtual void toggle_density() = 0;
    virtual void toggle_hidden() = 0;
    virtual void focus_address() = 0;
    virtual void focus_filter() = 0;
};

// per-frame input the app loop hands the shell
struct FrameInput {
    std::string_view typed; // text typed this frame (utf-8)
    float            dt = 0.0f;
};

struct Env {
    platform::Window& window;
    Theme&            theme;
    FontSet           fonts;
    model::TabSet&    tabs;
    Commands&         cmd;
};

} // namespace prism::ui
