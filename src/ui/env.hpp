#pragma once

// what every panel can reach: the window, the look, the tabs, file operations, settings and the app-level commands

#include "core/file_ops.hpp"
#include "model/tab_set.hpp"
#include "model/undo_stack.hpp"
#include "strata/keybinds.hpp"
#include "platform/image_cache.hpp"
#include "platform/window.hpp"
#include "ui/fonts.hpp"
#include "ui/settings.hpp"
#include "ui/theme.hpp"

#include <string>
#include <string_view>
#include <vector>

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
    virtual void open_settings() = 0;
    virtual void settings_changed() = 0; // a panel changed a setting: save it
    virtual void open_properties(std::vector<std::string> paths) = 0;
    virtual void toggle_view() = 0;       // details <-> thumbnails
    virtual void toggle_preview() = 0;
    virtual void toggle_split() = 0;
    // starts an OS drag of these paths (runs after the frame: the drag has its own message loop)
    virtual void begin_drag(std::vector<std::string> paths) = 0;
    // copy / move paths into a folder (drops and "copy to other pane")
    virtual void transfer(std::vector<std::string> paths, const std::string& dest, bool move) = 0;
    // delete these paths (asks first when the setting says so, or when permanent); the shell shows the dialog
    virtual void request_delete(std::vector<std::string> paths, bool permanent) = 0;
    virtual void toast(std::string text, strata::toast_kind kind) = 0;
    // pointer shape for this frame (strata only knows its own widgets)
    virtual void set_cursor(strata::cursor_kind kind) = 0;
};

// per-frame input the app loop hands the shell
struct FrameInput {
    std::string_view typed; // text typed this frame (utf-8)
    float            dt = 0.0f;
};

struct Env {
    platform::Window&     window;
    Theme&                theme;
    FontSet               fonts;
    model::TabSet&        tabs;
    Commands&             cmd;
    core::FileOps&        ops;
    Settings&             settings;
    platform::ImageCache& images;
    strata::keybinds&     binds;
    model::UndoStack&     history;
    // the other half of a split view (null when not split) and which half has the keyboard
    model::Tab*           right{};
    bool                  right_active{};

    // the folder the toolbar, shortcuts and status bar act on
    [[nodiscard]] model::Tab& tab() noexcept { return right_active && right != nullptr ? *right : tabs.active(); }
    [[nodiscard]] model::Tab* other() noexcept { return right == nullptr ? nullptr : (right_active ? &tabs.active() : right); }
};

} // namespace prism::ui
