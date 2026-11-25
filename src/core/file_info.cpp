#include "core/file_info.hpp"
#include "core/thread_priority.hpp"

#include "core/file_kind.hpp"
#include "core/path.hpp"
#include "core/rclone.hpp"
#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <objbase.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <winver.h>
#include <wrl/client.h>
#else
#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <thread>

namespace prism::core {

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

namespace {

[[nodiscard]] std::uint64_t pack(DWORD hi, DWORD lo) noexcept { return (static_cast<std::uint64_t>(hi) << 32) | lo; }

struct ComGuard {
    bool init{};
    ComGuard() : init{SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))} {}
    ~ComGuard() { if (init) { ::CoUninitialize(); } }
};

[[nodiscard]] std::uint64_t cluster_size(const std::wstring& path)
{
    wchar_t root[MAX_PATH]{};
    if (!::GetVolumePathNameW(path.c_str(), root, MAX_PATH)) { return 4096; }
    DWORD spc = 0, bps = 0, free_c = 0, total_c = 0;
    if (!::GetDiskFreeSpaceW(root, &spc, &bps, &free_c, &total_c)) { return 4096; }
    return static_cast<std::uint64_t>(spc) * bps;
}

[[nodiscard]] std::string version_info(const std::wstring& w, std::vector<std::pair<std::string, std::string>>& out)
{
    DWORD handle = 0;
    const DWORD n = ::GetFileVersionInfoSizeW(w.c_str(), &handle);
    if (n == 0) { return {}; }
    std::vector<char> data(n);
    if (!::GetFileVersionInfoW(w.c_str(), 0, n, data.data())) { return {}; }

    struct Lang { WORD lang, cp; };
    Lang* tr = nullptr;
    UINT  len = 0;
    if (!::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &len) || len < sizeof(Lang)) { return {}; }

    static constexpr const wchar_t* keys[] = {L"FileDescription", L"FileVersion", L"ProductName", L"ProductVersion", L"CompanyName", L"LegalCopyright", L"OriginalFilename", L"InternalName"};
    for (const wchar_t* key : keys) {
        wchar_t sub[96];
        std::swprintf(sub, 96, L"\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, key);
        wchar_t* value = nullptr;
        UINT vlen = 0;
        if (::VerQueryValueW(data.data(), sub, reinterpret_cast<LPVOID*>(&value), &vlen) && vlen > 1 && value != nullptr) {
            out.emplace_back(text::narrow(key), text::narrow(value));
        }
    }
    return {};
}

[[nodiscard]] std::string format_duration_100ns(std::uint64_t ticks)
{
    const std::uint64_t total_sec = ticks / 10000000ull;
    const std::uint64_t h = total_sec / 3600;
    const std::uint64_t m = (total_sec % 3600) / 60;
    const std::uint64_t s = total_sec % 60;
    if (h > 0) { return std::format("{}:{:02}:{:02}", h, m, s); }
    return std::format("{:02}:{:02}", m, s);
}

[[nodiscard]] std::string format_aspect(std::uint32_t w, std::uint32_t h)
{
    if (w == 0 || h == 0) { return {}; }
    const std::uint32_t g = std::gcd(w, h);
    const std::uint32_t rw = w / g;
    const std::uint32_t rh = h / g;
    if (rw <= 32 && rh <= 32) { return std::format("{}:{}", rw, rh); }
    return std::format("{:.2f}:1", static_cast<double>(w) / static_cast<double>(h));
}

[[nodiscard]] std::string format_megapixels(std::uint32_t w, std::uint32_t h)
{
    const double mp = (static_cast<double>(w) * static_cast<double>(h)) / 1000000.0;
    if (mp >= 10.0) { return std::format("{:.1f} MP", mp); }
    if (mp >= 0.1)  { return std::format("{:.2f} MP", mp); }
    return {};
}

[[nodiscard]] std::string pixel_format_label(const WICPixelFormatGUID& fmt, std::uint32_t& bpp_out)
{
    if (fmt == GUID_WICPixelFormat32bppBGRA || fmt == GUID_WICPixelFormat32bppRGBA || fmt == GUID_WICPixelFormat32bppPBGRA || fmt == GUID_WICPixelFormat32bppPRGBA) {
        bpp_out = 32; return "32-bit RGBA";
    }
    if (fmt == GUID_WICPixelFormat32bppBGR || fmt == GUID_WICPixelFormat32bppRGB) { bpp_out = 32; return "32-bit RGB"; }
    if (fmt == GUID_WICPixelFormat24bppBGR || fmt == GUID_WICPixelFormat24bppRGB) { bpp_out = 24; return "24-bit RGB"; }
    if (fmt == GUID_WICPixelFormat64bppRGBA || fmt == GUID_WICPixelFormat64bppBGRA) { bpp_out = 64; return "64-bit RGBA"; }
    if (fmt == GUID_WICPixelFormat48bppRGB || fmt == GUID_WICPixelFormat48bppBGR) { bpp_out = 48; return "48-bit RGB"; }
    if (fmt == GUID_WICPixelFormat16bppGray) { bpp_out = 16; return "16-bit Grayscale"; }
    if (fmt == GUID_WICPixelFormat8bppGray)  { bpp_out = 8;  return "8-bit Grayscale"; }
    if (fmt == GUID_WICPixelFormat8bppIndexed) { bpp_out = 8; return "8-bit Indexed"; }
    if (fmt == GUID_WICPixelFormat4bppIndexed) { bpp_out = 4; return "4-bit Indexed"; }
    if (fmt == GUID_WICPixelFormat1bppIndexed || fmt == GUID_WICPixelFormatBlackWhite) { bpp_out = 1; return "1-bit Monochrome"; }
    if (fmt == GUID_WICPixelFormat32bppCMYK) { bpp_out = 32; return "32-bit CMYK"; }
    return {};
}

