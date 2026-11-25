#include "core/rclone.hpp"

#include "core/log.hpp"
#include "core/path.hpp"
#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace prism::core::rclone {

namespace {

// --- minimal JSON helpers (no exceptions, no external deps) --------------------------------------------------

[[nodiscard]] std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { out += std::format("\\u{:04x}", c); }
            else { out.push_back(static_cast<char>(c)); }
            break;
        }
    }
    out.push_back('"');
    return out;
}

struct JsonValue {
    enum class Type { null, boolean, number, string, array, object } type{Type::null};
    bool                                        b{};
    double                                      num{};
    std::int64_t                                i64{};
    std::string                                 str;
    std::vector<JsonValue>                      arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept
    {
        if (type != Type::object) { return nullptr; }
        for (const auto& [k, v] : obj) {
            if (k == key) { return &v; }
        }
        return nullptr;
    }
    [[nodiscard]] std::string as_string(std::string_view def = {}) const
    {
        return type == Type::string ? str : std::string{def};
    }
    [[nodiscard]] std::int64_t as_i64(std::int64_t def = 0) const noexcept
    {
        return type == Type::number ? i64 : def;
    }
    [[nodiscard]] bool as_bool(bool def = false) const noexcept
    {
        return type == Type::boolean ? b : def;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : s_{src} {}

    bool parse(JsonValue& out)
    {
        skip_ws();
        return parse_val(out);
    }

private:
    void skip_ws() noexcept
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) { ++pos_; }
    }

    static void append_utf8(std::string& dst, std::uint32_t cp)
    {
        if (cp <= 0x7f) {
            dst.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            dst.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            dst.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0x10ffff) {
            dst.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool parse_hex4(std::uint32_t& out) noexcept
    {
        if (pos_ + 4 > s_.size()) { return false; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') { v |= static_cast<std::uint32_t>(c - '0'); }
            else if (c >= 'a' && c <= 'f') { v |= static_cast<std::uint32_t>(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { v |= static_cast<std::uint32_t>(c - 'A' + 10); }
            else { return false; }
        }
        out = v;
        return true;
    }

    bool parse_string(std::string& out)
    {
        if (pos_ >= s_.size() || s_[pos_] != '"') { return false; }
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') { return true; }
            if (c == '\\') {
                if (pos_ >= s_.size()) { return false; }
                const char e = s_[pos_++];
                switch (e) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parse_hex4(cp)) { return false; }
                    if (cp >= 0xd800 && cp <= 0xdbff && pos_ + 2 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        std::uint32_t lo = 0;
                        if (parse_hex4(lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                            cp = 0x10000 + (((cp - 0xd800) << 10) | (lo - 0xdc00));
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_val(JsonValue& out, int depth = 0)
    {
        if (depth > 32) { return false; }
        skip_ws();
        if (pos_ >= s_.size()) { return false; }
        const char c = s_[pos_];
        if (c == '"') {
            out.type = JsonValue::Type::string;
            return parse_string(out.str);
        }
        if (c == '{') {
            out.type = JsonValue::Type::object;
            ++pos_;
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            while (pos_ < s_.size()) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) { return false; }
                skip_ws();
                if (pos_ >= s_.size() || s_[pos_] != ':') { return false; }
                ++pos_;
                JsonValue val;
                if (!parse_val(val, depth + 1)) { return false; }
                out.obj.emplace_back(std::move(key), std::move(val));
                skip_ws();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
                return false;
            }
            return false;
        }
        if (c == '[') {
            out.type = JsonValue::Type::array;
            ++pos_;
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            while (pos_ < s_.size()) {
                JsonValue val;
                if (!parse_val(val, depth + 1)) { return false; }
                out.arr.push_back(std::move(val));
                skip_ws();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
                return false;
            }
            return false;
        }
        if (s_.substr(pos_).starts_with("true"))  { pos_ += 4; out.type = JsonValue::Type::boolean; out.b = true; return true; }
        if (s_.substr(pos_).starts_with("false")) { pos_ += 5; out.type = JsonValue::Type::boolean; out.b = false; return true; }
        if (s_.substr(pos_).starts_with("null"))  { pos_ += 4; out.type = JsonValue::Type::null; return true; }
        // number
        const std::size_t start = pos_;
        if (s_[pos_] == '-') { ++pos_; }
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) { ++pos_; }
        if (pos_ == start) { return false; }
        const std::string num_s{s_.substr(start, pos_ - start)};
        out.type = JsonValue::Type::number;
        out.num  = std::strtod(num_s.c_str(), nullptr);
        if (num_s.find_first_of(".eE") != std::string::npos) {
            out.i64 = static_cast<std::int64_t>(out.num);
        } else {
            out.i64 = std::strtoll(num_s.c_str(), nullptr, 10);
        }
        return true;
    }

    std::string_view s_;
    std::size_t      pos_{};
};

// RFC3339 ("2026-10-19T14:23:45.123456Z") -> Windows FILETIME (100ns ticks since 1601-01-01)
[[nodiscard]] std::uint64_t parse_rfc3339_filetime(std::string_view iso) noexcept
{
    if (iso.size() < 19) { return 0; }
    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
    if (std::sscanf(std::string{iso.substr(0, 19)}.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) { return 0; }
#ifdef _WIN32
    SYSTEMTIME st{};
    st.wYear   = static_cast<WORD>(Y);
    st.wMonth  = static_cast<WORD>(M);
    st.wDay    = static_cast<WORD>(D);
    st.wHour   = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(m);
    st.wSecond = static_cast<WORD>(s);
    FILETIME ft{};
    if (!::SystemTimeToFileTime(&st, &ft)) { return 0; }
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#else
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
    const std::time_t tt = ::timegm(&tm);
    return tt < 0 ? 0 : (static_cast<std::uint64_t>(tt) + 11644473600ULL) * 10000000ULL;
#endif
}

// --- librclone C ABI -----------------------------------------------------------------------------------------

struct RcloneRPCResult {
    char* Output;
    int   Status;
};

#ifdef _WIN32
using FnInit = void(__cdecl*)();
using FnFin  = void(__cdecl*)();
using FnRpc  = RcloneRPCResult(__cdecl*)(char* method, char* input);
using FnFree = void(__cdecl*)(char* str);
#else
using FnInit = void (*)();
using FnFin  = void (*)();
using FnRpc  = RcloneRPCResult (*)(char* method, char* input);
using FnFree = void (*)(char* str);
#endif

// --- binary discovery ----------------------------------------------------------------------------------------

#ifdef _WIN32

[[nodiscard]] std::wstring exe_dir()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return {}; }
    std::wstring d{buf, n};
    const std::size_t sep = d.find_last_of(L'\\');
    return sep == std::wstring::npos ? std::wstring{} : d.substr(0, sep + 1);
}

[[nodiscard]] std::wstring find_binary(const wchar_t* filename)
{
    // 1. beside prismfs.exe
    const std::wstring beside = exe_dir() + filename;
    if (::GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES) { return beside; }
    // 2. on PATH
    wchar_t found[MAX_PATH]{};
    if (::SearchPathW(nullptr, filename, nullptr, MAX_PATH, found, nullptr) > 0) { return found; }
    return {};
}

// --- unified client (librclone.dll preferred, rclone.exe rcd fallback) ---------------------------------------

class Client {
public:
    ~Client() { shutdown(); }

    bool available()
    {
        std::lock_guard lock{mu_};
        if (dll_ != nullptr || proc_ != nullptr) { return true; }
        if (!probed_) {
            probed_ = true;
            has_bin_ = !find_binary(L"librclone.dll").empty() || !find_binary(L"rclone.exe").empty();
        }
        return has_bin_;
    }

    std::string backend_name()
    {
        std::lock_guard lock{mu_};
        if (dll_ != nullptr) { return "librclone.dll"; }
        if (proc_ != nullptr) { return "rclone.exe"; }
        if (!find_binary(L"librclone.dll").empty()) { return "librclone.dll"; }
        if (!find_binary(L"rclone.exe").empty()) { return "rclone.exe"; }
        return {};
    }

    bool rpc(const std::string& method, const std::string& body, std::string& response, int& status)
    {
        if (!ensure_started()) {
            status = 503;
            response = R"json({"error":"rclone is not installed (place librclone.dll or rclone.exe beside prismfs.exe or on PATH)"})json";
            return false;
        }
        if (dll_ != nullptr) {
            return rpc_dll(method, body, response, status);
        }
        return rpc_http(method, body, response, status);
    }

    void shutdown()
    {
        std::lock_guard lock{mu_};
        // Never call FreeLibrary() on Go's c-shared librclone.dll: Go's runtime threads do not support
        // DLL unloading and FreeLibrary deadlocks inside the Windows loader lock. OS process exit cleans it up.
        dll_     = nullptr;
        fn_init_ = nullptr;
        fn_fin_  = nullptr;
        fn_rpc_  = nullptr;
        fn_free_ = nullptr;
        if (proc_ != nullptr) {
            ::TerminateProcess(proc_, 0);
            ::CloseHandle(proc_);
            proc_ = nullptr;
        }
        if (job_ != nullptr) {
            ::CloseHandle(job_);
            job_ = nullptr;
        }
        port_ = 0;
    }

private:
    bool ensure_started()
    {
        std::lock_guard lock{mu_};
        if (dll_ != nullptr) { return true; }
        if (proc_ != nullptr) {
            DWORD code = 0;
            if (::GetExitCodeProcess(proc_, &code) && code == STILL_ACTIVE) { return true; }
            ::CloseHandle(proc_);
            proc_ = nullptr;
            port_ = 0;
        }

        // 1. Try in-process librclone.dll
        if (const std::wstring dll_path = find_binary(L"librclone.dll"); !dll_path.empty()) {
            HMODULE m = ::LoadLibraryW(dll_path.c_str());
            if (m != nullptr) {
                auto init = reinterpret_cast<FnInit>(reinterpret_cast<void*>(::GetProcAddress(m, "RcloneInitialize")));
                auto fin  = reinterpret_cast<FnFin>(reinterpret_cast<void*>(::GetProcAddress(m, "RcloneFinalize")));
                auto rpc  = reinterpret_cast<FnRpc>(reinterpret_cast<void*>(::GetProcAddress(m, "RcloneRPC")));
                auto free = reinterpret_cast<FnFree>(reinterpret_cast<void*>(::GetProcAddress(m, "RcloneFreeString")));
                if (init != nullptr && rpc != nullptr) {
                    if (free == nullptr) {
                        // librclone.dll built with MinGW allocates C.CString via msvcrt.dll malloc;
                        // never call prismfs's static UCRT std::free() on a foreign DLL heap pointer!
                        if (HMODULE crt = ::GetModuleHandleW(L"msvcrt.dll")) {
                            free = reinterpret_cast<FnFree>(reinterpret_cast<void*>(::GetProcAddress(crt, "free")));
                        }
                    }
                    init();
                    dll_     = m;
                    fn_init_ = init;
                    fn_fin_  = fin;
                    fn_rpc_  = rpc;
                    fn_free_ = free;
                    // Tune rclone for responsive interactive browsing (10s connect, 20s I/O, 2 low-level retries)
                    {
                        std::string m_opt = "options/set";
                        std::string b_opt = R"json({"main":{"Contimeout":10000000000,"Timeout":20000000000,"LowLevelRetries":2}})json";
                        RcloneRPCResult r = rpc(m_opt.data(), b_opt.data());
                        if (r.Output != nullptr && free != nullptr) { free(r.Output); }
                    }
                    log::info("rclone: loaded in-process librclone.dll");
                    return true;
                }
                ::FreeLibrary(m);
            }
        }

        // 2. Fallback to rclone.exe rcd local HTTP daemon
        const std::wstring exe_path = find_binary(L"rclone.exe");
        if (exe_path.empty()) { return false; }

        if (job_ == nullptr) {
            job_ = ::CreateJobObjectW(nullptr, nullptr);
            if (job_ != nullptr) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
                jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &jeli, sizeof jeli);
            }
        }

        // pick a deterministic-ish high port per PID so multiple windows don't collide
        port_ = static_cast<unsigned short>(55720 + (::GetCurrentProcessId() % 4000));
        std::wstring cmd = L"\"" + exe_path + L"\" rcd --rc-addr 127.0.0.1:" + std::to_wstring(port_) + L" --rc-no-auth --log-level ERROR";
        STARTUPINFOW si{};
        si.cb          = sizeof si;
        si.dwFlags     = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi)) {
            // retry without BREAKAWAY if parent job disallows it
            if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                return false;
            }
        }
        if (job_ != nullptr) { ::AssignProcessToJobObject(job_, pi.hProcess); }
        ::CloseHandle(pi.hThread);
        proc_ = pi.hProcess;

        // wait briefly for rcd to begin listening (up to 2.5s)
        for (int attempt = 0; attempt < 25; ++attempt) {
            std::string resp;
            int st = 0;
            if (rpc_http_unlocked("rc/noop", "{}", resp, st) && st == 200) {
                log::info("rclone: started rclone.exe rcd on 127.0.0.1:{}", port_);
                return true;
            }
            ::Sleep(100);
        }
        return true;
    }

    bool rpc_dll(const std::string& method, const std::string& body, std::string& response, int& status)
    {
        FnRpc  rpc_fn{};
        FnFree free_fn{};
        {
            std::lock_guard lock{mu_};
            rpc_fn  = fn_rpc_;
            free_fn = fn_free_;
        }
        if (rpc_fn == nullptr) { status = 503; return false; }
        std::string m = method;
        std::string b = body.empty() ? std::string{"{}"} : body;
        RcloneRPCResult res = rpc_fn(m.data(), b.data());
        status = res.Status;
        if (res.Output != nullptr) {
            response = res.Output;
            if (free_fn != nullptr) { free_fn(res.Output); }
        }
        return status >= 200 && status < 300;
    }

    bool rpc_http(const std::string& method, const std::string& body, std::string& response, int& status)
    {
        unsigned short p = 0;
        {
            std::lock_guard lock{mu_};
            p = port_;
        }
        return rpc_http_port(p, method, body, response, status);
    }

    bool rpc_http_unlocked(const std::string& method, const std::string& body, std::string& response, int& status)
    {
        return rpc_http_port(port_, method, body, response, status);
    }

    static bool rpc_http_port(unsigned short port, const std::string& method, const std::string& body, std::string& response, int& status)
    {
        if (port == 0) { return false; }
        HINTERNET hSession = ::WinHttpOpen(L"PrismFS/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession == nullptr) { return false; }
        ::WinHttpSetTimeouts(hSession, 5000, 5000, 30000, 120000);
        HINTERNET hConnect = ::WinHttpConnect(hSession, L"127.0.0.1", port, 0);
        if (hConnect == nullptr) { ::WinHttpCloseHandle(hSession); return false; }
        const std::wstring path = L"/" + text::widen(method);
        HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        bool ok = false;
        if (hRequest != nullptr) {
            const wchar_t* hdrs = L"Content-Type: application/json\r\n";
            const std::string payload = body.empty() ? std::string{"{}"} : body;
            if (::WinHttpSendRequest(hRequest, hdrs, static_cast<DWORD>(-1), const_cast<char*>(payload.data()),
                                     static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0) &&
                ::WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD code = 0, code_sz = sizeof code;
                ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_sz, WINHTTP_NO_HEADER_INDEX);
                status = static_cast<int>(code);
                response.clear();
                for (;;) {
                    DWORD avail = 0;
                    if (!::WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) { break; }
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!::WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0) { break; }
                    chunk.resize(read);
                    response += chunk;
                }
                ok = status >= 200 && status < 300;
            }
            ::WinHttpCloseHandle(hRequest);
        }
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return ok;
    }

    std::mutex     mu_;
    HMODULE        dll_{};
    FnInit         fn_init_{};
    FnFin          fn_fin_{};
    FnRpc          fn_rpc_{};
    FnFree         fn_free_{};
    HANDLE         proc_{};
    HANDLE         job_{};
    unsigned short port_{};
    bool           probed_{};
    bool           has_bin_{};
};
#else
class Client {
public:
    ~Client() { shutdown(); }
    bool available()
    {
        std::lock_guard lock{mu_};
        if (so_ != nullptr) { return true; }
        return !find_so().empty();
    }
    std::string backend_name() { return available() ? "librclone.so" : std::string{}; }
    bool rpc(const std::string& method, const std::string& body, std::string& response, int& status)
    {
        if (!ensure_started()) {
            status = 503;
            response = R"json({"error":"librclone.so not found beside prismfs or in library path"})json";
            return false;
        }
        std::string m = method;
        std::string b = body.empty() ? std::string{"{}"} : body;
        RcloneRPCResult res = fn_rpc_(m.data(), b.data());
        status = res.Status;
        if (res.Output != nullptr) {
            response = res.Output;
            if (fn_free_ != nullptr) { fn_free_(res.Output); }
            else { std::free(res.Output); }
        }
        return status >= 200 && status < 300;
    }
    void shutdown()
    {
        std::lock_guard lock{mu_};
        // Never dlclose() Go's c-shared librclone.so; OS process exit tears it down cleanly.
        so_ = nullptr;
        fn_init_ = nullptr; fn_fin_ = nullptr; fn_rpc_ = nullptr; fn_free_ = nullptr;
    }
private:
    static std::string find_so()
    {
        char exe[PATH_MAX]{};
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            std::string dir(exe, static_cast<std::size_t>(n));
            if (const std::size_t sep = dir.rfind('/'); sep != std::string::npos) {
                const std::string beside = dir.substr(0, sep + 1) + "librclone.so";
                struct stat st{};
                if (::stat(beside.c_str(), &st) == 0) { return beside; }
            }
        }
        return {};
    }
    bool ensure_started()
    {
        std::lock_guard lock{mu_};
        if (so_ != nullptr) { return true; }
        std::string p = find_so();
        void* h = ::dlopen(p.empty() ? "librclone.so" : p.c_str(), RTLD_LAZY);
        if (h == nullptr) { return false; }
        auto init = reinterpret_cast<FnInit>(::dlsym(h, "RcloneInitialize"));
        auto fin  = reinterpret_cast<FnFin>(::dlsym(h, "RcloneFinalize"));
        auto rpc  = reinterpret_cast<FnRpc>(::dlsym(h, "RcloneRPC"));
        auto free = reinterpret_cast<FnFree>(::dlsym(h, "RcloneFreeString"));
        if (init == nullptr || rpc == nullptr) { ::dlclose(h); return false; }
        init();
        so_ = h; fn_init_ = init; fn_fin_ = fin; fn_rpc_ = rpc; fn_free_ = free;
        {
            std::string m_opt = "options/set";
            std::string b_opt = R"json({"main":{"Contimeout":10000000000,"Timeout":20000000000,"LowLevelRetries":2}})json";
            RcloneRPCResult r = rpc(m_opt.data(), b_opt.data());
            if (r.Output != nullptr) {
                if (free != nullptr) { free(r.Output); }
                else { std::free(r.Output); }
            }
        }
        return true;
    }
    std::mutex mu_;
    void*      so_{};
    FnInit     fn_init_{};
    FnFin      fn_fin_{};
    FnRpc      fn_rpc_{};
    FnFree     fn_free_{};
};
#endif

