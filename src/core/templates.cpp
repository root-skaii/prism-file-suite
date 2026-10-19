#include "core/templates.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>

#include <algorithm>

namespace prism::core {

const std::vector<Template>& builtin_templates()
{
    // none shipped for now: the menu offers what the user puts in the Templates folder
    static const std::vector<Template> list;
    return list;
}

std::vector<Template> user_templates(const std::string& folder)
{
    std::vector<Template> out;
    WIN32_FIND_DATAW      fd{};
    const std::wstring    pattern = text::widen(path::join(folder, "*"));
    const HANDLE h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) { return out; }
    do {
        if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) { continue; }
        const std::string name = text::narrow(fd.cFileName);
        const std::size_t dot  = name.rfind('.');
        Template          t;
        t.stem   = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
        t.ext    = dot == std::string::npos || dot == 0 ? std::string{} : name.substr(dot);
        t.label  = t.stem;
        t.source = path::join(folder, name);
        out.push_back(std::move(t));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    std::ranges::sort(out, [](const Template& a, const Template& b) { return text::compare_natural(a.source, b.source) < 0; });
    return out;
}

std::string create_from_template(const std::string& dir, const Template& t)
{
    for (int i = 1; i < 1000; ++i) {
        const std::string name   = (i == 1 ? t.stem : t.stem + " (" + std::to_string(i) + ")") + t.ext;
        const std::wstring target = text::widen(path::join(dir, name));
        if (!t.source.empty()) {
            if (::CopyFileW(text::widen(t.source).c_str(), target.c_str(), TRUE)) {
                ::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL); // (a read-only template gives a normal file)
                return name;
            }
        } else {
            const HANDLE h = ::CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                const bool ok = t.content.empty() || ::WriteFile(h, t.content.data(), static_cast<DWORD>(t.content.size()), &wrote, nullptr);
                ::CloseHandle(h);
                return ok ? name : std::string{};
            }
        }
        const DWORD err = ::GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) { return {}; }
    }
    return {};
}

} // namespace prism::core