[[nodiscard]] std::string prop_str(IPropertyStore* ps, const PROPERTYKEY& key)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string out;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt != VT_EMPTY && pv.vt != VT_NULL) {
        PWSTR s = nullptr;
        if (SUCCEEDED(::PropVariantToStringAlloc(pv, &s)) && s != nullptr) {
            out = text::narrow(s);
            ::CoTaskMemFree(s);
        }
    }
    PropVariantClear(&pv);
    return out;
}

[[nodiscard]] std::uint64_t prop_u64(IPropertyStore* ps, const PROPERTYKEY& key)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::uint64_t v = 0;
    if (SUCCEEDED(ps->GetValue(key, &pv))) {
        ULONGLONG tmp = 0;
        if (SUCCEEDED(::PropVariantToUInt64(pv, &tmp))) { v = tmp; }
    }
    PropVariantClear(&pv);
    return v;
}

[[nodiscard]] double prop_double(IPropertyStore* ps, const PROPERTYKEY& key, bool& ok)
{
    ok = false;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    double v = 0.0;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt != VT_EMPTY && pv.vt != VT_NULL) {
        if (SUCCEEDED(::PropVariantToDouble(pv, &v))) { ok = true; }
    }
    PropVariantClear(&pv);
    return v;
}

[[nodiscard]] std::uint64_t prop_filetime(IPropertyStore* ps, const PROPERTYKEY& key)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::uint64_t v = 0;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt == VT_FILETIME) {
        v = pack(pv.filetime.dwHighDateTime, pv.filetime.dwLowDateTime);
    }
    PropVariantClear(&pv);
    return v;
}

void read_image_wic(const std::wstring& w, FileInfo& f, MetaGroup& img)
{
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) { return; }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(w.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) { return; }

    UINT frames = 0;
    if (SUCCEEDED(decoder->GetFrameCount(&frames))) { f.img_frames = frames; }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) { return; }

    UINT width = 0, height = 0;
    if (SUCCEEDED(frame->GetSize(&width, &height)) && width > 0 && height > 0) {
        f.img_width  = width;
        f.img_height = height;
        img.rows.emplace_back("Dimensions", std::format("{} \xc3\x97 {} px", width, height));
        if (const std::string mp = format_megapixels(width, height); !mp.empty()) {
            img.rows.emplace_back("Megapixels", mp);
        }
        if (const std::string ar = format_aspect(width, height); !ar.empty()) {
            img.rows.emplace_back("Aspect ratio", ar);
        }
    }
    WICPixelFormatGUID pf{};
    if (SUCCEEDED(frame->GetPixelFormat(&pf))) {
        std::string label = pixel_format_label(pf, f.img_bpp);
        if (label.empty()) {
            ComPtr<IWICComponentInfo> ci;
            ComPtr<IWICPixelFormatInfo> pfi;
            UINT bpp = 0;
            if (SUCCEEDED(factory->CreateComponentInfo(pf, &ci)) && SUCCEEDED(ci.As(&pfi)) && SUCCEEDED(pfi->GetBitsPerPixel(&bpp)) && bpp > 0) {
                f.img_bpp = bpp;
                label = std::format("{}-bit", bpp);
            }
        }
        if (!label.empty()) { img.rows.emplace_back("Color depth", label); }
    }
    double dx = 0.0, dy = 0.0;
    if (SUCCEEDED(frame->GetResolution(&dx, &dy)) && dx > 0.5 && dy > 0.5) {
        f.img_dpi_x = dx;
        f.img_dpi_y = dy;
        if (std::abs(dx - dy) < 0.5) {
            img.rows.emplace_back("DPI", std::format("{:.0f} DPI", dx));
        } else {
            img.rows.emplace_back("DPI", std::format("{:.0f} \xc3\x97 {:.0f} DPI", dx, dy));
        }
    }
    if (f.img_frames > 1) {
        img.rows.emplace_back("Frames", std::to_string(f.img_frames));
    }
}

