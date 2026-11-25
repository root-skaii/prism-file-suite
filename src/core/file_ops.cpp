#include "core/file_ops.hpp"

#include "core/path.hpp"
#include "core/rclone.hpp"
#include "core/shell_ops.hpp"
#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstring>

namespace prism::core {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

namespace {

#ifndef FOFX_ADDUNDORECORD
#define FOFX_ADDUNDORECORD 0x20000000
#endif

struct ComScope {
    bool ok;
    ComScope() : ok{SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))} {}
    ~ComScope() { if (ok) { ::CoUninitialize(); } }
};

} // namespace
#endif

FileOps::~FileOps()
{
    for (auto& w : workers_) {
        if (w->thread.joinable()) { w->thread.join(); }
    }
}

void FileOps::reap_finished()
{
    for (std::size_t i = 0; i < workers_.size();) {
        if (workers_[i]->done.load(std::memory_order_acquire)) {
            if (workers_[i]->thread.joinable()) { workers_[i]->thread.join(); }
            workers_.erase(workers_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

bool FileOps::busy() const
{
    if (transfers_.busy()) { return true; }
    std::lock_guard g{lock_};
    return running_ > 0;
}

bool FileOps::poll(OpResult& out)
{
    reap_finished();
    if (transfers_.poll(out)) {
        // if a built-in transfer hit Access Denied while running unelevated, hand the failed items to IFileOperation
        // so Windows shows its standard UAC elevation prompt (only with an owner window, never during headless tests)
        if (!out.ok && !out.aborted && out.access_denied && owner_ != nullptr && !shell::is_elevated() &&
            (out.kind == OpKind::copy || out.kind == OpKind::move)) {
            std::vector<std::string> retry;
            for (std::size_t i = 0; i < out.sources.size(); ++i) {
                if (i >= out.made.size() || out.made[i].empty()) { retry.push_back(out.sources[i]); }
            }
            if (!retry.empty()) {
                {
                    std::lock_guard g{lock_};
                    ++running_;
                }
                auto w = std::make_unique<Worker>();
                Worker* raw = w.get();
                w->thread = std::thread([this, kind = out.kind, silent = out.silent, raw, s = std::move(retry), d = out.dest]() mutable {
                    run(kind, std::move(s), std::move(d), silent, raw);
                });
                workers_.push_back(std::move(w));
                return false;
            }
        }
        return true;
    }
    std::lock_guard g{lock_};
    if (results_.empty()) { return false; }
    out = std::move(results_.front());
    results_.erase(results_.begin());
    return true;
}

void FileOps::start(OpKind kind, std::vector<std::string> sources, std::string dest_dir, bool silent)
{
    if (sources.empty()) { return; }
    const bool any_rclone = rclone::is_path(dest_dir) ||
                            std::ranges::any_of(sources, [](const std::string& s) { return rclone::is_path(s); });
    if (!any_rclone && builtin_ && (kind == OpKind::copy || kind == OpKind::move) && !dest_dir.empty()) {
        transfers_.add(kind, std::move(sources), std::move(dest_dir), silent);
        return;
    }
    reap_finished();
    {
        std::lock_guard g{lock_};
        ++running_;
    }
    auto w = std::make_unique<Worker>();
    Worker* raw = w.get();
    w->thread = std::thread([this, kind, silent, raw, s = std::move(sources), d = std::move(dest_dir)]() mutable { run(kind, std::move(s), std::move(d), silent, raw); });
    workers_.push_back(std::move(w));
}

void FileOps::run(OpKind kind, std::vector<std::string> sources, std::string dest, bool silent, Worker* w)
{
    OpResult res;
    res.kind    = kind;
    res.silent  = silent;
    res.count   = sources.size();
    res.sources = sources;
    res.dest    = dest.empty() ? path::parent(sources.front()) : dest;
    const bool any_rclone = rclone::is_path(dest) ||
                            std::ranges::any_of(sources, [](const std::string& s) { return rclone::is_path(s); });
    if (any_rclone) {
        bool all_ok = true;
        for (const std::string& s : sources) {
            std::string err;
            bool ok = false;
            if (kind == OpKind::copy || kind == OpKind::move) {
                ok = rclone::copy_or_move(kind == OpKind::move, s, dest, err);
            } else {
                ok = rclone::remove(s, err);
            }
            if (!ok) {
                all_ok = false;
                if (res.message.empty() && !err.empty()) { res.message = err; }
            }
        }
        res.ok = all_ok;
    } else {
#ifdef _WIN32
        ComScope com;
        ComPtr<IFileOperation> op;
        if (com.ok && SUCCEEDED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
            DWORD flags = FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD;
            if (kind != OpKind::erase) { flags |= FOF_ALLOWUNDO; }
            // deleting is confirmed by our own dialog (or by the user pressing Del), not twice
            if (kind == OpKind::erase || kind == OpKind::recycle) { flags |= FOF_NOCONFIRMATION; }
            // a copy into the folder it came from keeps both: "name - Copy"
            if (kind == OpKind::copy && !sources.empty() && text::lower_ascii(path::normalize(path::parent(sources.front()))) == text::lower_ascii(path::normalize(dest))) { flags |= FOF_RENAMEONCOLLISION; }
            op->SetOperationFlags(flags);
            if (owner_ != nullptr) { op->SetOwnerWindow(static_cast<HWND>(owner_)); }

            ComPtr<IShellItem> target;
            if (kind == OpKind::copy || kind == OpKind::move) {
                (void)::SHCreateItemFromParsingName(text::widen(dest).c_str(), nullptr, IID_PPV_ARGS(&target));
            }
            bool queued = false;
            for (const std::string& s : sources) {
                ComPtr<IShellItem> item;
                if (FAILED(::SHCreateItemFromParsingName(text::widen(s).c_str(), nullptr, IID_PPV_ARGS(&item)))) { continue; }
                switch (kind) {
                case OpKind::copy:    queued |= target != nullptr && SUCCEEDED(op->CopyItem(item.Get(), target.Get(), nullptr, nullptr)); break;
                case OpKind::move:    queued |= target != nullptr && SUCCEEDED(op->MoveItem(item.Get(), target.Get(), nullptr, nullptr)); break;
                case OpKind::recycle:
                case OpKind::erase:   queued |= SUCCEEDED(op->DeleteItem(item.Get(), nullptr)); break;
                }
            }
            if (queued) {
                const HRESULT hr = op->PerformOperations();
                BOOL aborted = FALSE;
                (void)op->GetAnyOperationsAborted(&aborted);
                res.aborted = aborted != FALSE;
                res.ok      = SUCCEEDED(hr) && !res.aborted;
            }
        }
#else
        bool all_ok = true;
        for (const std::string& s : sources) {
            if (kind == OpKind::erase || kind == OpKind::recycle) {
                if (::unlink(s.c_str()) != 0 && ::rmdir(s.c_str()) != 0) { all_ok = false; }
            } else if (kind == OpKind::move && !dest.empty()) {
                const std::string to = path::join(dest, path::leaf(s));
                if (::rename(s.c_str(), to.c_str()) != 0) { all_ok = false; }
            } else {
                all_ok = false;
            }
        }
        res.ok = all_ok;
#endif
    }
    {
        std::lock_guard g{lock_};
        results_.push_back(std::move(res));
        --running_;
    }
    if (w != nullptr) { w->done.store(true, std::memory_order_release); }
    if (wake_) { wake_(); }
}

bool FileOps::rename(const std::string& path, const std::string& new_name)
{
    if (new_name.empty() || new_name.find_first_of("\\/:*?\"<>|") != std::string::npos) { return false; }
    if (rclone::is_path(path)) {
        std::string err;
        return rclone::rename(path, new_name, err);
    }
    const std::string target = path::join(path::parent(path), new_name);
#ifdef _WIN32
    // (case-only renames are allowed: MoveFileEx treats them as a rename of the same file)
    const bool ok = ::MoveFileExW(path::long_wide(path).c_str(), path::long_wide(target).c_str(), 0) != FALSE;
#else
    const bool ok = ::rename(path.c_str(), target.c_str()) == 0;
#endif
    if (ok) {
        const auto hooks = rename_hooks(); // (a copy: a hook may close its window)
        for (const auto& h : hooks) { if (h.second) { h.second(path, target); } }
    }
    return ok;
}

std::string FileOps::create_folder(const std::string& dir)
{
    if (rclone::is_path(dir)) {
        std::vector<FileEntry> cur;
        std::string list_err;
        (void)rclone::list_dir(dir, cur, list_err);
        auto exists = [&](const std::string& n) {
            const std::string nl = text::lower_ascii(n);
            return std::ranges::any_of(cur, [&](const FileEntry& e) { return text::lower_ascii(e.name) == nl; });
        };
        for (int i = 1; i < 1000; ++i) {
            const std::string name = i == 1 ? "New folder" : "New folder (" + std::to_string(i) + ")";
            if (exists(name)) { continue; }
            std::string err;
            return rclone::mkdir(path::join(dir, name), err) ? name : std::string{};
        }
        return {};
    }
    for (int i = 1; i < 1000; ++i) {
        const std::string name = i == 1 ? "New folder" : "New folder (" + std::to_string(i) + ")";
#ifdef _WIN32
        if (::CreateDirectoryW(path::long_wide(path::join(dir, name)).c_str(), nullptr)) { return name; }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) { return {}; }
#else
        if (::mkdir(path::join(dir, name).c_str(), 0755) == 0) { return name; }
        if (errno != EEXIST) { return {}; }
#endif
    }
    return {};
}

std::string FileOps::create_file(const std::string& dir, const std::string& stem, const std::string& ext)
{
    if (rclone::is_path(dir)) {
        std::vector<FileEntry> cur;
        std::string list_err;
        (void)rclone::list_dir(dir, cur, list_err);
        auto exists = [&](const std::string& n) {
            const std::string nl = text::lower_ascii(n);
            return std::ranges::any_of(cur, [&](const FileEntry& e) { return text::lower_ascii(e.name) == nl; });
        };
        for (int i = 1; i < 1000; ++i) {
            const std::string name = (i == 1 ? stem : stem + " (" + std::to_string(i) + ")") + ext;
            if (exists(name)) { continue; }
            std::string err;
            return rclone::create_empty_file(path::join(dir, name), err) ? name : std::string{};
        }
        return {};
    }
    for (int i = 1; i < 1000; ++i) {
        const std::string name = (i == 1 ? stem : stem + " (" + std::to_string(i) + ")") + ext;
#ifdef _WIN32
        HANDLE h = ::CreateFileW(path::long_wide(path::join(dir, name)).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
            return name;
        }
        if (::GetLastError() != ERROR_FILE_EXISTS && ::GetLastError() != ERROR_ALREADY_EXISTS) { return {}; }
#else
        const int fd = ::open(path::join(dir, name).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            ::close(fd);
            return name;
        }
        if (errno != EEXIST) { return {}; }
#endif
    }
    return {};
}

bool FileOps::remove_empty_file(const std::string& p)
{
    if (rclone::is_path(p)) {
        std::string err;
        return rclone::remove(p, err);
    }
#ifdef _WIN32
    const std::wstring w = path::long_wide(p);
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d) || d.nFileSizeLow != 0 || d.nFileSizeHigh != 0) { return false; }
    return ::DeleteFileW(w.c_str()) != FALSE;
#else
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0 || st.st_size != 0) { return false; }
    return ::unlink(p.c_str()) == 0;
#endif
}

