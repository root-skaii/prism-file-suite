#include "core/process.hpp"

#include <windows.h>

#include <vector>

namespace prism::core {

ProcessResult run_process(const std::wstring& command_line, const std::wstring& cwd, unsigned timeout_ms, const std::atomic<bool>* quit)
{
    ProcessResult res;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE              read_end{}, write_end{};
    if (!::CreatePipe(&read_end, &write_end, &sa, 0)) { return res; }
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    const HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = nul;
    si.hStdOutput = write_end;
    si.hStdError  = write_end;
    PROCESS_INFORMATION pi{};
    std::wstring        line = command_line; // (CreateProcess may write into it)
    const BOOL          ok   = ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    ::CloseHandle(write_end);
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
    if (!ok) {
        ::CloseHandle(read_end);
        return res;
    }
    res.started = true;

    const ULONGLONG start = ::GetTickCount64();
    std::vector<char> buf(16 * 1024);
    for (;;) {
        DWORD avail = 0;
        while (::PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (!::ReadFile(read_end, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0) { break; }
            res.output.append(buf.data(), got);
        }
        if (::WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            while (::PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) { // (what it wrote last)
                DWORD got = 0;
                if (!::ReadFile(read_end, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0) { break; }
                res.output.append(buf.data(), got);
            }
            break;
        }
        if ((quit != nullptr && quit->load()) || ::GetTickCount64() - start > timeout_ms) {
            res.timed_out = true;
            ::TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    res.exit_code = code;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(read_end);
    return res;
}

} // namespace prism::core