void read_shortcut(const std::wstring& w, FileInfo& f)
{
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> pf;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&pf))) { return; }
    if (FAILED(pf->Load(w.c_str(), STGM_READ))) { return; }

    MetaGroup grp{"Shortcut", {}};
    wchar_t buf[1024]{};
    if (SUCCEEDED(link->GetPath(buf, 1024, nullptr, SLGP_RAWPATH)) && buf[0] != L'\0') {
        const std::string t = text::narrow(buf);
        if (f.link_target.empty()) { f.link_target = t; }
        grp.rows.emplace_back("Target", t);
    }
    buf[0] = L'\0';
    if (SUCCEEDED(link->GetArguments(buf, 1024)) && buf[0] != L'\0') {
        grp.rows.emplace_back("Arguments", text::narrow(buf));
    }
    buf[0] = L'\0';
    if (SUCCEEDED(link->GetWorkingDirectory(buf, 1024)) && buf[0] != L'\0') {
        grp.rows.emplace_back("Start in", text::narrow(buf));
    }
    buf[0] = L'\0';
    if (SUCCEEDED(link->GetDescription(buf, 1024)) && buf[0] != L'\0') {
        grp.rows.emplace_back("Comment", text::narrow(buf));
    }
    if (!grp.rows.empty()) { f.meta_groups.push_back(std::move(grp)); }
}

void read_shell_properties(const std::wstring& w, FileKind kind, FileInfo& f, MetaGroup& img_grp)
{
    // avoid hydrating cloud-only placeholders when reading extended shell properties
    if ((f.attributes & (0x00400000u | 0x00040000u | FILE_ATTRIBUTE_OFFLINE)) != 0) { return; }

    ComPtr<IPropertyStore> ps;
    if (FAILED(::SHGetPropertyStoreFromParsingName(w.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))) || !ps) { return; }

    if (kind == FileKind::image) {
        if (f.img_width == 0 || f.img_height == 0) {
            const auto pw = static_cast<std::uint32_t>(prop_u64(ps.Get(), PKEY_Image_HorizontalSize));
            const auto ph = static_cast<std::uint32_t>(prop_u64(ps.Get(), PKEY_Image_VerticalSize));
            if (pw > 0 && ph > 0) {
                f.img_width  = pw;
                f.img_height = ph;
                img_grp.rows.emplace_back("Dimensions", std::format("{} \xc3\x97 {} px", pw, ph));
                if (const std::string mp = format_megapixels(pw, ph); !mp.empty()) { img_grp.rows.emplace_back("Megapixels", mp); }
                if (const std::string ar = format_aspect(pw, ph); !ar.empty()) { img_grp.rows.emplace_back("Aspect ratio", ar); }
            }
        }
        MetaGroup cam{"Camera", {}};
        if (const std::string v = prop_str(ps.Get(), PKEY_Photo_CameraManufacturer); !v.empty()) { cam.rows.emplace_back("Camera maker", v); }
        if (const std::string v = prop_str(ps.Get(), PKEY_Photo_CameraModel); !v.empty()) { cam.rows.emplace_back("Camera model", v); }
        if (const std::uint64_t dt = prop_filetime(ps.Get(), PKEY_Photo_DateTaken); dt != 0) { cam.rows.emplace_back("Date taken", path::format_time(dt)); }
        bool ok = false;
        if (const double fn = prop_double(ps.Get(), PKEY_Photo_FNumber, ok); ok && fn > 0.0) { cam.rows.emplace_back("Aperture", std::format("f/{:.1f}", fn)); }
        if (const double ex = prop_double(ps.Get(), PKEY_Photo_ExposureTime, ok); ok && ex > 0.0) {
            cam.rows.emplace_back("Exposure", ex < 0.5 ? std::format("1/{:.0f} s", std::round(1.0 / ex)) : std::format("{:.2f} s", ex));
        }
        if (const std::uint64_t iso = prop_u64(ps.Get(), PKEY_Photo_ISOSpeed); iso > 0) { cam.rows.emplace_back("ISO speed", std::format("ISO-{}", iso)); }
        if (const double fl = prop_double(ps.Get(), PKEY_Photo_FocalLength, ok); ok && fl > 0.0) { cam.rows.emplace_back("Focal length", std::format("{:.0f} mm", fl)); }
        if (!cam.rows.empty()) { f.meta_groups.push_back(std::move(cam)); }
    } else if (kind == FileKind::video) {
        MetaGroup vid{"Video", {}};
        const auto vw = static_cast<std::uint32_t>(prop_u64(ps.Get(), PKEY_Video_FrameWidth));
        const auto vh = static_cast<std::uint32_t>(prop_u64(ps.Get(), PKEY_Video_FrameHeight));
        if (vw > 0 && vh > 0) {
            f.img_width  = vw;
            f.img_height = vh;
            vid.rows.emplace_back("Resolution", std::format("{} \xc3\x97 {}", vw, vh));
        }
        if (const std::uint64_t dur = prop_u64(ps.Get(), PKEY_Media_Duration); dur > 0) {
            f.media_duration = format_duration_100ns(dur);
            vid.rows.emplace_back("Duration", f.media_duration);
        }
        if (const std::uint64_t fps = prop_u64(ps.Get(), PKEY_Video_FrameRate); fps > 0) {
            vid.rows.emplace_back("Frame rate", std::format("{:.2f} fps", static_cast<double>(fps) / 1000.0));
        }
        if (const std::uint64_t br = prop_u64(ps.Get(), PKEY_Video_TotalBitrate); br > 0) {
            f.media_bitrate = br >= 1000000 ? std::format("{:.1f} Mbps", static_cast<double>(br) / 1000000.0) : std::format("{} kbps", br / 1000);
            vid.rows.emplace_back("Total bitrate", f.media_bitrate);
        }
        if (const std::uint64_t sr = prop_u64(ps.Get(), PKEY_Audio_SampleRate); sr > 0) {
            vid.rows.emplace_back("Audio sample rate", std::format("{:.1f} kHz", static_cast<double>(sr) / 1000.0));
        }
        if (const std::uint64_t ch = prop_u64(ps.Get(), PKEY_Audio_ChannelCount); ch > 0) {
            vid.rows.emplace_back("Audio channels", ch == 1 ? "Mono" : (ch == 2 ? "Stereo" : std::to_string(ch)));
        }
        if (!vid.rows.empty()) { f.meta_groups.push_back(std::move(vid)); }
    } else if (kind == FileKind::audio) {
        MetaGroup aud{"Audio", {}};
        if (const std::uint64_t dur = prop_u64(ps.Get(), PKEY_Media_Duration); dur > 0) {
            f.media_duration = format_duration_100ns(dur);
            aud.rows.emplace_back("Duration", f.media_duration);
        }
        if (const std::uint64_t br = prop_u64(ps.Get(), PKEY_Audio_EncodingBitrate); br > 0) {
            f.media_bitrate = std::format("{} kbps", br / 1000);
            aud.rows.emplace_back("Bitrate", f.media_bitrate);
        }
        if (const std::uint64_t sr = prop_u64(ps.Get(), PKEY_Audio_SampleRate); sr > 0) {
            aud.rows.emplace_back("Sample rate", std::format("{:.1f} kHz", static_cast<double>(sr) / 1000.0));
        }
        if (const std::uint64_t ch = prop_u64(ps.Get(), PKEY_Audio_ChannelCount); ch > 0) {
            aud.rows.emplace_back("Channels", ch == 1 ? "Mono" : (ch == 2 ? "Stereo" : std::to_string(ch)));
        }
        if (const std::string v = prop_str(ps.Get(), PKEY_Title); !v.empty()) { aud.rows.emplace_back("Title", v); }
        if (const std::string v = prop_str(ps.Get(), PKEY_Music_Artist); !v.empty()) { aud.rows.emplace_back("Artist", v); }
        if (const std::string v = prop_str(ps.Get(), PKEY_Music_AlbumTitle); !v.empty()) { aud.rows.emplace_back("Album", v); }
        if (const std::uint64_t yr = prop_u64(ps.Get(), PKEY_Media_Year); yr > 0) { aud.rows.emplace_back("Year", std::to_string(yr)); }
        if (const std::string v = prop_str(ps.Get(), PKEY_Music_Genre); !v.empty()) { aud.rows.emplace_back("Genre", v); }
        if (!aud.rows.empty()) { f.meta_groups.push_back(std::move(aud)); }
    } else if (kind == FileKind::pdf || kind == FileKind::document || kind == FileKind::spreadsheet || kind == FileKind::presentation) {
        MetaGroup doc{"Document", {}};
        if (const std::string v = prop_str(ps.Get(), PKEY_Title); !v.empty()) { doc.rows.emplace_back("Title", v); }
        if (const std::string v = prop_str(ps.Get(), PKEY_Author); !v.empty()) { doc.rows.emplace_back("Authors", v); }
        if (const std::string v = prop_str(ps.Get(), PKEY_Subject); !v.empty()) { doc.rows.emplace_back("Subject", v); }
        if (const std::uint64_t pg = prop_u64(ps.Get(), PKEY_Document_PageCount); pg > 0) { doc.rows.emplace_back("Pages", std::to_string(pg)); }
        if (const std::uint64_t wd = prop_u64(ps.Get(), PKEY_Document_WordCount); wd > 0) { doc.rows.emplace_back("Words", std::to_string(wd)); }
        if (const std::string v = prop_str(ps.Get(), PKEY_ApplicationName); !v.empty()) { doc.rows.emplace_back("Program", v); }
        if (!doc.rows.empty()) { f.meta_groups.push_back(std::move(doc)); }
    }
}

