#include "app/application.hpp"

#include "core/text.hpp"

#include <windows.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // prismfs.exe [folder]
    prism::app::Application::Options opts;
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view a = argv[i];
            if (a == L"--light") { opts.light = true; }
            else if (a == L"--compact") { opts.compact = true; }
            else { opts.path = prism::core::text::narrow(a); }
        }
        ::LocalFree(argv);
    }

    prism::app::Application app;
    if (!app.init(opts)) {
        ::MessageBoxW(nullptr, L"PrismFS could not start (window, Direct3D 11 or fonts failed).", L"PrismFS", MB_ICONERROR);
        return 1;
    }
    return app.run();
}
