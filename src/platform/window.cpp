#include "platform/window.hpp"

#include "core/crash.hpp"
#include "core/text.hpp"
#include "platform/instance.hpp"

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#else
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <cmath>

namespace prism::platform {

#ifdef _WIN32

namespace {

constexpr const wchar_t* window_class = instance::k_main_window_class;
constexpr UINT    wake_message   = WM_APP + 1;
constexpr ULONG_PTR hint_message = 0x50524954; // WM_COPYDATA id: "outline yourself, a tab is about to drop here" ('1') or not ('0')
constexpr wchar_t   ghost_class[] = L"prismfs_drag_ghost";
HWND                g_ghost{};          // the pointer card (one per process: only one drag runs at a time)
HWND                g_hinted{};         // the window last told to outline itself
std::wstring        g_ghost_title, g_ghost_action;
COLORREF            g_ghost_accent{};
bool                g_ghost_hot{};

bool face_exists(const wchar_t* name)
{
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, name, _TRUNCATE);
    bool found = false;
    HDC  dc    = ::GetDC(nullptr);
    ::EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *reinterpret_cast<bool*>(p) = true; return 0; },
                          reinterpret_cast<LPARAM>(&found), 0);
    ::ReleaseDC(nullptr, dc);
    return found;
}

// windows 11 has the variable font and Fluent icons, windows 10 Segoe UI and MDL2 (the same choice the ui makes)
const wchar_t* ghost_text_face()
{
    static const bool variable = face_exists(L"Segoe UI Variable Text");
    return variable ? L"Segoe UI Variable Text" : L"Segoe UI";
}
const wchar_t* ghost_icon_face()
{
    static const bool fluent = face_exists(L"Segoe Fluent Icons");
    return fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}