Client& client()
{
    static Client c;
    return c;
}

[[nodiscard]] std::string extract_error(const std::string& resp)
{
    JsonValue v;
    if (JsonParser{resp}.parse(v)) {
        if (const JsonValue* e = v.find("error"); e != nullptr && e->type == JsonValue::Type::string && !e->str.empty()) {
            return e->str;
        }
    }
    return resp.empty() ? std::string{"rclone request failed"} : resp;
}

[[nodiscard]] std::string to_unix_path(std::string_view p)
{
    std::string out{p};
    for (char& c : out) { if (c == '\\') { c = '/'; } }
    return out;
}

[[nodiscard]] std::string cache_root()
{
#ifdef _WIN32
    wchar_t tmp[MAX_PATH + 1]{};
    const DWORD n = ::GetTempPathW(MAX_PATH, tmp);
    std::wstring w{tmp, n};
    while (!w.empty() && w.back() == L'\\') { w.pop_back(); }
    w += L"\\PrismFS\\rclone";
    ::CreateDirectoryW((w.substr(0, w.rfind(L'\\'))).c_str(), nullptr);
    ::CreateDirectoryW(w.c_str(), nullptr);
    return text::narrow(w);
#else
    const char* t = std::getenv("TMPDIR");
    std::string dir = std::string{t != nullptr && t[0] != '\0' ? t : "/tmp"} + "/PrismFS/rclone";
    ::mkdir((dir.substr(0, dir.rfind('/'))).c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    return dir;
#endif
}

void ensure_dirs(const std::string& dir)
{
#ifdef _WIN32
    const std::wstring w = text::widen(dir);
    for (std::size_t i = 3; i < w.size(); ++i) {
        if (w[i] == L'\\') { ::CreateDirectoryW(w.substr(0, i).c_str(), nullptr); }
    }
    ::CreateDirectoryW(w.c_str(), nullptr);
#else
    for (std::size_t i = 1; i < dir.size(); ++i) {
        if (dir[i] == '/') { ::mkdir(dir.substr(0, i).c_str(), 0755); }
    }
    ::mkdir(dir.c_str(), 0755);
#endif
}

} // namespace

