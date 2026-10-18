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

std::vector<Place> quick_access()
{
    struct Item { const char* name; const KNOWNFOLDERID* id; };
    const Item items[] = {
        {"Home",      &FOLDERID_Profile},
        {"Desktop",   &FOLDERID_Desktop},
        {"Downloads", &FOLDERID_Downloads},
        {"Documents", &FOLDERID_Documents},
        {"Pictures",  &FOLDERID_Pictures},
        {"Music",     &FOLDERID_Music},
        {"Videos",    &FOLDERID_Videos},
    };
    std::vector<Place> out;
    for (const Item& it : items) {
        std::string p = known_folder(*it.id);
        if (!p.empty() && path::is_directory(p)) { out.push_back({it.name, std::move(p), 0, 0, false}); }
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
