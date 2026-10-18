#include "core/shell_ops.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <shellapi.h>

namespace prism::core::shell {

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

} // namespace prism::core::shell