LRESULT CALLBACK ghost_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC         dc = ::BeginPaint(h, &ps);
        RECT        r{};
        ::GetClientRect(h, &r);
        const UINT  dpi = ::GetDpiForWindow(h);
        const int   pad = ::MulDiv(12, static_cast<int>(dpi), 96);
        HBRUSH      bg  = ::CreateSolidBrush(RGB(38, 38, 44));
        HPEN        pen = ::CreatePen(PS_SOLID, g_ghost_hot ? 2 : 1, g_ghost_hot ? g_ghost_accent : RGB(90, 90, 100));
        const HGDIOBJ ob = ::SelectObject(dc, bg);
        const HGDIOBJ op = ::SelectObject(dc, pen);
        const int rr = ::MulDiv(10, static_cast<int>(dpi), 96);
        ::RoundRect(dc, r.left, r.top, r.right, r.bottom, rr, rr);
        ::SelectObject(dc, ob);
        ::SelectObject(dc, op);
        ::DeleteObject(bg);
        ::DeleteObject(pen);
        ::SetBkMode(dc, TRANSPARENT);
        const auto font = [&](int pt, int weight) {
            return ::CreateFontW(-::MulDiv(pt, static_cast<int>(dpi), 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ghost_text_face());
        };
        HFONT big = font(13, FW_SEMIBOLD), dim = font(11, FW_NORMAL), icons = ::CreateFontW(-::MulDiv(18, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ghost_icon_face());
        const int icon_w = ::MulDiv(26, static_cast<int>(dpi), 96);
        HGDIOBJ prev = ::SelectObject(dc, icons);
        ::SetTextColor(dc, g_ghost_hot ? g_ghost_accent : RGB(230, 200, 110));
        RECT ir{r.left + pad, r.top, r.left + pad + icon_w, r.bottom};
        ::DrawTextW(dc, L"\xE8B7", -1, &ir, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX); // folder
        const int tx = r.left + pad + icon_w + ::MulDiv(6, static_cast<int>(dpi), 96);
        ::SelectObject(dc, big);
        ::SetTextColor(dc, RGB(240, 240, 245));
        RECT t1{tx, r.top + (g_ghost_action.empty() ? 0 : ::MulDiv(6, static_cast<int>(dpi), 96)), r.right - pad, g_ghost_action.empty() ? r.bottom : r.top + ::MulDiv(28, static_cast<int>(dpi), 96)};
        ::DrawTextW(dc, g_ghost_title.c_str(), -1, &t1, DT_SINGLELINE | (g_ghost_action.empty() ? DT_VCENTER : 0) | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!g_ghost_action.empty()) {
            ::SelectObject(dc, dim);
            ::SetTextColor(dc, g_ghost_hot ? g_ghost_accent : RGB(160, 160, 170));
            RECT t2{tx, r.top + ::MulDiv(28, static_cast<int>(dpi), 96), r.right - pad, r.bottom};
            ::DrawTextW(dc, g_ghost_action.c_str(), -1, &t2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        ::SelectObject(dc, prev);
        ::DeleteObject(big);
        ::DeleteObject(dim);
        ::DeleteObject(icons);
        ::EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) { return 1; }
    return ::DefWindowProcW(h, msg, wp, lp);
}

void send_hint(HWND to, bool on)
{
    if (to == nullptr) { return; }
    char flag = on ? '1' : '0';
    COPYDATASTRUCT cd{hint_message, 1, &flag};
    DWORD_PTR      res = 0;
    (void)::SendMessageTimeoutW(to, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 100, &res);
}
constexpr ULONG_PTR tab_message  = 0x50524953; // WM_COPYDATA id: a folder from a tab dragged out of another window
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

// the window's own rectangle, or where it sits when not maximised / minimised (a new window should not open full screen)
[[nodiscard]] RECT restored_rect(HWND hwnd)
{
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    RECT r{};
    if (::IsZoomed(hwnd) != FALSE || ::IsIconic(hwnd) != FALSE) {
        if (::GetWindowPlacement(hwnd, &wp) != FALSE) { r = wp.rcNormalPosition; }
    } else {
        ::GetWindowRect(hwnd, &r);
    }
    return r;
}

[[nodiscard]] RECT work_area_of(HMONITOR mon)
{
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    ::GetMonitorInfoW(mon, &mi);
    return mi.rcWork;
}

// every visible PrismFS window, top of the z-order first (the last one used leads)
[[nodiscard]] std::vector<RECT> prism_windows()
{
    std::vector<RECT> out;
    ::EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        wchar_t cls[64]{};
        if (::IsWindowVisible(top) != FALSE && ::IsIconic(top) == FALSE && ::GetClassNameW(top, cls, 64) != 0 && std::wstring_view{cls} == window_class) {
            reinterpret_cast<std::vector<RECT>*>(lp)->push_back(restored_rect(top));
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

// down and right of `base` by one step per try, skipping corners another window already holds; off the screen it
// starts the staircase again from the top-left
[[nodiscard]] POINT cascade_spot(const RECT& base, int w, int h, int step, const RECT& work, const std::vector<RECT>& others)
{
    POINT out{base.left, base.top};
    for (int n = 1; n <= 24; ++n) {
        int x = base.left + n * step;
        int y = base.top + n * step;
        if (x + w > work.right || y + h > work.bottom) {
            x = work.left + (n % 6) * step;
            y = work.top + (n % 6) * step;
        }
        out.x = std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - w));
        out.y = std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - h));
        const bool taken = std::ranges::any_of(others, [&](const RECT& c) { return std::abs(c.left - out.x) < step / 3 && std::abs(c.top - out.y) < step / 3; });
        if (!taken) { break; }
    }
    return out;
}

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
    wc.hIconSm       = static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (wc.hIconSm == nullptr) { wc.hIconSm = wc.hIcon; }
    wc.lpszClassName = window_class;
    ::RegisterClassExW(&wc);

    // the requested size is logical: grown by the dpi of the monitor under the cursor, centred on its work area
    POINT cursor{};
    ::GetCursorPos(&cursor);
    if (cfg.has_at) { cursor = {cfg.at_x, cfg.at_y}; }
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
    int x = mi.rcWork.left + (work_w - w) / 2;
    int y = mi.rcWork.top + (work_h - h) / 2;
    if (cfg.has_at) {
        x = std::clamp(cfg.at_x, static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - w);
        y = std::clamp(cfg.at_y, static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - h);
    } else if (const std::vector<RECT> open = prism_windows(); !open.empty()) {
        // another PrismFS window is open (a folder opened from Windows, a second launch): stagger from the front one instead of centring on top of it
        const RECT work = work_area_of(::MonitorFromPoint({open.front().left, open.front().top}, MONITOR_DEFAULTTONEAREST));
        const POINT p   = cascade_spot(open.front(), w, h, static_cast<int>(std::lround(32.0f * scale)), work, open);
        x = p.x;
        y = p.y;
    }

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
    ::SetForegroundWindow(as_hwnd(hwnd_)); // (a window the user just asked for opens on top; refused when this process has no right to it)
}

