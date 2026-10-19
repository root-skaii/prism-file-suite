#include "platform/instance.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdint>
#include <format>
#include <memory>

namespace prism::platform::instance {

namespace {

constexpr wchar_t     k_class[]      = L"prismfs_instance";
constexpr wchar_t     k_tray_class[] = L"prismfs_instance_tray";
constexpr ULONG_PTR   k_launch_id    = 0x50524c43; // WM_COPYDATA id of a launch
constexpr DWORD       k_timeout_ms   = 2000;
constexpr UINT        k_tray_msg     = WM_APP + 1;
constexpr UINT        k_cmd_open = 1, k_cmd_settings = 2, k_cmd_exit = 3;

HANDLE g_mutex = nullptr; // held for the life of the process

// lower-case full path of this exe: the instance window's title, so only the same exe matches
[[nodiscard]] std::wstring exe_key()
{
    wchar_t exe[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s{exe, n < MAX_PATH ? n : 0};
    for (wchar_t& c : s) { c = static_cast<wchar_t>(::towlower(c)); }
    if (::IsUserAnAdmin() != FALSE) { s += L"|admin"; } // (windows of different rights cannot talk to each other: each kind is its own instance)
    return s;
}

[[nodiscard]] HWND find_instance() { return ::FindWindowExW(HWND_MESSAGE, nullptr, k_class, exe_key().c_str()); }

} // namespace

struct Server::Impl {
    Callbacks         cb;
    HWND              msg_win{};
    HWND              tray_win{};
    UINT              taskbar_created{};
    TrayLabels        labels;
    bool              tray_on{};
    bool              class_registered{};

    NOTIFYICONDATAW icon_data() const
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize           = sizeof nid;
        nid.hWnd             = tray_win;
        nid.uID              = 1;
        nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = k_tray_msg;
        nid.hIcon            = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0));
        wcsncpy_s(nid.szTip, labels.tip.c_str(), _TRUNCATE);
        return nid;
    }

    void add_icon()
    {
        NOTIFYICONDATAW nid = icon_data();
        ::Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
        if (nid.hIcon != nullptr) { ::DestroyIcon(nid.hIcon); } // (the shell keeps its own copy)
    }

    void remove_icon()
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof nid;
        nid.hWnd   = tray_win;
        nid.uID    = 1;
        ::Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void show_menu(POINT at)
    {
        HMENU menu = ::CreatePopupMenu();
        ::AppendMenuW(menu, MF_STRING, k_cmd_open, labels.open.c_str());
        ::AppendMenuW(menu, MF_STRING, k_cmd_settings, labels.settings.c_str());
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, k_cmd_exit, labels.exit.c_str());
        ::SetMenuDefaultItem(menu, k_cmd_open, FALSE);
        ::SetForegroundWindow(tray_win); // (without it the menu does not close when clicking elsewhere)
        const UINT cmd = static_cast<UINT>(::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD, at.x, at.y, 0, tray_win, nullptr));
        ::PostMessageW(tray_win, WM_NULL, 0, 0);
        ::DestroyMenu(menu);
        if (cmd == k_cmd_open && cb.open) { cb.open(); }
        else if (cmd == k_cmd_settings && cb.settings) { cb.settings(); }
        else if (cmd == k_cmd_exit && cb.exit) { cb.exit(); }
    }

    static LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE) { ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams)); }
        auto* s = reinterpret_cast<Impl*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_COPYDATA && s != nullptr) {
            const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cd != nullptr && cd->dwData == k_launch_id && cd->lpData != nullptr && s->cb.launch) {
                const std::wstring payload{static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t)};
                if (const std::size_t cut = payload.find(L'\0'); cut != std::wstring::npos) {
                    s->cb.launch(Launch{payload.substr(0, cut), payload.substr(cut + 1)}); // (queued by the host: this is a sent message, the launcher waits for it)
                    return TRUE;
                }
            }
            return FALSE;
        }
        return ::DefWindowProcW(h, msg, wp, lp);
    }

    // the icon's window: a real (hidden) top-level one, because only those get the broadcast that says explorer restarted
    static LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE) { ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams)); }
        auto* s = reinterpret_cast<Impl*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (s != nullptr && msg == k_tray_msg) {
            switch (LOWORD(lp)) {
            case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP:
                if (s->cb.open) { s->cb.open(); }
                break;
            case WM_CONTEXTMENU: case WM_RBUTTONUP:
                s->show_menu(POINT{static_cast<LONG>(static_cast<short>(LOWORD(wp))), static_cast<LONG>(static_cast<short>(HIWORD(wp)))});
                break;
            default: break;
            }
            return 0;
        }
        if (s != nullptr && s->tray_on && msg != 0 && msg == s->taskbar_created) {
            s->add_icon();
            return 0;
        }
        return ::DefWindowProcW(h, msg, wp, lp);
    }
};

