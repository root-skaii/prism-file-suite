#include "platform/standby.hpp"

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <format>
#include <thread>

namespace prism::platform::standby {

namespace {

constexpr wchar_t     k_class[]      = L"prismfs_standby";
constexpr wchar_t     k_tray_class[] = L"prismfs_standby_tray";
constexpr ULONG_PTR   k_launch_id    = 0x50524c43; // WM_COPYDATA id of a launch
constexpr DWORD       k_timeout_ms   = 2000;
constexpr UINT        k_tray_msg     = WM_APP + 1;
constexpr UINT        k_cmd_open = 1, k_cmd_settings = 2, k_cmd_exit = 3;

// lower-case full path of this exe: the spare's window title, so only the same exe matches
[[nodiscard]] std::wstring exe_key()
{
    wchar_t exe[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s{exe, n < MAX_PATH ? n : 0};
    for (wchar_t& c : s) { c = static_cast<wchar_t>(::towlower(c)); }
    return s;
}

[[nodiscard]] HWND find_spare() { return ::FindWindowExW(HWND_MESSAGE, nullptr, k_class, exe_key().c_str()); }

struct State {
    const std::function<bool()>* ready{};
    const TrayLabels*            tray{};
    HWND                         msg_win{};
    HWND                         tray_win{};
    UINT                         taskbar_created{};
    std::wstring                 payload;
    bool                         claimed{};
};

// a launch of this very process: the spare becomes the window ("" = a plain new window)
void launch_self(State& s, const std::wstring& args)
{
    if (s.claimed) { return; }
    wchar_t dir[MAX_PATH]{};
    const DWORD n = ::GetCurrentDirectoryW(MAX_PATH, dir);
    wchar_t exe[MAX_PATH]{};
    const DWORD e = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    s.payload.assign(dir, n < MAX_PATH ? n : 0);
    s.payload.push_back(L'\0');
    s.payload += std::format(L"\"{}\" {}", std::wstring_view{exe, e < MAX_PATH ? e : 0}, args);
    s.claimed = true;
    ::PostMessageW(s.msg_win, WM_NULL, 0, 0);
}

// every PrismFS window of this exe (any process) is asked to close, so each saves its session
void close_all_windows()
{
    const std::wstring me = exe_key();
    ::EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        wchar_t cls[64]{};
        if (::GetClassNameW(top, cls, 64) == 0 || std::wstring_view{cls} != k_main_window_class) { return TRUE; }
        DWORD pid = 0;
        ::GetWindowThreadProcessId(top, &pid);
        if (pid == ::GetCurrentProcessId()) { return TRUE; }
        if (HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH]{};
            DWORD   len = MAX_PATH;
            if (::QueryFullProcessImageNameW(h, 0, path, &len)) {
                std::wstring p{path, len};
                for (wchar_t& c : p) { c = static_cast<wchar_t>(::towlower(c)); }
                if (p == *reinterpret_cast<const std::wstring*>(lp)) { ::PostMessageW(top, WM_CLOSE, 0, 0); }
            }
            ::CloseHandle(h);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&me));
}

NOTIFYICONDATAW icon_data(State& s)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof nid;
    nid.hWnd             = s.tray_win;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = k_tray_msg;
    nid.hIcon            = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0));
    wcsncpy_s(nid.szTip, s.tray->tip.c_str(), _TRUNCATE);
    return nid;
}

void add_icon(State& s)
{
    NOTIFYICONDATAW nid = icon_data(s);
    ::Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
    if (nid.hIcon != nullptr) { ::DestroyIcon(nid.hIcon); } // (the shell keeps its own copy)
}

void remove_icon(State& s)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd   = s.tray_win;
    nid.uID    = 1;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
}

