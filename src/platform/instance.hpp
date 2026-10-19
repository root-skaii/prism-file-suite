#pragma once

// one PrismFS process per exe: every window lives in it. a second launch (double click on a folder, `prismfs.exe path`,
// a shortcut) finds the running process, hands it its command line and exits; the running one opens the window.
// it matches by exe path, so another build or a portable copy is its own instance.
// the running process also owns the notification-area icon (open / settings / exit)

#include <functional>
#include <string>
#include <string_view>

namespace prism::platform::instance {

inline constexpr wchar_t k_main_window_class[] = L"prismfs_main_window"; // (platform/window.cpp registers it)

// launcher side: true = the running PrismFS took this launch and the caller should exit
[[nodiscard]] bool hand_off(std::wstring_view command_line);
// becomes the one instance of this exe (a named mutex); false: another process already is, or is just starting
[[nodiscard]] bool claim();

struct Launch {
    std::wstring directory;
    std::wstring command_line;
};

struct TrayLabels {
    std::wstring tip, open, settings, exit;
};

class Server {
public:
    struct Callbacks {
        std::function<void(Launch)> launch;   // another process handed a launch over
        std::function<void()>       open;     // tray: a new window
        std::function<void()>       settings; // tray: a new window on the settings
        std::function<void()>       exit;     // tray: close everything
    };

    Server() = default;
    ~Server();
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // the hidden window that receives launches. false: could not be made
    [[nodiscard]] bool create(Callbacks callbacks);
    // shows the icon with these labels, or removes it (nullptr)
    void set_tray(const TrayLabels* labels);

private:
    struct Impl;
    Impl* impl_{};
};

} // namespace prism::platform::instance