// --- path helpers --------------------------------------------------------------------------------------------

bool is_path(std::string_view path) noexcept
{
    return path.size() > 7 && path.starts_with("rclone:");
}

SplitPath split(std::string_view path)
{
    SplitPath out;
    if (!is_path(path)) { return out; }
    path.remove_prefix(7); // drop "rclone:"
    const std::size_t colon = path.find(':');
    if (colon == std::string_view::npos) {
        const std::size_t slash = path.find_first_of("\\/");
        out.remote = std::string{slash == std::string_view::npos ? path : path.substr(0, slash)};
        if (slash != std::string_view::npos) { path.remove_prefix(slash + 1); }
        else { path = {}; }
    } else {
        out.remote = std::string{path.substr(0, colon)};
        path.remove_prefix(colon + 1);
    }
    while (!path.empty() && (path.front() == '\\' || path.front() == '/')) { path.remove_prefix(1); }
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) { path.remove_suffix(1); }
    out.rel_unix = to_unix_path(path);
    return out;
}

std::string make_root(std::string_view remote)
{
    return "rclone:" + std::string{remote} + ":";
}

std::string normalize_path(std::string_view path)
{
    const SplitPath sp = split(path);
    if (sp.remote.empty()) { return {}; }
    std::string out = "rclone:" + sp.remote + ":";
    if (!sp.rel_unix.empty()) {
        out.push_back('\\');
        for (const char c : sp.rel_unix) { out.push_back(c == '/' ? '\\' : c); }
    }
    return out;
}

