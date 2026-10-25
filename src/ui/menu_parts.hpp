#pragma once

// the entries every context menu builds from, so the same thing is worded, ordered and behaves the same wherever it is
// offered (file list, path bar, sidebar, disk usage). Order inside a menu:
//   open  |  edit (cut / copy / paste, rename, delete)  |  terminal  |  path (copy path, reveal, pin)  |  properties  |  More options, Windows
// call inside an open menu

#include "core/shell_menu.hpp"
#include "ui/env.hpp"
#include "ui/shell_page.hpp"

#include <string>
#include <vector>

namespace prism::ui::menus {

// opens a file with its association; says so when Windows could not
void open_file(Env& env, const std::string& path);

// "Open in new tab" / "Open in new window" for a folder
void open_in(strata::context& ui, Env& env, const std::string& folder, bool enabled = true);

// "Open in terminal" submenu: the installed shells, then the administrator variants
void terminal(strata::context& ui, Env& env, const std::string& folder);

// "Paste into folder" from the clipboard
void paste_into(strata::context& ui, Env& env, const std::string& folder, bool clipboard_has_files);

// "Copy path" / "Copy paths" and "Show in File Explorer"
void path_items(strata::context& ui, Env& env, const std::vector<std::string>& paths);

// "Pin to sidebar" / "Unpin from sidebar" for one folder
void pin_item(strata::context& ui, Env& env, const std::string& folder);

// "Properties" (ours)
void properties_item(strata::context& ui, Env& env, const std::vector<std::string>& paths);

// the "More options" submenu for entries already read (empty list = no submenu)
void more_items(strata::context& ui, ShellPage& page, const std::vector<core::ShellMenuItem>& items);

// Explorer's own entries (7-Zip, Open with Code ...) in a "More options" submenu, when the setting is on and the worker has some
void more_options(strata::context& ui, Env& env, ShellPage& page, core::ShellMenu& menu, const std::vector<std::string>& paths, bool background);

// the way into Windows' own properties sheet and menu
void windows_items(strata::context& ui, Env& env, const std::vector<std::string>& paths);

// a folder the terminal can start in (a real folder, not "This PC", the Recycle Bin or an empty path)
[[nodiscard]] bool terminal_target(const std::string& folder);

} // namespace prism::ui::menus
