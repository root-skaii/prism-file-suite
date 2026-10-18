#pragma once

// direct3d 11 device, flip-model swap chain and strata's renderer for one window

#include "strata/backend/d3d11.hpp"
#include "strata/context.hpp"

#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace prism::platform {

class Gpu {
public:
    Gpu();
    ~Gpu();
    Gpu(const Gpu&)            = delete;
    Gpu& operator=(const Gpu&) = delete;

    // `ui` must have its font pixels (a fresh context, or after rebuild_font_atlas)
    [[nodiscard]] bool create(void* hwnd, strata::context& ui);

    void resize();                 // swap-chain buffers follow the window
    // while the window is being dragged to a new size: present without waiting for the next refresh, so each step costs
    // the drawing and nothing else (the vsync wait per step is what makes a live resize feel like it trails the mouse)
    void set_immediate(bool on) noexcept;
    void wait_for_frame() const;   // returns when the swap chain can take a frame (least input lag)
    // draws `ui`'s last frame over `clear` and presents. false: the device was lost (call recreate)
    [[nodiscard]] bool present(strata::context& ui, strata::color clear);
    [[nodiscard]] bool recreate(strata::context& ui);
    void update_atlas(strata::context& ui); // after set_scale / glyph-set changes

    [[nodiscard]] strata::d3d11_renderer& renderer() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::platform
