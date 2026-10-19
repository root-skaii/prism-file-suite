#include "core/image_tools.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <format>

namespace prism::core {

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::array<std::string_view, 11> k_image_exts{".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff", ".heic", ".webp", ".jfif", ".ico"};

// COM for this thread: a thread that already has it (in the other threading mode) is fine too, it just must not be undone
struct ComScope {
    bool ok;
    bool owned;
    ComScope()
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr);
        ok    = owned || hr == RPC_E_CHANGED_MODE;
    }
    ~ComScope() { if (owned) { ::CoUninitialize(); } }
};

[[nodiscard]] std::string lower_ext(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string{} : text::lower_ascii(name.substr(dot));
}

struct Target {
    GUID        container{};
    std::string ext;        // ".jpg" ...
    bool        jpeg{};
};

// the file format a resized or rotated copy keeps: the original's when it can be written, else PNG
[[nodiscard]] Target same_format(std::string_view name)
{
    const std::string e = lower_ext(name);
    if (e == ".jpg" || e == ".jpeg" || e == ".jfif") { return {GUID_ContainerFormatJpeg, ".jpg", true}; }
    if (e == ".bmp") { return {GUID_ContainerFormatBmp, ".bmp", false}; }
    if (e == ".tif" || e == ".tiff") { return {GUID_ContainerFormatTiff, ".tif", false}; }
    if (e == ".gif") { return {GUID_ContainerFormatGif, ".gif", false}; }
    return {GUID_ContainerFormatPng, ".png", false};
}

[[nodiscard]] std::string unique_path(const std::string& dir, const std::string& stem, const std::string& ext)
{
    for (int i = 1; i < 1000; ++i) {
        const std::string name = (i == 1 ? stem : std::format("{} ({})", stem, i)) + ext;
        if (!path::exists(path::join(dir, name))) { return path::join(dir, name); }
    }
    return path::join(dir, stem + ext);
}

[[nodiscard]] std::string stem_of(const std::string& name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

} // namespace

bool is_image_name(std::string_view name)
{
    const std::string e = lower_ext(name);
    return std::ranges::any_of(k_image_exts, [&](std::string_view x) { return e == x; });
}

