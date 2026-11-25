#include "core/places.hpp"
#include "core/i18n.hpp"

#include "core/path.hpp"
#include "core/recycle.hpp"
#include "core/text.hpp"

#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <winioctl.h>
#include <winnetwk.h>
#else
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>
#endif

namespace prism::core {

#ifdef _WIN32

namespace {

[[nodiscard]] std::string known_folder(const KNOWNFOLDERID& id)
{
    PWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw != nullptr) { out = text::narrow(raw); }
    ::CoTaskMemFree(raw);
    return out;
}

} // namespace

std::string home_folder()
{
    return known_folder(FOLDERID_Profile);
}

namespace {

struct KnownEntry {
    std::string_view     token;
    std::string_view     name;
    const KNOWNFOLDERID* id;
};

const KnownEntry k_known[] = {
    {"home",      "Home",      &FOLDERID_Profile},
    {"desktop",   "Desktop",   &FOLDERID_Desktop},
    {"downloads", "Downloads", &FOLDERID_Downloads},
    {"documents", "Documents", &FOLDERID_Documents},
    {"pictures",  "Pictures",  &FOLDERID_Pictures},
    {"music",     "Music",     &FOLDERID_Music},
    {"videos",    "Videos",    &FOLDERID_Videos},
};

} // namespace

std::vector<KnownFolder> known_folders()
{
    std::vector<KnownFolder> out;
    for (const KnownEntry& k : k_known) { out.push_back({k.token, k.name}); }
    return out;
}

std::string known_path(std::string_view token)
{
    for (const KnownEntry& k : k_known) {
        if (k.token != token) { continue; }
        std::string p = known_folder(*k.id);
        return !p.empty() && path::is_directory(p) ? p : std::string{};
    }
    return {};
}

std::vector<Place> quick_access()
{
    std::vector<Place> out;
    for (const KnownEntry& k : k_known) {
        std::string p = known_path(k.token);
        if (!p.empty()) { out.push_back({std::string{k.name}, std::move(p), 0, 0, false}); }
    }
    return out;
}

std::vector<Place> drives(bool include_remote)
{
    std::vector<Place> out;
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) { continue; }
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        const UINT type = ::GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE && type != DRIVE_CDROM) { continue; }
        if (type == DRIVE_REMOTE && !include_remote) { continue; }

        Place p;
        p.path      = text::narrow(root);
        p.removable = type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
        p.optical   = type == DRIVE_CDROM;
        p.network   = type == DRIVE_REMOTE;
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) { p.bin = recycle::folder(text::narrow(root)); } // (not for shares: a slow lookup)
        wchar_t label[MAX_PATH + 1]{};
        const bool ready = ::GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) != FALSE;
        // (removable media without a disk are skipped, but disconnected mapped network drives stay visible so the user can reconnect)
        if (!ready && type != DRIVE_REMOTE) { continue; }
        if (ready) {
            ULARGE_INTEGER avail{}, total{}, free_bytes{};
            if (::GetDiskFreeSpaceExW(root, &avail, &total, &free_bytes)) {
                p.total = total.QuadPart;
                p.free  = free_bytes.QuadPart;
            }
        }
        std::string name = text::narrow(label);
        if (!name.empty()) {
            const std::string low = text::lower_ascii(name);
            if (low.find("google drive") != std::string::npos || low == "my drive") { p.cloud = true; }
        }
        if (name.empty()) { name = std::string{type == DRIVE_REMOTE ? i18n::tr("Network drive") : (type == DRIVE_CDROM ? i18n::tr("Optical drive") : i18n::tr("Local Disk"))}; }
        p.name = name + " (" + static_cast<char>('A' + i) + ":)";
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Place> portable_devices()
{
    std::vector<Place> out;
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) { return out; }

    PIDLIST_ABSOLUTE this_pc  = nullptr;
    IShellFolder*    desktop  = nullptr;
    IShellFolder*    folder   = nullptr;
    IEnumIDList*     children = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &this_pc)) && SUCCEEDED(::SHGetDesktopFolder(&desktop)) &&
        SUCCEEDED(desktop->BindToObject(this_pc, nullptr, IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS, &children))) {
        PITEMID_CHILD child = nullptr;
        while (children->Next(1, &child, nullptr) == S_OK) {
            PIDLIST_ABSOLUTE full = ::ILCombine(this_pc, child);
            PWSTR parse = nullptr, shown = nullptr;
            // (drives parse as "C:\"; only devices carry a "\\?\" interface path)
            if (full != nullptr && SUCCEEDED(::SHGetNameFromIDList(full, SIGDN_DESKTOPABSOLUTEPARSING, &parse)) && std::wstring_view{parse}.contains(L"\\\\?\\") &&
                SUCCEEDED(::SHGetNameFromIDList(full, SIGDN_NORMALDISPLAY, &shown))) {
                Place p;
                p.name   = text::narrow(shown);
                p.path   = text::narrow(parse);
                p.device = true;
                out.push_back(std::move(p));
            }
            ::CoTaskMemFree(parse);
            ::CoTaskMemFree(shown);
            ::ILFree(full);
            ::ILFree(child);
        }
    }
    // (two identical phones: the shell gives both the same name; number them in the order it lists them)
    std::vector<std::string> names;
    for (const Place& p : out) { names.push_back(p.name); }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto same_name = [&](const std::string& n) { return n == names[i]; };
        if (std::ranges::count_if(names, same_name) > 1) {
            out[i].name += " (" + std::to_string(std::ranges::count_if(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(i) + 1, same_name)) + ")";
        }
    }
    if (children != nullptr) { children->Release(); }
    if (folder != nullptr) { folder->Release(); }
    if (desktop != nullptr) { desktop->Release(); }
    ::CoTaskMemFree(this_pc);
    if (SUCCEEDED(init)) { ::CoUninitialize(); }
    return out;
}

