#include "platform/dnd.hpp"

#include "core/file_kind.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/rclone.hpp"
#include "core/text.hpp"
#include "core/virtual_files.hpp"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#else
#include <GLFW/glfw3.h>
#include <mutex>
#include <unordered_map>
#endif

#include <atomic>

namespace prism::platform {

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

namespace {

std::vector<std::string> g_own_drag; // the paths of the drag this process is running (empty otherwise)

// --- the target ----------------------------------------------------------------------------------------------

class DropTarget final : public IDropTarget {
public:
    DropTarget(HWND hwnd, DropCallbacks cb) : hwnd_{hwnd}, cb_{std::move(cb)} {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs_;
        if (r == 0) { delete this; }
        return r;
    }

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        data_       = data;
        sources_    = read_paths(data);
        virtual_    = sources_.empty() && core::offers_virtual_files(data);
        right_drag_ = (keys & MK_RBUTTON) != 0;
        cached_dest_.clear();
        *effect  = acceptable() ? decide(keys, pt, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override
    {
        if ((keys & MK_RBUTTON) != 0) { right_drag_ = true; }
        *effect = acceptable() ? decide(keys, pt, *effect) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        leave_program();
        data_.Reset();
        sources_.clear();
        virtual_ = false;
        right_drag_ = false;
        if (cb_.hover) { cb_.hover(std::nullopt); }
        if (cb_.redraw) { cb_.redraw(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        std::vector<std::string> paths = read_paths(data);
        virtual_ = paths.empty() && core::offers_virtual_files(data);
        sources_ = paths;
        const std::string dest = resolve(pt);
        // a program under the pointer was asked during the drag already: asking again here, with the button released, would look
        // like a right-button drop to its handler (it answers with its own "Open with / Cancel" menu)
        const bool on_program = program_ != nullptr && program_path_ == dest && core::is_program_name(dest);
        DWORD eff = on_program ? program_effect_ : (acceptable() ? decide(keys, pt, *effect) : DROPEFFECT_NONE);
        if (cb_.hover) { cb_.hover(std::nullopt); }
        if (eff != DROPEFFECT_NONE && !dest.empty()) {
            if (program_ != nullptr && program_path_ == dest) {
                // the program's own drop handler runs it with the files (what Explorer does): nothing is copied here
                DWORD program_effect = eff;
                // (the keys of the drag: the button that was held, as the handler saw it all along)
                if (FAILED(program_->Drop(data, last_keys_, pt, &program_effect))) { core::log::info("drop on {}: the program's drop handler failed", dest); }
                program_.Reset();
            } else if (virtual_) {
                // the contents come from the other program while we wait (a download, for a remote file): into a staging folder
                // first, then moved to the destination like any other drop
                core::ReceivedFiles got = core::receive_virtual_files(data, staging_root());
                if (got.failed > 0 && cb_.failed) { cb_.failed(got.failed); }
                if (!got.items.empty() && cb_.dropped) { cb_.dropped(std::move(got.items), dest, DropAction::move); }
            } else if (cb_.dropped) {
                const bool is_pin = cb_.link && cb_.link(dest);
                if (right_drag_ && !is_pin) {
                    HMENU menu = ::CreatePopupMenu();
                    if (menu != nullptr) {
                        ::AppendMenuW(menu, MF_STRING, 1, core::text::widen(core::i18n::tr("Copy here")).c_str());
                        ::AppendMenuW(menu, MF_STRING, 2, core::text::widen(core::i18n::tr("Move here")).c_str());
                        ::AppendMenuW(menu, MF_STRING, 3, core::text::widen(core::i18n::tr("Create shortcut here")).c_str());
                        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                        ::AppendMenuW(menu, MF_STRING, 0, core::text::widen(core::i18n::tr("Cancel")).c_str());
                        ::SetMenuDefaultItem(menu, cached_same_vol_ ? 2 : 1, FALSE);
                        ::SetForegroundWindow(hwnd_);
                        const UINT pick = ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr);
                        ::DestroyMenu(menu);
                        if (pick == 1) { eff = DROPEFFECT_COPY; cb_.dropped(std::move(paths), dest, DropAction::copy); }
                        else if (pick == 2) { eff = DROPEFFECT_MOVE; cb_.dropped(std::move(paths), dest, DropAction::move); }
                        else if (pick == 3) { eff = DROPEFFECT_LINK; cb_.dropped(std::move(paths), dest, DropAction::link); }
                        else { eff = DROPEFFECT_NONE; }
                    }
                } else {
                    const DropAction act = eff == DROPEFFECT_LINK ? DropAction::link
                                         : eff == DROPEFFECT_MOVE ? DropAction::move : DropAction::copy;
                    cb_.dropped(std::move(paths), dest, act);
                }
            }
        }
        // report COPY / LINK back to external drag sources so they know the drop succeeded;
        // keep MOVE as NONE because we already performed the move ourselves (avoids double-delete by external sources)
        *effect = (eff == DROPEFFECT_COPY || eff == DROPEFFECT_LINK) ? eff : DROPEFFECT_NONE;
        leave_program();
        data_.Reset();
        sources_.clear();
        virtual_ = false;
        right_drag_ = false;
        if (cb_.redraw) { cb_.redraw(); }
        return S_OK;
    }

private:
    [[nodiscard]] bool acceptable() const noexcept { return !sources_.empty() || virtual_; }

    // a program / script / shortcut under the pointer: the shell's drop target for that file says whether (and how) it takes
    // the files, exactly as in Explorer. null when it takes nothing (a ".bat" that is gone, a shortcut to a document ...)
    [[nodiscard]] DWORD program_effect(const std::string& path, DWORD keys, POINTL pt, DWORD allowed)
    {
        if ((keys & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0) { last_keys_ = keys; }
        const DWORD offered = (allowed & DROPEFFECT_COPY) != 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (offered == DROPEFFECT_NONE) { return DROPEFFECT_NONE; }
        if (program_path_ != path) {
            leave_program();
            program_path_ = path;
            ComPtr<IShellItem> item;
            if (data_ != nullptr && SUCCEEDED(::SHCreateItemFromParsingName(core::text::widen(path).c_str(), nullptr, IID_PPV_ARGS(&item))) &&
                SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&program_)))) {
                DWORD effect = offered;
                if (FAILED(program_->DragEnter(data_.Get(), keys, pt, &effect)) || effect == DROPEFFECT_NONE) { program_.Reset(); }
                program_effect_ = effect & offered;
            } else {
                program_.Reset();
            }
        } else if (program_ != nullptr) {
            DWORD effect = offered;
            if (FAILED(program_->DragOver(keys, pt, &effect))) { effect = DROPEFFECT_NONE; }
            program_effect_ = effect & offered;
        }
        return program_ != nullptr ? program_effect_ : DROPEFFECT_NONE;
    }
    void leave_program()
    {
        if (program_ != nullptr) { (void)program_->DragLeave(); }
        program_.Reset();
        program_path_.clear();
    }

