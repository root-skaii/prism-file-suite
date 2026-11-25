#include "app/host.hpp"

#include "core/crash.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
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
    const auto clean_path = [](std::wstring_view raw) {
        std::wstring p{raw};
        while (!p.empty() && p.back() == L'"') { p.pop_back(); }
        while (!p.empty() && p.front() == L'"') { p.erase(p.begin()); }
        if (p.size() == 2 && p[1] == L':') { p.push_back(L'\\'); }
        return core::path::resolve_input(core::text::narrow(p));
    };
    const auto add_positional = [&](std::string p) {
        if (p.empty()) { return; }
        if (opts.path.empty()) { opts.path = std::move(p); }
        else if (opts.second.empty() && opts.open_tabs.empty()) { opts.second = std::move(p); }
        else {
            // 3+ positional paths open as tabs instead of dropping earlier ones
            if (opts.open_tabs.empty()) {
                opts.open_tabs.push_back(std::move(opts.path));
                opts.open_tabs.push_back(std::move(opts.second));
                opts.path.clear();
                opts.second.clear();
            }
            opts.open_tabs.push_back(std::move(p));
        }
    };
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(command_line, &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view a = argv[i];
            std::wstring lower_a{a};
            for (wchar_t& c : lower_a) { c = static_cast<wchar_t>(::towlower(c)); }
            if (a == L"--light") { opts.light = true; }
            else if (a == L"--compact") { opts.compact = true; }
            else if (a == L"--settings") { opts.settings = true; }
            else if (a == L"--palette") { opts.palette = true; }
            else if (a == L"--tab" && i + 1 < argc) {
                if (std::string p = clean_path(argv[++i]); !p.empty()) { opts.open_tabs.push_back(std::move(p)); }
            }
            else if (a == L"--active-tab" && i + 1 < argc) { opts.active_tab = _wtoi(argv[++i]); }
            else if (a == L"--right" && i + 1 < argc) {
                opts.second = clean_path(argv[++i]);
                opts.split  = true;
            }
            else if (a == L"--properties" && i + 1 < argc) { opts.properties = clean_path(argv[++i]); }
            else if (a == L"--properties-tab" && i + 1 < argc) { opts.properties_tab = _wtoi(argv[++i]); }
            else if (a == L"--usage" && i + 1 < argc) { opts.usage = clean_path(argv[++i]); }
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
            // standard Windows Explorer switches used by browsers / IDEs ("Show in Folder"): /select,<path>, /e, /n, /root,<path>
            else if (lower_a == L"/select," || lower_a == L"/select") {
                opts.select_file = true;
                if (i + 1 < argc) { add_positional(clean_path(argv[++i])); }
            }
            else if (lower_a.starts_with(L"/select,")) {
                opts.select_file = true;
                add_positional(clean_path(a.substr(8)));
            }
            else if (lower_a == L"/e" || lower_a == L"/e," || lower_a == L"/n" || lower_a == L"/n," || lower_a == L"/separate") {
                continue;
            }
            else if (lower_a.starts_with(L"/e,") || lower_a.starts_with(L"/n,")) {
                add_positional(clean_path(a.substr(3)));
            }
            else if (lower_a.starts_with(L"/root,")) {
                add_positional(clean_path(a.substr(6)));
            }
            else {
                // (registered as `"prismfs.exe" "%1"`: for a drive the %1 is `C:\`, and the backslash escapes the closing quote)
                add_positional(clean_path(a));
            }
        }
        ::LocalFree(argv);
    }
    return opts;
}

void Host::WakeHub::add(void* h)
{
    if (h == nullptr) { return; }
    std::lock_guard g{lock};
    hwnds.push_back(h);
}

void Host::WakeHub::remove(void* h)
{
    if (h == nullptr) { return; }
    std::lock_guard g{lock};
    std::erase(hwnds, h);
}

void Host::WakeHub::wake_all()
{
    std::vector<void*> copy;
    {
        std::lock_guard g{lock};
        copy = hwnds;
    }
    for (void* h : copy) { platform::Window::wake_handle(h); }
}

Host::Host()
    : wake_hub_{std::make_shared<WakeHub>()},
      sizes_{std::make_unique<core::FolderSizes>([hub = wake_hub_] { hub->wake_all(); })},
      git_{std::make_unique<core::GitStatus>([hub = wake_hub_] { hub->wake_all(); })},
      index_{std::make_unique<core::FileIndex>([hub = wake_hub_] { hub->wake_all(); })}
{}

Host::~Host()
{
    // stop shared workers before windows / hub vanish
    index_.reset();
    git_.reset();
    sizes_.reset();
}