void Window::show_blank(std::uint32_t rgb)
{
    HWND h = as_hwnd(hwnd_);
    ::ShowWindow(h, SW_SHOW);
    ::SetForegroundWindow(h);
    RECT r{};
    ::GetClientRect(h, &r);
    if (HDC dc = ::GetDC(h)) {
        HBRUSH brush = ::CreateSolidBrush(RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff));
        ::FillRect(dc, &r, brush);
        ::DeleteObject(brush);
        ::ReleaseDC(h, dc);
    }
}

bool Window::pump_messages()
{
    bool alive = true;
    MSG  msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { alive = false; }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return alive;
}

void Window::wait_messages(std::uint32_t timeout_ms)
{
    ::MsgWaitForMultipleObjectsEx(0, nullptr, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

void Window::abandon() noexcept
{
    faulted_ = true;
    quit_    = true;
    if (HWND h = as_hwnd(hwnd_)) {
        ::ShowWindow(h, SW_HIDE);
        ::DestroyWindow(h);
    }
}

void Window::wake() const noexcept
{
    wake_handle(hwnd_);
}

void Window::wake_handle(void* hwnd) noexcept
{
    if (hwnd != nullptr) { ::PostMessageW(as_hwnd(hwnd), wake_message, 0, 0); }
}

std::function<void()> Window::waker() const
{
    HWND h = as_hwnd(hwnd_);
    return [h] { ::PostMessageW(h, wake_message, 0, 0); };
}

bool Window::contains_screen_point(int screen_x, int screen_y) const noexcept
{
    RECT r{};
    return ::GetWindowRect(as_hwnd(hwnd_), &r) != FALSE && screen_x >= r.left && screen_x < r.right && screen_y >= r.top && screen_y < r.bottom;
}

Window::Placement Window::cascaded_placement() const
{
    const HWND self = as_hwnd(hwnd_);
    const RECT r    = restored_rect(self);
    const RECT work = work_area_of(::MonitorFromWindow(self, MONITOR_DEFAULTTONEAREST));
    const int  w    = std::min(static_cast<int>(r.right - r.left), static_cast<int>(work.right - work.left));
    const int  h    = std::min(static_cast<int>(r.bottom - r.top), static_cast<int>(work.bottom - work.top));
    const POINT p   = cascade_spot(r, w, h, static_cast<int>(std::lround(32.0f * dpi_scale())), work, prism_windows());
    return {p.x, p.y, w, h};
}

Window::Placement Window::placement_at(int screen_x, int screen_y) const
{
    const RECT r    = restored_rect(as_hwnd(hwnd_));
    const RECT work = work_area_of(::MonitorFromPoint({screen_x, screen_y}, MONITOR_DEFAULTTONEAREST));
    const int  w    = std::min(static_cast<int>(r.right - r.left), static_cast<int>(work.right - work.left));
    const int  h    = std::min(static_cast<int>(r.bottom - r.top), static_cast<int>(work.bottom - work.top));
    // (the pointer ends up on the new window's tab strip, about where it held the tab)
    const int x = screen_x - static_cast<int>(std::lround(90.0f * dpi_scale()));
    const int y = screen_y - static_cast<int>(std::lround(20.0f * dpi_scale()));
    return {std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - w)),
            std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - h)), w, h};
}

