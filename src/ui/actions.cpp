#include "ui/actions.hpp"

#include "core/file_entry.hpp"
#include "ui/tr.hpp"

#include <string>

namespace prism::ui {

namespace {

// display names of the actions (the palette and the shortcut table show these instead of the ids)
constexpr struct { const char* name; const char* title; } k_titles[] = {
    {"new_tab", N_("New tab")},
    {"close_tab", N_("Close tab")},
    {"reopen_tab", N_("Reopen closed tab")},
    {"duplicate_tab", N_("Duplicate tab")},
    {"next_tab", N_("Next tab")},
    {"prev_tab", N_("Previous tab")},
    {"new_window", N_("New window")},
    {"sessions", N_("Sessions")},
    {"maximize", N_("Maximize window")},
    {"back", N_("Back")},
    {"forward", N_("Forward")},
    {"up", N_("Up")},
    {"home", N_("Home folder")},
    {"refresh", N_("Refresh")},
    {"address", N_("Edit path")},
    {"filter", N_("Filter")},
    {"search_everywhere", N_("Search everywhere")},
    {"terminal", N_("Open terminal")},
    {"copy", N_("Copy")},
    {"cut", N_("Cut")},
    {"paste", N_("Paste")},
    {"delete", N_("Delete")},
    {"delete_forever", N_("Delete permanently")},
    {"rename", N_("Rename")},
    {"batch_rename", N_("Batch rename")},
    {"disk_usage", N_("Disk usage")},
    {"find_duplicates", N_("Find duplicate files")},
    {"img_jpg", N_("Save as JPEG")},
    {"img_png", N_("Save as PNG")},
    {"img_50", N_("Half size")},
    {"img_25", N_("Quarter size")},
    {"img_1920", N_("Fit 1920 pixels")},
    {"img_left", N_("Rotate left")},
    {"img_right", N_("Rotate right")},
    {"empty_folders", N_("Find empty folders")},
    {"compress_zip", N_("Compress to zip")},
    {"extract_folder", N_("Extract to folder")},
    {"extract_here", N_("Extract here")},
    {"link_symbolic", N_("Create symbolic link")},
    {"link_hard", N_("Create hard link")},
    {"link_junction", N_("Create junction")},
    {"new_folder", N_("New folder")},
    {"new_file", N_("New file")},
    {"new_text", N_("New text document")},
    {"copy_path", N_("Copy path")},
    {"properties", N_("Properties")},
    {"pin_folder", N_("Pin to sidebar")},
    {"save_search", N_("Save search")},
    {"goto_folder", N_("Go to folder")},
    {"undo", N_("Undo")},
    {"redo", N_("Redo")},
    {"tag_clear", N_("Clear labels")},
    {"select_all", N_("Select all")},
    {"select_invert", N_("Invert selection")},
    {"select_none", N_("Select none")},
    {"group_none", N_("No grouping")},
    {"group_name", N_("Group by name")},
    {"group_type", N_("Group by type")},
    {"group_modified", N_("Group by date modified")},
    {"group_size", N_("Group by size")},
    {"hidden", N_("Show hidden files")},
    {"view_grid", N_("Thumbnails view")},
    {"density", N_("Compact rows")},
    {"split", N_("Two panes")},
    {"switch_pane", N_("Switch pane")},
    {"compare_panes", N_("Compare panes")},
    {"copy_to_pane", N_("Copy to other pane")},
    {"move_to_pane", N_("Move to other pane")},
    {"preview", N_("Preview pane")},
    {"quick_look", N_("Quick look")},
    {"settings", N_("Settings")},
    {"palette", N_("Command palette")},
};

} // namespace

void register_actions(strata::keybinds& b)
{
    // tabs and windows
    b.add("new_tab",       "Ctrl+T",           tr("Open a new tab here"));
    b.add("close_tab",     "Ctrl+W",           tr("Close this tab"));
    b.add("reopen_tab",    "Ctrl+Shift+T",     tr("Reopen the tab you closed last"));
    b.add("duplicate_tab", "Ctrl+Shift+D",     tr("Duplicate this tab"));
    b.add("next_tab",      "Ctrl+Tab",         tr("Next tab"));
    b.add("prev_tab",      "Ctrl+Shift+Tab",   tr("Previous tab"));
    b.add("new_window",    "Ctrl+N",           tr("Open this folder in a new window"));
    b.add("sessions",      "Ctrl+Shift+O",     tr("Save the open tabs as a session, or switch to a saved one"));
    b.add("maximize",      "F11",              tr("Maximize or restore the window"));

    // navigation
    b.add("back",          "Alt+Left",         tr("Go back"));
    b.add("forward",       "Alt+Right",        tr("Go forward"));
    b.add("up",            "Alt+Up",           tr("Go to the parent folder"));
    b.add("home",          "Alt+Home",         tr("Go to your home folder"));
    b.add("refresh",       "F5",               tr("Reload the folder"));
    b.add("address",       "Ctrl+L",           tr("Edit the path"));
    b.add("filter",        "Ctrl+F",           tr("Filter this folder; Ctrl+Enter searches subfolders"));
    b.add("search_everywhere", "Ctrl+Shift+F", tr("Search every drive by name, instantly (needs the drive index)"));
    b.add("terminal",     "Ctrl+Alt+T",       tr("Open a terminal in this folder"));

    // files
    b.add("copy",          "Ctrl+C",           tr("Copy the selection"));
    b.add("cut",           "Ctrl+X",           tr("Cut the selection"));
    b.add("paste",         "Ctrl+V",           tr("Paste into this folder"));
    b.add("delete",        "Delete",           tr("Move the selection to the Recycle Bin"));
    b.add("delete_forever","Shift+Delete",     tr("Delete the selection for good"));
    b.add("rename",        "F2",               tr("Rename the selected item"));
    b.add("batch_rename",  "Shift+F2",         tr("Rename the selected items by a pattern (Rename does this when several are selected)"));
    b.add("disk_usage",    "Ctrl+Shift+U",     tr("Show where the space goes in this folder, as a treemap"));
    b.add("find_duplicates", "",               tr("Find duplicate files in this folder and below"));
    b.add("img_jpg",       "",                 tr("Save the selected pictures as JPEG copies"));
    b.add("img_png",       "",                 tr("Save the selected pictures as PNG copies"));
    b.add("img_50",        "",                 tr("Save the selected pictures at half the size"));
    b.add("img_25",        "",                 tr("Save the selected pictures at a quarter of the size"));
    b.add("img_1920",      "",                 tr("Save the selected pictures to fit 1920 pixels"));
    b.add("img_left",      "",                 tr("Save the selected pictures turned left"));
    b.add("img_right",     "",                 tr("Save the selected pictures turned right"));
    b.add("empty_folders","",                 tr("Find empty folders in this folder and below, and recycle them"));
    b.add("compress_zip", "",                 tr("Compress the selection into a zip file"));
    b.add("extract_folder", "",                tr("Extract the selected archive into a folder of its name"));
    b.add("extract_here",  "",                 tr("Extract the selected archive into this folder"));
    b.add("link_symbolic", "",                 tr("Make a symbolic link to the selection (next to it, or in the other pane's folder)"));
    b.add("link_hard",     "",                 tr("Make a hard link to the selected files"));
    b.add("link_junction", "",                 tr("Make a junction to the selected folders"));
    b.add("new_folder",    "Ctrl+Shift+N",     tr("Create a folder"));
    b.add("new_file",      "",                 tr("Create an empty file"));
    b.add("new_text",      "",                 tr("Create an empty text document"));
    b.add("copy_path",     "Ctrl+Shift+C",     tr("Copy the full path of the selection"));
    b.add("properties",    "Alt+Enter",        tr("Show properties"));
    b.add("pin_folder",    "Ctrl+D",           tr("Pin this folder to the sidebar, or unpin it"));
    b.add("save_search",   "Ctrl+Shift+S",     tr("Pin the current search to the sidebar"));
    b.add("goto_folder",   "Ctrl+G",           tr("Jump to a pinned folder or a drive"));
    b.add("undo",          "Ctrl+Z",           tr("Undo the last file operation"));
    b.add("redo",          "Ctrl+Y",           tr("Redo what was undone"));

    // colour labels: Alt+1 .. Alt+8 toggle a label on the selection, Alt+0 clears them
    for (int i = 0; i < core::tag::count; ++i) {
        b.add("tag_" + std::to_string(i + 1), "Alt+" + std::to_string(i + 1), trf("Toggle the {} label on the selection", tag_label(i)));
        (void)b.set_title("tag_" + std::to_string(i + 1), trf("Label: {}", tag_label(i)));
    }
    b.add("tag_clear",     "Alt+0",            tr("Remove every label from the selection"));

    // selection
    b.add("select_all",    "Ctrl+A",           tr("Select everything"));
    b.add("select_invert", "Ctrl+I",           tr("Invert the selection"));
    b.add("select_none",   "",                 tr("Clear the selection"));

    // view
    b.add("group_none",    "",                 tr("Do not group the list"));
    b.add("group_name",    "",                 tr("Group the list by first letter"));
    b.add("group_type",    "",                 tr("Group the list by kind of file"));
    b.add("group_modified","",                 tr("Group the list by date modified"));
    b.add("group_size",    "",                 tr("Group the list by size"));
    b.add("hidden",        "Ctrl+H",           tr("Show or hide hidden files"));
    b.add("view_grid",     "Ctrl+Shift+G",     tr("Switch between list and thumbnails"));
    b.add("density",       "Ctrl+Shift+M",     tr("Switch between comfortable and compact"));
    b.add("split",         "F3",               tr("Show two folders side by side"));
    b.add("switch_pane",   "Tab",              tr("Move between the two panes"));
    b.add("compare_panes", "Ctrl+Shift+E",     tr("Compare the two panes' folders and copy what differs"));
    b.add("copy_to_pane",  "Ctrl+Alt+C",       tr("Copy the selection to the other pane"));
    b.add("move_to_pane",  "Ctrl+Alt+X",       tr("Move the selection to the other pane"));
    b.add("preview",       "Alt+P",            tr("Show or hide the preview pane"));
    b.add("quick_look",    "Space",            tr("Look at the selected file in a large floating view"));
    b.add("settings",      "Ctrl+,",           tr("Open settings"));
    b.add("palette",       "Ctrl+Shift+P",     tr("Search every command"));
    for (const auto& a : k_titles) { (void)b.set_title(a.name, tr(a.title)); }
}

} // namespace prism::ui
