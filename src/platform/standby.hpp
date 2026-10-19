#pragma once

// a spare PrismFS process (`prismfs.exe --standby`): it has already paid for the process start and the gpu driver load
// (~200 ms) and waits, invisible, for the next launch. a launch hands its command line over and exits; the spare then
// starts as that window. nothing else is prepared ahead: settings, folders and fonts are read at launch, so a spare is
// never stale. it matches by exe path, so another build or a portable copy never takes over a launch
//
// with `--tray` the spare also owns the notification-area icon (open / settings / exit). it stays after the last window
// closes, so the icon is always there; a launch (or "Open" in its menu) turns it into a window and the next spare takes the icon over

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace prism::platform::standby {

inline constexpr wchar_t k_main_window_class[] = L"prismfs_main_window"; // (platform/window.cpp registers it)

// launcher side: true = a spare took this launch and the caller should exit
[[nodiscard]] bool hand_off(std::wstring_view command_line);

void ensure(bool tray); // starts a spare for this exe unless one is already there
void stop(bool wait);   // closes this exe's spare (wait: until its process has ended, up to ~1.5 s)
void restart(bool tray); // stop + ensure on a worker thread (the tray setting changed)

struct TrayLabels {
    std::wstring tip, open, settings, exit;
};

// spare side: waits (without a window the user can see) until a launch arrives and returns its working directory and
// command line. `ready` says whether the spare has finished preparing; nullopt: closed or exited from the tray menu, or
// another spare already exists. `tray`: show the icon with these labels
struct Request {
    std::wstring directory;
    std::wstring command_line;
};
[[nodiscard]] std::optional<Request> wait(const std::function<bool()>& ready, const TrayLabels* tray = nullptr);

} // namespace prism::platform::standby
