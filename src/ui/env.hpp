#pragma once

// what every panel can reach: the window, the look, the tabs, file operations, settings and the app-level commands

#include "core/archive.hpp"
#include "core/file_ops.hpp"
#include "core/folder_sizes.hpp"
#include "core/git_status.hpp"
#include "model/sidebar_layout.hpp"
#include "model/tab_set.hpp"
#include "model/tags.hpp"
#include "model/undo_stack.hpp"
#include "model/visit_log.hpp"
#include "strata/keybinds.hpp"
#include "platform/image_cache.hpp"
#include "platform/window.hpp"
#include "ui/fonts.hpp"
#include "ui/settings.hpp"
#include "ui/theme.hpp"

#include <functional>
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
    virtual void move_tab_out(std::size_t index, int screen_x, int screen_y) = 0; // a tab released outside the window
    virtual void restart_elevated() = 0; // reopen the current folder(s) in an administrator window, then close this one
    virtual void restart_unelevated() = 0; // the way back: a normal window from an administrator one
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
    // sets the order of the folder shown in a pane: remembered for that folder when Settings::sort_per_folder, else the default
    virtual void set_sort(const std::string& folder, model::SortKey key, bool ascending) = 0;
    // starts an OS drag of these paths (runs after the frame: the drag has its own message loop)
    virtual void begin_drag(std::vector<std::string> paths) = 0;
    // copy / move paths into a folder (drops and "copy to other pane")
    virtual void transfer(std::vector<std::string> paths, const std::string& dest, bool move) = 0;
    // delete these paths (asks first when the setting says so, or when permanent); the shell shows the dialog
    virtual void request_delete(std::vector<std::string> paths, bool permanent) = 0;
    virtual void restore_from_bin(std::vector<std::string> items) = 0; // Recycle Bin items back where they were deleted from
    virtual void empty_recycle_bin(std::string drive_root) = 0; // one drive's ("D:\"), or all of them when empty
    // removes a sidebar section; when it holds folders the shell asks whether they go too or move to a neighbour section
    virtual void request_remove_section(std::size_t section) = 0;
    // pins the folder to the sidebar, or unpins it when it is there (says so in a toast)
    virtual void toggle_pin(const std::string& path) = 0;
    // the sidebar layout as text on the clipboard / a layout from the clipboard text (share it, back it up)
    virtual void copy_sidebar() = 0;
    virtual void paste_sidebar() = 0;
    // runs an edit of the sidebar layout and puts it on the undo stack (false: it changed nothing)
    virtual bool edit_sidebar(std::string_view label, const std::function<void(model::SidebarLayout&)>& edit) = 0;
    // runs a named action (ui/actions.cpp) at the end of the frame, on the pane that has the keyboard
    virtual void request_action(std::string name) = 0;
    // colour labels on these paths: label `index` (core::tag::names) goes on, or off when all of them already have it
    virtual void toggle_tag(const std::vector<std::string>& paths, int index) = 0;
    virtual void clear_tags(const std::vector<std::string>& paths) = 0;
    // opens an archive as a folder in the active pane (it is unpacked into a temporary folder first)
    virtual void open_archive(const std::string& path) = 0;
    // one step up: out of a folder, or out of an archive to the folder that holds it
    virtual void go_up(model::Tab& tab) = 0;
    virtual void toast(std::string_view text, strata::toast_kind kind) = 0;
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
    model::SidebarLayout& layout;
    model::VisitLog&      visits;
    core::FolderSizes&    sizes;
    core::GitStatus&      git;
    model::Tags&          tags;
    core::ArchiveViews&   views;   // archives shown as folders (unpacked temporary copies)
    // the other half of a split view (null when not split) and which half has the keyboard
    model::Tab*           right{};
    bool                  right_active{};

    // the folder the toolbar, shortcuts and status bar act on
    [[nodiscard]] model::Tab& tab() noexcept { return right_active && right != nullptr ? *right : tabs.active(); }
    [[nodiscard]] model::Tab* other() noexcept { return right == nullptr ? nullptr : (right_active ? &tabs.active() : right); }
};

} // namespace prism::ui
