#pragma once

// consoles started in a folder: Windows Terminal hosts them when it is installed, a plain console window otherwise

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::core::terminal {

enum class Shell { default_terminal, cmd, powershell, pwsh, wsl, bash };

struct Request {
    Shell       shell = Shell::default_terminal;
    std::string folder;   // "" = the program's own default
    std::string args;     // after the program name
    bool        admin = false;
};

// false when the shell is not installed (pwsh falls back to Windows PowerShell) or Windows refused to start it
[[nodiscard]] bool open(const Request& request);

// the shells to offer in a menu, in menu order. installed ones beyond what Windows always has are probed once on a worker:
// until that is done the list holds only the always-present ones
[[nodiscard]] std::vector<Shell> installed();

[[nodiscard]] std::string_view label(Shell shell);                    // English, for tr()
[[nodiscard]] std::optional<Shell> from_program(std::wstring_view lower_name); // "cmd", "pwsh" ... (no ".exe")

// wt.exe command line after the program name: `-d "folder" program args`. wt treats ';' as its own command separator and a
// trailing backslash would escape the closing quote
[[nodiscard]] std::wstring wt_arguments(std::wstring_view folder, std::wstring_view program, std::wstring_view args);

} // namespace prism::core::terminal
