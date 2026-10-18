#pragma once

// the top-level window: borderless (the title bar is drawn by the ui) but still a real resizable, snappable, shadowed
// window. owns the strata input translation. windows.h stays out of this header.

#include "strata/platform/win32.hpp"

#include <cstdint>
#include <functional>
#include <string_view>

namespace prism::platform {

// what the ui says about a point of the title bar (the window asks while handling WM_NCHITTEST)
enum class HitZone : std::uint8_t {
    client,          // the ui handles it
    caption,         // drags the window, double click maximises
    maximize_button, // the system answers hover with the snap layouts flyout (windows 11)
};

class Window {
public:
    struct Config {
        std::string_view title  = "PrismFS";
        int              width  = 1360; // client size, logical pixels
        int              height = 840;
    };

    Window() = default;
    ~Window();
    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] bool create(const Config& cfg);
    void show();

    // false once the window is closed
    [[nodiscard]] bool pump();
    // sleeps until a message arrives or `timeout_ms` passes (INFINITE = 0xffffffff)
    void wait(std::uint32_t timeout_ms) const;
    // wakes wait() from any thread
    void wake() const noexcept;
    // the same as a free-standing callable (copies the handle: safe on a worker thread that outlives the window)
    [[nodiscard]] std::function<void()> waker() const;

    [[nodiscard]] void*                  handle() const noexcept { return hwnd_; }
    [[nodiscard]] strata::win32_platform& input() noexcept { return platform_; }
    [[nodiscard]] float                  dpi_scale() const noexcept { return platform_.dpi_scale(); }
    [[nodiscard]] bool                   minimized() const noexcept { return minimized_; }
    [[nodiscard]] bool                   maximized() const noexcept;

    // true once after the client area changed size; the new dpi scale (0: none) after WM_DPICHANGED
    [[nodiscard]] bool  take_resized() noexcept;
    [[nodiscard]] float take_dpi_change() noexcept;

    void minimize() const noexcept;
    void toggle_maximize() const noexcept;
    void request_close() const noexcept;
    void set_title(std::string_view utf8) const;
    void set_dark_frame(bool dark) const noexcept; // the system's idea of the frame (shadow tint, rounded corner colour)

    // the maximise button is answered by the system (snap layouts), so the ui cannot see the pointer over it
    [[nodiscard]] bool maximize_hovered() const noexcept { return max_hover_; }
    [[nodiscard]] bool maximize_pressed() const noexcept { return max_down_; }

    // set by the owner of the ui
    std::function<HitZone(strata::vec2 logical)> hit_test;
    std::function<bool()>                        on_close_request; // false keeps the window open
    std::function<void()>                        on_live_resize;   // paint while the frame is being dragged

private:
    static std::intptr_t __stdcall proc(void* hwnd, std::uint32_t msg, std::uintptr_t wparam, std::intptr_t lparam);
    std::intptr_t handle_message(void* hwnd, std::uint32_t msg, std::uintptr_t wparam, std::intptr_t lparam);
    [[nodiscard]] std::intptr_t nc_hit_test(std::intptr_t lparam) const;

    void*                 hwnd_{};
    strata::win32_platform platform_;
    bool                  quit_{};
    bool                  minimized_{};
    bool                  resized_{};
    bool                  sizing_{}; // inside the system's resize / move loop
    bool                  max_hover_{};
    bool                  max_down_{};
    float                 pending_dpi_{};
};

} // namespace prism::platform