// --- API -----------------------------------------------------------------------------------------------------

bool available() { return client().available(); }
std::string backend_name() { return client().backend_name(); }
void shutdown() { client().shutdown(); }

namespace {

struct DirCacheEntry {
    std::vector<FileEntry>                entries;
    std::chrono::steady_clock::time_point at{};
};
std::mutex                                         g_dir_cache_mu;
std::unordered_map<std::string, DirCacheEntry>     g_dir_cache;

std::mutex                                         g_rem_cache_mu;
std::vector<Remote>                                g_rem_cache;
std::chrono::steady_clock::time_point              g_rem_cache_at{};
bool                                               g_rem_cache_valid{};

void invalidate_remotes_cache()
{
    std::lock_guard g{g_rem_cache_mu};
    g_rem_cache_valid = false;
}

} // namespace

void invalidate_cache(std::string_view rclone_path)
{
    std::lock_guard g{g_dir_cache_mu};
    if (rclone_path.empty()) {
        g_dir_cache.clear();
        return;
    }
    const std::string key = text::lower_ascii(normalize_path(rclone_path));
    g_dir_cache.erase(key);
}

std::vector<Remote> list_remotes()
{
    if (!available()) { return {}; }
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard g{g_rem_cache_mu};
        if (g_rem_cache_valid && now - g_rem_cache_at < std::chrono::seconds{30}) {
            return g_rem_cache;
        }
    }
    std::vector<Remote> out;
    std::string resp;
    int status = 0;
    if (!client().rpc("config/dump", "{}", resp, status)) { return out; }
    JsonValue root;
    if (!JsonParser{resp}.parse(root) || root.type != JsonValue::Type::object) { return out; }
    for (const auto& [name, val] : root.obj) {
        if (val.type != JsonValue::Type::object) { continue; }
        Remote r;
        r.name = name;
        if (const JsonValue* t = val.find("type")) { r.type = t->as_string(); }
        out.push_back(std::move(r));
    }
    std::ranges::sort(out, [](const Remote& a, const Remote& b) { return text::compare_natural(a.name, b.name) < 0; });
    {
        std::lock_guard g{g_rem_cache_mu};
        g_rem_cache       = out;
        g_rem_cache_at    = now;
        g_rem_cache_valid = true;
    }
    return out;
}

