#include "core/file_ops.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstring>

namespace prism::core {

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

FileOps::~FileOps()
{
    for (std::thread& t : threads_) {
        if (t.joinable()) { t.join(); }
    }
}

bool FileOps::busy() const
{
    std::lock_guard g{lock_};
    return running_ > 0;
}

bool FileOps::poll(OpResult& out)
{
    std::lock_guard g{lock_};
    if (results_.empty()) { return false; }
    out = std::move(results_.front());
    results_.erase(results_.begin());
    return true;
}

void FileOps::start(OpKind kind, std::vector<std::string> sources, std::string dest_dir, bool silent)
{
    if (sources.empty()) { return; }
    {
        std::lock_guard g{lock_};
        ++running_;
    }
    threads_.emplace_back([this, kind, silent, s = std::move(sources), d = std::move(dest_dir)]() mutable { run(kind, std::move(s), std::move(d), silent); });
}

void FileOps::run(OpKind kind, std::vector<std::string> sources, std::string dest, bool silent)
{
    OpResult res;
    res.kind    = kind;
    res.silent  = silent;
    res.count   = sources.size();
    res.sources = sources;
    res.dest    = dest.empty() ? path::parent(sources.front()) : dest;
    {
        ComScope com;
        ComPtr<IFileOperation> op;
        if (com.ok && SUCCEEDED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
            DWORD flags = FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD;
            if (kind != OpKind::erase) { flags |= FOF_ALLOWUNDO; }
            // deleting is confirmed by our own dialog (or by the user pressing Del), not twice
            if (kind == OpKind::erase || kind == OpKind::recycle) { flags |= FOF_NOCONFIRMATION; }
            // a copy into the folder it came from keeps both: "name - Copy"
            if (kind == OpKind::copy && !sources.empty() && path::parent(sources.front()) == dest) { flags |= FOF_RENAMEONCOLLISION; }
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
    }
    {
        std::lock_guard g{lock_};
        results_.push_back(std::move(res));
        --running_;
    }
    if (wake_) { wake_(); }
}

bool FileOps::rename(const std::string& path, const std::string& new_name)
{
    if (new_name.empty() || new_name.find_first_of("\\/:*?\"<>|") != std::string::npos) { return false; }
    const std::string target = path::join(path::parent(path), new_name);
    // (case-only renames are allowed: MoveFileEx treats them as a rename of the same file)
    return ::MoveFileExW(text::widen(path).c_str(), text::widen(target).c_str(), 0) != FALSE;
}

std::string FileOps::create_folder(const std::string& dir)
{
    for (int i = 1; i < 1000; ++i) {
        const std::string name = i == 1 ? "New folder" : "New folder (" + std::to_string(i) + ")";
        if (::CreateDirectoryW(text::widen(path::join(dir, name)).c_str(), nullptr)) { return name; }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) { return {}; }
    }
    return {};
}

bool FileOps::remove_empty_folder(const std::string& p)
{
    return ::RemoveDirectoryW(text::widen(p).c_str()) != FALSE;
}

bool FileOps::restore_from_recycle(const std::vector<std::string>& originals)
{
    if (originals.empty()) { return false; }
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
}

// --- clipboard -----------------------------------------------------------------------------------------------

namespace {

[[nodiscard]] UINT drop_effect_format()
{
    static const UINT f = ::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return f;
}

} // namespace

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
    if (::OpenClipboard(nullptr)) {
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
    if (!::OpenClipboard(nullptr)) { return false; }
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

} // namespace prism::core
