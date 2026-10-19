#include "app/application.hpp"

#include "core/crash.hpp"
#include "core/text.hpp"
#include "platform/standby.hpp"
#include "ui/settings.hpp"
#include "ui/tr.hpp"

#include <windows.h>
#include <shellapi.h>

namespace {

// prismfs.exe [folder [second folder]] [flags]
prism::app::Application::Options parse_command_line(const wchar_t* command_line)
{
    prism::app::Application::Options opts;
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(command_line, &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view a = argv[i];
            if (a == L"--light") { opts.light = true; }
            else if (a == L"--compact") { opts.compact = true; }
            else if (a == L"--settings") { opts.settings = true; }
            else if (a == L"--palette") { opts.palette = true; }
            else if (a == L"--properties" && i + 1 < argc) { opts.properties = prism::core::text::narrow(argv[++i]); }
            else if (a == L"--properties-tab" && i + 1 < argc) { opts.properties_tab = _wtoi(argv[++i]); }
            else if (a == L"--grid") { opts.grid = true; }
            else if (a == L"--preview") { opts.preview = true; }
            else if (a == L"--split") { opts.split = true; }
            else if (a == L"--log") { opts.log = true; }
            else if (a == L"--standby") { opts.standby = true; }
            else if (a == L"--tray") { opts.tray = true; }
            else if (a == L"--crash-test") { opts.crash_test = true; }
            else if (a == L"--crashed" && i + 1 < argc) { opts.crashed = prism::core::text::narrow(argv[++i]); }
            else if (a == L"--at" && i + 1 < argc) { opts.has_at = swscanf_s(argv[++i], L"%d,%d", &opts.at_x, &opts.at_y) == 2; }
            else if (a == L"--size" && i + 1 < argc) { (void)swscanf_s(argv[++i], L"%d,%d", &opts.width, &opts.height); }
            else {
                // (registered as `"prismfs.exe" "%1"`: for a drive the %1 is `C:\`, and the backslash escapes the closing quote)
                std::wstring p{a};
                while (!p.empty() && p.back() == L'"') { p.pop_back(); }
                if (p.size() == 2 && p[1] == L':') { p.push_back(L'\\'); }
                // the first folder opens in the left pane, a second one opens two panes with it on the right
                (opts.path.empty() ? opts.path : opts.second) = prism::core::text::narrow(p);
            }
        }
        ::LocalFree(argv);
    }
    return opts;
}

// the process that crashed started this one: say so, offer to start again
int show_crash_notice(const std::string& report)
{
    prism::ui::Settings settings;
    settings.load();
    prism::core::i18n::scan(prism::ui::Settings::language_dirs());
    prism::core::i18n::set_language(settings.language);
    const std::wstring text = prism::core::text::widen(prism::ui::trf("PrismFS closed unexpectedly. A report was saved:\n{}\n\nStart PrismFS again?", report));
    const std::wstring title = prism::core::text::widen(std::string{prism::ui::tr("PrismFS crashed")});
    if (::MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND) == IDYES) {
        wchar_t exe[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring{exe} + L"\"";
        STARTUPINFOW        si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    prism::core::crash::install();
    prism::app::Application::Options opts = parse_command_line(::GetCommandLineW());
    if (!opts.crashed.empty()) { return show_crash_notice(opts.crashed); }
    if (opts.crash_test) { *static_cast<volatile int*>(nullptr) = 1; } // (checks the crash report path: --crash-test)
    if (!opts.standby && prism::platform::standby::hand_off(::GetCommandLineW())) {
        return 0; // a spare process is now this window
    }
    prism::app::Application app;

    if (opts.standby) {
        // a spare: load the gpu driver now, then sleep until a launch hands its command line over
        app.prewarm();
        prism::platform::standby::TrayLabels labels;
        if (opts.tray) { // (read now, so the menu speaks the current language; every launch makes a fresh spare)
            prism::ui::Settings settings;
            settings.load();
            prism::core::i18n::scan(prism::ui::Settings::language_dirs());
            prism::core::i18n::set_language(settings.language);
            const auto w = [](std::string_view s) { return prism::core::text::widen(std::string{s}); };
            labels = {L"PrismFS", w(prism::ui::tr("Open PrismFS")), w(prism::ui::tr("Settings")), w(prism::ui::tr("Exit PrismFS"))};
        }
        const auto request = prism::platform::standby::wait([&app] { return app.device_ready(); }, opts.tray ? &labels : nullptr);
        if (!request) { return 0; }
        ::SetCurrentDirectoryW(request->directory.c_str()); // (relative folders in the launch mean where it was started)
        opts = parse_command_line(request->command_line.c_str());
        opts.standby = false;
    }

    if (!app.init(opts)) {
        ::MessageBoxW(nullptr, L"PrismFS could not start (window, Direct3D 11 or fonts failed).", L"PrismFS", MB_ICONERROR);
        return 1;
    }
    return app.run();
}