void read_motw(const std::wstring& w, FileInfo& f)
{
    const std::wstring ads = w + L":Zone.Identifier";
    HANDLE h = ::CreateFileW(ads.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return; }
    char buf[2048]{};
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    ::CloseHandle(h);
    if (!ok || got == 0) { return; }
    std::string_view text_v{buf, got};
    while (!text_v.empty()) {
        std::size_t nl = text_v.find('\n');
        std::string_view line = nl == std::string_view::npos ? text_v : text_v.substr(0, nl);
        if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
        if (line.starts_with("ZoneId=")) {
            f.motw_zone = std::atoi(std::string{line.substr(7)}.c_str());
            if (f.motw_zone >= 3) { f.motw_blocked = true; }
        } else if (line.starts_with("HostUrl=") && f.motw_host.empty()) {
            f.motw_host = std::string{line.substr(8)};
        } else if (line.starts_with("ReferrerUrl=") && f.motw_host.empty()) {
            f.motw_host = std::string{line.substr(12)};
        }
        if (nl == std::string_view::npos) { break; }
        text_v.remove_prefix(nl + 1);
    }
}

} // namespace

FileInfo query_file_info(const std::string& p)
{
    FileInfo f;
    f.path = p;
    f.name = path::leaf(p);
    if (rclone::is_path(p)) {
        const rclone::SplitPath sp = rclone::split(p);
        f.exists       = true;
        f.volume       = rclone::make_root(sp.remote);
        f.volume_label = sp.remote;
        f.filesystem   = "Cloud";
        if (sp.rel_unix.empty()) {
            f.is_dir   = true;
            f.is_drive = true;
            f.type     = "Cloud drive";
            (void)rclone::about(sp.remote, f.drive_total, f.drive_free);
        } else {
            FileEntry entry;
            std::string err;
            if (rclone::stat(p, entry, err)) {
                f.is_dir       = entry.is_dir();
                f.size         = entry.size;
                f.size_on_disk = entry.size;
                f.modified     = entry.modified;
                if (f.is_dir) {
                    f.type = "File folder";
                } else {
                    const std::string ext = text::lower_ascii(entry.ext());
                    f.type = type_label(entry.kind, ext);
                }
            } else {
                f.type = "Cloud item";
            }
        }
        return f;
    }
    const std::wstring w = path::long_wide(p);

    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d)) { return f; }
    f.exists     = true;
    f.attributes = d.dwFileAttributes;
    f.is_dir     = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    f.is_drive   = f.is_dir && path::is_root(p);
    f.created    = pack(d.ftCreationTime.dwHighDateTime, d.ftCreationTime.dwLowDateTime);
    f.modified   = pack(d.ftLastWriteTime.dwHighDateTime, d.ftLastWriteTime.dwLowDateTime);
    f.accessed   = pack(d.ftLastAccessTime.dwHighDateTime, d.ftLastAccessTime.dwLowDateTime);

    FileKind kind = FileKind::folder;
    std::string ext;
    if (f.is_dir) {
        f.type = f.is_drive ? "Local disk" : "File folder";
    } else {
        f.size = pack(d.nFileSizeHigh, d.nFileSizeLow);
        const std::size_t dot = f.name.rfind('.');
        ext  = dot == std::string::npos || dot == 0 ? std::string{} : text::lower_ascii(std::string_view{f.name}.substr(dot + 1));
        kind = classify_extension(ext);
        f.type = type_label(kind, ext);
        DWORD hi = 0;
        const DWORD lo = ::GetCompressedFileSizeW(w.c_str(), &hi);
        const std::uint64_t phys = lo == INVALID_FILE_SIZE && ::GetLastError() != NO_ERROR ? f.size : pack(hi, lo);
        const std::uint64_t cl = cluster_size(w);
        f.size_on_disk = (phys + cl - 1) / cl * cl;
        (void)version_info(w, f.version);
    }

    // ntfs details through a handle: file id, hard links, the final path of a link
    HANDLE h = ::CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION bi{};
        if (::GetFileInformationByHandle(h, &bi)) {
            f.hard_links = bi.nNumberOfLinks;
            f.file_id    = pack(bi.nFileIndexHigh, bi.nFileIndexLow);
        }
        if ((f.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            std::wstring fin(MAX_PATH, L'\0');
            DWORD got = ::GetFinalPathNameByHandleW(h, fin.data(), static_cast<DWORD>(fin.size()), FILE_NAME_NORMALIZED);
            if (got >= fin.size()) {
                fin.assign(got + 1, L'\0');
                got = ::GetFinalPathNameByHandleW(h, fin.data(), static_cast<DWORD>(fin.size()), FILE_NAME_NORMALIZED);
            }
            if (got > 0 && got < fin.size()) {
                fin.resize(got);
                std::string t = text::narrow(fin);
                if (t.rfind("\\\\?\\UNC\\", 0) == 0) { t = "\\\\" + t.substr(8); }
                else if (t.rfind("\\\\?\\", 0) == 0) { t.erase(0, 4); }
                if (t != p) { f.link_target = t; }
            }
        }
        ::CloseHandle(h);
    }

    wchar_t root[MAX_PATH]{};
    if (::GetVolumePathNameW(w.c_str(), root, MAX_PATH)) {
        f.volume = text::narrow(root);
        wchar_t label[MAX_PATH + 1]{}, fs[64]{};
        if (::GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, fs, 64)) {
            f.volume_label = text::narrow(label);
            f.filesystem   = text::narrow(fs);
        }
        if (f.is_drive) {
            ULARGE_INTEGER avail{}, total{}, free_b{};
            if (::GetDiskFreeSpaceExW(root, &avail, &total, &free_b)) {
                f.drive_total = total.QuadPart;
                f.drive_free  = free_b.QuadPart;
            }
        }
    }

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(w.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) == ERROR_SUCCESS && owner != nullptr) {
        wchar_t name[256]{}, domain[256]{};
        DWORD nl = 256, dl = 256;
        SID_NAME_USE use{};
        if (::LookupAccountSidW(nullptr, owner, name, &nl, domain, &dl, &use)) {
            f.owner = text::narrow(domain) + "\\" + text::narrow(name);
        }
    }
    if (sd != nullptr) { ::LocalFree(sd); }

    if (!f.is_dir) {
        WIN32_FIND_STREAM_DATA sd2{};
        HANDLE s = ::FindFirstStreamW(w.c_str(), FindStreamInfoStandard, &sd2, 0);
        if (s != INVALID_HANDLE_VALUE) {
            do {
                std::string n = text::narrow(sd2.cStreamName);
                if (n != "::$DATA") { f.streams.emplace_back(std::move(n), static_cast<std::uint64_t>(sd2.StreamSize.QuadPart)); }
            } while (::FindNextStreamW(s, &sd2));
            ::FindClose(s);
        }
        read_motw(w, f);

        // format-specific metadata (WIC for images, IShellLink for .lnk, Windows Property Store for media / EXIF / docs)
        const bool cloud_only = (f.attributes & (0x00400000u | 0x00040000u | FILE_ATTRIBUTE_OFFLINE)) != 0;
        ComGuard com;
        MetaGroup img_grp{"Image", {}};
        if (kind == FileKind::image && !cloud_only) { read_image_wic(w, f, img_grp); }
        if (ext == "lnk" && !cloud_only) { read_shortcut(w, f); }
        read_shell_properties(text::widen(p), kind, f, img_grp);
        if (!img_grp.rows.empty()) { f.meta_groups.insert(f.meta_groups.begin(), std::move(img_grp)); }

        // concise header summary
        if (f.img_width > 0 && f.img_height > 0) {
            f.media_summary = std::format("{} \xc3\x97 {}", f.img_width, f.img_height);
            if (kind == FileKind::image) {
                if (const std::string mp = format_megapixels(f.img_width, f.img_height); !mp.empty()) { f.media_summary += "  \xc2\xb7  " + mp; }
            } else if (!f.media_duration.empty()) {
                f.media_summary += "  \xc2\xb7  " + f.media_duration;
            }
        } else if (!f.media_duration.empty()) {
            f.media_summary = f.media_duration;
            if (!f.media_bitrate.empty()) { f.media_summary += "  \xc2\xb7  " + f.media_bitrate; }
        }
    }
    return f;
}