std::pair<int, int> Window::logical_size() const
{
    const RECT r = restored_rect(as_hwnd(hwnd_));
    const float s = dpi_scale() > 0.0f ? dpi_scale() : 1.0f;
    return {static_cast<int>(std::lround(static_cast<float>(r.right - r.left) / s)), static_cast<int>(std::lround(static_cast<float>(r.bottom - r.top) / s))};
}

void* Window::prism_window_at(int screen_x, int screen_y) const
{
    const HWND top = ::GetAncestor(::WindowFromPoint({screen_x, screen_y}), GA_ROOT);
    wchar_t    cls[64]{};
    if (top == nullptr || top == as_hwnd(hwnd_) || ::GetClassNameW(top, cls, 64) == 0 || std::wstring_view{cls} != window_class) { return nullptr; }
    return top;
}

bool Window::send_folder_to_window_at(int screen_x, int screen_y, std::string_view folder) const
{
    const auto top = static_cast<HWND>(prism_window_at(screen_x, screen_y));
    if (top == nullptr) { return false; }
    COPYDATASTRUCT cd{tab_message, static_cast<DWORD>(folder.size()), const_cast<char*>(folder.data())};
    return ::SendMessageW(top, WM_COPYDATA, reinterpret_cast<WPARAM>(hwnd_), reinterpret_cast<LPARAM>(&cd)) != 0;
}

void Window::show_drag_ghost(std::string_view title, std::string_view action, int screen_x, int screen_y, std::uint32_t accent_rgb, bool accent) const
{
    if (g_ghost == nullptr) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = ghost_proc;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = ghost_class;
        ::RegisterClassExW(&wc);
        g_ghost = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, ghost_class, L"", WS_POPUP,
                                    0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
        if (g_ghost == nullptr) { return; }
        ::SetLayeredWindowAttributes(g_ghost, 0, 235, LWA_ALPHA);
    }
    const std::wstring t = core::text::widen(title), a = core::text::widen(action);
    const COLORREF col = RGB((accent_rgb >> 16) & 0xff, (accent_rgb >> 8) & 0xff, accent_rgb & 0xff);
    const bool changed = t != g_ghost_title || a != g_ghost_action || accent != g_ghost_hot || col != g_ghost_accent;
    g_ghost_title = t; g_ghost_action = a; g_ghost_hot = accent; g_ghost_accent = col;

    UINT dpi_x = 96, dpi_y = 96;
    if (HMONITOR mon = ::MonitorFromPoint({screen_x, screen_y}, MONITOR_DEFAULTTONEAREST)) {
        (void)::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    }
    const UINT dpi = dpi_x > 0 ? dpi_x : ::GetDpiForWindow(as_hwnd(hwnd_));
    HDC        dc  = ::GetDC(g_ghost);
    HFONT      f   = ::CreateFontW(-::MulDiv(13, static_cast<int>(dpi), 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, ghost_text_face());
    HGDIOBJ    old = ::SelectObject(dc, f);
    SIZE       s1{}, s2{};
    ::GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &s1);
    ::GetTextExtentPoint32W(dc, a.c_str(), static_cast<int>(a.size()), &s2);
    ::SelectObject(dc, old);
    ::DeleteObject(f);
    ::ReleaseDC(g_ghost, dc);
    const int w = std::min(::MulDiv(340, static_cast<int>(dpi), 96), ::MulDiv(12 + 26 + 6 + 14, static_cast<int>(dpi), 96) + static_cast<int>(std::max(s1.cx, s2.cx)));
    const int h = ::MulDiv(a.empty() ? 40 : 54, static_cast<int>(dpi), 96);
    ::SetWindowPos(g_ghost, HWND_TOPMOST, screen_x + ::MulDiv(16, static_cast<int>(dpi), 96), screen_y + ::MulDiv(18, static_cast<int>(dpi), 96), w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (changed) { ::InvalidateRect(g_ghost, nullptr, FALSE); }
}