Server::~Server()
{
    if (impl_ == nullptr) { return; }
    set_tray(nullptr);
    if (impl_->msg_win != nullptr) { ::DestroyWindow(impl_->msg_win); }
    delete impl_;
}

bool Server::create(Callbacks callbacks)
{
    impl_ = new Impl;
    impl_->cb = std::move(callbacks);
    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = Impl::msg_proc;
    wc.hInstance     = inst;
    wc.lpszClassName = k_class;
    ::RegisterClassExW(&wc);
    WNDCLASSEXW tc{};
    tc.cbSize        = sizeof tc;
    tc.lpfnWndProc   = Impl::tray_proc;
    tc.hInstance     = inst;
    tc.lpszClassName = k_tray_class;
    ::RegisterClassExW(&tc);
    impl_->msg_win = ::CreateWindowExW(0, k_class, exe_key().c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, impl_);
    return impl_->msg_win != nullptr;
}

void Server::set_tray(const TrayLabels* labels)
{
    if (impl_ == nullptr) { return; }
    if (labels == nullptr) {
        if (impl_->tray_on) { impl_->remove_icon(); }
        impl_->tray_on = false;
        if (impl_->tray_win != nullptr) { ::DestroyWindow(impl_->tray_win); impl_->tray_win = nullptr; }
        return;
    }
    impl_->labels = *labels;
    if (impl_->tray_win == nullptr) {
        impl_->taskbar_created = ::RegisterWindowMessageW(L"TaskbarCreated");
        impl_->tray_win        = ::CreateWindowExW(WS_EX_TOOLWINDOW, k_tray_class, L"PrismFS tray", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, ::GetModuleHandleW(nullptr), impl_);
    }
    if (impl_->tray_win == nullptr) { return; }
    if (impl_->tray_on) {
        NOTIFYICONDATAW nid = impl_->icon_data(); // (new labels)
        ::Shell_NotifyIconW(NIM_MODIFY, &nid);
        if (nid.hIcon != nullptr) { ::DestroyIcon(nid.hIcon); }
    } else {
        impl_->add_icon();
        impl_->tray_on = true;
    }
}

bool hand_off(std::wstring_view command_line)
{
    const HWND running = find_instance();
    if (running == nullptr) { return false; }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(running, &pid);
    ::AllowSetForegroundWindow(pid); // (this process was just started by the user: the new window may come to the front)

    wchar_t dir[MAX_PATH]{};
    const DWORD n = ::GetCurrentDirectoryW(MAX_PATH, dir);
    std::wstring payload{dir, n < MAX_PATH ? n : 0};
    payload.push_back(L'\0');
    payload.append(command_line);
    COPYDATASTRUCT cd{k_launch_id, static_cast<DWORD>(payload.size() * sizeof(wchar_t)), payload.data()};
    DWORD_PTR result = 0;
    if (::SendMessageTimeoutW(running, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG | SMTO_BLOCK, k_timeout_ms, &result) == 0) { return false; }
    return result == TRUE;
}

bool claim()
{
    const std::wstring key = exe_key();
    std::uint64_t      h   = 1469598103934665603ull;
    for (const wchar_t c : key) { h = (h ^ static_cast<std::uint64_t>(c)) * 1099511628211ull; }
    g_mutex = ::CreateMutexW(nullptr, FALSE, std::format(L"Local\\prismfs_instance_{:016x}", h).c_str());
    if (g_mutex == nullptr) { return true; } // (cannot tell: behave as the only one)
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(g_mutex);
        g_mutex = nullptr;
        return false;
    }
    return true;
}

} // namespace prism::platform::instance