bool set_file_attribute(const std::string& p, std::uint32_t flag, bool on)
{
    const std::wstring w = path::long_wide(p);
    const DWORD a = ::GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { return false; }
    return ::SetFileAttributesW(w.c_str(), on ? (a | flag) : (a & ~flag)) != FALSE;
}

bool unblock_file(const std::string& p)
{
    const std::wstring ads = path::long_wide(p) + L":Zone.Identifier";
    if (::DeleteFileW(ads.c_str()) != FALSE) { return true; }
    const DWORD e = ::GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

// --- async file info job ------------------------------------------------------------------------------------

void FileInfoJob::start(std::vector<std::string> paths, std::function<void()> wake)
{
    cancel();
    s_ = std::make_shared<State>();
    std::thread{[s = s_, paths = std::move(paths), wake = std::move(wake)] {
        lower_thread_priority();
        std::vector<FileInfo> out;
        out.reserve(paths.size());
        for (const std::string& p : paths) {
            if (s->cancel.load(std::memory_order_relaxed)) { return; }
            out.push_back(query_file_info(p));
        }
        {
            std::lock_guard g{s->lock};
            s->results = std::move(out);
        }
        s->done.store(true, std::memory_order_release);
        if (wake) { wake(); }
    }}.detach();
}

void FileInfoJob::cancel() noexcept
{
    if (s_) { s_->cancel.store(true, std::memory_order_relaxed); }
    s_.reset();
}

std::vector<FileInfo> FileInfoJob::take()
{
    if (!s_ || !s_->done.load(std::memory_order_acquire)) { return {}; }
    std::lock_guard g{s_->lock};
    return std::move(s_->results);
}

// --- folder scan -------------------------------------------------------------------------------------------

void FolderScan::start(const std::string& root)
{
    cancel();
    s_ = std::make_shared<State>();
    std::thread{[s = s_, root] {
        lower_thread_priority();
        if (rclone::is_path(root)) {
            std::uint64_t count = 0, bytes = 0;
            std::string err;
            if (rclone::dir_size(root, count, bytes, err) && !s->cancel.load(std::memory_order_relaxed)) {
                s->files.store(count, std::memory_order_relaxed);
                s->bytes.store(bytes, std::memory_order_relaxed);
                s->disk.store(bytes, std::memory_order_relaxed);
            }
            s->done.store(true);
            return;
        }
        std::vector<std::wstring> stack{path::long_wide(root)};
        std::uint64_t cl = cluster_size(stack.back());
        while (!stack.empty() && !s->cancel.load(std::memory_order_relaxed)) {
            const std::wstring dir = std::move(stack.back());
            stack.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) { continue; }
            do {
                const wchar_t* n = fd.cFileName;
                if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    s->folders.fetch_add(1, std::memory_order_relaxed);
                    // (symlinks / junctions are counted but not followed: no cycles; cloud folders ARE counted)
                    const bool is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && path::is_link_reparse_tag(fd.dwReserved0);
                    if (!is_link) { stack.push_back(dir + L"\\" + n); }
                } else {
                    const std::uint64_t sz = pack(fd.nFileSizeHigh, fd.nFileSizeLow);
                    s->files.fetch_add(1, std::memory_order_relaxed);
                    s->bytes.fetch_add(sz, std::memory_order_relaxed);
                    s->disk.fetch_add((sz + cl - 1) / cl * cl, std::memory_order_relaxed);
                }
            } while (::FindNextFileW(h, &fd) && !s->cancel.load(std::memory_order_relaxed));
            ::FindClose(h);
        }
        s->done.store(true);
    }}.detach();
}