void Window::hide_drag_ghost() const
{
    if (g_ghost != nullptr) { ::ShowWindow(g_ghost, SW_HIDE); }
}

bool Window::hint_window_at(int screen_x, int screen_y) const
{
    const HWND top = ::GetAncestor(::WindowFromPoint({screen_x, screen_y}), GA_ROOT);
    wchar_t    cls[64]{};
    const bool ours = top != nullptr && top != as_hwnd(hwnd_) && ::GetClassNameW(top, cls, 64) != 0 && std::wstring_view{cls} == window_class;
    HWND       now  = ours ? top : nullptr;
    if (now != g_hinted) {
        send_hint(g_hinted, false);
        send_hint(now, true);
        g_hinted = now;
    }
    return ours;
}

void Window::clear_window_hint() const
{
    send_hint(g_hinted, false);
    g_hinted = nullptr;
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
    if (self->faulted_) { return ::DefWindowProcW(h, msg, wparam, lparam); }
    // all of this window's code (the ui callbacks) runs from here: a fault drops this window, not the others
    std::intptr_t result = 0;
    if (!core::crash::guard_window("window message", [&] { result = self->handle_message(hwnd, msg, wparam, lparam); })) {
        self->faulted_ = true;
        self->quit_    = true;
        return 0;
    }
    return result;
}

std::intptr_t Window::handle_message(void* hwnd, std::uint32_t msg, std::uintptr_t wparam, std::intptr_t lparam)
{
    HWND h = as_hwnd(hwnd);
    activity_ = true;
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
                // leave a 1 px hover edge when an auto-hide taskbar sits on this monitor so the user can summon it
                APPBARDATA abd{};
                abd.cbSize = sizeof abd;
                if ((::SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0 && ::SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
                    const HMONITOR mon = ::MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
                    const HMONITOR tb  = ::MonitorFromRect(&abd.rc, MONITOR_DEFAULTTONEAREST);
                    if (mon == tb) {
                        switch (abd.uEdge) {
                        case ABE_BOTTOM: p->rgrc[0].bottom -= 1; break;
                        case ABE_TOP:    p->rgrc[0].top    += 1; break;
                        case ABE_LEFT:   p->rgrc[0].left   += 1; break;
                        case ABE_RIGHT:  p->rgrc[0].right  -= 1; break;
                        default: break;
                        }
                    }
                }
            }
            return 0;
        }
        break;
    case WM_APPCOMMAND: {
        const UINT cmd = GET_APPCOMMAND_LPARAM(lparam);
        if (cmd == APPCOMMAND_BROWSER_BACKWARD) { app_command_ = -1; return TRUE; }
        if (cmd == APPCOMMAND_BROWSER_FORWARD)  { app_command_ = 1;  return TRUE; }
        if (cmd == APPCOMMAND_BROWSER_REFRESH)  { app_command_ = 2;  return TRUE; }
        break;
    }
    case WM_NCHITTEST: {
        const std::intptr_t r = nc_hit_test(lparam);
        if (r != HTCLIENT) { return r; }
        return HTCLIENT;
    }
    case WM_NCACTIVATE:
        active_ = wparam != 0;
        // (-1: do not repaint the frame we do not have)
        return ::DefWindowProcW(h, msg, wparam, -1);
    case WM_GETMINMAXINFO: {
        auto*       mm    = reinterpret_cast<MINMAXINFO*>(lparam);
        const UINT  dpi   = ::GetDpiForWindow(h);
        const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : platform_.dpi_scale();
        mm->ptMinTrackSize.x = static_cast<LONG>((min_w_ > 0 ? min_w_ : min_width) * scale);
        mm->ptMinTrackSize.y = static_cast<LONG>((min_h_ > 0 ? min_h_ : min_height) * scale);
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
    case WM_COPYDATA: {
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (cd != nullptr && cd->dwData == hint_message && cd->lpData != nullptr && cd->cbData > 0) {
            drop_hint_ = static_cast<const char*>(cd->lpData)[0] == '1';
            return TRUE;
        }
        if (cd != nullptr && cd->dwData == tab_message && cd->lpData != nullptr) {
            incoming_.emplace_back(static_cast<const char*>(cd->lpData), cd->cbData);
            ::SetForegroundWindow(h);
            if (::IsIconic(h)) { ::ShowWindow(h, SW_RESTORE); }
            return TRUE;
        }
        break;
    }
    case wake_message:
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(h, msg, wparam, lparam);
}
#else
namespace {
bool ensure_glfw()
{
    static const bool ok = ::glfwInit() == GLFW_TRUE;
    return ok;
}
} // namespace

