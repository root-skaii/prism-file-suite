#include "core/recycle.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

namespace prism::core::recycle {

#ifdef _WIN32

namespace {

[[nodiscard]] bool ensure_dirs(const std::string& dir)
{
    if (dir.empty() || path::is_directory(dir)) { return true; }
    const std::string p = path::parent(dir);
    if (!p.empty() && p != dir && !ensure_dirs(p)) { return false; }
    if (::CreateDirectoryW(path::long_wide(dir).c_str(), nullptr)) { return true; }
    return ::GetLastError() == ERROR_ALREADY_EXISTS && path::is_directory(dir);
}

[[nodiscard]] bool delete_tree_long(const std::string& p)
{
    const std::wstring w = path::long_wide(p);
    const DWORD a = ::GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { return true; }
    if ((a & FILE_ATTRIBUTE_READONLY) != 0) { ::SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY); }
    if ((a & FILE_ATTRIBUTE_DIRECTORY) == 0) { return ::DeleteFileW(w.c_str()) != FALSE; }
    if ((a & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && path::is_reparse_link(p)) { return ::RemoveDirectoryW(w.c_str()) != FALSE; }
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileExW((w + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
            (void)delete_tree_long(path::join(p, text::narrow(n)));
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    return ::RemoveDirectoryW(w.c_str()) != FALSE;
}

[[nodiscard]] std::string user_sid()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) { return {}; }
    std::string out;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buf(size);
    if (size > 0 && ::GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        PWSTR str = nullptr;
        if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) { out = text::narrow(str); }
        ::LocalFree(str);
    }
    ::CloseHandle(token);
    return out;
}

} // namespace

bool inside(std::string_view path)
{
    return path.starts_with(all) || text::lower_ascii(path).find("\\$recycle.bin") != std::string::npos;
}

std::vector<std::string> folders()
{
    std::vector<std::string> out;
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) { continue; }
        const std::wstring root = {static_cast<wchar_t>(L'A' + i), L':', L'\\'};
        const UINT type = ::GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) { continue; } // (not shares: a slow lookup)
        if (std::string f = folder(text::narrow(root)); !f.empty()) { out.push_back(std::move(f)); }
    }
    return out;
}

std::optional<Record> read(const std::string& item)
{
    const std::string leaf = path::leaf(item);
    if (leaf.size() < 3 || leaf[0] != '$' || (leaf[1] != 'R' && leaf[1] != 'r') || !inside(item)) { return std::nullopt; }
    const std::string info = path::join(path::parent(item), "$I" + leaf.substr(2));
    HANDLE h = ::CreateFileW(path::long_wide(info).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return std::nullopt; }
    std::vector<unsigned char> buf(4096);
    DWORD got = 0;
    const bool ok = ::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) != FALSE;
    ::CloseHandle(h);
    if (!ok || got < 28) { return std::nullopt; }

    // version 1: 8 version, 8 size, 8 time, then a fixed 520-byte name. version 2: the same, then a character count and the name
    std::int64_t version = 0;
    std::memcpy(&version, buf.data(), sizeof version);
    Record rec;
    std::memcpy(&rec.deleted, buf.data() + 16, sizeof rec.deleted);
    std::size_t offset = 24;
    std::size_t chars  = 260;
    if (version == 2) {
        std::uint32_t n = 0;
        std::memcpy(&n, buf.data() + 24, sizeof n);
        offset = 28;
        chars  = n;
    }
    chars = std::min(chars, (got - offset) / sizeof(wchar_t));
    std::wstring original(chars, L'\0');
    std::memcpy(original.data(), buf.data() + offset, chars * sizeof(wchar_t));
    if (const std::size_t nul = original.find(L'\0'); nul != std::wstring::npos) { original.resize(nul); }
    if (original.empty()) { return std::nullopt; }
    rec.original = text::narrow(original);
    rec.info     = info;
    return rec;
}

std::string drive_of(std::string_view p)
{
    return p.size() >= 2 && p[1] == ':' && p[0] != ':' ?std::string{p.substr(0, 2)} + "\\" : std::string{};
}

std::string folder_path(std::string_view drive_root)
{
    std::string drive = drive_of(drive_root);
    if (drive.empty()) {
        wchar_t sys[8]{};
        if (::GetEnvironmentVariableW(L"SystemDrive", sys, 8) == 0) { return {}; }
        drive = text::narrow(sys) + "\\";
    }
    const std::string sid = user_sid();
    return sid.empty() ? std::string{} : drive + "$Recycle.Bin\\" + sid;
}

