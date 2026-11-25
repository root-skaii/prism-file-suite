#include "core/terminal.hpp"

#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#endif

#include <atomic>
#include <thread>

namespace prism::core::terminal {

#ifdef _WIN32

namespace {

struct Probe {
    std::atomic<int> state{0}; // 0 not started, 1 running, 2 done (the flags below are written before the release store)
    bool             wt{}, pwsh{}, wsl{}, bash{};
};
Probe g_probe;

[[nodiscard]] bool on_path(const wchar_t* exe)
{
    return ::SearchPathW(nullptr, exe, nullptr, 0, nullptr, nullptr) > 0;
}

[[nodiscard]] std::wstring wt_path()
{
    wchar_t local[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) { return {}; }
    return std::wstring{local} + L"\\Microsoft\\WindowsApps\\wt.exe";
}

[[nodiscard]] bool wt_exists()
{
    const std::wstring wt = wt_path();
    return !wt.empty() && ::GetFileAttributesW(wt.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void run_probe()
{
    g_probe.wt   = wt_exists();
    g_probe.pwsh = on_path(L"pwsh.exe");
    g_probe.wsl  = on_path(L"wsl.exe");
    g_probe.bash = on_path(L"bash.exe");
    g_probe.state.store(2, std::memory_order_release);
}

// PATH can hold slow (network) folders: the probe runs off the calling thread
void start_probe()
{
    int expected = 0;
    if (g_probe.state.compare_exchange_strong(expected, 1)) { std::thread{run_probe}.detach(); }
}

[[nodiscard]] bool probe_done() noexcept { return g_probe.state.load(std::memory_order_acquire) == 2; }

[[nodiscard]] bool has_wt() { return probe_done() ? g_probe.wt : wt_exists(); }

[[nodiscard]] bool has(Shell s)
{
    switch (s) {
    case Shell::default_terminal:
    case Shell::cmd:
    case Shell::powershell: return true; // (System32)
    case Shell::pwsh:       return probe_done() ? g_probe.pwsh : on_path(L"pwsh.exe");
    case Shell::wsl:        return probe_done() ? g_probe.wsl : on_path(L"wsl.exe");
    case Shell::bash:       return probe_done() ? g_probe.bash : on_path(L"bash.exe");
    }
    return false;
}

[[nodiscard]] std::wstring program_of(Shell s)
{
    switch (s) {
    case Shell::cmd:        return L"cmd";
    case Shell::powershell: return L"powershell";
    case Shell::pwsh:       return L"pwsh";
    case Shell::wsl:        return L"wsl";
    case Shell::bash:       return L"bash";
    case Shell::default_terminal: break;
    }
    return {};
}

// wt.exe is a Store alias: ShellExecute reports success for it and starts nothing, CreateProcess on the alias itself works
[[nodiscard]] bool start_in_wt(const std::wstring& dir, const std::wstring& program, const std::wstring& args)
{
    const std::wstring exe = wt_path();
    if (exe.empty()) { return false; }
    std::wstring cmdline = L"\"" + exe + L"\" " + wt_arguments(dir, program, args);
    STARTUPINFOW        si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { return false; }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

} // namespace

std::wstring wt_arguments(std::wstring_view folder, std::wstring_view program, std::wstring_view args)
{
    std::wstring out;
    if (!folder.empty()) {
        out += L"-d \"";
        for (const wchar_t c : folder) {
            if (c == L';') { out += L'\\'; }
            out += c;
        }
        if (folder.back() == L'\\') { out += L'\\'; }
        out += L"\"";
    }
    if (!program.empty()) {
        if (!out.empty()) { out += L' '; }
        out += program;
    }
    if (!args.empty()) {
        if (!out.empty()) { out += L' '; }
        out += args;
    }
    return out;
}

bool open(const Request& r)
{
    Shell shell = r.shell;
    if (shell == Shell::pwsh && !has(Shell::pwsh)) { shell = Shell::powershell; }
    if (!has(shell)) { return false; }
    const std::wstring dir  = text::widen(r.folder);
    const std::wstring args = text::widen(r.args);
    const std::wstring prog = program_of(shell);

    if (!r.admin && has_wt() && start_in_wt(dir, prog, args)) { return true; }

    // a plain console window; also the way to elevate (`runas` on the wt.exe alias asks for permission and starts nothing)
    const std::wstring exe = (prog.empty() ? std::wstring{L"cmd"} : prog) + L".exe";
    const auto         r2  = ::ShellExecuteW(nullptr, r.admin ? L"runas" : L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r2) > 32;
}

std::vector<Shell> installed()
{
    start_probe();
    std::vector<Shell> out;
    if (has_wt()) { out.push_back(Shell::default_terminal); }
    out.push_back(Shell::cmd);
    out.push_back(Shell::powershell);
    if (probe_done()) {
        if (g_probe.pwsh) { out.push_back(Shell::pwsh); }
        if (g_probe.wsl) { out.push_back(Shell::wsl); }
    }
    return out;
}

std::string_view label(Shell shell)
{
    switch (shell) {
    case Shell::default_terminal: return "Windows Terminal";
    case Shell::cmd:              return "Command Prompt";
    case Shell::powershell:       return "Windows PowerShell";
    case Shell::pwsh:             return "PowerShell 7";
    case Shell::wsl:              return "WSL";
    case Shell::bash:             return "Bash";
    }
    return {};
}

std::optional<Shell> from_program(std::wstring_view name)
{
    if (name == L"cmd") { return Shell::cmd; }
    if (name == L"powershell") { return Shell::powershell; }
    if (name == L"pwsh") { return Shell::pwsh; }
    if (name == L"wsl") { return Shell::wsl; }
    if (name == L"bash") { return Shell::bash; }
    return std::nullopt;
}
#else
bool open(const Request& req)
{
    const std::string dir = req.folder.empty() ? std::string{"$HOME"} : req.folder;
    const std::string cmd = "cd \"" + dir + "\" && (x-terminal-emulator || gnome-terminal || konsole || xterm) >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
}
std::vector<Shell> installed() { return {Shell::default_terminal, Shell::bash}; }
std::string_view label(Shell shell)
{
    switch (shell) {
    case Shell::default_terminal: return "Terminal";
    case Shell::bash:             return "Bash";
    default:                      return "Shell";
    }
}
std::optional<Shell> from_program(std::wstring_view name) { return name == L"bash" ? std::optional{Shell::bash} : std::nullopt; }
std::wstring wt_arguments(std::wstring_view, std::wstring_view, std::wstring_view) { return {}; }
#endif

} // namespace prism::core::terminal
