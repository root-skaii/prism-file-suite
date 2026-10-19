#include "core/shell_ops.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace prism::core::shell {

namespace {

// the menu's owner window forwards the messages shell extensions need to fill and draw submenus (Send to, Open with)
LRESULT CALLBACK native_menu_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* cm = reinterpret_cast<IContextMenu2*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR:
        if (cm != nullptr) {
            if (Microsoft::WRL::ComPtr<IContextMenu3> cm3; SUCCEEDED(cm->QueryInterface(IID_PPV_ARGS(&cm3)))) {
                LRESULT res = 0;
                if (SUCCEEDED(cm3->HandleMenuMsg2(msg, wp, lp, &res))) { return msg == WM_MENUCHAR ? res : 0; }
            } else if (SUCCEEDED(cm->HandleMenuMsg(msg, wp, lp))) {
                return 0;
            }
        }
        break;
    default: break;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

bool show_native_menu(const std::vector<std::string>& paths, void* owner_hwnd)
{
    using Microsoft::WRL::ComPtr;
    if (paths.empty()) { return false; }
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const std::string& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(text::widen(p).c_str(), nullptr, &pidl, 0, nullptr)) && pidl != nullptr) { ids.push_back(pidl); }
    }
    ComPtr<IContextMenu> cm;
    if (ids.size() == paths.size()) {
        ComPtr<IShellItemArray> arr;
        if (SUCCEEDED(::SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE*>(ids.data()), &arr)) && arr != nullptr) {
            (void)arr->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm));
        }
    }
    for (PIDLIST_ABSOLUTE id : ids) { ::CoTaskMemFree(id); }
    if (cm == nullptr) { return false; }

    static const wchar_t* k_class = L"PrismFSNativeMenuOwner";
    static const bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = native_menu_proc;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = k_class;
        return ::RegisterClassW(&wc) != 0;
    }();
    if (!registered) { return false; }

    HWND owner = ::CreateWindowExW(0, k_class, L"", WS_POPUP, 0, 0, 0, 0, static_cast<HWND>(owner_hwnd), nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (owner == nullptr) { return false; }
    ComPtr<IContextMenu2> cm2;
    (void)cm.As(&cm2);
    ::SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cm2.Get()));

    bool   ok   = false;
    HMENU  menu = ::CreatePopupMenu();
    constexpr UINT first = 1, last = 0x7fff;
    if (menu != nullptr && SUCCEEDED(cm->QueryContextMenu(menu, 0, first, last, CMF_NORMAL | CMF_EXTENDEDVERBS))) {
        POINT pt{};
        ::GetCursorPos(&pt);
        ::SetForegroundWindow(owner);
        const UINT cmd = static_cast<UINT>(::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr));
        if (cmd >= first && cmd <= last) {
            CMINVOKECOMMANDINFOEX ci{};
            ci.cbSize = sizeof ci;
            ci.fMask  = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            ci.hwnd   = static_cast<HWND>(owner_hwnd);
            ci.lpVerb = MAKEINTRESOURCEA(cmd - first);
            ci.lpVerbW = MAKEINTRESOURCEW(cmd - first);
            ci.nShow  = SW_SHOWNORMAL;
            ci.ptInvoke = pt;
            ok = SUCCEEDED(cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ci)));
        } else {
            ok = true; // (dismissed)
        }
    }
    if (menu != nullptr) { ::DestroyMenu(menu); }
    ::SetWindowLongPtrW(owner, GWLP_USERDATA, 0);
    ::DestroyWindow(owner);
    return ok;
}

bool open(const std::string& path)
{
    const std::wstring w   = text::widen(path);
    const std::wstring dir = text::widen(path::parent(path));
    const auto r = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL));
    return r > 32;
}

bool show_properties(const std::string& path)
{
    const std::wstring w = text::widen(path);
    // the shell's own entry point for "Properties" (it runs the sheet on its own thread and copes with cloud folders such as
    // OneDrive, where the plain "properties" verb below fails with "Unspecified error")
    if (::SHObjectProperties(nullptr, SHOP_FILEPATH, w.c_str(), nullptr)) { return true; }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask  = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC;
    info.lpVerb = L"properties";
    info.lpFile = w.c_str();
    info.nShow  = SW_SHOW;
    return ::ShellExecuteExW(&info) != FALSE;
}

bool reveal_in_explorer(const std::string& path)
{
    const std::wstring args = L"/select,\"" + text::widen(path) + L"\"";
    const auto r = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    return r > 32;
}

