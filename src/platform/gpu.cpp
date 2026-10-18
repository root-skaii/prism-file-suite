#include "platform/gpu.hpp"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

namespace prism::platform {

using Microsoft::WRL::ComPtr;

struct Gpu::Impl {
    HWND                           hwnd{};
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGISwapChain2>        chain;
    ComPtr<ID3D11RenderTargetView> rtv;
    HANDLE                         latency_wait{};
    bool                           tearing{};
    UINT                           chain_flags{};
    bool                           first_present_done{};
    bool                           immediate{};
    strata::d3d11_renderer         renderer;

    void release() noexcept
    {
        if (ctx != nullptr) { ctx->ClearState(); }
        rtv.Reset();
        renderer.destroy();
        if (latency_wait != nullptr) { ::CloseHandle(latency_wait); latency_wait = nullptr; }
        chain.Reset();
        ctx.Reset();
        device.Reset();
    }

    [[nodiscard]] bool make_target() noexcept
    {
        rtv.Reset();
        ComPtr<ID3D11Texture2D> back;
        return SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back))) && SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, &rtv));
    }

    [[nodiscard]] bool build(strata::context& ui)
    {
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef PRISM_DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &device, nullptr, &ctx);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) { // (no sdk layers installed)
            flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
            hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &device, nullptr, &ctx);
        }
        if (FAILED(hr) && FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
            return false;
        }
        ComPtr<IDXGIDevice>   dxgi_device;
        ComPtr<IDXGIAdapter>  adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        ComPtr<IDXGIFactory5> factory5;
        BOOL allow_tearing = FALSE;
        tearing = SUCCEEDED(factory.As(&factory5)) &&
                  SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof allow_tearing)) &&
                  allow_tearing != FALSE;
        chain_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount      = 2;
        sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags            = chain_flags;
        ComPtr<IDXGISwapChain1> chain1;
        if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &chain1)) || FAILED(chain1.As(&chain))) {
            return false;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        chain->SetMaximumFrameLatency(1);
        latency_wait = chain->GetFrameLatencyWaitableObject();
        if (!make_target()) { return false; }

        if (ui.font().pixels().empty() && !ui.rebuild_font_atlas()) { return false; }
        if (!renderer.create(device.Get(), ctx.Get(), ui.font())) { return false; }
        renderer.set_state_restore(false); // (the device is ours: nothing to put back)
        ui.release_font_pixels();
        ui.invalidate();
        first_present_done = false;
        return true;
    }
};

Gpu::Gpu() : impl_{std::make_unique<Impl>()} {}

Gpu::~Gpu()
{
    if (impl_ != nullptr) { impl_->release(); }
}

bool Gpu::create(void* hwnd, strata::context& ui)
{
    impl_->hwnd = static_cast<HWND>(hwnd);
    return impl_->build(ui);
}

void Gpu::resize()
{
    Impl& s = *impl_;
    if (s.chain == nullptr) { return; }
    s.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    s.rtv.Reset();
    if (SUCCEEDED(s.chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, s.chain_flags))) { (void)s.make_target(); }
}

void Gpu::set_immediate(bool on) noexcept { impl_->immediate = on; }

void Gpu::wait_for_frame() const
{
    // the waitable is signalled by a completed Present: waiting after a frame that presented nothing would stall
    if (impl_->latency_wait != nullptr && impl_->first_present_done) { ::WaitForSingleObjectEx(impl_->latency_wait, 100, TRUE); }
}

bool Gpu::present(strata::context& ui, strata::color clear)
{
    Impl& s = *impl_;
    if (s.rtv == nullptr) { return true; }
    const float c[4] = {clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, clear.a / 255.0f};
    ID3D11RenderTargetView* t = s.rtv.Get();
    s.ctx->OMSetRenderTargets(1, &t, nullptr);
    s.ctx->ClearRenderTargetView(t, c);
    s.renderer.render(ui.render_data());
    const HRESULT hr = s.immediate ? s.chain->Present(0, s.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0) : s.chain->Present(1, 0);
    s.first_present_done = true;
    return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
}

bool Gpu::recreate(strata::context& ui)
{
    impl_->release();
    return impl_->build(ui);
}

void Gpu::update_atlas(strata::context& ui)
{
    (void)impl_->renderer.update_atlas(ui.font());
    ui.release_font_pixels();
}

strata::d3d11_renderer& Gpu::renderer() noexcept { return impl_->renderer; }

} // namespace prism::platform