std::vector<Place> wsl_distros()
{
    std::vector<Place> out;
    HKEY root{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &root) != ERROR_SUCCESS) { return out; }
    for (DWORD idx = 0;; ++idx) {
        wchar_t sub[256]{};
        DWORD sub_len = 256;
        if (::RegEnumKeyExW(root, idx, sub, &sub_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) { break; }
        HKEY k{};
        if (::RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) { continue; }
        wchar_t name[256]{};
        DWORD size = sizeof(name) - sizeof(wchar_t);
        DWORD type = 0;
        if (::RegQueryValueExW(k, L"DistributionName", nullptr, &type, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS && type == REG_SZ && name[0] != L'\0') {
            const std::string distro = text::narrow(name);
            Place p;
            p.name = distro + " (WSL)";
            p.path = "\\\\wsl.localhost\\" + distro;
            p.wsl  = true;
            out.push_back(std::move(p));
        }
        ::RegCloseKey(k);
    }
    ::RegCloseKey(root);
    std::ranges::sort(out, [](const Place& a, const Place& b) { return text::compare_natural(a.name, b.name) < 0; });
    return out;
}

std::vector<Place> google_drive_places()
{
    std::vector<Place> out;
    const auto add_unique = [&](std::string name, std::string raw_path) {
        std::string norm = path::normalize(raw_path);
        if (norm.empty() || !path::is_directory(norm)) { return; }
        const std::string low = text::lower_ascii(norm);
        for (const Place& existing : out) {
            if (text::lower_ascii(path::normalize(existing.path)) == low) { return; }
        }
        Place p;
        p.name  = name.empty() ? std::string{"Google Drive"} : std::move(name);
        p.path  = std::move(norm);
        p.cloud = true;
        ULARGE_INTEGER avail{}, total{}, free_bytes{};
        if (::GetDiskFreeSpaceExW(text::widen(p.path).c_str(), &avail, &total, &free_bytes) && total.QuadPart > 0) {
            // only attach capacity bar when it is its own virtual volume (e.g. G:\ or G:\My Drive), not C:\Users\...
            if (p.path.size() >= 2 && p.path[1] == ':') {
                wchar_t label[MAX_PATH + 1]{};
                const wchar_t root[] = {static_cast<wchar_t>(p.path[0]), L':', L'\\', L'\0'};
                if (::GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) {
                    const std::string lbl = text::lower_ascii(text::narrow(label));
                    if (lbl.find("google") != std::string::npos) {
                        p.total = total.QuadPart;
                        p.free  = free_bytes.QuadPart;
                    }
                }
            }
        }
        out.push_back(std::move(p));
    };

    const auto read_reg_sz = [](HKEY root, const wchar_t* subkey, const wchar_t* value) -> std::string {
        HKEY k{};
        if (::RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) { return {}; }
        wchar_t buf[1024]{};
        DWORD bytes = sizeof(buf) - sizeof(wchar_t);
        DWORD type = 0;
        std::string res;
        if (::RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bytes) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0] != L'\0') {
            if (type == REG_EXPAND_SZ) {
                wchar_t exp[1024]{};
                const DWORD n = ::ExpandEnvironmentStringsW(buf, exp, 1024);
                res = text::narrow(n > 0 && n <= 1024 ? exp : buf);
            } else {
                res = text::narrow(buf);
            }
        }
        ::RegCloseKey(k);
        return res;
    };

    // 1. Windows Explorer Desktop\NameSpace shell extensions (how Google Drive for Desktop registers in Explorer's sidebar)
    {
        HKEY ns{};
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace", 0, KEY_READ, &ns) == ERROR_SUCCESS) {
            for (DWORD idx = 0;; ++idx) {
                wchar_t clsid[128]{};
                DWORD len = 128;
                if (::RegEnumKeyExW(ns, idx, clsid, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) { break; }
                const std::wstring clsid_key = std::wstring{L"Software\\Classes\\CLSID\\"} + clsid;
                const std::string title = read_reg_sz(HKEY_CURRENT_USER, clsid_key.c_str(), nullptr);
                const std::string low   = text::lower_ascii(title);
                if (low.find("google drive") == std::string::npos && low.find("google disk") == std::string::npos) { continue; }
                const std::string target = read_reg_sz(HKEY_CURRENT_USER, (clsid_key + L"\\Instance\\InitPropertyBag").c_str(), L"TargetFolderPath");
                if (!target.empty()) { add_unique(title, target); }
            }
            ::RegCloseKey(ns);
        }
    }

    // 2. Windows Cloud Filter SyncRootManager (covers Google Drive mirror/stream roots)
    {
        HKEY srm{};
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SyncRootManager", 0, KEY_READ, &srm) == ERROR_SUCCESS) {
            for (DWORD idx = 0;; ++idx) {
                wchar_t sub[256]{};
                DWORD len = 256;
                if (::RegEnumKeyExW(srm, idx, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) { break; }
                const std::string low = text::lower_ascii(text::narrow(sub));
                if (low.find("googledrive") == std::string::npos && low.find("google.drive") == std::string::npos && low.find("drivefs") == std::string::npos) { continue; }
                const std::string display = read_reg_sz(srm, sub, L"DisplayNameResource");
                HKEY paths_key{};
                const std::wstring pk = std::wstring{sub} + L"\\UserSyncRootPaths";
                if (::RegOpenKeyExW(srm, pk.c_str(), 0, KEY_QUERY_VALUE, &paths_key) == ERROR_SUCCESS) {
                    for (DWORD v = 0;; ++v) {
                        wchar_t vname[256]{};
                        DWORD vlen = 256;
                        wchar_t data[1024]{};
                        DWORD dbytes = sizeof(data) - sizeof(wchar_t);
                        DWORD type = 0;
                        if (::RegEnumValueW(paths_key, v, vname, &vlen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dbytes) != ERROR_SUCCESS) { break; }
                        if ((type == REG_SZ || type == REG_EXPAND_SZ) && data[0] != L'\0') {
                            add_unique(display.empty() || display[0] == '@' ? "Google Drive" : display, text::narrow(data));
                        }
                    }
                    ::RegCloseKey(paths_key);
                }
            }
            ::RegCloseKey(srm);
        }
    }

    // 3. Local mirror folder fallback (%USERPROFILE%\Google Drive or %USERPROFILE%\My Drive)
    if (out.empty()) {
        const std::string home = home_folder();
        if (!home.empty()) {
            add_unique("Google Drive", path::join(home, "Google Drive"));
            add_unique("Google Drive", path::join(home, "My Drive"));
        }
    }
    return out;
}

