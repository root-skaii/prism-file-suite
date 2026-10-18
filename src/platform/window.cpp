#include "platform/window.hpp"

#include "core/text.hpp"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

namespace prism::platform {

namespace {

constexpr wchar_t window_class[] = L"prismfs_main_window";
constexpr UINT    wake_message   = WM_APP + 1;
constexpr int     resize_border  = 6; // logical pixels of grab area inside the edge
constexpr int     min_width      = 720;
constexpr int     min_height     = 440;

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

[[nodiscard]] HWND as_hwnd(void* h) noexcept { return static_cast<HWND>(h); }

} // namespace

Window::~Window()
{
    if (hwnd_ != nullptr) { ::DestroyWindow(as_hwnd(hwnd_)); }
}

bool Window::create(const Config& cfg)
{
    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = reinterpret_cast<WNDPROC>(&Window::proc);
    wc.hInstance     = inst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    // the icon embedded by prismfs.rc (id 1): taskbar, alt-tab and the system menu
    wc.hIcon         = ::LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm       = wc.hIcon;
    wc.lpszClassName = window_class;
    ::RegisterClassExW(&wc);

    // the requested size is logical: grown by the dpi of the monitor under the cursor, centred on its work area
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const HMONITOR mon = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi_x = 96;
    UINT dpi_y = 96;
    (void)::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    const float scale = static_cast<float>(dpi_x) / 96.0f;

    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    ::GetMonitorInfoW(mon, &mi);
    const int work_w = mi.rcWork.right - mi.rcWork.left;
    const int work_h = mi.rcWork.bottom - mi.rcWork.top;
    const int w      = std::min(static_cast<int>(std::lround(cfg.width * scale)), work_w);
    const int h      = std::min(static_cast<int>(std::lround(cfg.height * scale)), work_h);
    const int x      = mi.rcWork.left + (work_w - w) / 2;
    const int y      = mi.rcWork.top + (work_h - h) / 2;

    // popup + thick frame + caption: no visible frame (nc_calc_size removes it) but resize, snap, aero shadow and the
    // minimise / maximise animations keep working
    constexpr DWORD style = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    const std::wstring title = core::text::widen(cfg.title);
    hwnd_ = ::CreateWindowExW(0, window_class, title.c_str(), style, x, y, w, h, nullptr, nullptr, inst, this);
    if (hwnd_ == nullptr) { return false; }

    platform_.attach(hwnd_);

    const MARGINS shadow{1, 1, 1, 1};
    ::DwmExtendFrameIntoClientArea(as_hwnd(hwnd_), &shadow);
    const int round = 2; // DWMWCP_ROUND (windows 11; ignored elsewhere)
    ::DwmSetWindowAttribute(as_hwnd(hwnd_), DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof round);
    // the frame changed after creation: make the system recompute the client area now
    ::SetWindowPos(as_hwnd(hwnd_), nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void Window::show()
{
    ::ShowWindow(as_hwnd(hwnd_), SW_SHOW);
    ::UpdateWindow(as_hwnd(hwnd_));
}

bool Window::pump()
{
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { quit_ = true; }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return !quit_;
}

void Window::wait(std::uint32_t timeout_ms) const
{
    ::MsgWaitForMultipleObjectsEx(0, nullptr, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

void Window::wake() const noexcept
{
    if (hwnd_ != nullptr) { ::PostMessageW(as_hwnd(hwnd_), wake_message, 0, 0); }
}

std::function<void()> Window::waker() const
{
    HWND h = as_hwnd(hwnd_);
    return [h] { ::PostMessageW(h, wake_message, 0, 0); };
}

bool Window::maximized() const noexcept
{
    return hwnd_ != nullptr && ::IsZoomed(as_hwnd(hwnd_)) != FALSE;
}

bool Window::take_resized() noexcept
{
    return std::exchange(resized_, false);
}

float Window::take_dpi_change() noexcept
{
    return std::exchange(pending_dpi_, 0.0f);
}

void Window::minimize() const noexcept { ::ShowWindow(as_hwnd(hwnd_), SW_MINIMIZE); }

void Window::toggle_maximize() const noexcept
{
    ::ShowWindow(as_hwnd(hwnd_), maximized() ? SW_RESTORE : SW_MAXIMIZE);
}

void Window::request_close() const noexcept { ::PostMessageW(as_hwnd(hwnd_), WM_CLOSE, 0, 0); }

void Window::set_title(std::string_view utf8) const
{
    const std::wstring w = core::text::widen(utf8);
    ::SetWindowTextW(as_hwnd(hwnd_), w.c_str());
}

void Window::set_dark_frame(bool dark) const noexcept
{
    const BOOL v = dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(as_hwnd(hwnd_), DWMWA_USE_IMMERSIVE_DARK_MODE, &v, sizeof v);
}

std::intptr_t Window::nc_hit_test(std::intptr_t lparam) const
{
    HWND hwnd = as_hwnd(hwnd_);
    RECT wr{};
    ::GetWindowRect(hwnd, &wr);
    const int   x     = GET_X_LPARAM(lparam) - wr.left;
    const int   y     = GET_Y_LPARAM(lparam) - wr.top;
    const int   w     = wr.right - wr.left;
    const int   h     = wr.bottom - wr.top;
    const float scale = platform_.dpi_scale();

    if (!maximized()) {
        const int  b = std::max(4, static_cast<int>(std::lround(resize_border * scale)));
        const bool l = x < b;
        const bool r = x >= w - b;
        const bool t = y < b;
        const bool d = y >= h - b;
        if (t && l) { return HTTOPLEFT; }
        if (t && r) { return HTTOPRIGHT; }
        if (d && l) { return HTBOTTOMLEFT; }
        if (d && r) { return HTBOTTOMRIGHT; }
        if (l) { return HTLEFT; }
        if (r) { return HTRIGHT; }
        if (t) { return HTTOP; }
        if (d) { return HTBOTTOM; }
    }
    if (hit_test) {
        const float ls = logical_scale();
        switch (hit_test({static_cast<float>(x) / ls, static_cast<float>(y) / ls})) {
        case HitZone::caption:         return HTCAPTION;
        case HitZone::maximize_button: return HTMAXBUTTON;
        case HitZone::client:          break;
        }
    }
    return HTCLIENT;
}

std::intptr_t __stdcall Window::proc(void* hwnd, std::uint32_t msg, std::uintptr_t wparam, std::intptr_t lparam)
{
    HWND h    = as_hwnd(hwnd);
    auto* self = reinterpret_cast<Window*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self == nullptr) {
        if (msg == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return ::DefWindowProcW(h, msg, wparam, lparam);
    }
    return self->handle_message(hwnd, msg, wparam, lparam);
}

std::intptr_t Window::handle_message(void* hwnd, std::uint32_t msg, std::uintptr_t wparam, std::intptr_t lparam)
{
    HWND h = as_hwnd(hwnd);
    (void)platform_.handle_message(hwnd, msg, wparam, lparam);
    if (strata::win32_platform::swallows(msg)) { return 0; }

    switch (msg) {
    case WM_NCCALCSIZE:
        if (wparam != 0) {
            // the whole window is client area. maximised, the system overshoots the monitor by the frame width: take it back
            if (::IsZoomed(h)) {
                auto*      p   = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                const UINT dpi = ::GetDpiForWindow(h);
                const int  pad = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int  fx  = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi) + pad;
                const int  fy  = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi) + pad;
                p->rgrc[0].left += fx;
                p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy;
                p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        const std::intptr_t r = nc_hit_test(lparam);
        if (r != HTCLIENT) { return r; }
        return HTCLIENT;
    }
    case WM_NCACTIVATE:
        // (-1: do not repaint the frame we do not have)
        return ::DefWindowProcW(h, msg, wparam, -1);
    case WM_GETMINMAXINFO: {
        auto*       mm    = reinterpret_cast<MINMAXINFO*>(lparam);
        const float scale = platform_.dpi_scale();
        mm->ptMinTrackSize.x = static_cast<LONG>(min_width * scale);
        mm->ptMinTrackSize.y = static_cast<LONG>(min_height * scale);
        return 0;
    }
    case WM_NCMOUSEMOVE:
        if (wparam == HTMAXBUTTON) {
            if (!max_hover_) {
                max_hover_ = true;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE | TME_NONCLIENT, h, 0};
                ::TrackMouseEvent(&t);
            }
        } else {
            max_hover_ = false;
        }
        break;
    case WM_NCMOUSELEAVE:
        max_hover_ = false;
        max_down_  = false;
        break;
    case WM_NCLBUTTONDOWN:
        if (wparam == HTMAXBUTTON) {
            max_down_ = true;
            return 0; // (the system would run its own pressed-button loop)
        }
        break;
    case WM_NCLBUTTONUP:
        if (wparam == HTMAXBUTTON) {
            if (max_down_) { toggle_maximize(); }
            max_down_ = false;
            return 0;
        }
        break;
    case WM_NCRBUTTONUP:
        if (wparam == HTCAPTION) {
            const HMENU menu = ::GetSystemMenu(h, FALSE);
            const BOOL  cmd  = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), 0, h, nullptr);
            if (cmd != 0) { ::PostMessageW(h, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0); }
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE: sizing_ = true; break;
    case WM_EXITSIZEMOVE:  sizing_ = false; break;
    case WM_SIZE:
        minimized_ = wparam == SIZE_MINIMIZED;
        if (!minimized_) {
            resized_ = true;
            if (sizing_ && on_live_resize) { on_live_resize(); }
        }
        return 0;
    case WM_DPICHANGED: {
        pending_dpi_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
        const RECT* r = reinterpret_cast<const RECT*>(lparam);
        ::SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && platform_.apply_cursor()) { return TRUE; }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        if (on_close_request && !on_close_request()) { return 0; }
        quit_ = true;
        return 0;
    case wake_message:
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(h, msg, wparam, lparam);
}

} // namespace prism::platform
