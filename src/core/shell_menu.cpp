#include "core/shell_menu.hpp"

#include "core/bitmap.hpp"
#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

namespace prism::core {

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT k_first = 1;
constexpr UINT k_last  = 0x7fff;

struct Built {
    ComPtr<IContextMenu> menu;
    HMENU                hmenu{};
    std::wstring         dir;
};

void release(Built& b)
{
    if (b.hmenu != nullptr) { ::DestroyMenu(b.hmenu); }
    b = {};
}

[[nodiscard]] ComPtr<IContextMenu> bind_menu(const std::vector<std::string>& paths, bool background)
{
    ComPtr<IContextMenu> cm;
    if (paths.empty()) { return cm; }
    if (background) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(::SHParseDisplayName(text::widen(paths.front()).c_str(), nullptr, &pidl, 0, nullptr)) || pidl == nullptr) { return cm; }
        ComPtr<IShellFolder> folder;
        const HRESULT bound = ::SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder));
        ::CoTaskMemFree(pidl);
        if (SUCCEEDED(bound) && folder != nullptr) { (void)folder->CreateViewObject(nullptr, IID_PPV_ARGS(&cm)); }
        return cm;
    }
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const std::string& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(text::widen(p).c_str(), nullptr, &pidl, 0, nullptr)) && pidl != nullptr) { ids.push_back(pidl); }
    }
    if (ids.size() == paths.size()) {
        ComPtr<IShellItemArray> arr;
        if (SUCCEEDED(::SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE*>(ids.data()), &arr)) && arr != nullptr) {
            (void)arr->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm));
        }
    }
    for (PIDLIST_ABSOLUTE id : ids) { ::CoTaskMemFree(id); }
    return cm;
}

// "&Open with &Code\tCtrl+O" -> "Open with Code"
[[nodiscard]] std::string clean_label(const wchar_t* raw)
{
    std::wstring s{raw};
    if (const std::size_t tab = s.find(L'\t'); tab != std::wstring::npos) { s.resize(tab); }
    std::wstring out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'&') {
            if (i + 1 < s.size() && s[i + 1] == L'&') { out.push_back(L'&'); ++i; }
            continue;
        }
        out.push_back(s[i]);
    }
    return text::narrow(out);
}

// what the file list already offers itself (or does better): showing these twice would only confuse
[[nodiscard]] bool ours(const std::wstring& verb)
{
    static constexpr const wchar_t* skip[] = {L"open", L"cut", L"copy", L"paste", L"delete", L"rename", L"properties", L"copyaspath",
                                              L"opennewwindow", L"opennewprocess", L"opennewtab", L"pintohome", L"unpinfromhome", L"find"};
    return std::ranges::any_of(skip, [&](const wchar_t* s) { return _wcsicmp(verb.c_str(), s) == 0; });
}

void tidy(std::vector<ShellMenuItem>& v)
{
    while (!v.empty() && v.front().separator) { v.erase(v.begin()); }
    while (!v.empty() && v.back().separator) { v.pop_back(); }
    for (std::size_t i = 1; i < v.size();) {
        if (v[i].separator && v[i - 1].separator) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(i)); } else { ++i; }
    }
}

void collect(HMENU menu, IContextMenu* cm, IContextMenu2* cm2, std::vector<ShellMenuItem>& out, int depth)
{
    const int n = ::GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i) {
        wchar_t label[256]{};
        MENUITEMINFOW mi{};
        mi.cbSize     = sizeof mi;
        mi.fMask      = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING | MIIM_BITMAP;
        mi.dwTypeData = label;
        mi.cch        = 255;
        if (!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mi)) { continue; }

        ShellMenuItem item;
        if ((mi.fType & MFT_SEPARATOR) != 0) {
            item.separator = true;
            out.push_back(std::move(item));
            continue;
        }
        if ((mi.fType & (MFT_OWNERDRAW | MFT_BITMAP)) != 0) { continue; } // (nothing we could draw)
        item.label = clean_label(label);
        if (item.label.empty()) { continue; }
        item.enabled = (mi.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
        // (values up to HBMMENU_POPUP_MAXIMIZE are the system's own marks, not bitmaps)
        if (reinterpret_cast<std::uintptr_t>(mi.hbmpItem) > 16) {
            auto icon = std::make_shared<ShellIcon>();
            if (bitmap_to_bgra(mi.hbmpItem, icon->w, icon->h, icon->bgra)) {
                // an icon is a few hundred bytes: hashing the pixels is the cheapest way to tell two apart
                std::uint64_t hash = 1469598103934665603ull;
                for (const unsigned char c : icon->bgra) { hash = (hash ^ c) * 1099511628211ull; }
                icon->key  = "menu:" + std::to_string(icon->w) + "x" + std::to_string(icon->h) + ":" + std::to_string(hash);
                item.icon = std::move(icon);
            }
        }

        if (mi.hSubMenu != nullptr) {
            if (depth >= 4) { continue; }
            // many handlers (New, Send to, Open with) only fill their submenu when it is about to open
            if (cm2 != nullptr) { (void)cm2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(mi.hSubMenu), MAKELPARAM(i, 0)); }
            collect(mi.hSubMenu, cm, cm2, item.children, depth + 1);
            tidy(item.children);
            if (item.children.empty()) { continue; }
        } else {
            if (mi.wID < k_first || mi.wID > k_last) { continue; }
            wchar_t verb[128]{};
            if (SUCCEEDED(cm->GetCommandString(mi.wID - k_first, GCS_VERBW, nullptr, reinterpret_cast<char*>(verb), 127)) && ours(verb)) { continue; }
            item.id = mi.wID;
        }
        out.push_back(std::move(item));
    }
}

