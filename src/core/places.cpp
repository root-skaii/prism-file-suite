#include "core/places.hpp"
#include "core/i18n.hpp"

#include "core/path.hpp"
#include "core/recycle.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace prism::core {

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

std::vector<Place> drives()
{
    std::vector<Place> out;
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) { continue; }
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        const UINT type = ::GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE && type != DRIVE_CDROM) { continue; }

        Place p;
        p.path      = text::narrow(root);
        p.removable = type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
        p.optical   = type == DRIVE_CDROM;
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) { p.bin = recycle::folder(text::narrow(root)); } // (not for shares: a slow lookup)
        // (removable media without a disk fail here: skipped instead of showing an empty slot)
        wchar_t label[MAX_PATH + 1]{};
        if (!::GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) { continue; }
        ULARGE_INTEGER avail{}, total{}, free_bytes{};
        if (::GetDiskFreeSpaceExW(root, &avail, &total, &free_bytes)) {
            p.total = total.QuadPart;
            p.free  = free_bytes.QuadPart;
        }
        std::string name = text::narrow(label);
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

} // namespace prism::core
