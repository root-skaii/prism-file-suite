#include "core/crash.hpp"

#include "core/log.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace prism::core::crash {

namespace {

constexpr DWORD k_abort_code = 0xE0505246; // raised by abort() / purecall / invalid parameter so they reach the one handler
constexpr int   k_max_frames = 40;
constexpr int   k_keep       = 8;            // reports kept in the folder

std::atomic<bool> g_reporting{false};
wchar_t           g_dir[MAX_PATH]{};

struct Out {
    HANDLE file{INVALID_HANDLE_VALUE};
    void line(const char* fmt, ...) const
    {
        char    buf[1024];
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(buf, sizeof buf - 1, fmt, ap);
        va_end(ap);
        if (n < 0) { return; }
        if (n > static_cast<int>(sizeof buf) - 2) { n = static_cast<int>(sizeof buf) - 2; }
        buf[n++] = '\n';
        DWORD wrote = 0;
        if (file != INVALID_HANDLE_VALUE) { ::WriteFile(file, buf, static_cast<DWORD>(n), &wrote, nullptr); }
    }
};

// "prismfs.exe+0x1a2b3c" for an address (the module it lies in, and the offset: with the pdb that is the source line)
void describe(ULONG_PTR addr, char* out, std::size_t cap)
{
    HMODULE mod = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &mod) && mod != nullptr) {
        wchar_t path[MAX_PATH]{};
        ::GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* leaf = path;
        for (const wchar_t* p = path; *p != L'\0'; ++p) { if (*p == L'\\') { leaf = p + 1; } }
        char name[128]{};
        ::WideCharToMultiByte(CP_UTF8, 0, leaf, -1, name, static_cast<int>(sizeof name) - 1, nullptr, nullptr);
        std::snprintf(out, cap, "%s+0x%llx", name, static_cast<unsigned long long>(addr - reinterpret_cast<ULONG_PTR>(mod)));
    } else {
        std::snprintf(out, cap, "0x%llx", static_cast<unsigned long long>(addr));
    }
}

void write_stack(const Out& o, const CONTEXT& start)
{
    CONTEXT ctx = start;
    for (int i = 0; i < k_max_frames && ctx.Rip != 0; ++i) {
        char where[300];
        describe(static_cast<ULONG_PTR>(ctx.Rip), where, sizeof where);
        o.line("  #%d %s", i, where);
        DWORD64 base = 0;
        if (const RUNTIME_FUNCTION* fn = ::RtlLookupFunctionEntry(ctx.Rip, &base, nullptr)) {
            void*   handler_data = nullptr;
            DWORD64 frame        = 0;
            ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, const_cast<RUNTIME_FUNCTION*>(fn), &ctx, &handler_data, &frame, nullptr);
        } else { // leaf function without unwind data: the return address is on top of the stack
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        }
    }
}

void ensure_dir(wchar_t* dir, std::size_t cap)
{
    if (g_dir[0] != L'\0') {
        wcsncpy_s(dir, cap, g_dir, _TRUNCATE);
    } else {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        _snwprintf_s(dir, cap, _TRUNCATE, L"%sPrismFS-crashes", tmp);
    }
    ::CreateDirectoryW(dir, nullptr);
}

void prune(const wchar_t* dir)
{
    // newest k_keep .log / .dmp pairs stay (names sort by time)
    wchar_t        pattern[MAX_PATH + 16];
    _snwprintf_s(pattern, _TRUNCATE, L"%s\\crash-*.log", dir);
    std::wstring   names[64];
    int            count = 0;
    WIN32_FIND_DATAW fd{};
    if (HANDLE h = ::FindFirstFileW(pattern, &fd); h != INVALID_HANDLE_VALUE) {
        do { if (count < 64) { names[count++] = fd.cFileName; } } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    for (int i = 0; i < count; ++i) { // (selection sort of the oldest out: a handful of names)
        int rank = 0;
        for (int j = 0; j < count; ++j) { if (names[j] > names[i]) { ++rank; } }
        if (rank >= k_keep) {
            std::wstring base = names[i].substr(0, names[i].size() - 4);
            ::DeleteFileW((std::wstring{dir} + L"\\" + base + L".log").c_str());
            ::DeleteFileW((std::wstring{dir} + L"\\" + base + L".dmp").c_str());
        }
    }
}

const char* code_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
    case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
    case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
    case EXCEPTION_BREAKPOINT:            return "breakpoint";
    case 0xC0000409:                      return "fast fail (stack buffer / abort)";
    case k_abort_code:                    return "abort / invalid call";
    default:                              return "exception";
    }
}

void write_dump(const wchar_t* path, EXCEPTION_POINTERS* ep)
{
    HMODULE dbg = ::LoadLibraryW(L"dbghelp.dll");
    if (dbg == nullptr) { return; }
    using WriteFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    if (const auto write = reinterpret_cast<WriteFn>(::GetProcAddress(dbg, "MiniDumpWriteDump"))) {
        HANDLE f = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{::GetCurrentThreadId(), ep, FALSE};
            write(::GetCurrentProcess(), ::GetCurrentProcessId(), f,
                  static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), ep != nullptr ? &mei : nullptr, nullptr, nullptr);
            ::CloseHandle(f);
        }
    }
}