bool Host::transfer_tab(std::unique_ptr<model::Tab>& tab, void* target_hwnd, int x, int y, int w, int h)
{
    if (!tab) { return false; }
    if (target_hwnd != nullptr) {
        for (auto& win : windows_) {
            if (win->window().handle() == target_hwnd) {
                win->adopt_tab(std::move(tab));
                const auto hwnd = static_cast<HWND>(target_hwnd);
                ::SetForegroundWindow(hwnd);
                if (::IsIconic(hwnd)) { ::ShowWindow(hwnd, SW_RESTORE); }
                return true;
            }
        }
        // target belongs to a different PrismFS process (e.g. elevated <-> normal): leave `tab` intact for WM_COPYDATA fallback
        return false;
    }
    Application::Options o;
    o.has_at = true;
    o.at_x   = x;
    o.at_y   = y;
    o.width  = w;
    o.height = h;
    pending_.push_back({std::move(o), std::move(tab)});
    return true;
}

bool Host::make_window(const Application::Options& o, std::unique_ptr<model::Tab> initial_tab)
{
    SharedServices shared{*sizes_, *git_, *index_, index_started_, shared_gpu_};
    auto app = std::make_unique<Application>(
        settings_, binds_, shared,
        [this](const Application::Options& more) { open_window(more); },
        [this](std::unique_ptr<model::Tab>& tab, void* target, int x, int y, int w, int h) {
            return transfer_tab(tab, target, x, y, w, h);
        });
    bool ok  = false;
    if (!core::crash::guard_window("window start", [&] { ok = app->init(o, std::move(initial_tab)); })) {
        wake_hub_->remove(app->window().handle());
        app->abandon();
        (void)app.release(); // (a fault while starting: its state is not to be trusted, so it is not destroyed)
        return false;
    }
    if (!ok) { return false; }
    wake_hub_->add(app->window().handle());
    windows_.push_back(std::move(app));
    return true;
}

void Host::close_window(std::size_t index)
{
    std::unique_ptr<Application> w = std::move(windows_[index]);
    windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(index));
    wake_hub_->remove(w->window().handle());
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
    // the hand-off window comes first: from the moment this process holds the instance, launches find it (they queue until the
    // first window exists)
    if (serve) {
        platform::instance::Server::Callbacks cb;
        cb.launch = [this](platform::instance::Launch l) {
            // temporarily switch cwd only while parsing relative paths so the tray process never keeps a user folder locked
            wchar_t saved[MAX_PATH]{};
            const DWORD saved_n = ::GetCurrentDirectoryW(MAX_PATH, saved);
            if (!l.directory.empty()) { ::SetCurrentDirectoryW(l.directory.c_str()); }
            Application::Options parsed = parse_command_line(l.command_line.c_str());
            if (saved_n > 0 && saved_n < MAX_PATH) { ::SetCurrentDirectoryW(saved); }
            open_window(parsed);
        };
        cb.open = [this] {
            for (auto& w : windows_) {
                if (HWND h = static_cast<HWND>(w->window().handle()); h != nullptr && !w->properties_only()) {
                    if (::IsIconic(h)) { ::ShowWindow(h, SW_RESTORE); }
                    ::SetForegroundWindow(h);
                    return;
                }
            }
            open_window({});
        };
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

    ui::register_actions(binds_);
    settings_.binds = &binds_;
    settings_.load();
    {
        const std::string p = ui::Settings::file_path(); // (crash reports go in a folder beside the settings)
        core::crash::set_directory(core::text::widen(p.substr(0, p.find_last_of('\\') == std::string::npos ? 0 : p.find_last_of('\\') + 1) + "crashes"));
    }
    core::i18n::scan(ui::Settings::language_dirs());
    core::i18n::set_language(settings_.language);
    index_->set_cache_path(ui::Settings::cache_path() + "\\index.bin");
    if (first.log) { settings_.log_enabled = true; }
    if (settings_.log_enabled) { Application::start_log(); }

    if (!make_window(first)) { return 1; }

    while (!exit_requested_ || !windows_.empty()) {
        if (!platform::Window::pump_messages()) { break; }
        for (PendingWindow& p : std::exchange(pending_, {})) { (void)make_window(p.opts, std::move(p.initial_tab)); }
        apply_background();

        for (std::size_t i = 0; i < windows_.size();) {
            Application& w = *windows_[i];
            if (w.closed()) {
                close_window(i);
                continue;
            }
            if (!core::crash::guard_window("window frame", [&] { w.tick(); })) {
                wake_hub_->remove(w.window().handle());
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
    while (!windows_.empty()) { close_window(0); }
    return 0;
}

} // namespace prism::app