bool about(const std::string& remote, std::uint64_t& total, std::uint64_t& free)
{
    total = free = 0;
    if (!available() || remote.empty()) { return false; }
    // cache quota for 60s so DriveWatcher's 3s poll never hammers Google Drive over the network
    struct QuotaCache {
        std::uint64_t total{}, free{};
        std::chrono::steady_clock::time_point at{};
        bool ok{};
    };
    static std::mutex cache_mu;
    static std::unordered_map<std::string, QuotaCache> cache;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard g{cache_mu};
        if (const auto it = cache.find(remote); it != cache.end()) {
            const auto ttl = it->second.ok ? std::chrono::seconds{300} : std::chrono::seconds{10};
            if (now - it->second.at < ttl) {
                total = it->second.total;
                free  = it->second.free;
                return it->second.ok;
            }
        }
    }
    std::string resp;
    int status = 0;
    const std::string body = "{\"fs\":" + json_escape(remote + ":") + "}";
    const bool ok = client().rpc("operations/about", body, resp, status);
    if (ok) {
        JsonValue root;
        if (JsonParser{resp}.parse(root) && root.type == JsonValue::Type::object) {
            if (const JsonValue* t = root.find("total"); t != nullptr && t->i64 > 0) { total = static_cast<std::uint64_t>(t->i64); }
            if (const JsonValue* f = root.find("free"); f != nullptr && f->i64 >= 0) { free = static_cast<std::uint64_t>(f->i64); }
            else if (total > 0) {
                if (const JsonValue* u = root.find("used"); u != nullptr && u->i64 >= 0 && static_cast<std::uint64_t>(u->i64) <= total) {
                    free = total - static_cast<std::uint64_t>(u->i64);
                } else {
                    free = total;
                }
            }
        }
    } else {
        log::info("rclone: about({}) failed ({}): {}", remote, status, extract_error(resp));
    }
    {
        std::lock_guard g{cache_mu};
        cache[remote] = QuotaCache{total, free, now, total > 0};
    }
    return total > 0;
}

bool stat(const std::string& rclone_path, FileEntry& out, std::string& error)
{
    out = {};
    const SplitPath sp = split(rclone_path);
    if (sp.remote.empty()) { error = "Invalid remote path"; return false; }
    if (sp.rel_unix.empty()) {
        out.name       = sp.remote;
        out.attributes = attr::directory;
        out.kind       = FileKind::folder;
        out.ext_offset = static_cast<std::uint16_t>(out.name.size());
        return true;
    }
    // fast path: check cached parent directory listing first (zero network calls)
    {
        const std::string parent_key = text::lower_ascii(normalize_path(path::parent(rclone_path)));
        const std::string leaf_l     = text::lower_ascii(path::leaf(rclone_path));
        const auto now               = std::chrono::steady_clock::now();
        std::lock_guard g{g_dir_cache_mu};
        if (const auto it = g_dir_cache.find(parent_key); it != g_dir_cache.end() && now - it->second.at < std::chrono::seconds{180}) {
            for (const FileEntry& e : it->second.entries) {
                if (text::lower_ascii(e.name) == leaf_l) {
                    out = e;
                    return true;
                }
            }
        }
    }
    const std::string body = "{\"fs\":" + json_escape(sp.remote + ":") + ",\"remote\":" + json_escape(sp.rel_unix) + "}";
    std::string resp;
    int status = 0;
    if (!client().rpc("operations/stat", body, resp, status)) {
        error = extract_error(resp);
        return false;
    }
    JsonValue root;
    if (!JsonParser{resp}.parse(root) || root.type != JsonValue::Type::object) { error = "Invalid JSON from rclone"; return false; }
    const JsonValue* item = root.find("item");
    if (item == nullptr || item->type != JsonValue::Type::object) { error = "Item not found"; return false; }
    if (const JsonValue* n = item->find("Name")) { out.name = n->as_string(); }
    if (out.name.empty()) { out.name = path::leaf(rclone_path); }
    const bool is_dir = item->find("IsDir") != nullptr && item->find("IsDir")->as_bool();
    if (is_dir) {
        out.attributes = attr::directory;
        out.kind       = FileKind::folder;
        out.ext_offset = static_cast<std::uint16_t>(out.name.size());
    } else {
        if (const JsonValue* sz = item->find("Size"); sz != nullptr && sz->i64 >= 0) { out.size = static_cast<std::uint64_t>(sz->i64); }
        const std::size_t dot = out.name.rfind('.');
        out.ext_offset = (dot == std::string::npos || dot == 0 || dot + 1 >= out.name.size())
                             ? static_cast<std::uint16_t>(out.name.size())
                             : static_cast<std::uint16_t>(dot + 1);
        out.kind = classify_extension(text::lower_ascii(out.ext()));
    }
    if (const JsonValue* mt = item->find("ModTime")) { out.modified = parse_rfc3339_filetime(mt->as_string()); }
    return true;
}