bool run_command(const std::string& line, const std::string& folder)
{
    // split "program rest of the line"; a quoted program name keeps its spaces
    std::size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) { return false; }
    std::string prog;
    std::size_t e;
    if (line[b] == '"') {
        const std::size_t q = line.find('"', b + 1);
        if (q == std::string::npos) { return false; }
        prog = line.substr(b + 1, q - b - 1);
        e    = q + 1;
    } else {
        e    = line.find_first_of(" \t", b);
        prog = line.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (e == std::string::npos) { e = line.size(); }
    }
    const std::size_t a = line.find_first_not_of(" \t", e);
    std::wstring args = a == std::string::npos ? std::wstring() : text::widen(line.substr(a));

    // a last word "admin" ("cmd admin", "regedit sudo") runs it elevated: Windows asks for permission first
    bool elevate = false;
    {
        const std::size_t cut = args.find_last_of(L" \t");
        std::wstring last = cut == std::wstring::npos ? args : args.substr(cut + 1);
        for (auto& c : last) { c = static_cast<wchar_t>(::towlower(c)); }
        if (last == L"admin" || last == L"sudo" || last == L"runas" || last == L"elevated") {
            elevate = true;
            args    = cut == std::wstring::npos ? std::wstring() : args.substr(0, cut);
            while (!args.empty() && (args.back() == L' ' || args.back() == L'\t')) { args.pop_back(); }
        }
    }
    const wchar_t* verb = elevate ? L"runas" : L"open";

    std::wstring name = text::widen(prog);
    for (auto& c : name) { c = static_cast<wchar_t>(::towlower(c)); }
    if (name.size() > 4 && name.compare(name.size() - 4, 4, L".exe") == 0) { name.resize(name.size() - 4); }

    const std::wstring dir = text::widen(folder);
    const wchar_t*     cwd = dir.empty() ? nullptr : dir.c_str();

    const bool console = name == L"cmd" || name == L"powershell" || name == L"pwsh" || name == L"wsl" || name == L"bash";
    if (console) {
        // a shell that is not installed would make Windows Terminal show its own error: look first. PowerShell 7 (pwsh) is not on
        // every machine, Windows PowerShell stands in for it
        const auto installed = [](const std::wstring& n) { return ::SearchPathW(nullptr, (n + L".exe").c_str(), nullptr, 0, nullptr, nullptr) > 0; };
        if (!installed(name)) {
            if (name == L"pwsh" && installed(L"powershell")) { name = L"powershell"; }
            else { return false; }
        }
        // Windows Terminal hosts the shell when it is installed, a plain console window otherwise
        // (wt.exe is a Store alias: given a working directory it starts nothing and still reports success, so the folder goes
        // in -d only. a drive root ends in a backslash, which would escape the closing quote: it is doubled)
        const std::wstring wt_dir  = !dir.empty() && dir.back() == L'\\' ? dir + L"\\" : dir;
        const std::wstring wt_args = L"-d \"" + wt_dir + L"\" " + name + (args.empty() ? L"" : L" " + args);
        if (!dir.empty() && !elevate) {
            // ShellExecute reports success for the wt.exe alias and starts nothing: CreateProcess on the alias itself works
            wchar_t local[MAX_PATH]{};
            if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
                const std::wstring exe = std::wstring{local} + L"\\Microsoft\\WindowsApps\\wt.exe";
                std::wstring cmdline = L"\"" + exe + L"\" " + wt_args;
                STARTUPINFOW si{};
                si.cb = sizeof si;
                PROCESS_INFORMATION pi{};
                if (::CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                    ::CloseHandle(pi.hThread);
                    ::CloseHandle(pi.hProcess);
                    return true;
                }
            }
        } else if (!dir.empty() && reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, verb, L"wt.exe", wt_args.c_str(), nullptr, SW_SHOWNORMAL)) > 32) { return true; }
        // (an elevated wt.exe is not always allowed through its alias: the plain console is the fallback, still elevated)
        return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, verb, (name + L".exe").c_str(), args.empty() ? nullptr : args.c_str(), cwd, SW_SHOWNORMAL)) > 32;
    }
    if (reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, verb, text::widen(prog).c_str(), args.empty() ? nullptr : args.c_str(), cwd, SW_SHOWNORMAL)) > 32) { return true; }
    if (elevate) { return false; } // (the cmd /c fallback below cannot elevate)
    // scripts on PATH ("code" is code.cmd): let cmd resolve it, without a console window
    const std::wstring via = L"/c start \"\" /b " + text::widen(line);
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", L"cmd.exe", via.c_str(), cwd, SW_HIDE)) > 32;
}

bool set_clipboard_text(std::string_view utf8)
{
    const std::wstring w = text::widen(utf8);
    if (!::OpenClipboard(nullptr)) { return false; }
    ::EmptyClipboard();
    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem != nullptr) {
        if (void* dst = ::GlobalLock(mem)) {
            std::memcpy(dst, w.c_str(), bytes);
            ::GlobalUnlock(mem);
            ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) { ::GlobalFree(mem); } // (on success the clipboard owns it)
    }
    ::CloseClipboard();
    return ok;
}

bool clipboard_text_contains(std::wstring_view needle)
{
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) || !::OpenClipboard(nullptr)) { return false; }
    bool found = false;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* w = static_cast<const wchar_t*>(::GlobalLock(h))) {
            found = std::wstring_view{w, ::wcsnlen(w, 65536)}.find(needle) != std::wstring_view::npos;
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return found;
}

std::string get_clipboard_text()
{
    std::string out;
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) || !::OpenClipboard(nullptr)) { return out; }
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* w = static_cast<const wchar_t*>(::GlobalLock(h))) {
            out = text::narrow(w);
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return out;
}

} // namespace prism::core::shell