    // where files without a path on disk are collected; emptied folders from earlier drops are removed on the way
    [[nodiscard]] static std::string staging_root()
    {
        wchar_t tmp[MAX_PATH + 1]{};
        const DWORD n = ::GetTempPathW(MAX_PATH, tmp);
        std::wstring root{tmp, n};
        while (!root.empty() && root.back() == L'\\') { root.pop_back(); }
        root += L"\\PrismFS\\drop";
        WIN32_FIND_DATAW fd;
        if (HANDLE h = ::FindFirstFileExW((root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0); h != INVALID_HANDLE_VALUE) {
            do {
                // (RemoveDirectory refuses a folder that still holds files, e.g. a transfer in progress)
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && fd.cFileName[0] != L'.') { ::RemoveDirectoryW((root + L"\\" + fd.cFileName).c_str()); }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        return core::text::narrow(root);
    }

    static std::vector<std::string> read_paths(IDataObject* data)
    {
        std::vector<std::string> out;
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM med{};
        if (data != nullptr && SUCCEEDED(data->GetData(&fmt, &med))) {
            if (auto* drop = static_cast<HDROP>(::GlobalLock(med.hGlobal))) {
                const UINT n = ::DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
                    std::wstring w(len, L'\0');
                    ::DragQueryFileW(drop, i, w.data(), len + 1);
                    out.push_back(core::text::narrow(w));
                }
                ::GlobalUnlock(med.hGlobal);
            }
            ::ReleaseStgMedium(&med);
        }
        // the data object we make for our own drags (SHCreateDataObject) has item ids but no CF_HDROP, so a drop back
        // onto this window would read nothing: we know what we are dragging
        if (out.empty()) { out = g_own_drag; }
        return out;
    }

    [[nodiscard]] std::string resolve(POINTL pt) const
    {
        POINT p{pt.x, pt.y};
        ::ScreenToClient(hwnd_, &p);
        const float s = cb_.scale ? cb_.scale() : 1.0f;
        return cb_.resolve ? cb_.resolve({static_cast<float>(p.x) / s, static_cast<float>(p.y) / s}) : std::string{};
    }

    // where it would land, and what happens to the files: move on the same drive, copy across drives; ctrl / shift force
    [[nodiscard]] DWORD decide(DWORD keys, POINTL pt, DWORD allowed)
    {
        POINT p{pt.x, pt.y};
        ::ScreenToClient(hwnd_, &p);
        const float s = cb_.scale ? cb_.scale() : 1.0f;
        const strata::vec2 logical{static_cast<float>(p.x) / s, static_cast<float>(p.y) / s};
        if (cb_.hover) { cb_.hover(logical); }
        const std::string dest = cb_.resolve ? cb_.resolve(logical) : std::string{};
        if (cb_.redraw) { cb_.redraw(); }
        if (dest.empty()) { return DROPEFFECT_NONE; }
        if (core::is_program_name(dest)) {
            if (virtual_) { return DROPEFFECT_NONE; } // (nothing on disk to hand to a program)
            return program_effect(dest, keys, pt, allowed);
        }
        leave_program();
        if (virtual_) {
            if (cb_.link && cb_.link(dest)) { return DROPEFFECT_NONE; } // (nothing on disk to pin)
            return (allowed & DROPEFFECT_COPY) != 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        if (cb_.link && cb_.link(dest)) { return (allowed & DROPEFFECT_LINK) != 0 ? DROPEFFECT_LINK : ((allowed & DROPEFFECT_COPY) != 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE); }

        // dropping a folder into itself or a file into the folder it already is in does nothing
        if (dest != cached_dest_) {
            cached_dest_ = dest;
            cached_valid_ = true;
            cached_same_vol_ = true;
            bool all_here = true;
            const std::string dest_l = core::text::lower_ascii(core::path::normalize(dest));
            for (const std::string& src : sources_) {
                const std::string src_l = core::text::lower_ascii(core::path::normalize(src));
                if (core::text::lower_ascii(core::path::normalize(core::path::parent(src))) != dest_l) { all_here = false; }
                if (src_l == dest_l || (dest_l.size() > src_l.size() && dest_l.compare(0, src_l.size(), src_l) == 0 && (dest_l[src_l.size()] == '\\' || src_l.back() == '\\'))) {
                    cached_valid_ = false;
                    break;
                }
                if (cached_same_vol_ && !core::path::same_volume(src, dest)) { cached_same_vol_ = false; }
            }
            if (all_here) { cached_valid_ = false; }
        }
        if (!cached_valid_) { return DROPEFFECT_NONE; }

        DWORD want = cached_same_vol_ ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        if ((keys & MK_ALT) != 0 || ((keys & MK_CONTROL) != 0 && (keys & MK_SHIFT) != 0)) { want = DROPEFFECT_LINK; }
        else if ((keys & MK_CONTROL) != 0) { want = DROPEFFECT_COPY; }
        else if ((keys & MK_SHIFT) != 0) { want = DROPEFFECT_MOVE; }
        if ((allowed & want) != 0) { return want; }
        return (allowed & DROPEFFECT_COPY) != 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }

    std::atomic<ULONG>       refs_{1};
    HWND                     hwnd_;
    DropCallbacks            cb_;
    std::vector<std::string> sources_;
    bool                     virtual_{}; // the drag offers files with no path on disk (core/virtual_files)
    ComPtr<IDataObject>      data_;      // the drag being handled (kept for the program's drop target)
    ComPtr<IDropTarget>      program_;   // the shell's drop target of the program under the pointer
    std::string              program_path_;
    DWORD                    program_effect_{};
    DWORD                    last_keys_{};   // the key state of the drag while a button was held
    bool                     right_drag_{};
    std::string              cached_dest_;
    bool                     cached_valid_{};
    bool                     cached_same_vol_{};
};

// --- the source ----------------------------------------------------------------------------------------------

class DropSource final : public IDropSource {
public:
    explicit DropSource(DWORD button = MK_LBUTTON) : button_{button} {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (id == IID_IUnknown || id == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs_;
        if (r == 0) { delete this; }
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        if (escape != FALSE) { return DRAGDROP_S_CANCEL; }
        if ((keys & button_) == 0) { return DRAGDROP_S_DROP; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    std::atomic<ULONG> refs_{1};
    DWORD              button_{MK_LBUTTON};
};

// Minimal IDataObject for in-app drags of virtual/cloud paths (rclone:) that Windows Shell cannot SHParseDisplayName.
class VirtualDragDataObject final : public IDataObject {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (id == IID_IUnknown || id == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs_;
        if (r == 0) { delete this; }
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC*, STGMEDIUM*) override { return DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC*) override { return DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    {
        if (out != nullptr) { out->ptd = nullptr; }
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC** out) override
    {
        if (out != nullptr) { *out = nullptr; }
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override
    {
        if (out != nullptr) { *out = nullptr; }
        return OLE_E_ADVISENOTSUPPORTED;
    }
private:
    std::atomic<ULONG> refs_{1};
};

} // namespace

bool attach_drop_target(void* hwnd, DropCallbacks callbacks)
{
    auto* target = new DropTarget{static_cast<HWND>(hwnd), std::move(callbacks)};
    const HRESULT hr = ::RegisterDragDrop(static_cast<HWND>(hwnd), target);
    target->Release(); // (the registration holds its own reference)
    return SUCCEEDED(hr);
}

void detach_drop_target(void* hwnd)
{
    ::RevokeDragDrop(static_cast<HWND>(hwnd));
}

bool run_drag(const std::vector<std::string>& paths)
{
    if (paths.empty()) { return false; }
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const std::string& p : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(core::text::widen(p).c_str(), nullptr, &id, 0, nullptr)) && id != nullptr) { ids.push_back(id); }
    }
    bool dropped = false;
    if (ids.empty() && std::ranges::any_of(paths, [](const std::string& p) { return core::rclone::is_path(p); })) {
        const DWORD btn = (::GetKeyState(VK_RBUTTON) & 0x8000) != 0 && (::GetKeyState(VK_LBUTTON) & 0x8000) == 0 ? MK_RBUTTON : MK_LBUTTON;
        auto* data = new VirtualDragDataObject{};
        auto* source = new DropSource{btn};
        DWORD effect = DROPEFFECT_NONE;
        g_own_drag = paths;
        const HRESULT hr = ::DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
        g_own_drag.clear();
        source->Release();
        data->Release();
        return hr == DRAGDROP_S_DROP;
    }
    if (!ids.empty()) {
        // check whether all items share one parent folder (fast path: SHCreateDataObject gives Explorer full CF_HDROP + shell metadata);
        // otherwise bind an IShellItemArray across folders so drags from search results / unified Recycle Bin keep every item
        PIDLIST_ABSOLUTE parent = ::ILClone(ids.front());
        ::ILRemoveLastID(parent);
        std::vector<PCUITEMID_CHILD> items;
        bool single_parent = true;
        for (PIDLIST_ABSOLUTE id : ids) {
            PIDLIST_ABSOLUTE p = ::ILClone(id);
            ::ILRemoveLastID(p);
            if (::ILIsEqual(p, parent) != FALSE) { items.push_back(::ILFindLastID(id)); }
            else { single_parent = false; }
            ::ILFree(p);
        }
        ComPtr<IDataObject> data;
        HRESULT created = E_FAIL;
        if (single_parent) {
            created = ::SHCreateDataObject(parent, static_cast<UINT>(items.size()), items.data(), nullptr, IID_PPV_ARGS(&data));
        } else {
            ComPtr<IShellItemArray> arr;
            if (SUCCEEDED(::SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE*>(ids.data()), &arr)) && arr != nullptr) {
                created = arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
            }
            if (FAILED(created)) {
                created = ::SHCreateDataObject(parent, static_cast<UINT>(items.size()), items.data(), nullptr, IID_PPV_ARGS(&data));
            }
        }
        ::ILFree(parent);
        if (SUCCEEDED(created)) {
            const DWORD btn = (::GetKeyState(VK_RBUTTON) & 0x8000) != 0 && (::GetKeyState(VK_LBUTTON) & 0x8000) == 0 ? MK_RBUTTON : MK_LBUTTON;
            auto* source = new DropSource{btn};
            DWORD effect = DROPEFFECT_NONE;
            g_own_drag = paths;
            const HRESULT hr = ::DoDragDrop(data.Get(), source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
            g_own_drag.clear();
            source->Release();
            dropped = hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
        }
    }
    for (PIDLIST_ABSOLUTE id : ids) { ::CoTaskMemFree(id); }
    return dropped;
}
#else
namespace {
std::mutex g_drop_mu;
std::unordered_map<GLFWwindow*, DropCallbacks> g_drops;

void on_glfw_drop(GLFWwindow* win, int count, const char** paths)
{
    DropCallbacks cb;
    {
        std::lock_guard g{g_drop_mu};
        auto it = g_drops.find(win);
        if (it == g_drops.end()) { return; }
        cb = it->second;
    }
    if (!cb.dropped || count <= 0) { return; }
    std::vector<std::string> list;
    list.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (paths[i] != nullptr) { list.emplace_back(paths[i]); }
    }
    double mx = 0.0, my = 0.0;
    ::glfwGetCursorPos(win, &mx, &my);
    const std::string dest = cb.resolve ? cb.resolve({static_cast<float>(mx), static_cast<float>(my)}) : std::string{};
    if (!dest.empty() && !list.empty()) { cb.dropped(std::move(list), dest, DropAction::copy); }
    if (cb.redraw) { cb.redraw(); }
}
} // namespace

bool attach_drop_target(void* hwnd, DropCallbacks callbacks)
{
    auto* win = static_cast<GLFWwindow*>(hwnd);
    if (win == nullptr) { return false; }
    {
        std::lock_guard g{g_drop_mu};
        g_drops[win] = std::move(callbacks);
    }
    ::glfwSetDropCallback(win, on_glfw_drop);
    return true;
}

void detach_drop_target(void* hwnd)
{
    auto* win = static_cast<GLFWwindow*>(hwnd);
    if (win == nullptr) { return; }
    ::glfwSetDropCallback(win, nullptr);
    std::lock_guard g{g_drop_mu};
    g_drops.erase(win);
}

bool run_drag(const std::vector<std::string>&) { return false; }
#endif

} // namespace prism::platform