bool eject_drive(const std::string& root, std::string& error)
{
    if (root.size() < 2 || root[1] != ':') { error = "Not a removable drive"; return false; }
    const std::wstring vol = std::wstring{L"\\\\.\\"} + static_cast<wchar_t>(root[0]) + L":";
    HANDLE h = ::CreateFileW(vol.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        h = ::CreateFileW(vol.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) {
        error = "Could not access the drive (it may be in use)";
        return false;
    }
    DWORD bytes = 0;
    (void)::DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr);
    (void)::DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytes, nullptr);
    PREVENT_MEDIA_REMOVAL pmr{FALSE};
    (void)::DeviceIoControl(h, IOCTL_STORAGE_MEDIA_REMOVAL, &pmr, sizeof pmr, nullptr, 0, &bytes, nullptr);
    const BOOL ok = ::DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytes, nullptr);
    ::CloseHandle(h);
    if (!ok) {
        error = "Could not eject the drive (a file on it may still be open)";
        return false;
    }
    return true;
}

bool disconnect_network_drive(const std::string& root, std::string& error)
{
    if (root.size() < 2 || root[1] != ':') { error = "Not a mapped network drive"; return false; }
    const std::wstring local = std::wstring{static_cast<wchar_t>(root[0]), L':'};
    const DWORD res = ::WNetCancelConnection2W(local.c_str(), CONNECT_UPDATE_PROFILE, FALSE);
    if (res != NO_ERROR) {
        error = "Could not disconnect the network drive (error " + std::to_string(res) + ")";
        return false;
    }
    return true;
}
#else
std::string home_folder()
{
    const char* h = std::getenv("HOME");
    return h != nullptr ? std::string{h} : std::string{};
}

