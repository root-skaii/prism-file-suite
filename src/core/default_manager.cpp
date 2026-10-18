#include "core/default_manager.hpp"

#include <windows.h>

#include <string>

namespace prism::core {

namespace {

constexpr const wchar_t* k_classes[] = {L"Directory", L"Drive"};

[[nodiscard]] std::wstring shell_key(const wchar_t* cls) { return std::wstring{L"Software\\Classes\\"} + cls + L"\\shell"; }

[[nodiscard]] std::wstring exe_path()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring{buf, n} : std::wstring{};
}

// the command line this exe would be registered with
[[nodiscard]] std::wstring command_line() { return L"\"" + exe_path() + L"\" \"%1\""; }

[[nodiscard]] std::wstring read_default(const std::wstring& key)
{
    HKEY k{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) { return {}; }
    wchar_t buf[1024]{};
    DWORD   size = sizeof buf - sizeof(wchar_t);
    DWORD   type = 0;
    const bool ok = ::RegQueryValueExW(k, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS && type == REG_SZ;
    ::RegCloseKey(k);
    return ok ? std::wstring{buf} : std::wstring{};
}

[[nodiscard]] bool write_value(HKEY k, const wchar_t* name, const std::wstring& v)
{
    return ::RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

[[nodiscard]] bool is_ours(const std::wstring& command)
{
    // (any copy of the exe, the test build too: the name is what says it is ours)
    std::wstring lower = command;
    for (wchar_t& c : lower) { c = static_cast<wchar_t>(::towlower(c)); }
    return lower.find(L"prismfs") != std::wstring::npos;
}

bool register_class(const wchar_t* cls, const std::wstring& command)
{
    HKEY shell{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, shell_key(cls).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &shell, nullptr) != ERROR_SUCCESS) { return false; }
    // (the system's default verb here is "none": without this the double click does not look for ours)
    bool ok = write_value(shell, nullptr, L"open");
    ::RegCloseKey(shell);

    HKEY cmd{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, (shell_key(cls) + L"\\open\\command").c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &cmd, nullptr) != ERROR_SUCCESS) { return false; }
    ok = write_value(cmd, nullptr, command) && ok;
    ok = write_value(cmd, L"DelegateExecute", L"") && ok; // (empty: Explorer's own handler is not called instead)
    ::RegCloseKey(cmd);
    return ok;
}

void unregister_class(const wchar_t* cls)
{
    const std::wstring key = shell_key(cls);
    if (!is_ours(read_default(key + L"\\open\\command"))) { return; } // (somebody else's verb: not ours to remove)
    (void)::RegDeleteTreeW(HKEY_CURRENT_USER, (key + L"\\open").c_str());
    HKEY shell{};
    if (read_default(key) == L"open" && ::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &shell) == ERROR_SUCCESS) {
        (void)::RegDeleteValueW(shell, nullptr);
        ::RegCloseKey(shell);
    }
    (void)::RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()); // (only if nothing else is in it)
}

} // namespace

bool default_manager_active()
{
    return is_ours(read_default(shell_key(k_classes[0]) + L"\\open\\command"));
}

bool set_default_manager(bool on)
{
    if (!on) {
        for (const wchar_t* cls : k_classes) { unregister_class(cls); }
        return true;
    }
    const std::wstring command = command_line();
    bool ok = exe_path().size() > 0;
    for (const wchar_t* cls : k_classes) { ok = register_class(cls, command) && ok; }
    return ok;
}

void refresh_default_manager()
{
    if (default_manager_active() && read_default(shell_key(k_classes[0]) + L"\\open\\command") != command_line()) { (void)set_default_manager(true); }
}

} // namespace prism::core
