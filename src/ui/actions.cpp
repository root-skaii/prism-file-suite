#include "ui/actions.hpp"

#include "core/file_entry.hpp"

#include <string>

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
    b.add("sessions",      "Ctrl+Shift+O",     "Save the open tabs as a session, or switch to a saved one");
    b.add("maximize",      "F11",              "Maximize or restore the window");

    // navigation
    b.add("back",          "Alt+Left",         "Go back");
    b.add("forward",       "Alt+Right",        "Go forward");
    b.add("up",            "Alt+Up",           "Go to the parent folder");
    b.add("home",          "Alt+Home",         "Go to your home folder");
    b.add("refresh",       "F5",               "Reload the folder");
    b.add("address",       "Ctrl+L",           "Edit the path");
    b.add("filter",        "Ctrl+F",           "Filter this folder; Ctrl+Enter searches subfolders");
    b.add("search_everywhere", "Ctrl+Shift+F", "Search every drive by name, instantly (needs the drive index)");
    b.add("terminal",     "Ctrl+Alt+T",       "Open a terminal in this folder");

    // files
    b.add("copy",          "Ctrl+C",           "Copy the selection");
    b.add("cut",           "Ctrl+X",           "Cut the selection");
    b.add("paste",         "Ctrl+V",           "Paste into this folder");
    b.add("delete",        "Delete",           "Move the selection to the Recycle Bin");
    b.add("delete_forever","Shift+Delete",     "Delete the selection for good");
    b.add("rename",        "F2",               "Rename the selected item");
    b.add("batch_rename",  "Shift+F2",         "Rename the selected items by a pattern (F2 does this when several are selected)");
    b.add("disk_usage",    "Ctrl+Shift+U",     "Show where the space goes in this folder, as a treemap");
    b.add("find_duplicates", "",               "Find duplicate files in this folder and below");
    b.add("img_jpg",       "",                 "Save the selected pictures as JPEG copies");
    b.add("img_png",       "",                 "Save the selected pictures as PNG copies");
    b.add("img_50",        "",                 "Save the selected pictures at half the size");
    b.add("img_25",        "",                 "Save the selected pictures at a quarter of the size");
    b.add("img_1920",      "",                 "Save the selected pictures to fit 1920 pixels");
    b.add("img_left",      "",                 "Save the selected pictures turned left");
    b.add("img_right",     "",                 "Save the selected pictures turned right");
    b.add("empty_folders","",                 "Find empty folders in this folder and below, and recycle them");
    b.add("compress_zip", "",                 "Compress the selection into a zip file");
    b.add("extract_folder", "",                "Extract the selected archive into a folder of its name");
    b.add("extract_here",  "",                 "Extract the selected archive into this folder");
    b.add("link_symbolic", "",                 "Make a symbolic link to the selection (next to it, or in the other pane's folder)");
    b.add("link_hard",     "",                 "Make a hard link to the selected files");
    b.add("link_junction", "",                 "Make a junction to the selected folders");
    b.add("new_folder",    "Ctrl+Shift+N",     "Create a folder");
    b.add("new_file",      "",                 "Create an empty file");
    b.add("new_text",      "",                 "Create an empty text document");
    b.add("copy_path",     "Ctrl+Shift+C",     "Copy the full path of the selection");
    b.add("properties",    "Alt+Enter",        "Show properties");
    b.add("pin_folder",    "Ctrl+D",           "Pin this folder to the sidebar, or unpin it");
    b.add("save_search",   "Ctrl+Shift+S",     "Pin the current search to the sidebar");
    b.add("goto_folder",   "Ctrl+G",           "Jump to a pinned folder or a drive");
    b.add("undo",          "Ctrl+Z",           "Undo the last file operation");
    b.add("redo",          "Ctrl+Y",           "Redo what was undone");

    // colour labels: Alt+1 .. Alt+8 toggle a label on the selection, Alt+0 clears them
    for (int i = 0; i < core::tag::count; ++i) {
        const std::string name{core::tag::names[i]};
        b.add("tag_" + std::to_string(i + 1), "Alt+" + std::to_string(i + 1), "Toggle the " + name + " label on the selection");
    }
    b.add("tag_clear",     "Alt+0",            "Remove every label from the selection");

    // selection
    b.add("select_all",    "Ctrl+A",           "Select everything");
    b.add("select_invert", "Ctrl+I",           "Invert the selection");
    b.add("select_none",   "",                 "Clear the selection");

    // view
    b.add("group_none",    "",                 "Do not group the list");
    b.add("group_name",    "",                 "Group the list by first letter");
    b.add("group_type",    "",                 "Group the list by kind of file");
    b.add("group_modified","",                 "Group the list by date modified");
    b.add("group_size",    "",                 "Group the list by size");
    b.add("hidden",        "Ctrl+H",           "Show or hide hidden files");
    b.add("view_grid",     "Ctrl+Shift+G",     "Switch between list and thumbnails");
    b.add("density",       "Ctrl+Shift+M",     "Switch between comfortable and compact");
    b.add("split",         "F3",               "Show two folders side by side");
    b.add("switch_pane",   "Tab",              "Move between the two panes");
    b.add("compare_panes", "Ctrl+Shift+E",     "Compare the two panes' folders and copy what differs");
    b.add("copy_to_pane",  "Ctrl+Alt+C",       "Copy the selection to the other pane");
    b.add("move_to_pane",  "Ctrl+Alt+X",       "Move the selection to the other pane");
    b.add("preview",       "Alt+P",            "Show or hide the preview pane");
    b.add("quick_look",    "Space",            "Look at the selected file in a large floating view");
    b.add("settings",      "Ctrl+,",           "Open settings");
    b.add("palette",       "Ctrl+Shift+P",     "Search every command");
}

} // namespace prism::ui
