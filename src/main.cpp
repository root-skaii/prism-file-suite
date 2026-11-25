#include "app/host.hpp"

#include "core/crash.hpp"
#include "core/rclone.hpp"
#include "core/text.hpp"
#include "platform/instance.hpp"
#include "ui/settings.hpp"
#include "ui/tr.hpp"

#include <windows.h>

namespace {

// the process that crashed (or lost a window) started this one: say so, offer to start again
int show_crash_notice(const std::string& report, bool one_window)
{
    prism::ui::Settings settings;
    settings.load();
    prism::core::i18n::scan(prism::ui::Settings::language_dirs());
    prism::core::i18n::set_language(settings.language);
    const std::wstring text = prism::core::text::widen(one_window
        ? prism::ui::trf("A PrismFS window crashed and was closed. The other windows keep running. A report was saved:\n{}\n\nOpen a new window?", report)
        : prism::ui::trf("PrismFS closed unexpectedly. A report was saved:\n{}\n\nStart PrismFS again?", report));
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
    const prism::app::Application::Options opts = prism::app::parse_command_line(::GetCommandLineW());
    if (!opts.crashed.empty()) { return show_crash_notice(opts.crashed, opts.window_notice); }
    if (opts.crash_test) { *static_cast<volatile int*>(nullptr) = 1; } // (checks the crash report path: --crash-test)

    // one PrismFS process per exe holds every window: a launch hands its command line to the running one and leaves.
    // when none is there, this one becomes it. one that finds the instance claimed but not answering yet is still starting:
    // wait for it. only a host silent for the whole time is given up on; this process then runs alone (no tray icon)
    bool serve = false;
    const ULONGLONG give_up = ::GetTickCount64() + 15000;
    for (;;) {
        if (prism::platform::instance::hand_off(::GetCommandLineW())) { return 0; }
        if (prism::platform::instance::claim()) { serve = true; break; }
        if (::GetTickCount64() >= give_up) { break; }
        ::Sleep(50);
    }

    prism::app::Host host;
    const int rc = host.run(opts, serve);
    prism::core::rclone::shutdown();
    if (rc != 0) {
        ::MessageBoxW(nullptr, L"PrismFS could not start (window, Direct3D 11 or fonts failed).", L"PrismFS", MB_ICONERROR);
        return 1;
    }
    return 0;
}
