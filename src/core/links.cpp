#include "core/links.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <format>
#include <vector>

namespace prism::core {

namespace {

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

// the kernel's layout of a mount-point reparse record (ntifs.h is not in the user-mode sdk)
struct MountPointBuffer {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR  PathBuffer[1];
};

[[nodiscard]] std::string reason(DWORD error)
{
    wchar_t buf[256]{};
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, buf, 256, nullptr);
    std::string s = text::narrow(std::wstring_view{buf, n});
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '.')) { s.pop_back(); }
    return s.empty() ? std::format("error {}", error) : s;
}

std::string make_junction(const std::string& target, const std::string& link)
{
    const std::wstring wl = text::widen(link);
    if (!::CreateDirectoryW(wl.c_str(), nullptr)) { return "Could not create the junction: " + reason(::GetLastError()); }
    HANDLE h = ::CreateFileW(wl.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        ::RemoveDirectoryW(wl.c_str());
        return "Could not create the junction: " + reason(e);
    }
    const std::wstring print = text::widen(path::normalize(target));
    const std::wstring subst = L"\\??\\" + print;
    const std::size_t path_bytes = (subst.size() + 1 + print.size() + 1) * sizeof(WCHAR);
    std::vector<unsigned char> raw(offsetof(MountPointBuffer, PathBuffer) + path_bytes, 0);
    auto* b = reinterpret_cast<MountPointBuffer*>(raw.data());
    b->ReparseTag            = IO_REPARSE_TAG_MOUNT_POINT;
    b->ReparseDataLength     = static_cast<USHORT>(raw.size() - 8);
    b->SubstituteNameOffset  = 0;
    b->SubstituteNameLength  = static_cast<USHORT>(subst.size() * sizeof(WCHAR));
    b->PrintNameOffset       = static_cast<USHORT>((subst.size() + 1) * sizeof(WCHAR));
    b->PrintNameLength       = static_cast<USHORT>(print.size() * sizeof(WCHAR));
    std::memcpy(b->PathBuffer, subst.c_str(), (subst.size() + 1) * sizeof(WCHAR));
    std::memcpy(reinterpret_cast<unsigned char*>(b->PathBuffer) + b->PrintNameOffset, print.c_str(), (print.size() + 1) * sizeof(WCHAR));
    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, b, static_cast<DWORD>(raw.size()), nullptr, 0, &returned, nullptr);
    const DWORD e = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);
    if (!ok) {
        ::RemoveDirectoryW(wl.c_str());
        return "Could not create the junction: " + reason(e);
    }
    return {};
}

} // namespace

std::string make_link(LinkKind kind, const std::string& target, bool target_is_dir, const std::string& link)
{
    if (path::exists(link)) { return "There is already something called " + path::leaf(link); }
    switch (kind) {
    case LinkKind::symbolic: {
        const DWORD flags = (target_is_dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0u) | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (::CreateSymbolicLinkW(text::widen(link).c_str(), text::widen(target).c_str(), flags)) { return {}; }
        const DWORD e = ::GetLastError();
        if (e == ERROR_PRIVILEGE_NOT_HELD) { return "Windows only allows symbolic links with Developer Mode on (Settings > For developers) or as administrator"; }
        return "Could not create the link: " + reason(e);
    }
    case LinkKind::hard:
        if (target_is_dir) { return "A hard link needs a file (use a junction for a folder)"; }
        if (::CreateHardLinkW(text::widen(link).c_str(), text::widen(target).c_str(), nullptr)) { return {}; }
        return "Could not create the hard link: " + reason(::GetLastError()) + " (same drive only)";
    case LinkKind::junction:
        if (!target_is_dir) { return "A junction needs a folder"; }
        return make_junction(target, link);
    }
    return {};
}

bool remove_link(const std::string& link)
{
    const std::wstring w = text::widen(link);
    const DWORD a = ::GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { return false; }
    if ((a & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && ::RemoveDirectoryW(w.c_str()) != FALSE; // (a real folder is never removed here)
    }
    return ::DeleteFileW(w.c_str()) != FALSE;
}

} // namespace prism::core