bool dir_size(const std::string& rclone_path, std::uint64_t& count, std::uint64_t& bytes, std::string& error)
{
    count = bytes = 0;
    const SplitPath sp = split(rclone_path);
    if (sp.remote.empty()) { error = "Invalid remote path"; return false; }
    const std::string fs_str = sp.rel_unix.empty() ? (sp.remote + ":") : (sp.remote + ":" + sp.rel_unix);
    const std::string body   = "{\"fs\":" + json_escape(fs_str) + "}";
    std::string resp;
    int status = 0;
    if (!client().rpc("operations/size", body, resp, status)) {
        error = extract_error(resp);
        return false;
    }
    JsonValue root;
    if (!JsonParser{resp}.parse(root) || root.type != JsonValue::Type::object) { error = "Invalid JSON from rclone"; return false; }
    if (const JsonValue* c = root.find("count"); c != nullptr && c->i64 >= 0) { count = static_cast<std::uint64_t>(c->i64); }
    if (const JsonValue* b = root.find("bytes"); b != nullptr && b->i64 >= 0) { bytes = static_cast<std::uint64_t>(b->i64); }
    return true;
}

bool list_dir(const std::string& rclone_path, std::vector<FileEntry>& out, std::string& error, bool force_refresh)
{
    out.clear();
    const SplitPath sp = split(rclone_path);
    if (sp.remote.empty()) { error = "Invalid remote path"; return false; }
    const std::string cache_key = text::lower_ascii(normalize_path(rclone_path));
    const auto now = std::chrono::steady_clock::now();
    if (!force_refresh) {
        std::lock_guard g{g_dir_cache_mu};
        if (const auto it = g_dir_cache.find(cache_key); it != g_dir_cache.end() && now - it->second.at < std::chrono::seconds{180}) {
            out = it->second.entries;
            return true;
        }
    }
    const std::string body = "{\"fs\":" + json_escape(sp.remote + ":") +
                             ",\"remote\":" + json_escape(sp.rel_unix) +
                             ",\"opt\":{\"noMimeType\":true,\"showHash\":false,\"showOrigIDs\":false}}";
    std::string resp;
    int status = 0;
    if (!client().rpc("operations/list", body, resp, status)) {
        error = extract_error(resp);
        log::info("rclone: list_dir({}) failed ({}): {}", rclone_path, status, error);
        return false;
    }
    JsonValue root;
    if (!JsonParser{resp}.parse(root)) { error = "Invalid JSON from rclone"; return false; }
    const JsonValue* list = root.find("list");
    if (list == nullptr || list->type != JsonValue::Type::array) { return true; }
    out.reserve(list->arr.size());
    for (const JsonValue& item : list->arr) {
        if (item.type != JsonValue::Type::object) { continue; }
        FileEntry e;
        if (const JsonValue* n = item.find("Name")) { e.name = n->as_string(); }
        if (e.name.empty()) { continue; }
        const bool is_dir = item.find("IsDir") != nullptr && item.find("IsDir")->as_bool();
        if (is_dir) {
            e.attributes = attr::directory;
            e.kind       = FileKind::folder;
            e.ext_offset = static_cast<std::uint16_t>(e.name.size());
        } else {
            e.attributes = 0;
            if (const JsonValue* sz = item.find("Size"); sz != nullptr && sz->i64 >= 0) { e.size = static_cast<std::uint64_t>(sz->i64); }
            const std::size_t dot = e.name.rfind('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 >= e.name.size()) {
                e.ext_offset = static_cast<std::uint16_t>(e.name.size());
            } else {
                e.ext_offset = static_cast<std::uint16_t>(dot + 1);
            }
            e.kind = classify_extension(text::lower_ascii(e.ext()));
        }
        if (const JsonValue* mt = item.find("ModTime")) { e.modified = parse_rfc3339_filetime(mt->as_string()); }
        out.push_back(std::move(e));
    }
    {
        std::lock_guard g{g_dir_cache_mu};
        if (g_dir_cache.size() > 256) { g_dir_cache.clear(); }
        g_dir_cache[cache_key] = DirCacheEntry{out, std::chrono::steady_clock::now()};
    }
    return true;
}

bool download_to_cache(const std::string& rclone_path, std::string& local_path, std::string& error)
{
    const SplitPath sp = split(rclone_path);
    if (sp.remote.empty() || sp.rel_unix.empty()) { error = "Cannot open a remote root as a file"; return false; }
    const std::string leaf = path::leaf(rclone_path);
    std::string dir = cache_root() + "\\" + sp.remote;
    if (const std::size_t sep = sp.rel_unix.rfind('/'); sep != std::string::npos) {
        std::string sub = sp.rel_unix.substr(0, sep);
        for (char& c : sub) { if (c == '/') { c = '\\'; } }
        dir += "\\" + sub;
    }
    ensure_dirs(dir);
    const std::string body = "{\"srcFs\":" + json_escape(sp.remote + ":") +
                             ",\"srcRemote\":" + json_escape(sp.rel_unix) +
                             ",\"dstFs\":" + json_escape(dir) +
                             ",\"dstRemote\":" + json_escape(leaf) + "}";
    std::string resp;
    int status = 0;
    if (!client().rpc("operations/copyfile", body, resp, status)) {
        error = extract_error(resp);
        return false;
    }
    local_path = path::join(dir, leaf);
    return true;
}