void FolderScan::cancel() noexcept
{
    if (s_) { s_->cancel.store(true); }
    s_.reset();
}

// --- hashes ---------------------------------------------------------------------------------------------------

namespace {

struct Alg {
    BCRYPT_ALG_HANDLE  alg{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<UCHAR> obj;
    DWORD              len{};
    bool open(LPCWSTR id)
    {
        DWORD got = 0, objlen = 0;
        if (BCryptOpenAlgorithmProvider(&alg, id, nullptr, 0) < 0) { return false; }
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objlen), sizeof objlen, &got, 0);
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof len, &got, 0);
        obj.resize(objlen);
        return BCryptCreateHash(alg, &hash, obj.data(), objlen, nullptr, 0, 0) >= 0;
    }
    std::string finish()
    {
        std::vector<UCHAR> out(len);
        if (BCryptFinishHash(hash, out.data(), len, 0) < 0) { return {}; }
        static constexpr char hex[] = "0123456789abcdef";
        std::string s;
        for (const UCHAR b : out) { s.push_back(hex[b >> 4]); s.push_back(hex[b & 15]); }
        return s;
    }
    ~Alg()
    {
        if (hash != nullptr) { BCryptDestroyHash(hash); }
        if (alg != nullptr) { BCryptCloseAlgorithmProvider(alg, 0); }
    }
};

} // namespace