ImageResult transform_image(const std::string& source, ImageOp op)
{
    ImageResult res;
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    if (!com.ok || FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        res.error = "Windows' imaging component is not available";
        return res;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(text::widen(source).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
        res.error = "Could not read " + path::leaf(source) + " as an image";
        return res;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) { res.error = "Could not read " + path::leaf(source); return res; }
    ComPtr<IWICBitmapSource> image = frame;
    UINT w = 0, h = 0;
    image->GetSize(&w, &h);
    if (w == 0 || h == 0) { res.error = path::leaf(source) + " is empty"; return res; }

    const std::string dir  = path::parent(source);
    const std::string stem = stem_of(path::leaf(source));
    Target target = same_format(path::leaf(source));
    std::string suffix;
    switch (op) {
    case ImageOp::to_jpeg: target = {GUID_ContainerFormatJpeg, ".jpg", true}; break;
    case ImageOp::to_png:  target = {GUID_ContainerFormatPng, ".png", false}; break;
    case ImageOp::half:
    case ImageOp::quarter:
    case ImageOp::fit_1920: {
        UINT nw = w, nh = h;
        if (op == ImageOp::half) { nw = std::max(1u, w / 2); nh = std::max(1u, h / 2); suffix = " - 50%"; }
        else if (op == ImageOp::quarter) { nw = std::max(1u, w / 4); nh = std::max(1u, h / 4); suffix = " - 25%"; }
        else {
            suffix = " - 1920";
            if (std::max(w, h) > 1920) {
                const double k = 1920.0 / static_cast<double>(std::max(w, h));
                nw = std::max(1u, static_cast<UINT>(static_cast<double>(w) * k + 0.5));
                nh = std::max(1u, static_cast<UINT>(static_cast<double>(h) * k + 0.5));
            }
        }
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(image.Get(), nw, nh, WICBitmapInterpolationModeHighQualityCubic))) {
            res.error = "Could not resize " + path::leaf(source);
            return res;
        }
        image = scaler;
        break;
    }
    case ImageOp::rotate_left:
    case ImageOp::rotate_right: {
        ComPtr<IWICBitmapFlipRotator> rotator;
        if (FAILED(factory->CreateBitmapFlipRotator(&rotator)) ||
            FAILED(rotator->Initialize(image.Get(), op == ImageOp::rotate_left ? WICBitmapTransformRotate270 : WICBitmapTransformRotate90))) {
            res.error = "Could not rotate " + path::leaf(source);
            return res;
        }
        image = rotator;
        suffix = op == ImageOp::rotate_left ? " - rotated left" : " - rotated right";
        break;
    }
    }
    if (op == ImageOp::to_jpeg && lower_ext(path::leaf(source)) == ".jpg") { res.error = path::leaf(source) + " is a JPEG already"; return res; }
    if (op == ImageOp::to_png && lower_ext(path::leaf(source)) == ".png") { res.error = path::leaf(source) + " is a PNG already"; return res; }

    const std::string out = unique_path(dir, stem + suffix, target.ext);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(text::widen(out).c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(target.container, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        res.error = "Could not write " + path::leaf(out);
        return res;
    }
    ComPtr<IWICBitmapFrameEncode> out_frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&out_frame, &props))) { res.error = "Could not write " + path::leaf(out); ::DeleteFileW(text::widen(out).c_str()); return res; }
    if (target.jpeg && props) {
        PROPBAG2 opt{};
        wchar_t name[] = L"ImageQuality";
        opt.pstrName = name;
        VARIANT v;
        ::VariantInit(&v);
        v.vt     = VT_R4;
        v.fltVal = 0.92f;
        (void)props->Write(1, &opt, &v);
    }
    UINT nw = 0, nh = 0;
    image->GetSize(&nw, &nh);
    WICPixelFormatGUID format = target.jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(out_frame->Initialize(props.Get())) && SUCCEEDED(out_frame->SetSize(nw, nh)) && SUCCEEDED(out_frame->SetPixelFormat(&format));
    if (ok) {
        // (the encoder may have picked another pixel format than the one asked for: the pixels are converted to what it chose)
        ComPtr<IWICBitmapSource> source_pixels = image;
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(image.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            source_pixels = converter;
        }
        ok = SUCCEEDED(out_frame->WriteSource(source_pixels.Get(), nullptr)) && SUCCEEDED(out_frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    stream.Reset();
    if (!ok) {
        ::DeleteFileW(text::widen(out).c_str());
        res.error = "Could not save " + path::leaf(out);
        return res;
    }
    res.ok   = true;
    res.path = out;
    return res;
}

ImageJobs::~ImageJobs()
{
    quit_ = true;
    for (std::thread& t : threads_) { if (t.joinable()) { t.join(); } }
}

void ImageJobs::start(ImageOp op, std::vector<std::string> sources)
{
    if (sources.empty()) { return; }
    ++running_;
    const std::lock_guard lock{lock_};
    threads_.emplace_back([this, op, sources = std::move(sources)] {
        Result r;
        std::string first_error;
        for (const std::string& s : sources) {
            if (quit_) { break; }
            const ImageResult one = transform_image(s, op);
            if (one.ok) {
                ++r.done;
                r.dir  = path::parent(one.path);
                r.made = path::leaf(one.path);
            } else if (first_error.empty()) {
                first_error = one.error;
            }
        }
        r.ok = r.done > 0;
        if (r.dir.empty() && !sources.empty()) { r.dir = path::parent(sources.front()); }
        if (r.done == sources.size()) { r.message = r.done == 1 ? "Saved " + r.made : std::format("Made {} new images", r.done); }
        else if (r.done > 0) { r.message = std::format("Made {} of {} images: {}", r.done, sources.size(), first_error); }
        else { r.message = first_error.empty() ? "Nothing was made" : first_error; }
        {
            const std::lock_guard l{lock_};
            results_.push_back(std::move(r));
        }
        --running_;
        if (wake_) { wake_(); }
    });
}

bool ImageJobs::poll(Result& out)
{
    const std::lock_guard lock{lock_};
    if (results_.empty()) { return false; }
    out = std::move(results_.front());
    results_.erase(results_.begin());
    return true;
}

} // namespace prism::core
