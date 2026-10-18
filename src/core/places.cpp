#include "core/places.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

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
        // (removable media without a disk fail here: skipped instead of showing an empty slot)
        wchar_t label[MAX_PATH + 1]{};
        if (!::GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) { continue; }
        ULARGE_INTEGER avail{}, total{}, free_bytes{};
        if (::GetDiskFreeSpaceExW(root, &avail, &total, &free_bytes)) {
            p.total = total.QuadPart;
            p.free  = free_bytes.QuadPart;
        }
        std::string name = text::narrow(label);
        if (name.empty()) { name = type == DRIVE_REMOTE ? "Network drive" : (type == DRIVE_CDROM ? "Optical drive" : "Local Disk"); }
        p.name = name + " (" + static_cast<char>('A' + i) + ":)";
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace prism::core