void HashJob::start(const std::string& p)
{
    cancel();
    s_ = std::make_shared<State>();
    std::thread{[s = s_, w = path::long_wide(p)] {
        lower_thread_priority();
        HANDLE f = ::CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        Alg a5, a1, a256;
        if (f == INVALID_HANDLE_VALUE || !a5.open(BCRYPT_MD5_ALGORITHM) || !a1.open(BCRYPT_SHA1_ALGORITHM) || !a256.open(BCRYPT_SHA256_ALGORITHM)) {
            if (f != INVALID_HANDLE_VALUE) { ::CloseHandle(f); }
            s->failed.store(true);
            s->done.store(true);
            return;
        }
        LARGE_INTEGER total{};
        ::GetFileSizeEx(f, &total);
        std::vector<UCHAR> buf(1 << 20);
        std::uint64_t read_total = 0;
        DWORD n = 0;
        while (!s->cancel.load(std::memory_order_relaxed) && ::ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) {
            BCryptHashData(a5.hash, buf.data(), n, 0);
            BCryptHashData(a1.hash, buf.data(), n, 0);
            BCryptHashData(a256.hash, buf.data(), n, 0);
            read_total += n;
            if (total.QuadPart > 0) { s->progress.store(static_cast<int>(read_total * 1000 / static_cast<std::uint64_t>(total.QuadPart))); }
        }
        ::CloseHandle(f);
        if (!s->cancel.load()) {
            std::lock_guard g{s->lock};
            s->md5    = a5.finish();
            s->sha1   = a1.finish();
            s->sha256 = a256.finish();
            s->progress.store(1000);
        }
        s->done.store(true);
    }}.detach();
}

void HashJob::cancel() noexcept
{
    if (s_) { s_->cancel.store(true); }
    s_.reset();
}

