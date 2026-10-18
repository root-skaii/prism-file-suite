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
            else if (a == L"--settings") { opts.settings = true; }
            else if (a == L"--properties" && i + 1 < argc) { opts.properties = prism::core::text::narrow(argv[++i]); }
            else if (a == L"--properties-tab" && i + 1 < argc) { opts.properties_tab = _wtoi(argv[++i]); }
            else if (a == L"--grid") { opts.grid = true; }
            else if (a == L"--preview") { opts.preview = true; }
            else if (a == L"--split") { opts.split = true; }
            else {
                // (registered as `"prismfs.exe" "%1"`: for a drive the %1 is `C:\`, and the backslash escapes the closing quote)
                std::wstring p{a};
                while (!p.empty() && p.back() == L'"') { p.pop_back(); }
                if (p.size() == 2 && p[1] == L':') { p.push_back(L'\\'); }
                opts.path = prism::core::text::narrow(p);
            }
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