[[nodiscard]] std::string key_of(const std::vector<std::string>& paths, bool background)
{
    std::string key = background ? "bg\n" : "it\n";
    for (const std::string& p : paths) {
        key += p;
        key.push_back('\n');
    }
    return key;
}

} // namespace

ShellMenu::ShellMenu(std::function<void()> wake) : wake_{std::move(wake)}
{
    signal_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread{[this] { run(); }};
}

ShellMenu::~ShellMenu()
{
    stop_ = true;
    ::SetEvent(static_cast<HANDLE>(signal_));
    if (thread_.joinable()) { thread_.join(); }
    ::CloseHandle(static_cast<HANDLE>(signal_));
}

void ShellMenu::query(const std::vector<std::string>& paths, bool background)
{
    std::string key = key_of(paths, background);
    {
        std::lock_guard lock{mutex_};
        if (key == asked_key_) { return; }
        asked_key_   = key;
        pending_     = Request{std::move(key), paths, background};
        has_pending_ = true;
    }
    ::SetEvent(static_cast<HANDLE>(signal_));
}

std::vector<ShellMenuItem> ShellMenu::items() const
{
    std::lock_guard lock{mutex_};
    return done_key_ == asked_key_ ? items_ : std::vector<ShellMenuItem>{};
}

bool ShellMenu::ready() const
{
    std::lock_guard lock{mutex_};
    return done_key_ == asked_key_;
}

void ShellMenu::invoke(unsigned id, void* hwnd)
{
    {
        std::lock_guard lock{mutex_};
        invoke_id_   = id;
        invoke_hwnd_ = hwnd;
        has_invoke_  = true;
    }
    ::SetEvent(static_cast<HANDLE>(signal_));
}

void ShellMenu::run()
{
    const bool com = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    Built cur;
    std::wstring dir;

    while (!stop_) {
        // sleep until asked, but keep answering the messages COM sends an apartment
        const HANDLE h = static_cast<HANDLE>(signal_);
        const DWORD  r = ::MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT);
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (r != WAIT_OBJECT_0) { continue; } // (only messages arrived)

        Request req;
        bool    do_query  = false;
        bool    do_invoke = false;
        unsigned id{};
        void*    hwnd{};
        {
            std::lock_guard lock{mutex_};
            if (has_invoke_) {
                do_invoke   = true;
                id          = invoke_id_;
                hwnd        = invoke_hwnd_;
                has_invoke_ = false;
            }
            if (has_pending_) {
                do_query     = true;
                req          = std::move(pending_);
                has_pending_ = false;
            }
        }

        if (do_invoke && cur.menu != nullptr) {
            const std::string narrow_dir = text::narrow(cur.dir);
            CMINVOKECOMMANDINFOEX ci{};
            ci.cbSize       = sizeof ci;
            ci.fMask        = CMIC_MASK_UNICODE;
            ci.hwnd         = static_cast<HWND>(hwnd);
            ci.lpVerb       = MAKEINTRESOURCEA(id - k_first);
            ci.lpVerbW      = MAKEINTRESOURCEW(id - k_first);
            ci.lpDirectoryW = cur.dir.c_str();
            ci.lpDirectory  = narrow_dir.c_str();
            ci.nShow        = SW_SHOWNORMAL;
            (void)cur.menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ci));
        }

        if (do_query) {
            release(cur);
            std::vector<ShellMenuItem> items;
            cur.menu = bind_menu(req.paths, req.background);
            if (cur.menu != nullptr) {
                cur.dir   = text::widen(req.background ? req.paths.front() : path::parent(req.paths.front()));
                cur.hmenu = ::CreatePopupMenu();
                if (SUCCEEDED(cur.menu->QueryContextMenu(cur.hmenu, 0, k_first, k_last, CMF_NORMAL))) {
                    ComPtr<IContextMenu2> cm2;
                    (void)cur.menu.As(&cm2);
                    collect(cur.hmenu, cur.menu.Get(), cm2.Get(), items, 0);
                    tidy(items);
                }
            }
            {
                std::lock_guard lock{mutex_};
                items_    = std::move(items);
                done_key_ = std::move(req.key);
            }
            if (wake_) { wake_(); }
        }
    }
    release(cur);
    if (com) { ::CoUninitialize(); }
}

} // namespace prism::core