bool copy_or_move(bool move, const std::string& src, const std::string& dst_dir, std::string& error)
{
    const bool src_rc = is_path(src);
    const bool dst_rc = is_path(dst_dir);
    if (!src_rc && !dst_rc) { error = "Neither source nor destination is an rclone path"; return false; }

    const std::string leaf = path::leaf(src);
    if (leaf.empty()) { error = "Invalid source name"; return false; }

    // determine if source is a directory
    bool is_dir = false;
    if (!src_rc) {
        is_dir = path::is_directory(src);
    } else {
        const SplitPath sp = split(src);
        std::string stat_resp;
        int st = 0;
        const std::string stat_body = "{\"fs\":" + json_escape(sp.remote + ":") + ",\"remote\":" + json_escape(sp.rel_unix) + "}";
        if (client().rpc("operations/stat", stat_body, stat_resp, st)) {
            JsonValue v;
            if (JsonParser{stat_resp}.parse(v)) {
                if (const JsonValue* item = v.find("item")) {
                    if (const JsonValue* d = item->find("IsDir")) { is_dir = d->as_bool(); }
                }
            }
        }
    }

    std::string resp;
    int status = 0;
    if (is_dir) {
        const std::string src_fs = src_rc ? (split(src).remote + ":" + split(src).rel_unix) : src;
        const std::string dst_fs = dst_rc
            ? (split(dst_dir).remote + ":" + (split(dst_dir).rel_unix.empty() ? leaf : split(dst_dir).rel_unix + "/" + leaf))
            : path::join(dst_dir, leaf);
        const std::string body = move
            ? ("{\"srcFs\":" + json_escape(src_fs) + ",\"dstFs\":" + json_escape(dst_fs) + ",\"deleteEmptySrcDirs\":true}")
            : ("{\"srcFs\":" + json_escape(src_fs) + ",\"dstFs\":" + json_escape(dst_fs) + "}");
        if (!client().rpc(move ? "sync/move" : "sync/copy", body, resp, status)) {
            error = extract_error(resp);
            log::info("rclone: {} dir {} -> {} failed ({}): {}", move ? "move" : "copy", src, dst_dir, status, error);
            return false;
        }
        if (src_rc && move) { invalidate_cache(path::parent(src)); }
        if (dst_rc) { invalidate_cache(dst_dir); }
        return true;
    }

    const std::string src_fs     = src_rc ? (split(src).remote + ":") : path::parent(src);
    const std::string src_remote = src_rc ? split(src).rel_unix : leaf;
    const std::string dst_fs     = dst_rc ? (split(dst_dir).remote + ":") : dst_dir;
    const std::string dst_remote = dst_rc
        ? (split(dst_dir).rel_unix.empty() ? leaf : split(dst_dir).rel_unix + "/" + leaf)
        : leaf;
    const std::string body = "{\"srcFs\":" + json_escape(src_fs) +
                             ",\"srcRemote\":" + json_escape(src_remote) +
                             ",\"dstFs\":" + json_escape(dst_fs) +
                             ",\"dstRemote\":" + json_escape(dst_remote) + "}";
    if (!client().rpc(move ? "operations/movefile" : "operations/copyfile", body, resp, status)) {
        error = extract_error(resp);
        log::info("rclone: {} file {} -> {} failed ({}): {}", move ? "move" : "copy", src, dst_dir, status, error);
        return false;
    }
    if (src_rc && move) { invalidate_cache(path::parent(src)); }
    if (dst_rc) { invalidate_cache(dst_dir); }
    return true;
}

bool mkdir(const std::string& rclone_path, std::string& error)
{
    const SplitPath sp = split(rclone_path);
    if (sp.remote.empty() || sp.rel_unix.empty()) { error = "Invalid folder path"; return false; }
    const std::string body = "{\"fs\":" + json_escape(sp.remote + ":") + ",\"remote\":" + json_escape(sp.rel_unix) + "}";
    std::string resp;
    int status = 0;
    if (!client().rpc("operations/mkdir", body, resp, status)) {
        error = extract_error(resp);
        log::info("rclone: mkdir({}) failed ({}): {}", rclone_path, status, error);
        return false;
    }
    invalidate_cache(path::parent(rclone_path));
    return true;
}

bool create_empty_file(const std::string& rclone_path, std::string& error)
{
    const SplitPath sp = split(rclone_path);
    if (sp.remote.empty() || sp.rel_unix.empty()) { error = "Invalid file path"; return false; }
    const std::string stage_dir = cache_root() + "\\_new";
    ensure_dirs(stage_dir);
    const std::string tmp_name = "empty_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string tmp_path = path::join(stage_dir, tmp_name);
    if (std::FILE* f = std::fopen(tmp_path.c_str(), "wb")) {
        std::fclose(f);
    } else {
        error = "Could not create staging file";
        return false;
    }
    const std::string body = "{\"srcFs\":" + json_escape(stage_dir) +
                             ",\"srcRemote\":" + json_escape(tmp_name) +
                             ",\"dstFs\":" + json_escape(sp.remote + ":") +
                             ",\"dstRemote\":" + json_escape(sp.rel_unix) + "}";
    std::string resp;
    int status = 0;
    const bool ok = client().rpc("operations/copyfile", body, resp, status);
    std::remove(tmp_path.c_str());
    if (!ok) {
        error = extract_error(resp);
        log::info("rclone: create_empty_file({}) failed ({}): {}", rclone_path, status, error);
        return false;
    }
    invalidate_cache(path::parent(rclone_path));
    return true;
}

bool rename(const std::string& rclone_path, const std::string& new_name, std::string& error)
{
    const SplitPath sp = split(rclone_path);
    if (sp.remote.empty() || sp.rel_unix.empty() || new_name.empty()) { error = "Invalid rename"; return false; }
    const std::size_t sep = sp.rel_unix.rfind('/');
    const std::string dst_rel = sep == std::string::npos ? new_name : sp.rel_unix.substr(0, sep + 1) + new_name;
    // try movefile first; if it's a directory, fall back to sync/move
    std::string resp;
    int status = 0;
    const std::string file_body = "{\"srcFs\":" + json_escape(sp.remote + ":") + ",\"srcRemote\":" + json_escape(sp.rel_unix) +
                                  ",\"dstFs\":" + json_escape(sp.remote + ":") + ",\"dstRemote\":" + json_escape(dst_rel) + "}";
    if (client().rpc("operations/movefile", file_body, resp, status)) { invalidate_cache(path::parent(rclone_path)); return true; }
    const std::string dir_body = "{\"srcFs\":" + json_escape(sp.remote + ":" + sp.rel_unix) +
                                 ",\"dstFs\":" + json_escape(sp.remote + ":" + dst_rel) + "}";
    if (client().rpc("sync/move", dir_body, resp, status)) { invalidate_cache(path::parent(rclone_path)); return true; }
    error = extract_error(resp);
    return false;
}