std::vector<KnownFolder> known_folders()
{
    return {{"home", "Home"}, {"desktop", "Desktop"}, {"downloads", "Downloads"}, {"documents", "Documents"},
            {"pictures", "Pictures"}, {"music", "Music"}, {"videos", "Videos"}};
}

std::string known_path(std::string_view token)
{
    const std::string h = home_folder();
    if (h.empty()) { return {}; }
    std::string p;
    if (token == "home")           { p = h; }
    else if (token == "desktop")   { p = path::join(h, "Desktop"); }
    else if (token == "downloads") { p = path::join(h, "Downloads"); }
    else if (token == "documents") { p = path::join(h, "Documents"); }
    else if (token == "pictures")  { p = path::join(h, "Pictures"); }
    else if (token == "music")     { p = path::join(h, "Music"); }
    else if (token == "videos")    { p = path::join(h, "Videos"); }
    return !p.empty() && path::is_directory(p) ? p : std::string{};
}

std::vector<Place> quick_access()
{
    std::vector<Place> out;
    for (const KnownFolder& k : known_folders()) {
        std::string p = known_path(k.token);
        if (!p.empty()) {
            Place pl;
            pl.name = std::string{k.name};
            pl.path = std::move(p);
            out.push_back(std::move(pl));
        }
    }
    return out;
}

std::vector<Place> drives(bool)
{
    std::vector<Place> out;
    std::ifstream in("/proc/mounts");
    std::string dev, mnt, fstype;
    while (in >> dev >> mnt >> fstype) {
        std::string rest;
        std::getline(in, rest);
        if (mnt != "/" && !mnt.starts_with("/mnt/") && !mnt.starts_with("/media/") && !mnt.starts_with("/run/media/")) { continue; }
        Place p;
        p.path = mnt;
        p.name = mnt == "/" ? std::string{"Root (/)"} : path::leaf(mnt);
        p.network = fstype == "cifs" || fstype == "nfs" || fstype == "nfs4" || fstype == "sshfs";
        p.removable = mnt.starts_with("/media/") || mnt.starts_with("/run/media/");
        struct statvfs sv{};
        if (::statvfs(mnt.c_str(), &sv) == 0 && sv.f_blocks > 0) {
            p.total = static_cast<std::uint64_t>(sv.f_blocks) * sv.f_frsize;
            p.free  = static_cast<std::uint64_t>(sv.f_bavail) * sv.f_frsize;
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Place> portable_devices() { return {}; }
std::vector<Place> wsl_distros() { return {}; }
std::vector<Place> google_drive_places() { return {}; }
bool eject_drive(const std::string&, std::string& error) { error = "Not supported on Linux yet"; return false; }
bool disconnect_network_drive(const std::string&, std::string& error) { error = "Not supported on Linux yet"; return false; }
#endif

} // namespace prism::core