Window::~Window()
{
    if (hwnd_ != nullptr) {
        platform_.detach();
        ::glfwDestroyWindow(static_cast<GLFWwindow*>(hwnd_));
        hwnd_ = nullptr;
    }
}

bool Window::create(const Config& cfg)
{
    if (!ensure_glfw()) { return false; }
    ::glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    ::glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    ::glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    ::glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    const std::string title{cfg.title};
    GLFWwindow* win = ::glfwCreateWindow(cfg.width, cfg.height, title.c_str(), nullptr, nullptr);
    if (win == nullptr) { return false; }
    hwnd_ = win;
    ::glfwSetWindowUserPointer(win, this);
    platform_.attach(win);

    ::glfwSetKeyCallback(win, [](GLFWwindow* w, int key, int sc, int act, int mods) {
        auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
        self->activity_ = true;
        self->platform_.on_key(key, sc, act, mods);
    });
    ::glfwSetCharCallback(win, [](GLFWwindow* w, unsigned int cp) {
        auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
        self->activity_ = true;
        self->platform_.on_char(cp);
    });
    ::glfwSetCursorPosCallback(win, [](GLFWwindow* w, double x, double y) {
        auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
        self->activity_ = true;
        self->platform_.on_cursor_pos(x, y);
    });
    ::glfwSetMouseButtonCallback(win, [](GLFWwindow* w, int btn, int act, int mods) {
        auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
        self->activity_ = true;
        self->platform_.on_mouse_button(btn, act, mods);
    });
    ::glfwSetScrollCallback(win, [](GLFWwindow* w, double x, double y) {
        auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
        self->activity_ = true;
        self->platform_.on_scroll(x, y);
    });
    ::glfwSetFramebufferSizeCallback(win, [](GLFWwindow* w, int width, int height) {
        auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
        self->activity_  = true;
        self->resized_   = true;
        self->minimized_ = (width == 0 || height == 0);
    });
    ::glfwSetWindowFocusCallback(win, [](GLFWwindow* w, int focused) {
        auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
        self->activity_ = true;
        self->active_   = focused == GLFW_TRUE;
    });
    ::glfwSetWindowCloseCallback(win, [](GLFWwindow* w) {
        auto* self = static_cast<Window*>(::glfwGetWindowUserPointer(w));
        if (self->on_close_request && !self->on_close_request()) {
            ::glfwSetWindowShouldClose(w, GLFW_FALSE);
            return;
        }
        self->quit_ = true;
    });
    if (cfg.has_at) { ::glfwSetWindowPos(win, cfg.at_x, cfg.at_y); }
    return true;
}