bool remove(const std::string& rclone_path, std::string& error)
{
    const SplitPath sp = split(rclone_path);
    if (sp.remote.empty() || sp.rel_unix.empty()) { error = "Cannot delete remote root"; return false; }
    std::string resp;
    int status = 0;
    const std::string body = "{\"fs\":" + json_escape(sp.remote + ":") + ",\"remote\":" + json_escape(sp.rel_unix) + "}";
    if (client().rpc("operations/deletefile", body, resp, status)) { invalidate_cache(path::parent(rclone_path)); return true; }
    if (client().rpc("operations/purge", body, resp, status)) { invalidate_cache(path::parent(rclone_path)); return true; }
    error = extract_error(resp);
    return false;
}

bool connect_google_drive(std::string& message)
{
    if (!available()) {
        message = "Place librclone.dll or rclone.exe beside prismfs.exe or on PATH to connect Google Drive";
        return false;
    }
    // pick a unique remote name ("gdrive", "gdrive2", ...)
    const auto existing = list_remotes();
    std::string name = "gdrive";
    for (int i = 2; i < 100; ++i) {
        const bool used = std::ranges::any_of(existing, [&](const Remote& r) { return text::lower_ascii(r.name) == name; });
        if (!used) { break; }
        name = "gdrive" + std::to_string(i);
    }

    // Drive rclone's built-in OAuth state machine directly through RPC (works with librclone.dll, librclone.so, and rclone.exe)
    std::string resp;
    int status = 0;
    const std::string create_body = "{\"name\":" + json_escape(name) +
                                    ",\"type\":\"drive\",\"parameters\":{},\"opt\":{\"nonInteractive\":true}}";
    if (!client().rpc("config/create", create_body, resp, status)) {
        message = extract_error(resp);
        return false;
    }

    for (int step = 0; step < 12; ++step) {
        JsonValue root;
        if (!JsonParser{resp}.parse(root) || root.type != JsonValue::Type::object) { break; }
        const JsonValue* st = root.find("State");
        if (st == nullptr || st->type != JsonValue::Type::string || st->str.empty()) {
            invalidate_remotes_cache();
            message = "Google Drive connected";
            return true;
        }
        const std::string state = st->str;
        std::string answer;
        if (state.find("oauth-islocal") != std::string::npos || state.find("oauth-confirm") != std::string::npos) {
            answer = "true"; // opens the default browser for Google sign-in
        } else if (state.find("teamdrive") != std::string::npos || state.find("team_drive") != std::string::npos) {
            answer = "false";
        } else if (const JsonValue* opt = root.find("Option"); opt != nullptr) {
            if (const JsonValue* def = opt->find("DefaultStr"); def != nullptr) { answer = def->as_string(); }
        }
        const std::string upd_body = "{\"name\":" + json_escape(name) +
                                     ",\"parameters\":{},\"opt\":{\"nonInteractive\":true,\"state\":" + json_escape(state) +
                                     ",\"result\":" + json_escape(answer) + "}}";
        if (!client().rpc("config/update", upd_body, resp, status)) {
            message = extract_error(resp);
            return false;
        }
    }
    message = "Google Drive connected";
    return true;
}

bool delete_remote(const std::string& remote, std::string& error)
{
    if (remote.empty()) { error = "Invalid remote name"; return false; }
    std::string resp;
    int status = 0;
    const std::string body = "{\"name\":" + json_escape(remote) + "}";
    if (!client().rpc("config/delete", body, resp, status)) {
        error = extract_error(resp);
        return false;
    }
    invalidate_remotes_cache();
    invalidate_cache();
    return true;
}

bool rename_remote(const std::string& old_name, const std::string& new_name, std::string& error)
{
    if (old_name.empty() || new_name.empty()) { error = "Remote name cannot be empty"; return false; }
    if (new_name.find_first_of(":/\\") != std::string::npos) {
        error = "Remote name cannot contain : / \\";
        return false;
    }
    if (old_name == new_name) { return true; }

    std::string dump_resp;
    int status = 0;
    if (!client().rpc("config/dump", "{}", dump_resp, status)) {
        error = extract_error(dump_resp);
        return false;
    }
    JsonValue root;
    if (!JsonParser{dump_resp}.parse(root) || root.type != JsonValue::Type::object) {
        error = "Could not read rclone configuration";
        return false;
    }
    if (root.find(new_name) != nullptr) {
        error = "A cloud drive with that name already exists";
        return false;
    }
    const JsonValue* src = root.find(old_name);
    if (src == nullptr || src->type != JsonValue::Type::object) {
        error = "Cloud drive not found";
        return false;
    }
    const JsonValue* type_v = src->find("type");
    const std::string type = type_v != nullptr ? type_v->as_string() : std::string{};
    if (type.empty()) {
        error = "Invalid remote configuration";
        return false;
    }

    // rebuild parameters JSON preserving every existing key (token, scope, team_drive, etc.)
    std::string params = "{";
    bool first = true;
    for (const auto& [k, v] : src->obj) {
        if (k == "type" || v.type != JsonValue::Type::string) { continue; }
        if (!first) { params.push_back(','); }
        first = false;
        params += json_escape(k) + ":" + json_escape(v.str);
    }
    params.push_back('}');

    const std::string create_body = "{\"name\":" + json_escape(new_name) +
                                    ",\"type\":" + json_escape(type) +
                                    ",\"parameters\":" + params +
                                    ",\"opt\":{\"nonInteractive\":true,\"noObscure\":true}}";
    std::string resp;
    if (!client().rpc("config/create", create_body, resp, status)) {
        error = extract_error(resp);
        return false;
    }
    if (!delete_remote(old_name, error)) {
        return false;
    }
    return true;
}

} // namespace prism::core::rclone
