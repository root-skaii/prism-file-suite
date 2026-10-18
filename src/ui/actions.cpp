#include "ui/actions.hpp"

namespace prism::ui {

void register_actions(strata::keybinds& b)
{
    // tabs and windows
    b.add("new_tab",       "Ctrl+T",           "Open a new tab here");
    b.add("close_tab",     "Ctrl+W",           "Close this tab");
    b.add("reopen_tab",    "Ctrl+Shift+T",     "Reopen the tab you closed last");
    b.add("duplicate_tab", "Ctrl+Shift+D",     "Duplicate this tab");
    b.add("next_tab",      "Ctrl+Tab",         "Next tab");
    b.add("prev_tab",      "Ctrl+Shift+Tab",   "Previous tab");
    b.add("new_window",    "Ctrl+N",           "Open this folder in a new window");
    b.add("maximize",      "F11",              "Maximize or restore the window");

    // navigation
    b.add("back",          "Alt+Left",         "Go back");
    b.add("forward",       "Alt+Right",        "Go forward");
    b.add("up",            "Alt+Up",           "Go to the parent folder");
    b.add("home",          "Alt+Home",         "Go to your home folder");
    b.add("refresh",       "F5",               "Reload the folder");
    b.add("address",       "Ctrl+L",           "Edit the path");
    b.add("filter",        "Ctrl+F",           "Filter this folder; Ctrl+Enter searches subfolders");
    b.add("terminal",      "Ctrl+Alt+T",       "Open a terminal in this folder");

    // files
    b.add("copy",          "Ctrl+C",           "Copy the selection");
    b.add("cut",           "Ctrl+X",           "Cut the selection");
    b.add("paste",         "Ctrl+V",           "Paste into this folder");
    b.add("delete",        "Delete",           "Move the selection to the Recycle Bin");
    b.add("delete_forever","Shift+Delete",     "Delete the selection for good");
    b.add("rename",        "F2",               "Rename the selected item");
    b.add("new_folder",    "Ctrl+Shift+N",     "Create a folder");
    b.add("copy_path",     "Ctrl+Shift+C",     "Copy the full path of the selection");
    b.add("properties",    "Alt+Enter",        "Show properties");
    b.add("undo",          "Ctrl+Z",           "Undo the last file operation");
    b.add("redo",          "Ctrl+Y",           "Redo what was undone");

    // selection
    b.add("select_all",    "Ctrl+A",           "Select everything");
    b.add("select_invert", "Ctrl+I",           "Invert the selection");
    b.add("select_none",   "",                 "Clear the selection");

    // view
    b.add("hidden",        "Ctrl+H",           "Show or hide hidden files");
    b.add("view_grid",     "Ctrl+Shift+G",     "Switch between list and thumbnails");
    b.add("density",       "Ctrl+Shift+M",     "Switch between comfortable and compact");
    b.add("split",         "F3",               "Show two folders side by side");
    b.add("switch_pane",   "Tab",              "Move between the two panes");
    b.add("copy_to_pane",  "Ctrl+Alt+C",       "Copy the selection to the other pane");
    b.add("move_to_pane",  "Ctrl+Alt+X",       "Move the selection to the other pane");
    b.add("preview",       "Alt+P",            "Show or hide the preview pane");
    b.add("settings",      "Ctrl+,",           "Open settings");
    b.add("palette",       "Ctrl+Shift+P",     "Search every command");
}

} // namespace prism::ui
