#pragma once

// things only the shell can do: open a file with its association, properties, clipboard text

#include <string>
#include <string_view>

namespace prism::core::shell {

bool open(const std::string& path);                 // default verb: folders are not passed here, the app navigates
bool show_properties(const std::string& path);
bool reveal_in_explorer(const std::string& path);   // opens explorer.exe with the item selected
bool set_clipboard_text(std::string_view utf8);
std::string get_clipboard_text();                   // "" when there is none

} // namespace prism::core::shell