void show_menu(State& s, POINT at)
{
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, k_cmd_open, s.tray->open.c_str());
    ::AppendMenuW(menu, MF_STRING, k_cmd_settings, s.tray->settings.c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, k_cmd_exit, s.tray->exit.c_str());
    ::SetMenuDefaultItem(menu, k_cmd_open, FALSE);
    ::SetForegroundWindow(s.tray_win); // (without it the menu does not close when clicking elsewhere)
    const UINT cmd = static_cast<UINT>(::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD, at.x, at.y, 0, s.tray_win, nullptr));
    ::PostMessageW(s.tray_win, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
    if (cmd == k_cmd_open) { launch_self(s, L""); }
    else if (cmd == k_cmd_settings) { launch_self(s, L"--settings"); }
    else if (cmd == k_cmd_exit) {
        close_all_windows();
        ::PostMessageW(s.msg_win, WM_CLOSE, 0, 0);
    }
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    }
    auto* s = reinterpret_cast<State*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_COPYDATA && s != nullptr) {
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        // one launch only, and not before the gpu is ready (the launcher then starts normally)
        if (cd != nullptr && cd->dwData == k_launch_id && cd->lpData != nullptr && !s->claimed && (*s->ready)()) {
            s->payload.assign(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t));
            s->claimed = true;
            ::PostMessageW(h, WM_NULL, 0, 0); // (a sent message does not end GetMessage: this does)
            return TRUE;
        }
        return FALSE;
    }
    if (msg == WM_CLOSE) {
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

// the icon's window: a real (hidden) top-level one, because only those get the broadcast that says explorer restarted
LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    }
    auto* s = reinterpret_cast<State*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (s != nullptr && msg == k_tray_msg) {
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: launch_self(*s, L""); break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: show_menu(*s, POINT{static_cast<LONG>(static_cast<short>(LOWORD(wp))), static_cast<LONG>(static_cast<short>(HIWORD(wp)))}); break;
        default: break;
        }
        return 0;
    }
    if (s != nullptr && msg == s->taskbar_created && msg != 0) {
        add_icon(*s);
        return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

bool hand_off(std::wstring_view command_line)
{
    const HWND spare = find_spare();
    if (spare == nullptr) { return false; }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(spare, &pid);
    ::AllowSetForegroundWindow(pid); // (this process was just started by the user: the new window may come to the front)

    wchar_t dir[MAX_PATH]{};
    const DWORD n = ::GetCurrentDirectoryW(MAX_PATH, dir);
    std::wstring payload{dir, n < MAX_PATH ? n : 0};
    payload.push_back(L'\0');
    payload.append(command_line);
    COPYDATASTRUCT cd{k_launch_id, static_cast<DWORD>(payload.size() * sizeof(wchar_t)), payload.data()};
    DWORD_PTR result = 0;
    if (::SendMessageTimeoutW(spare, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG | SMTO_BLOCK, k_timeout_ms, &result) == 0) { return false; }
    return result == TRUE;
}

void ensure(bool tray)
{
    if (find_spare() != nullptr) { return; }
    wchar_t exe[MAX_PATH]{};
    if (const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH); n == 0 || n >= MAX_PATH) { return; }
    std::wstring cmd = std::format(L"\"{}\" --standby{}", exe, tray ? L" --tray" : L"");
    STARTUPINFOW        si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // below normal: preparing must never compete with what the user is doing
    if (::CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
}

void stop(bool wait)
{
    const HWND spare = find_spare();
    if (spare == nullptr) { return; }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(spare, &pid);
    HANDLE proc_handle = wait ? ::OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    ::PostMessageW(spare, WM_CLOSE, 0, 0);
    if (proc_handle != nullptr) {
        ::WaitForSingleObject(proc_handle, 1500);
        ::CloseHandle(proc_handle);
    }
}

void restart(bool tray)
{
    std::thread{[tray] {
        stop(true);
        ensure(tray);
    }}.detach();
}

std::optional<Request> wait(const std::function<bool()>& ready, const TrayLabels* tray)
{
    // a second spare for the same exe (two windows started one after the other) leaves at once
    const std::wstring key = exe_key();
    std::uint64_t      h   = 1469598103934665603ull;
    for (const wchar_t c : key) { h = (h ^ static_cast<std::uint64_t>(c)) * 1099511628211ull; }
    HANDLE once = ::CreateMutexW(nullptr, FALSE, std::format(L"Local\\prismfs_standby_{:016x}", h).c_str());
    if (once == nullptr || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (once != nullptr) { ::CloseHandle(once); }
        return std::nullopt;
    }

    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = proc;
    wc.hInstance     = inst;
    wc.lpszClassName = k_class;
    ::RegisterClassExW(&wc);
    WNDCLASSEXW tc{};
    tc.cbSize        = sizeof tc;
    tc.lpfnWndProc   = tray_proc;
    tc.hInstance     = inst;
    tc.lpszClassName = k_tray_class;
    ::RegisterClassExW(&tc);

    State state;
    state.ready = &ready;
    state.tray  = tray;
    state.msg_win = ::CreateWindowExW(0, k_class, key.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, &state);
    std::optional<Request> out;
    if (state.msg_win != nullptr) {
        if (tray != nullptr) {
            state.taskbar_created = ::RegisterWindowMessageW(L"TaskbarCreated");
            state.tray_win        = ::CreateWindowExW(WS_EX_TOOLWINDOW, k_tray_class, L"PrismFS tray", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, &state);
            if (state.tray_win != nullptr) { add_icon(state); }
        }
        MSG m{};
        while (!state.claimed && ::GetMessageW(&m, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
        if (state.tray_win != nullptr) {
            remove_icon(state);
            ::DestroyWindow(state.tray_win);
        }
        ::DestroyWindow(state.msg_win);
    }
    ::UnregisterClassW(k_class, inst);
    ::UnregisterClassW(k_tray_class, inst);
    ::CloseHandle(once); // (released now: the launch that follows may start the next spare)
    if (state.claimed) {
        const std::size_t cut = state.payload.find(L'\0');
        if (cut != std::wstring::npos) { out = Request{state.payload.substr(0, cut), state.payload.substr(cut + 1)}; }
    }
    return out;
}

} // namespace prism::platform::standby
