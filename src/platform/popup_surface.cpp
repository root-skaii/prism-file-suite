#include "platform/popup_surface.hpp"

#include "core/log.hpp"

#include <windows.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>

namespace prism::platform {

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t k_class[] = L"prismfs_popup_surface";

LRESULT CALLBACK surface_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:    return HTTRANSPARENT; // (the main window has the mouse: clicks never land here)
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    default:              return ::DefWindowProcW(h, msg, wp, lp);
    }
}

} // namespace

struct PopupSurface::Impl {
    HWND                           hwnd{};
    ComPtr<ID3D11Device>           device;
    ComPtr<IDXGISwapChain1>        chain;
    ComPtr<IDCompositionDevice>    dcomp;
    ComPtr<IDCompositionTarget>    target;
    ComPtr<IDCompositionVisual>    visual;
    ComPtr<ID3D11RenderTargetView> rtv;
    int                            w{}, h{};
    bool                           shown{};

    [[nodiscard]] bool make_chain(int width, int height)
    {
        ComPtr<IDXGIDevice>   dxgi_device;
        ComPtr<IDXGIAdapter>  adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { return false; }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width            = static_cast<UINT>(width);
        sd.Height           = static_cast<UINT>(height);
        sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount      = 2;
        sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED; // (strata's shader output is premultiplied: see ui.hlsl)
        sd.Scaling          = DXGI_SCALING_STRETCH;
        if (FAILED(factory->CreateSwapChainForComposition(device.Get(), &sd, nullptr, &chain))) { return false; }
        if (FAILED(dcomp->CreateVisual(&visual)) || FAILED(visual->SetContent(chain.Get())) ||
            FAILED(target->SetRoot(visual.Get())) || FAILED(dcomp->Commit())) {
            return false;
        }
        return make_target();
    }

    [[nodiscard]] bool make_target()
    {
        rtv.Reset();
        ComPtr<ID3D11Texture2D> back;
        return SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back))) && SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, &rtv));
    }

    [[nodiscard]] bool fit(int width, int height)
    {
        if (chain == nullptr) {
            if (!make_chain(width, height)) { return false; }
            w = width;
            h = height;
            return true;
        }
        if (width == w && height == h) { return true; }
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        rtv.Reset();
        if (FAILED(chain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0)) || !make_target()) { return false; }
        w = width;
        h = height;
        return true;
    }
};

PopupSurface::PopupSurface() : impl_{std::make_unique<Impl>()} {}

PopupSurface::~PopupSurface() { destroy(); }

bool PopupSurface::created() const noexcept { return impl_->hwnd != nullptr; }

bool PopupSurface::visible() const noexcept { return impl_->shown; }

bool PopupSurface::create(void* owner, ID3D11Device* device)
{
    Impl& s = *impl_;
    if (s.hwnd != nullptr) { return true; }
    s.device = device;
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = surface_proc;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = k_class;
        return ::RegisterClassExW(&wc) != 0;
    }();
    if (!registered) { return false; }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(s.device.As(&dxgi_device)) || FAILED(::DCompositionCreateDevice(dxgi_device.Get(), IID_PPV_ARGS(&s.dcomp)))) {
        core::log::info("popup surface: DirectComposition is not available");
        s.device.Reset();
        return false;
    }
    // (no redirection bitmap: the visual is the window's whole content)
    s.hwnd = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, k_class, L"", WS_POPUP, 0, 0, 1, 1,
                               static_cast<HWND>(owner), nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (s.hwnd == nullptr || FAILED(s.dcomp->CreateTargetForHwnd(s.hwnd, TRUE, &s.target))) {
        destroy();
        return false;
    }
    return true;
}

void PopupSurface::destroy() noexcept
{
    Impl& s = *impl_;
    s.rtv.Reset();
    s.visual.Reset();
    s.target.Reset();
    s.chain.Reset();
    s.dcomp.Reset();
    s.device.Reset();
    if (s.hwnd != nullptr) { ::DestroyWindow(s.hwnd); s.hwnd = nullptr; }
    s.shown = false;
    s.w = s.h = 0;
}

void PopupSurface::hide() noexcept
{
    Impl& s = *impl_;
    if (s.hwnd != nullptr && s.shown) { ::ShowWindow(s.hwnd, SW_HIDE); }
    s.shown = false;
}

bool PopupSurface::show(strata::d3d11_renderer& renderer, strata::draw_data overlay, strata::vec2 origin, int screen_x, int screen_y, int width, int height)
{
    Impl& s = *impl_;
    if (s.hwnd == nullptr || width <= 0 || height <= 0) { return false; }
    // (a menu cannot be bigger than a few screens: this only guards a runaway size from a swap chain it cannot make)
    width  = std::min(width, 8192);
    height = std::min(height, 8192);
    if (!s.fit(width, height)) { return false; }

    ComPtr<ID3D11DeviceContext> ctx;
    s.device->GetImmediateContext(&ctx);
    ID3D11RenderTargetView* t = s.rtv.Get();
    ctx->OMSetRenderTargets(1, &t, nullptr);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->ClearRenderTargetView(t, clear);
    overlay.origin       = origin;
    overlay.display_size = {static_cast<float>(width), static_cast<float>(height)};
    renderer.render(overlay);

    // move before presenting: the new content and the new place appear together
    ::SetWindowPos(s.hwnd, nullptr, screen_x, screen_y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | (s.shown ? 0u : SWP_SHOWWINDOW));
    s.shown = true;
    // (no vertical sync wait: the main window's present already paces the frame, and the compositor shows the newest one)
    return SUCCEEDED(s.chain->Present(0, 0));
}

} // namespace prism::platform
