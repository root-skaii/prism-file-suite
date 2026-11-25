#pragma once

// a borderless window that shows the popup layers (menus, tooltips) of a ui frame where they leave the main window. it has
// per-pixel alpha (a DirectComposition visual on a premultiplied swap chain), so rounded corners and shadows look as they do
// inside the window. it never takes focus or mouse input: the main window keeps both (it captures the mouse meanwhile)

#ifdef _WIN32
#include "strata/backend/d3d11.hpp"
#else
#include "strata/backend/opengl.hpp"
#endif
#include "strata/draw_list.hpp"

#include <memory>

struct ID3D11Device;

namespace prism::platform {

class PopupSurface {
public:
#ifdef _WIN32
    using Renderer = strata::d3d11_renderer;
#else
    using Renderer = strata::gl_renderer;
#endif
    PopupSurface();
    ~PopupSurface();
    PopupSurface(const PopupSurface&)            = delete;
    PopupSurface& operator=(const PopupSurface&) = delete;

    // `owner`: the main window (the surface stays above it). false: composition is not available on this system / device
    [[nodiscard]] bool create(void* owner, ID3D11Device* device);
    void destroy() noexcept;
    [[nodiscard]] bool created() const noexcept;

    // draws `overlay` (commands of the ui frame) into the surface: `origin` is where the surface's top-left is in the ui's
    // physical pixels, (screen_x, screen_y, width, height) where it goes on the desktop. false: it could not be drawn (the
    // caller falls back to the main window)
    [[nodiscard]] bool show(Renderer& renderer, strata::draw_data overlay, strata::vec2 origin, int screen_x, int screen_y, int width, int height);
    void hide() noexcept;
    [[nodiscard]] bool visible() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::platform