bool FileOps::remove_empty_folder(const std::string& p)
{
    if (rclone::is_path(p)) {
        std::string err;
        return rclone::remove(p, err);
    }
#ifdef _WIN32
    return ::RemoveDirectoryW(path::long_wide(p).c_str()) != FALSE;
#else
    return ::rmdir(p.c_str()) == 0;
#endif
}

bool FileOps::restore_from_recycle(const std::vector<std::string>& originals)
{
#ifndef _WIN32
    (void)originals;
    return false;
#else
    if (originals.empty()) { return false; }
    ComScope com;
    ComPtr<IShellFolder> desktop;
    if (FAILED(::SHGetDesktopFolder(&desktop))) { return false; }
    PIDLIST_ABSOLUTE bin_id = nullptr;
    if (FAILED(::SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &bin_id)) || bin_id == nullptr) { return false; }
    ComPtr<IShellFolder2> bin;
    const HRESULT bound = desktop->BindToObject(bin_id, nullptr, IID_PPV_ARGS(&bin));
    ::CoTaskMemFree(bin_id);
    if (FAILED(bound)) { return false; }
    ComPtr<IEnumIDList> en;
    if (FAILED(bin->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en)) || en == nullptr) { return false; }

    const auto detail = [&](PCUITEMID_CHILD id, UINT column) {
        SHELLDETAILS d{};
        wchar_t buf[MAX_PATH * 2]{};
        if (SUCCEEDED(bin->GetDetailsOf(id, column, &d)) && SUCCEEDED(::StrRetToBufW(&d.str, id, buf, MAX_PATH * 2))) { return text::narrow(buf); }
        return std::string{};
    };
    const auto lower = [](std::string s) { return text::lower_ascii(s); };
    const auto without_ext = [](const std::string& s) {
        const std::size_t dot = s.rfind('.');
        const std::size_t sep = s.rfind('\\');
        return dot != std::string::npos && (sep == std::string::npos || dot > sep) ? s.substr(0, dot) : s;
    };

    std::vector<PITEMID_CHILD> match;
    std::vector<PITEMID_CHILD> all;
    PITEMID_CHILD id = nullptr;
    while (en->Next(1, &id, nullptr) == S_OK) {
        all.push_back(id);
        // the bin lists a name (the extension may be hidden) and the folder it came from
        const std::string full = path::join(detail(id, 1), detail(id, 0));
        for (const std::string& o : originals) {
            if (lower(full) == lower(o) || lower(full) == lower(without_ext(o))) {
                match.push_back(id);
                break;
            }
        }
    }
    bool restored = false;
    if (!match.empty()) {
        std::vector<PCUITEMID_CHILD> items(match.begin(), match.end());
        ComPtr<IContextMenu> menu;
        if (SUCCEEDED(bin->GetUIObjectOf(nullptr, static_cast<UINT>(items.size()), items.data(), IID_IContextMenu, nullptr, reinterpret_cast<void**>(menu.GetAddressOf())))) {
            CMINVOKECOMMANDINFO ci{};
            ci.cbSize = sizeof ci;
            ci.lpVerb = "undelete";
            ci.nShow  = SW_SHOWNORMAL;
            restored  = SUCCEEDED(menu->InvokeCommand(&ci));
        }
    }
    for (PITEMID_CHILD p : all) { ::CoTaskMemFree(p); }
    return restored;
