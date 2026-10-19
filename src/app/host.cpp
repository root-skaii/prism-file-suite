#include "app/host.hpp"

#include "core/crash.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "platform/window.hpp"
#include "ui/actions.hpp"
#include "ui/tr.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace prism::app {

Application::Options parse_command_line(const wchar_t* command_line)
{
    Application::Options opts;
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(command_line, &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view a = argv[i];
            if (a == L"--light") { opts.light = true; }
            else if (a == L"--compact") { opts.compact = true; }
            else if (a == L"--settings") { opts.settings = true; }
            else if (a == L"--palette") { opts.palette = true; }
            else if (a == L"--properties" && i + 1 < argc) { opts.properties = core::text::narrow(argv[++i]); }
            else if (a == L"--properties-tab" && i + 1 < argc) { opts.properties_tab = _wtoi(argv[++i]); }
            else if (a == L"--grid") { opts.grid = true; }
            else if (a == L"--preview") { opts.preview = true; }
            else if (a == L"--split") { opts.split = true; }
            else if (a == L"--log") { opts.log = true; }
            else if (a == L"--crash-test") { opts.crash_test = true; }
            else if (a == L"--crash-test-window") { opts.crash_test_window = true; }
            else if (a == L"--window") { opts.window_notice = true; }
            else if (a == L"--crashed" && i + 1 < argc) { opts.crashed = core::text::narrow(argv[++i]); }
            else if (a == L"--at" && i + 1 < argc) { opts.has_at = swscanf_s(argv[++i], L"%d,%d", &opts.at_x, &opts.at_y) == 2; }
            else if (a == L"--size" && i + 1 < argc) { (void)swscanf_s(argv[++i], L"%d,%d", &opts.width, &opts.height); }
            else {
                // (registered as `"prismfs.exe" "%1"`: for a drive the %1 is `C:\`, and the backslash escapes the closing quote)
                std::wstring p{a};
                while (!p.empty() && p.back() == L'"') { p.pop_back(); }
                if (p.size() == 2 && p[1] == L':') { p.push_back(L'\\'); }
                // the first folder opens in the left pane, a second one opens two panes with it on the right
                (opts.path.empty() ? opts.path : opts.second) = core::text::narrow(p);
            }
        }
        ::LocalFree(argv);
    }
    return opts;
}

bool Host::make_window(const Application::Options& o)
{
    auto app = std::make_unique<Application>(settings_, binds_, [this](const Application::Options& more) { open_window(more); });
    bool ok  = false;
    if (!core::crash::guard_window("window start", [&] { ok = app->init(o); })) {
        app->abandon();
        (void)app.release(); // (a fault while starting: its state is not to be trusted, so it is not destroyed)
        return false;
    }
    if (!ok) { return false; }
    windows_.push_back(std::move(app));
    return true;
}

void Host::close_window(std::size_t index)
{
    std::unique_ptr<Application> w = std::move(windows_[index]);
    windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(index));
    if (!core::crash::guard_window("window close", [&] {
            w->shutdown();
            w.reset();
        })) {
        if (w != nullptr) { w->abandon(); }
        (void)w.release();
    }
}

void Host::apply_background()
{
    if (!serving_) { return; }
    const bool want = settings_.background;
    if (want && (!tray_shown_ || tray_language_ != core::i18n::revision())) { // (also again when the language changed: new labels)
        const auto w = [](std::string_view s) { return core::text::widen(std::string{s}); };
        const platform::instance::TrayLabels labels{L"PrismFS", w(ui::tr("Open PrismFS")), w(ui::tr("Settings")), w(ui::tr("Exit PrismFS"))};
        server_.set_tray(&labels);
        tray_shown_    = true;
        tray_language_ = core::i18n::revision();
    } else if (!want && tray_shown_) {
        server_.set_tray(nullptr);
        tray_shown_ = false;
    }
}

int Host::run(const Application::Options& first, bool serve)
{
    ui::register_actions(binds_);
    settings_.binds = &binds_;
    settings_.load();
    {
        const std::string p = ui::Settings::file_path(); // (crash reports go in a folder beside the settings)
        core::crash::set_directory(core::text::widen(p.substr(0, p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1) + "crashes"));
    }
    core::i18n::scan(ui::Settings::language_dirs());
    core::i18n::set_language(settings_.language);
    if (first.log) { settings_.log_enabled = true; }
    if (settings_.log_enabled) { Application::start_log(); }

    if (serve) {
        platform::instance::Server::Callbacks cb;
        cb.launch = [this](platform::instance::Launch l) {
            ::SetCurrentDirectoryW(l.directory.c_str()); // (relative folders in the launch mean where it was started)
            open_window(parse_command_line(l.command_line.c_str()));
        };
        cb.open     = [this] { open_window({}); };
        cb.settings = [this] {
            Application::Options o;
            o.settings = true;
            open_window(o);
        };
        cb.exit = [this] {
            exit_requested_ = true;
            for (auto& w : windows_) { w->request_close(); }
        };
        serving_ = server_.create(std::move(cb));
    }

    if (!make_window(first)) { return 1; }

    while (!exit_requested_ || !windows_.empty()) {
        if (!platform::Window::pump_messages()) { break; }
        for (const Application::Options& o : std::exchange(pending_, {})) { (void)make_window(o); }
        apply_background();

        for (std::size_t i = 0; i < windows_.size();) {
            Application& w = *windows_[i];
            if (w.closed()) {
                close_window(i);
                continue;
            }
            if (!core::crash::guard_window("window frame", [&] { w.tick(); })) {
                w.abandon(); // this window is lost, the others carry on
                (void)windows_[i].release();
                windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }

        if (windows_.empty()) {
            if (!keep_alive() || exit_requested_) { break; }
            platform::Window::wait_messages(0xffffffffu); // (only the tray icon and launches from other processes are left)
            continue;
        }

        // sleep until a message arrives or the nearest timer is due; a window that is animating gets a frame per refresh
        double wait = 3600.0;
        Application* pacer = nullptr;
        for (auto& w : windows_) {
            const double s = w->seconds_until_due();
            if (s <= 0.0 && pacer == nullptr) { pacer = w.get(); }
            wait = std::min(wait, s);
        }
        if (pacer != nullptr) {
            pacer->pace(); // (not a busy loop: blocks until the next screen refresh)
        } else {
            platform::Window::wait_messages(wait >= 3600.0 ? 0xffffffffu : static_cast<std::uint32_t>(std::ceil(wait * 1000.0)));
        }
    }
    return 0;
}

} // namespace prism::app