std::string HashJob::md5() const { if (!s_) { return {}; } std::lock_guard g{s_->lock}; return s_->md5; }
std::string HashJob::sha1() const { if (!s_) { return {}; } std::lock_guard g{s_->lock}; return s_->sha1; }
std::string HashJob::sha256() const { if (!s_) { return {}; } std::lock_guard g{s_->lock}; return s_->sha256; }
#else
FileInfo query_file_info(const std::string& path)
{
    FileInfo info;
    info.path = path;
    info.name = path::leaf(path);
    if (rclone::is_path(path)) {
        const rclone::SplitPath sp = rclone::split(path);
        info.exists   = true;
        info.is_dir   = sp.rel_unix.empty();
        info.is_drive = sp.rel_unix.empty();
        info.type     = sp.rel_unix.empty() ? "Cloud drive" : "Cloud item";
        if (sp.rel_unix.empty()) {
            info.volume_label = sp.remote;
            (void)rclone::about(sp.remote, info.drive_total, info.drive_free);
        }
        return info;
    }
    struct stat lst{}, st{};
    if (::lstat(path.c_str(), &lst) != 0) { return info; }
    const bool is_link = S_ISLNK(lst.st_mode);
    if (is_link) {
        char target[PATH_MAX]{};
        const ssize_t n = ::readlink(path.c_str(), target, sizeof(target) - 1);
        if (n > 0) { info.link_target.assign(target, static_cast<std::size_t>(n)); }
    }
    const bool has_stat = ::stat(path.c_str(), &st) == 0;
    const struct stat& s = has_stat ? st : lst;
    info.exists       = true;
    info.is_dir       = S_ISDIR(s.st_mode);
    info.is_drive     = path == "/";
    info.size         = info.is_dir ? 0 : static_cast<std::uint64_t>(std::max<off_t>(0, s.st_size));
    info.size_on_disk = static_cast<std::uint64_t>(std::max<blkcnt_t>(0, s.st_blocks)) * 512ULL;
    info.modified     = (static_cast<std::uint64_t>(s.st_mtime) + 11644473600ULL) * 10000000ULL;
    info.accessed     = (static_cast<std::uint64_t>(s.st_atime) + 11644473600ULL) * 10000000ULL;
    info.created      = (static_cast<std::uint64_t>(s.st_ctime) + 11644473600ULL) * 10000000ULL;
    info.hard_links   = static_cast<std::uint32_t>(s.st_nlink);
    info.file_id      = static_cast<std::uint64_t>(s.st_ino);
    if (info.is_dir) { info.attributes |= attr::directory; }
    if (is_link) { info.attributes |= attr::reparse; }
    if (!info.name.empty() && info.name.front() == '.') { info.attributes |= attr::hidden; }
    if ((s.st_mode & 0222) == 0) { info.attributes |= attr::readonly; }
    if (const passwd* pw = ::getpwuid(s.st_uid); pw != nullptr && pw->pw_name != nullptr) { info.owner = pw->pw_name; }
    const std::size_t dot = info.is_dir ? std::string::npos : info.name.rfind('.');
    const std::string ext = (dot == std::string::npos || dot == 0) ? std::string{} : text::lower_ascii(std::string_view{info.name}.substr(dot + 1));
    info.type = std::string{type_label(info.is_dir ? FileKind::folder : classify_extension(ext), ext)};
    struct statvfs sv{};
    if (::statvfs(path.c_str(), &sv) == 0 && sv.f_blocks > 0) {
        info.drive_total = static_cast<std::uint64_t>(sv.f_blocks) * sv.f_frsize;
        info.drive_free  = static_cast<std::uint64_t>(sv.f_bavail) * sv.f_frsize;
    }
    return info;
}

bool set_file_attribute(const std::string& path, std::uint32_t flag, bool on)
{
    if (flag != attr::readonly) { return false; }
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) { return false; }
    const mode_t mode = on ? (st.st_mode & ~static_cast<mode_t>(0222)) : (st.st_mode | static_cast<mode_t>(0200));
    return ::chmod(path.c_str(), mode) == 0;
}

bool unblock_file(const std::string&) { return false; }

void FileInfoJob::start(std::vector<std::string> paths, std::function<void()> wake)
{
    cancel();
    auto s = std::make_shared<State>();
    s_ = s;
    std::thread{[s, paths = std::move(paths), wake = std::move(wake)] {
        lower_thread_priority();
        std::vector<FileInfo> out;
        out.reserve(paths.size());
        for (const std::string& p : paths) {
            if (s->cancel.load(std::memory_order_relaxed)) { return; }
            out.push_back(query_file_info(p));
        }
        {
            std::lock_guard g{s->lock};
            s->results = std::move(out);
        }
        s->done.store(true, std::memory_order_release);
        if (wake) { wake(); }
    }}.detach();
}

void FileInfoJob::cancel() noexcept { if (s_) { s_->cancel.store(true, std::memory_order_relaxed); } s_.reset(); }

std::vector<FileInfo> FileInfoJob::take()
{
    if (!s_) { return {}; }
    std::lock_guard g{s_->lock};
    return std::move(s_->results);
}

void FolderScan::start(const std::string& root)
{
    cancel();
    auto s = std::make_shared<State>();
    s_ = s;
    std::thread{[s, root] {
        lower_thread_priority();
        std::vector<std::string> stack{root};
        while (!stack.empty() && !s->cancel.load()) {
            std::string dir = std::move(stack.back());
            stack.pop_back();
            DIR* d = ::opendir(dir.c_str());
            if (d == nullptr) { continue; }
            while (const dirent* ent = ::readdir(d)) {
                if (s->cancel.load()) { break; }
                const char* n = ent->d_name;
                if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) { continue; }
                const std::string full = path::join(dir, n);
                struct stat lst{};
                if (::lstat(full.c_str(), &lst) != 0) { continue; }
                if (S_ISDIR(lst.st_mode)) {
                    s->folders.fetch_add(1);
                    stack.push_back(full);
                } else {
                    s->files.fetch_add(1);
                    s->bytes.fetch_add(static_cast<std::uint64_t>(std::max<off_t>(0, lst.st_size)));
                    s->disk.fetch_add(static_cast<std::uint64_t>(std::max<blkcnt_t>(0, lst.st_blocks)) * 512ULL);
                }
            }
            ::closedir(d);
        }
        s->done.store(true);
    }}.detach();
}

void FolderScan::cancel() noexcept { if (s_) { s_->cancel.store(true); } s_.reset(); }

void HashJob::start(const std::string&) { cancel(); }
void HashJob::cancel() noexcept { if (s_) { s_->cancel.store(true); } s_.reset(); }
std::string HashJob::md5() const { return {}; }
std::string HashJob::sha1() const { return {}; }
std::string HashJob::sha256() const { return {}; }
#endif

} // namespace prism::core