#endif
}

// --- clipboard -----------------------------------------------------------------------------------------------

#ifdef _WIN32

namespace {

[[nodiscard]] UINT drop_effect_format()
{
    static const UINT f = ::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return f;
}

[[nodiscard]] bool open_clipboard_retry()
{
    for (int i = 0; i < 5; ++i) {
        if (::OpenClipboard(nullptr) != FALSE) { return true; }
        ::Sleep(4);
    }
    return false;
}

} // namespace

unsigned long clipboard_sequence() noexcept
{
    return ::GetClipboardSequenceNumber();
}

void clipboard_clear_cut()
{
    if (!::IsClipboardFormatAvailable(CF_HDROP) || !open_clipboard_retry()) { return; }
    bool is_cut = false;
    if (HANDLE h = ::GetClipboardData(drop_effect_format())) {
        if (const auto* e = static_cast<const DWORD*>(::GlobalLock(h))) {
            is_cut = (*e & DROPEFFECT_MOVE) != 0 && (*e & DROPEFFECT_COPY) == 0;
            ::GlobalUnlock(h);
        }
    }
    if (is_cut) { ::EmptyClipboard(); }
    ::CloseClipboard();
}

bool clipboard_set_files(const std::vector<std::string>& paths, bool cut)
{
    if (paths.empty()) { return false; }
    std::wstring list;
    for (const std::string& p : paths) {
        list += text::widen(p);
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    const std::size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL files = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    HGLOBAL effect = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (files == nullptr || effect == nullptr) {
        if (files != nullptr) { ::GlobalFree(files); }
        if (effect != nullptr) { ::GlobalFree(effect); }
        return false;
    }
    if (auto* d = static_cast<DROPFILES*>(::GlobalLock(files))) {
        d->pFiles = sizeof(DROPFILES);
        d->fWide  = TRUE;
        std::memcpy(reinterpret_cast<char*>(d) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        ::GlobalUnlock(files);
    }
    if (auto* e = static_cast<DWORD*>(::GlobalLock(effect))) {
        *e = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        ::GlobalUnlock(effect);
    }
    bool ok = false;
    if (open_clipboard_retry()) {
        ::EmptyClipboard();
        ok = ::SetClipboardData(CF_HDROP, files) != nullptr;
        if (ok) { files = nullptr; }
        if (::SetClipboardData(drop_effect_format(), effect) != nullptr) { effect = nullptr; }
        ::CloseClipboard();
    }
    if (files != nullptr) { ::GlobalFree(files); }
    if (effect != nullptr) { ::GlobalFree(effect); }
    return ok;
}

bool clipboard_has_files()
{
    return ::IsClipboardFormatAvailable(CF_HDROP) != FALSE;
}

bool clipboard_get_files(std::vector<std::string>& paths, bool& cut)
{
    paths.clear();
    cut = false;
    // passive read (called from ui thread on clipboard change): never Sleep() here if another app holds the clipboard
    if (!::IsClipboardFormatAvailable(CF_HDROP) || !::OpenClipboard(nullptr)) { return false; }
    if (HANDLE h = ::GetClipboardData(CF_HDROP)) {
        auto* drop = static_cast<HDROP>(h);
        const UINT n = ::DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
            std::wstring w(len, L'\0');
            ::DragQueryFileW(drop, i, w.data(), len + 1);
            paths.push_back(text::narrow(w));
        }
    }
    if (HANDLE h = ::GetClipboardData(drop_effect_format())) {
        if (const auto* e = static_cast<const DWORD*>(::GlobalLock(h))) {
            cut = (*e & DROPEFFECT_MOVE) != 0 && (*e & DROPEFFECT_COPY) == 0;
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return !paths.empty();
}
#else
unsigned long clipboard_sequence() noexcept { return 0; }
void clipboard_clear_cut() {}
bool clipboard_set_files(const std::vector<std::string>&, bool) { return false; }
bool clipboard_has_files() { return false; }
bool clipboard_get_files(std::vector<std::string>& paths, bool& cut) { paths.clear(); cut = false; return false; }
#endif

} // namespace prism::core