void Window::show()
{
    if (hwnd_ != nullptr) { ::glfwShowWindow(static_cast<GLFWwindow*>(hwnd_)); }
}

void Window::show_blank(std::uint32_t) { show(); }

bool Window::pump_messages()
{
    ::glfwPollEvents();
    return true;
}

void Window::wait_messages(std::uint32_t timeout_ms)
{
    if (timeout_ms == 0xffffffffu) { ::glfwWaitEvents(); }
    else { ::glfwWaitEventsTimeout(static_cast<double>(timeout_ms) / 1000.0); }
}

void Window::abandon() noexcept
{
    faulted_ = true;
    quit_    = true;
    if (hwnd_ != nullptr) { ::glfwHideWindow(static_cast<GLFWwindow*>(hwnd_)); }
}

void Window::wake() const noexcept { ::glfwPostEmptyEvent(); }
void Window::wake_handle(void*) noexcept { ::glfwPostEmptyEvent(); }
std::function<void()> Window::waker() const { return [] { ::glfwPostEmptyEvent(); }; }

bool Window::maximized() const noexcept
{
    return hwnd_ != nullptr && ::glfwGetWindowAttrib(static_cast<GLFWwindow*>(hwnd_), GLFW_MAXIMIZED) == GLFW_TRUE;
}

bool Window::take_resized() noexcept { return std::exchange(resized_, false); }
float Window::take_dpi_change() noexcept { return std::exchange(pending_dpi_, 0.0f); }

void Window::minimize() const noexcept
{
    if (hwnd_ != nullptr) { ::glfwIconifyWindow(static_cast<GLFWwindow*>(hwnd_)); }
}

void Window::toggle_maximize() const noexcept
{
    if (hwnd_ == nullptr) { return; }
    auto* w = static_cast<GLFWwindow*>(hwnd_);
    if (maximized()) { ::glfwRestoreWindow(w); } else { ::glfwMaximizeWindow(w); }
}

void Window::request_close() const noexcept
{
    if (hwnd_ != nullptr) { ::glfwSetWindowShouldClose(static_cast<GLFWwindow*>(hwnd_), GLFW_TRUE); }
}

void Window::set_title(std::string_view utf8) const
{
    if (hwnd_ != nullptr) { ::glfwSetWindowTitle(static_cast<GLFWwindow*>(hwnd_), std::string{utf8}.c_str()); }
}

void Window::set_dark_frame(bool) const noexcept {}
void* Window::prism_window_at(int, int) const { return nullptr; }
bool Window::send_folder_to_window_at(int, int, std::string_view) const { return false; }
bool Window::contains_screen_point(int, int) const noexcept { return false; }

Window::Placement Window::cascaded_placement() const
{
    Placement p{80, 80, 1360, 840};
    if (hwnd_ != nullptr) {
        auto* w = static_cast<GLFWwindow*>(hwnd_);
        ::glfwGetWindowPos(w, &p.x, &p.y);
        ::glfwGetWindowSize(w, &p.w, &p.h);
        p.x += 32; p.y += 32;
    }
    return p;
}

Window::Placement Window::placement_at(int screen_x, int screen_y) const
{
    Placement p{screen_x, screen_y, 1360, 840};
    if (hwnd_ != nullptr) { ::glfwGetWindowSize(static_cast<GLFWwindow*>(hwnd_), &p.w, &p.h); }
    return p;
}

std::pair<int, int> Window::logical_size() const
{
    int w = 1360, h = 840;
    if (hwnd_ != nullptr) { ::glfwGetWindowSize(static_cast<GLFWwindow*>(hwnd_), &w, &h); }
    return {w, h};
}

void Window::show_drag_ghost(std::string_view, std::string_view, int, int, std::uint32_t, bool) const {}
void Window::hide_drag_ghost() const {}
bool Window::hint_window_at(int, int) const { return false; }
void Window::clear_window_hint() const {}
#endif

} // namespace prism::platform