std::string folder(std::string_view drive_root)
{
    const std::string p = folder_path(drive_root);
    return !p.empty() && path::is_directory(p) ? p : std::string{};
}

bool restore(const std::string& item, std::string& error)
{
    const std::optional<Record> rec = read(item);
    if (!rec) { error = "Its record is missing: restore it with Windows Explorer"; return false; }
    const std::string parent = path::parent(rec->original);
    if (!parent.empty() && !ensure_dirs(parent)) {
        error = "The folder it came from is gone and could not be made again";
        return false;
    }
    std::string dest = rec->original;
    if (::GetFileAttributesW(path::long_wide(dest).c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::string name = path::leaf(dest);
        const std::size_t dot  = name.rfind('.');
        const DWORD ia = ::GetFileAttributesW(path::long_wide(item).c_str());
        const bool dir = ia != INVALID_FILE_ATTRIBUTES && (ia & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const std::string stem = dir || dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
        const std::string ext  = dir || dot == std::string::npos || dot == 0 ? std::string{} : name.substr(dot);
        for (int i = 1; i < 1000; ++i) {
            dest = path::join(parent, stem + (i == 1 ? " (restored)" : " (restored " + std::to_string(i) + ")") + ext);
            if (::GetFileAttributesW(path::long_wide(dest).c_str()) == INVALID_FILE_ATTRIBUTES) { break; }
        }
    }
    if (!::MoveFileExW(path::long_wide(item).c_str(), path::long_wide(dest).c_str(), MOVEFILE_COPY_ALLOWED)) {
        error = "Could not move it back (Windows error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    ::DeleteFileW(path::long_wide(rec->info).c_str());
    return true;
}

bool erase(const std::string& item)
{
    const std::optional<Record> rec = read(item);
    const std::wstring w = text::widen(item);
    bool ok = false;
    if (w.size() >= 248) {
        // SHFileOperationW rejects \\?\ long paths: delete directly via long-path Win32 APIs
        ok = delete_tree_long(item);
    } else {
        std::wstring from = w;
        from.push_back(L'\0'); // (SHFileOperation wants a double-terminated list)
        SHFILEOPSTRUCTW op{};
        op.wFunc  = FO_DELETE;
        op.pFrom  = from.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        ok = ::SHFileOperationW(&op) == 0 && op.fAnyOperationsAborted == FALSE;
    }
    if (ok && rec) { ::DeleteFileW(path::long_wide(rec->info).c_str()); }
    return ok;
}

void forget(const std::string& item)
{
    if (const std::optional<Record> rec = read(item)) { ::DeleteFileW(path::long_wide(rec->info).c_str()); }
}

bool empty(const std::string& drive_root, void* owner_hwnd)
{
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring root = drive_root.empty() ? std::wstring{} : text::widen(drive_root);
    const bool ok = SUCCEEDED(::SHEmptyRecycleBinW(static_cast<HWND>(owner_hwnd), root.empty() ? nullptr : root.c_str(), SHERB_NOPROGRESSUI));
    if (SUCCEEDED(init)) { ::CoUninitialize(); }
    return ok;
}
#else
namespace {
std::string xdg_trash_files()
{
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base = xdg != nullptr && xdg[0] != '\0' ? std::string{xdg} : std::string{};
    if (base.empty()) {
        const char* home = std::getenv("HOME");
        if (home != nullptr) { base = std::string{home} + "/.local/share"; }
    }
    return base.empty() ? std::string{} : base + "/Trash/files";
}
} // namespace

bool inside(std::string_view p)
{
    const std::string tf = xdg_trash_files();
    return p.starts_with(all) || (!tf.empty() && p.starts_with(tf));
}
std::vector<std::string> folders()
{
    const std::string tf = xdg_trash_files();
    return !tf.empty() && path::is_directory(tf) ? std::vector<std::string>{tf} : std::vector<std::string>{};
}
std::optional<Record> read(const std::string&) { return std::nullopt; }
std::string drive_of(std::string_view) { return {}; }
std::string folder_path(std::string_view) { return xdg_trash_files(); }
std::string folder(std::string_view)
{
    const std::string tf = xdg_trash_files();
    return !tf.empty() && path::is_directory(tf) ? tf : std::string{};
}
bool restore(const std::string&, std::string& error) { error = "Not supported on Linux yet"; return false; }
bool erase(const std::string&) { return false; }
void forget(const std::string&) {}
bool empty(const std::string&, void*) { return false; }
#endif

} // namespace prism::core::recycle