// log, dump, tell the user from another process (this one is in no state to show anything safely).
// whole_process: the process is about to die; else one window was dropped and the rest keeps running
void write_report(EXCEPTION_POINTERS* ep, bool whole_process)
{
    wchar_t dir[MAX_PATH]{};
    ensure_dir(dir, MAX_PATH);
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    wchar_t stem[MAX_PATH + 48];
    _snwprintf_s(stem, _TRUNCATE, L"%s\\crash-%04u%02u%02u-%02u%02u%02u", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring log_path = std::wstring{stem} + L".log";

    Out o;
    o.file = ::CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    wchar_t exe[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    char exe8[MAX_PATH * 2]{};
    ::WideCharToMultiByte(CP_UTF8, 0, exe, -1, exe8, static_cast<int>(sizeof exe8) - 1, nullptr, nullptr);
    o.line("PrismFS crash report %04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    o.line("exe %s, pid %lu, thread %lu", exe8, ::GetCurrentProcessId(), ::GetCurrentThreadId());
    if (ep != nullptr && ep->ExceptionRecord != nullptr) {
        const EXCEPTION_RECORD& r = *ep->ExceptionRecord;
        char where[300];
        describe(reinterpret_cast<ULONG_PTR>(r.ExceptionAddress), where, sizeof where);
        o.line("%s (0x%08lx) at %s", code_name(r.ExceptionCode), r.ExceptionCode, where);
        if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2) {
            o.line("  %s address 0x%llx", r.ExceptionInformation[0] == 0 ? "reading" : r.ExceptionInformation[0] == 1 ? "writing" : "executing", static_cast<unsigned long long>(r.ExceptionInformation[1]));
        }
        o.line("stack:");
        if (ep->ContextRecord != nullptr) { write_stack(o, *ep->ContextRecord); }
    }
    if (o.file != INVALID_HANDLE_VALUE) { ::CloseHandle(o.file); }
    write_dump((std::wstring{stem} + L".dmp").c_str(), ep);
    prune(dir);

    // the notice comes from a new process: `prismfs.exe --crashed <report>` (see main.cpp)
    std::wstring cmd = L"\"" + std::wstring{exe} + L"\" --crashed \"" + log_path + L"\"" + (whole_process ? L"" : L" --window");
    STARTUPINFOW        si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
}

LONG WINAPI top_filter(EXCEPTION_POINTERS* ep)
{
    if (g_reporting.exchange(true)) { return EXCEPTION_CONTINUE_SEARCH; } // (a fault inside the handler: give up)
    write_report(ep, true);
    return EXCEPTION_EXECUTE_HANDLER;
}

// abort(), a pure virtual call and a bad CRT argument end the process without an exception: raise one, so they get the same report
void raise_abort() { ::RaiseException(k_abort_code, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
void __cdecl on_signal(int) { raise_abort(); }
void __cdecl on_purecall() { raise_abort(); }
void __cdecl on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { raise_abort(); }

[[nodiscard]] bool is_fault(DWORD c) noexcept
{
    return c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_IN_PAGE_ERROR || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_INT_DIVIDE_BY_ZERO ||
           c == EXCEPTION_PRIV_INSTRUCTION || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || c == EXCEPTION_DATATYPE_MISALIGNMENT;
}

int guard_filter(EXCEPTION_POINTERS* ep, const char* what)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!is_fault(code)) { return EXCEPTION_CONTINUE_SEARCH; } // (c++ exceptions and the like are not ours to swallow)
    char where[300];
    describe(reinterpret_cast<ULONG_PTR>(ep->ExceptionRecord->ExceptionAddress), where, sizeof where);
    log::info("guarded call '{}' faulted: {} at {} (the call was abandoned)", what, code_name(code), where);
    // a second line for the log file even when logging is off: which third-party module misbehaved
    char line[512];
    std::snprintf(line, sizeof line, "PrismFS: '%s' faulted: %s at %s\n", what, code_name(code), where);
    ::OutputDebugStringA(line);
    return EXCEPTION_EXECUTE_HANDLER;
}

int window_filter(EXCEPTION_POINTERS* ep)
{
    if (!is_fault(ep->ExceptionRecord->ExceptionCode) && ep->ExceptionRecord->ExceptionCode != k_abort_code) { return EXCEPTION_CONTINUE_SEARCH; }
    if (g_reporting.exchange(true)) { return EXCEPTION_CONTINUE_SEARCH; } // (already reporting: let it go to the top-level handler)
    write_report(ep, false);
    g_reporting.store(false);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void install()
{
    ::SetUnhandledExceptionFilter(top_filter);
    std::signal(SIGABRT, on_signal);
    _set_purecall_handler(on_purecall);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // (no CRT message box: the report is ours)
}

void set_directory(const std::wstring& dir) { wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE); }

bool guarded_window(const char*, void (*fn)(void*), void* context) noexcept
{
    __try {
        fn(context);
        return true;
    } __except (window_filter(GetExceptionInformation())) {
        return false;
    }
}

bool guarded(const char* what, void (*fn)(void*), void* context) noexcept
{
    __try {
        fn(context);
        return true;
    } __except (guard_filter(GetExceptionInformation(), what)) {
        return false;
    }
}

} // namespace prism::core::crash
