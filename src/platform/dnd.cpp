#include "platform/dnd.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <objbase.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <atomic>

namespace prism::platform {

using Microsoft::WRL::ComPtr;

namespace {

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
        sources_ = read_paths(data);
        *effect  = sources_.empty() ? DROPEFFECT_NONE : decide(keys, pt, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override
    {
        *effect = sources_.empty() ? DROPEFFECT_NONE : decide(keys, pt, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        sources_.clear();
        if (cb_.hover) { cb_.hover(std::nullopt); }
        if (cb_.redraw) { cb_.redraw(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override
    {
        std::vector<std::string> paths = read_paths(data);
        const std::string dest = resolve(pt);
        const DWORD eff = paths.empty() ? DROPEFFECT_NONE : decide(keys, pt, *effect);
        if (cb_.hover) { cb_.hover(std::nullopt); }
        if (eff != DROPEFFECT_NONE && !dest.empty() && cb_.dropped) { cb_.dropped(std::move(paths), dest, eff == DROPEFFECT_MOVE); }
        // the work is done here (shell dialogs included): the source must not move or delete anything itself
        *effect = DROPEFFECT_NONE;
        sources_.clear();
        if (cb_.redraw) { cb_.redraw(); }
        return S_OK;
    }

private:
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

        // dropping a folder into itself or a file into the folder it already is in does nothing
        bool all_here = true;
        bool same_drive = true;
        for (const std::string& src : sources_) {
            if (core::path::parent(src) != dest) { all_here = false; }
            if (src == dest || (dest.size() > src.size() && dest.compare(0, src.size(), src) == 0 && dest[src.size()] == '\\')) { return DROPEFFECT_NONE; }
            if (src.size() < 2 || dest.size() < 2 || (src[0] | 0x20) != (dest[0] | 0x20) || src[1] != ':') { same_drive = false; }
        }
        if (all_here) { return DROPEFFECT_NONE; }

        DWORD want = same_drive ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        if ((keys & MK_CONTROL) != 0) { want = DROPEFFECT_COPY; }
        if ((keys & MK_SHIFT) != 0) { want = DROPEFFECT_MOVE; }
        if ((allowed & want) != 0) { return want; }
        return (allowed & DROPEFFECT_COPY) != 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }

    std::atomic<ULONG>       refs_{1};
    HWND                     hwnd_;
    DropCallbacks            cb_;
    std::vector<std::string> sources_;
};

// --- the source ----------------------------------------------------------------------------------------------

class DropSource final : public IDropSource {
public:
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
        if ((keys & MK_LBUTTON) == 0) { return DRAGDROP_S_DROP; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

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
    if (!ids.empty()) {
        // (with no parent folder the shell reads the items as absolute ids)
        std::vector<PCUITEMID_CHILD> items;
        for (PIDLIST_ABSOLUTE id : ids) { items.push_back(reinterpret_cast<PCUITEMID_CHILD>(id)); }
        ComPtr<IDataObject> data;
        if (SUCCEEDED(::SHCreateDataObject(nullptr, static_cast<UINT>(items.size()), items.data(), nullptr, IID_PPV_ARGS(&data)))) {
            auto* source = new DropSource;
            DWORD effect = DROPEFFECT_NONE;
            const HRESULT hr = ::DoDragDrop(data.Get(), source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
            source->Release();
            dropped = hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
        }
    }
    for (PIDLIST_ABSOLUTE id : ids) { ::CoTaskMemFree(id); }
    return dropped;
}

} // namespace prism::platform
