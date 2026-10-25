#include "app/application.hpp"

#include "core/crash.hpp"
#include "core/default_manager.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/places.hpp"
#include "ui/actions.hpp"
#include "ui/ui_strings.hpp"
#include "core/text.hpp"
#include "platform/dnd.hpp"
#include "strata/frame.hpp"

#include <windows.h>
#include <objbase.h>
#include <ole2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <thread>

namespace prism::app {

void Application::prewarm()
{
    if (device_thread_.joinable()) { return; }
    // loading the gpu driver takes ~180 ms and needs nothing else: it starts first and everything else runs beside it
    device_thread_ = std::jthread{[this] {
        device_ok_ = gpu_.create_device();
        device_ready_.store(true, std::memory_order_release);
    }};
}

bool Application::init(const Options& opts)
{
    const std::string& start_path = opts.path;
    auto lap_t = std::chrono::steady_clock::now();
    // how long each start-up step took (the log is for finding the slow one)
    const auto lap = [&lap_t](const char* what) {
        const auto now = std::chrono::steady_clock::now();
        core::log::info("startup: {} {:.0f} ms", what, std::chrono::duration<double, std::milli>(now - lap_t).count());
        lap_t = std::chrono::steady_clock::now();
    };
    prewarm();
    // under full cpu load a normal-priority ui thread shares time evenly with the hogs (the "lag when the pc is busy"); an
    // interactive app wants to run first, and must opt out of efficiency-mode throttling (EcoQoS) to be sure of it
    ::SetPriorityClass(::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE throttle{PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
    ::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &throttle, sizeof throttle);
    lang_revision_ = core::i18n::revision(); // (the host registered the actions for this language already)
    crash_test_window_ = opts.crash_test_window;
    if (opts.light) { settings_.dark = false; }
    if (opts.compact) { settings_.compact = true; }
    if (opts.grid) { settings_.grid_view = true; }
    if (opts.preview) { settings_.preview = true; }
    if (opts.split) { settings_.split = true; }
    theme_.set_density(settings_.compact ? ui::Density::compact : ui::Density::comfortable);
    theme_.set_accent(strata::color{static_cast<strata::u8>(settings_.accent >> 16), static_cast<strata::u8>(settings_.accent >> 8), static_cast<strata::u8>(settings_.accent), 255});
    theme_.set_dark_style(static_cast<ui::DarkStyle>(settings_.dark_style));
    theme_.set_mode(settings_.dark ? ui::ThemeMode::dark : ui::ThemeMode::light);
    scale_pct_ = settings_.ui_scale;
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    (void)::OleInitialize(nullptr); // (a single-threaded apartment, plus what drag and drop needs)
    lap("ole / dpi setup");
    core::refresh_default_manager(); // (the folder was moved: opening folders for other apps must still find this exe)
    lap("default folder handler (registry)");

    const int win_w = opts.width > 0 ? opts.width : settings_.window_w > 0 ? settings_.window_w : 1360;
    const int win_h = opts.height > 0 ? opts.height : settings_.window_h > 0 ? settings_.window_h : 840;
    if (!window_.create({.title = "PrismFS", .width = win_w, .height = win_h, .has_at = opts.has_at, .at_x = opts.at_x, .at_y = opts.at_y})) { return false; }

    {
        const strata::color bg = theme_.pal().window; // (something on screen at once; the first real frame replaces it)
        window_.show_blank((static_cast<std::uint32_t>(bg.r) << 16) | (static_cast<std::uint32_t>(bg.g) << 8) | bg.b);
    }
    lap("window");

    // the folder listing, the d3d device and the font bake do not depend on each other: run them side by side
    ops_  = std::make_unique<core::FileOps>(window_.waker(), window_.handle());
    tabs_ = std::make_unique<model::TabSet>(window_.waker());
    if (!start_path.empty()) {
        const std::string first = core::path::normalize(start_path);
        tabs_->open(core::path::is_directory(first) ? first : "C:\\");
    } else if (settings_.restore_tabs && !settings_.tabs.empty()) {
        for (const std::string& t : settings_.tabs) {
            if (core::path::is_directory(t)) { tabs_->open(t); }
        }
        tabs_->activate(static_cast<std::size_t>(std::max(0, settings_.active_tab)));
    }
    if (tabs_->size() == 0) {
        const std::string home = core::home_folder();
        tabs_->open(core::path::is_directory(home) ? home : "C:\\");
    }

    lap("first tabs (folder listing starts)");

    // the language's own characters (and anything else already noted) go into the first bake, so it needs no rebuild at once
    baked_revision_ = core::text::demand_revision();
    auto created = ui::create_context(theme_.style(), window_.dpi_scale() * static_cast<float>(scale_pct_) / 100.0f, core::text::demanded_codepoints());
    lap("fonts and ui context");
    device_thread_.join();
    if (!created || !device_ok_ ||!gpu_.create_swap_chain(window_.handle())) { return false; }
    ui_.emplace(std::move(*created));
    ui_->set_clipboard(window_.input().clipboard());
    if (!gpu_.create_renderer(*ui_)) { return false; }
    lap("direct3d swap chain and renderer (the device was made meanwhile)");

    images_ = std::make_unique<platform::ImageCache>(window_.waker());
    shell_ = std::make_unique<ui::Shell>(window_, theme_, ui::FontSet{}, *tabs_, *ops_, settings_, *images_, binds_, history_);
    if (!opts.second.empty()) {
        const std::string right = core::path::normalize(opts.second);
        if (core::path::is_directory(right)) {
            settings_.split = true;
            shell_->set_second_folder(right);
        }
    }
    lap("shell (panels, sidebar, drives)");
    window_.set_logical_scale(ui_->scale());
    window_.hit_test       = [this](strata::vec2 p) { return shell_->hit_test(p); };
    platform::DropCallbacks drop;
    drop.resolve = [this](strata::vec2 p) { return shell_->resolve_drop(p); };
    drop.hover   = [this](std::optional<strata::vec2> p) { shell_->set_drop_point(p); };
    drop.link    = [](const std::string& dest) { return ui::Sidebar::is_pin_target(dest); };
    drop.dropped = [this](std::vector<std::string> paths, std::string dest, bool move) { shell_->drop_files(std::move(paths), dest, move); };
    drop.failed  = [this](std::size_t n) { shell_->drop_failed(n); };
    drop.redraw  = [this] { (void)step(true); };
    drop.scale   = [this] { return window_.logical_scale(); };
    (void)platform::attach_drop_target(window_.handle(), std::move(drop));
    window_.on_live_resize = [this] {
        // the window is being dragged: if a newer pointer position is already waiting, this size is about to be stale and
        // drawing it would only hold the next one back (on a slow gpu that is the "keeps resizing after I let go" lag).
        // never skip more than a few in a row, and the last size always draws
        static int skipped = 0;
        if (skipped < 3 && HIWORD(::GetQueueStatus(QS_MOUSEMOVE)) != 0) {
            ++skipped;
            return;
        }
        skipped = 0;
        (void)window_.take_resized();
        gpu_.resize();
        gpu_.set_immediate(true);
        (void)step(true);
        gpu_.set_immediate(false);
    };
    if (opts.settings) { shell_->show_settings(); }
    if (opts.palette) { shell_->show_palette(); }
    if (!opts.properties.empty()) { shell_->show_properties({opts.properties}, opts.properties_tab); }
    if (!opts.usage.empty()) { shell_->show_usage(opts.usage); }
    apply_theme();
    shell_->open_window_hook = [this](const std::string& folder, bool has_at, int x, int y, int w, int h) {
        Options o;
        o.path   = folder;
        o.has_at = has_at;
        o.at_x   = x;
        o.at_y   = y;
        o.width  = w;
        o.height = h;
        open_window_(o);
    };
    lap("final setup");
    core::log::info("startup: init finished, the first frame comes next");
    window_.show();
    return true;
}

void Application::start_log()
{
    core::log::enable(ui::Settings::log_path());
    if (!core::log::enabled()) { return; }
    // time between the process being created and main running: loader, antivirus scan of the exe, a cold disk
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
    ::GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    wchar_t exe[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    MEMORYSTATUSEX mem{sizeof mem};
    ::GlobalMemoryStatusEx(&mem);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW os{sizeof os};
    if (const auto fn = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) { fn(&os); }
    core::log::info("==== PrismFS start, pid {} ====", ::GetCurrentProcessId());
    core::log::info("exe {}", core::text::narrow(exe));
    core::log::info("settings {}", ui::Settings::file_path());
    core::log::info("windows {}.{} build {}, {} cpus, {} MB ram ({} MB free)", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, si.dwNumberOfProcessors,
                    mem.ullTotalPhys >> 20, mem.ullAvailPhys >> 20);
    core::log::info("startup: main reached {} ms after the process was created (before that: loading the exe and its dlls)", (ticks(now) - ticks(created)) / 10000);
}

void Application::apply_theme()
{
    ui_->theme() = theme_.style();
    window_.set_dark_frame(theme_.dark());
    ui_->invalidate();
}

bool Application::step(bool force_draw)
{
    if (const unsigned rev = core::i18n::revision(); rev != lang_revision_) { // (the action descriptions are translated when registered)
        lang_revision_ = rev;
        strings_dirty_ = true;
        strata::config chords;
        binds_.store(chords);
        binds_ = strata::keybinds{};
        ui::register_actions(binds_);
        (void)binds_.load(chords);
    }
    if (settings_.log_enabled != core::log::enabled()) { // (switched in Settings)
        if (settings_.log_enabled) { start_log(); } else { core::log::info("log switched off"); core::log::disable(); }
    }
    const auto ms_since = [](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); };
    // names with characters the fonts do not have (han, kana, hangul, arabic, emoji ...): bake exactly those glyphs. the
    // fonts are rebuilt as a whole, which takes a few tens of ms, so it happens only when a new character first appears
    if (const unsigned rev = core::text::demand_revision(); rev != baked_revision_) {
        baked_revision_ = rev; // (also on failure: no retry loop)
        core::log::Timer font_time{std::format("fonts rebuilt for new characters (revision {})", rev)};
        auto created = ui::create_context(theme_.style(), ui_->scale(), core::text::demanded_codepoints());
        if (created) {
            ui_.emplace(std::move(*created));
            ui_->set_clipboard(window_.input().clipboard());
            ui_->theme() = theme_.style();
            strings_dirty_ = true;
            images_->forget();
            popup_.destroy(); // (it lives on the device that is replaced; made again by the next frame)
            if (!gpu_.recreate(*ui_)) {
                window_.request_close();
                return false;
            }
        }
    }

    if (strings_dirty_) {
        strings_dirty_ = false;
        ui::apply_ui_strings(*ui_);
    }

    const auto apply_scale = [&](float dpi) {
        const float s = dpi * static_cast<float>(scale_pct_) / 100.0f;
        if (s != ui_->scale() && ui_->set_scale(s)) { gpu_.update_atlas(*ui_); }
    };
    if (const float s = window_.take_dpi_change(); s > 0.0f) { apply_scale(s); }

    if (images_->poll(gpu_.renderer())) { ui_->invalidate(); } // (a thumbnail arrived: draw it this frame)
    window_.set_logical_scale(ui_->scale());

    strata::input_state input = window_.input().new_frame();
    const ui::FrameInput frame_in{std::string_view{input.typed.data(), input.typed_len}, input.delta_time};
    const auto ui_start = std::chrono::steady_clock::now();
    prepare_popups();
    ui_->begin_frame(input);
    shell_->frame(*ui_, frame_in);
    strata::end_frame(*ui_, window_.input());
    const double ui_ms = ms_since(ui_start);
    // this frame met characters the fonts lack (they would draw as '?', on the dim first frame of the fade-in): do not show
    // it. the rebuild happens at the start of the next step, which must come at once, and the window keeps the last good
    // frame meanwhile (a rebuild takes a few hundred ms)
    const bool stale_glyphs = !force_draw && core::text::demand_revision() != baked_revision_;
    if (stale_glyphs) { ui_->invalidate(); }
    if (shell_->take_restyle()) { apply_theme(); }
    if (const int pct = shell_->take_scale_request(); pct > 0) {
        scale_pct_ = pct;
        apply_scale(window_.dpi_scale());
    }
    if (shell_->cursor() != strata::cursor_kind::arrow) { window_.input().set_cursor(shell_->cursor()); }

    bool presented = false;
    const auto present_start = std::chrono::steady_clock::now();
    if (!stale_glyphs && (force_draw || !ui_->frame_unchanged())) {
        presented = true;
        const bool ok = present_frame();
        static bool first_frame = true;
        if (first_frame) {
            first_frame = false;
            core::log::info("startup: first frame on screen (ui {:.0f} ms, draw + present {:.0f} ms)", ui_ms, ms_since(present_start));
        }
        if (core::log::enabled()) {
            const double present_ms = ms_since(present_start);
            if (ui_ms + present_ms >= 50.0) { core::log::info("slow frame: ui {:.0f} ms, draw + present {:.0f} ms (a frame should take under 16)", ui_ms, present_ms); }
        }
        if (!ok) {
            images_->forget(); // (every texture died with the device)
            popup_.destroy();
            if (!gpu_.recreate(*ui_)) {
                window_.request_close();
                return false;
            }
        }
    }

    // an OS drag of files starts after the frame: it runs its own message loop until the button is released
    if (std::vector<std::string> drag = shell_->take_drag(); !drag.empty()) {
        shell_->set_dragging(drag);
        const bool dropped = platform::run_drag(drag);
        // DoDragDrop swallows the button release, so strata still thinks the left button is held: the next click
        // would then not register as a press. Feed it the release it missed.
        {
            const auto wnd = static_cast<HWND>(window_.handle());
            POINT pt{};
            ::GetCursorPos(&pt);
            ::ScreenToClient(wnd, &pt);
            ::SendMessageW(wnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
        }
        shell_->set_dragging({});
        shell_->set_drop_point(std::nullopt);
        (void)dropped; // (the folder watcher picks up a move out of the folder: no reload flash)
        ui_->invalidate();
    }
    return presented;
}

// menus may reach past the window edge (Settings::detached_menus): they are then placed within the monitor under the pointer
// instead of the window, and drawn by present_frame into a window of their own. the monitor is looked at while no menu is open,
// so a menu does not jump to another screen when the pointer crosses over
void Application::prepare_popups()
{
    const auto off = [&] {
        ui_->set_detach_overlay(false);
        ui_->clear_popup_bounds();
    };
    if (!settings_.detached_menus || popup_failed_) {
        off();
        return;
    }
    const auto wnd = static_cast<HWND>(window_.handle());
    if (!popup_.created() && !popup_.create(wnd, gpu_.device())) {
        core::log::info("menus past the window edge: not available here, menus stay in the window");
        popup_failed_ = true;
        off();
        return;
    }
    ui_->set_detach_overlay(true);
    if (!ui_->menu_open()) {
        POINT cursor{};
        ::GetCursorPos(&cursor);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        POINT origin{};
        ::ClientToScreen(wnd, &origin);
        if (::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi)) {
            const float s = ui_->scale();
            popup_bounds_ = {{static_cast<float>(mi.rcWork.left - origin.x) / s, static_cast<float>(mi.rcWork.top - origin.y) / s},
                             {static_cast<float>(mi.rcWork.right - origin.x) / s, static_cast<float>(mi.rcWork.bottom - origin.y) / s}};
        } else {
            popup_bounds_ = {};
        }
    }
    if (popup_bounds_.empty()) { ui_->clear_popup_bounds(); } else { ui_->set_popup_bounds(popup_bounds_); }
}

bool Application::present_frame()
{
    const strata::color clear = theme_.pal().window;
    const auto          wnd   = static_cast<HWND>(window_.handle());
    const strata::draw_data frame = ui_->render_data();

    // the popup layers go to a window of their own only when they reach outside this one; inside it they look and behave as ever
    bool         detach = false;
    strata::vec2 origin{};
    int          sx = 0, sy = 0, w = 0, h = 0;
    if (settings_.detached_menus && !popup_failed_ && popup_.created() && frame.overlay_first < frame.commands.size() && !frame.overlay_bounds.empty()) {
        const strata::rect& b = frame.overlay_bounds;
        detach = b.min.x < 0.0f || b.min.y < 0.0f || b.max.x > frame.display_size.x || b.max.y > frame.display_size.y;
        if (detach) {
            origin = {std::floor(b.min.x), std::floor(b.min.y)};
            w      = static_cast<int>(std::ceil(b.max.x - origin.x));
            h      = static_cast<int>(std::ceil(b.max.y - origin.y));
            POINT client{};
            ::ClientToScreen(wnd, &client);
            sx = client.x + static_cast<int>(origin.x);
            sy = client.y + static_cast<int>(origin.y);
        }
    }

    // the popup first: its present does not wait for the display, the main window's does, so both reach the screen together
    if (detach && !popup_.show(gpu_.renderer(), frame.tail(frame.overlay_first), origin, sx, sy, w, h)) {
        core::log::info("menus past the window edge: drawing failed, menus stay in the window");
        popup_failed_ = true;
        popup_.destroy();
        detach = false;
        ui_->invalidate();
    }
    const bool ok = gpu_.present(detach ? frame.head(frame.overlay_first) : frame, clear);

    if (detach) {
        // the main window keeps the mouse while the menu is out over the desktop: moves and clicks outside it still arrive
        // (a click away closes the menu), and the menu goes when this window is no longer the active one
        if (::GetCapture() != wnd) { ::SetCapture(wnd); }
        if (::GetForegroundWindow() != wnd) { ui_->close_menus(); }
    } else if (popup_.visible()) {
        popup_.hide();
        const bool button = ((::GetAsyncKeyState(VK_LBUTTON) | ::GetAsyncKeyState(VK_RBUTTON) | ::GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
        if (!button && ::GetCapture() == wnd) { ::ReleaseCapture(); }
    }
    return ok;
}

void Application::tick()
{
    if (window_.minimized()) { // (nothing to draw: the next message that matters is the restore)
        (void)window_.take_activity();
        due_      = std::chrono::steady_clock::now() + std::chrono::hours{1};
        animating_ = false;
        return;
    }
    const bool active = window_.take_activity();
    if (popup_.visible()) {
        const auto wnd = static_cast<HWND>(window_.handle());
        if (::GetForegroundWindow() != wnd) {
            // a click on another program (it gets the click: Windows only routes captured mouse input to our windows) or an
            // Alt+Tab: the menu is done
            ui_->close_menus();
            ui_->invalidate();
        } else if (::GetCapture() != wnd) {
            // (strata lets go of the mouse when a button goes up: while a menu is out over the desktop it has to stay captured)
            ::SetCapture(wnd);
        }
    }
    if (!active && !animating_ && std::chrono::steady_clock::now() < due_) { return; }

    if (crash_test_window_ && ++frames_ == 30) { *static_cast<volatile int*>(nullptr) = 1; } // (--crash-test-window)

    if (window_.take_resized()) {
        gpu_.resize();
        ui_->invalidate();
    }
    if (presented_) { gpu_.wait_for_frame(); }
    presented_ = step(false);

    // the next frame is due when input arrives, a timer runs out (caret, tooltip delay) or an animation is still moving.
    // a directory loader posts a message when it has rows, which counts as input
    const double wait = ui_ != std::nullopt ? ui_->next_wake_seconds() : 3600.0;
    animating_ = wait <= 0.0;
    due_       = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(std::min(wait, 3600.0)));
}

double Application::seconds_until_due() const
{
    if (animating_) { return 0.0; }
    return std::max(0.0, std::chrono::duration<double>(due_ - std::chrono::steady_clock::now()).count());
}

void Application::shutdown()
{
    platform::detach_drop_target(window_.handle());
    if (!ui_ || !shell_) { return; }
    const strata::vec2 logical = ui_->display_size();
    shell_->save_session(static_cast<int>(logical.x * static_cast<float>(scale_pct_) / 100.0f), static_cast<int>(logical.y * static_cast<float>(scale_pct_) / 100.0f));
}

} // namespace prism::app
