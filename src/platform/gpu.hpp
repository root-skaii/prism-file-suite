#pragma once

// direct3d 11 device, flip-model swap chain and strata's renderer for one window

#ifdef _WIN32
#include "strata/backend/d3d11.hpp"
#else
#include "strata/backend/opengl.hpp"
#endif
#include "strata/context.hpp"

#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace prism::platform {

class Gpu {
public:
#ifdef _WIN32
    using Renderer = strata::d3d11_renderer;
#else
    using Renderer = strata::gl_renderer;
#endif
    // opaque handle to a D3D11 device + immediate context that multiple windows on the ui thread can share
    // (avoids repeating the ~180 ms D3D11CreateDevice call for every new window or Properties window)
    struct SharedDevice;

    Gpu();
    ~Gpu();
    Gpu(const Gpu&)            = delete;
    Gpu& operator=(const Gpu&) = delete;

    // `ui` must have its font pixels (a fresh context, or after rebuild_font_atlas)
    [[nodiscard]] bool create(void* hwnd, strata::context& ui);
    // the same in three steps, in this order: the device needs neither window nor context (the driver load is ~180 ms, so
    // it can run on its own thread from the very start), the swap chain needs the window, the renderer the fonts
    [[nodiscard]] bool create_device();
    // reuses an existing healthy device from another window; false if null or lost (caller falls back to create_device)
    [[nodiscard]] bool use_shared_device(const std::shared_ptr<SharedDevice>& shared);
    [[nodiscard]] std::shared_ptr<SharedDevice> shared_device() const;
    [[nodiscard]] bool create_swap_chain(void* hwnd);
    [[nodiscard]] bool create_renderer(strata::context& ui);

    void resize();                 // swap-chain buffers follow the window
    // while the window is being dragged to a new size: present without waiting for the next refresh, so each step costs
    // the drawing and nothing else (the vsync wait per step is what makes a live resize feel like it trails the mouse)
    void set_immediate(bool on) noexcept;
    // blocks until the next screen refresh. the loop calls it while something animates, so frames come once per refresh
    // instead of as fast as the cpu can make them (that floods the display queue and then stalls for several refreshes)
    void pace_to_display() const noexcept;
    void wait_for_frame() const;   // returns when the swap chain can take a frame (least input lag)
    // draws `ui`'s last frame over `clear` and presents. false: the device was lost (call recreate)
    [[nodiscard]] bool present(strata::context& ui, strata::color clear);
    // the same for a frame the caller has already split (see strata::draw_data::head): only these commands go to the window
    [[nodiscard]] bool present(const strata::draw_data& frame, strata::color clear);
    [[nodiscard]] ID3D11Device*        device() const noexcept;   // shared with the popup surface
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept;
    [[nodiscard]] bool recreate(strata::context& ui);
    void update_atlas(strata::context& ui); // after set_scale / glyph-set changes

    [[nodiscard]] Renderer& renderer() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::platform
