#include "core/file_info.hpp"
#include "core/thread_priority.hpp"

#include "core/file_kind.hpp"
#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <winver.h>

#include <thread>

namespace prism::core {

namespace {

[[nodiscard]] std::uint64_t pack(DWORD hi, DWORD lo) noexcept { return (static_cast<std::uint64_t>(hi) << 32) | lo; }

[[nodiscard]] std::uint64_t cluster_size(const std::wstring& path)
{
    wchar_t root[MAX_PATH]{};
    if (!::GetVolumePathNameW(path.c_str(), root, MAX_PATH)) { return 4096; }
    DWORD spc = 0, bps = 0, free_c = 0, total_c = 0;
    if (!::GetDiskFreeSpaceW(root, &spc, &bps, &free_c, &total_c)) { return 4096; }
    return static_cast<std::uint64_t>(spc) * bps;
}

[[nodiscard]] std::string version_info(const std::wstring& w, std::vector<std::pair<std::string, std::string>>& out)
{
    DWORD handle = 0;
    const DWORD n = ::GetFileVersionInfoSizeW(w.c_str(), &handle);
    if (n == 0) { return {}; }
    std::vector<char> data(n);
    if (!::GetFileVersionInfoW(w.c_str(), 0, n, data.data())) { return {}; }

    struct Lang { WORD lang, cp; };
    Lang* tr = nullptr;
    UINT  len = 0;
    if (!::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &len) || len < sizeof(Lang)) { return {}; }

    static constexpr const wchar_t* keys[] = {L"FileDescription", L"FileVersion", L"ProductName", L"ProductVersion", L"CompanyName", L"LegalCopyright", L"OriginalFilename", L"InternalName"};
    for (const wchar_t* key : keys) {
        wchar_t sub[96];
        std::swprintf(sub, 96, L"\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, key);
        wchar_t* value = nullptr;
        UINT vlen = 0;
        if (::VerQueryValueW(data.data(), sub, reinterpret_cast<LPVOID*>(&value), &vlen) && vlen > 1 && value != nullptr) {
            out.emplace_back(text::narrow(key), text::narrow(value));
        }
    }
    return {};
}

} // namespace

FileInfo query_file_info(const std::string& p)
{
    FileInfo f;
    f.path = p;
    f.name = path::leaf(p);
    const std::wstring w = text::widen(p);

    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d)) { return f; }
    f.exists     = true;
    f.attributes = d.dwFileAttributes;
    f.is_dir     = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    f.created    = pack(d.ftCreationTime.dwHighDateTime, d.ftCreationTime.dwLowDateTime);
    f.modified   = pack(d.ftLastWriteTime.dwHighDateTime, d.ftLastWriteTime.dwLowDateTime);
    f.accessed   = pack(d.ftLastAccessTime.dwHighDateTime, d.ftLastAccessTime.dwLowDateTime);

    if (f.is_dir) {
        f.type = path::is_root(p) ? "Local disk" : "File folder";
    } else {
        f.size = pack(d.nFileSizeHigh, d.nFileSizeLow);
        const std::size_t dot = f.name.rfind('.');
        const std::string ext = dot == std::string::npos || dot == 0 ? std::string{} : text::lower_ascii(std::string_view{f.name}.substr(dot + 1));
        f.type = type_label(classify_extension(ext), ext);
        DWORD hi = 0;
        const DWORD lo = ::GetCompressedFileSizeW(w.c_str(), &hi);
        const std::uint64_t phys = lo == INVALID_FILE_SIZE && ::GetLastError() != NO_ERROR ? f.size : pack(hi, lo);
        const std::uint64_t cl = cluster_size(w);
        f.size_on_disk = (phys + cl - 1) / cl * cl;
        (void)version_info(w, f.version);
    }

    // ntfs details through a handle: file id, hard links, the final path of a link
    HANDLE h = ::CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION bi{};
        if (::GetFileInformationByHandle(h, &bi)) {
            f.hard_links = bi.nNumberOfLinks;
            f.file_id    = pack(bi.nFileIndexHigh, bi.nFileIndexLow);
        }
        if ((f.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            wchar_t fin[MAX_PATH * 2]{};
            if (::GetFinalPathNameByHandleW(h, fin, MAX_PATH * 2, FILE_NAME_NORMALIZED) > 0) {
                std::string t = text::narrow(fin);
                if (t.rfind("\\\\?\\", 0) == 0) { t.erase(0, 4); }
                if (t != p) { f.link_target = t; }
            }
        }
        ::CloseHandle(h);
    }

    wchar_t root[MAX_PATH]{};
    if (::GetVolumePathNameW(w.c_str(), root, MAX_PATH)) {
        f.volume = text::narrow(root);
        wchar_t fs[64]{};
        if (::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 64)) { f.filesystem = text::narrow(fs); }
    }

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(w.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) == ERROR_SUCCESS && owner != nullptr) {
        wchar_t name[256]{}, domain[256]{};
        DWORD nl = 256, dl = 256;
        SID_NAME_USE use{};
        if (::LookupAccountSidW(nullptr, owner, name, &nl, domain, &dl, &use)) {
            f.owner = text::narrow(domain) + "\\" + text::narrow(name);
        }
    }
    if (sd != nullptr) { ::LocalFree(sd); }

    if (!f.is_dir) {
        WIN32_FIND_STREAM_DATA sd2{};
        HANDLE s = ::FindFirstStreamW(w.c_str(), FindStreamInfoStandard, &sd2, 0);
        if (s != INVALID_HANDLE_VALUE) {
            do {
                std::string n = text::narrow(sd2.cStreamName);
                if (n != "::$DATA") { f.streams.emplace_back(std::move(n), static_cast<std::uint64_t>(sd2.StreamSize.QuadPart)); }
            } while (::FindNextStreamW(s, &sd2));
            ::FindClose(s);
        }
    }
    return f;
}

bool set_file_attribute(const std::string& p, std::uint32_t flag, bool on)
{
    const std::wstring w = text::widen(p);
    const DWORD a = ::GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { return false; }
    return ::SetFileAttributesW(w.c_str(), on ? (a | flag) : (a & ~flag)) != FALSE;
}

// --- folder scan -------------------------------------------------------------------------------------------

void FolderScan::start(const std::string& root)
{
    cancel();
    s_ = std::make_shared<State>();
    std::thread{[s = s_, root] {
        lower_thread_priority();
        std::vector<std::wstring> stack{text::widen(root)};
        std::uint64_t cl = cluster_size(stack.back());
        while (!stack.empty() && !s->cancel.load(std::memory_order_relaxed)) {
            const std::wstring dir = std::move(stack.back());
            stack.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) { continue; }
            do {
                const wchar_t* n = fd.cFileName;
                if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'))) { continue; }
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    s->folders.fetch_add(1, std::memory_order_relaxed);
                    // (links are counted but not followed: no cycles, no double counting)
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) { stack.push_back(dir + L"\\" + n); }
                } else {
                    const std::uint64_t sz = pack(fd.nFileSizeHigh, fd.nFileSizeLow);
                    s->files.fetch_add(1, std::memory_order_relaxed);
                    s->bytes.fetch_add(sz, std::memory_order_relaxed);
                    s->disk.fetch_add((sz + cl - 1) / cl * cl, std::memory_order_relaxed);
                }
            } while (::FindNextFileW(h, &fd) && !s->cancel.load(std::memory_order_relaxed));
            ::FindClose(h);
        }
        s->done.store(true);
    }}.detach();
}

void FolderScan::cancel() noexcept
{
    if (s_) { s_->cancel.store(true); }
    s_.reset();
}

// --- hashes ---------------------------------------------------------------------------------------------------

namespace {

struct Alg {
    BCRYPT_ALG_HANDLE  alg{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<UCHAR> obj;
    DWORD              len{};
    bool open(LPCWSTR id)
    {
        DWORD got = 0, objlen = 0;
        if (BCryptOpenAlgorithmProvider(&alg, id, nullptr, 0) < 0) { return false; }
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objlen), sizeof objlen, &got, 0);
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof len, &got, 0);
        obj.resize(objlen);
        return BCryptCreateHash(alg, &hash, obj.data(), objlen, nullptr, 0, 0) >= 0;
    }
    std::string finish()
    {
        std::vector<UCHAR> out(len);
        if (BCryptFinishHash(hash, out.data(), len, 0) < 0) { return {}; }
        static constexpr char hex[] = "0123456789abcdef";
        std::string s;
        for (const UCHAR b : out) { s.push_back(hex[b >> 4]); s.push_back(hex[b & 15]); }
        return s;
    }
    ~Alg()
    {
        if (hash != nullptr) { BCryptDestroyHash(hash); }
        if (alg != nullptr) { BCryptCloseAlgorithmProvider(alg, 0); }
    }
};

} // namespace

void HashJob::start(const std::string& p)
{
    cancel();
    s_ = std::make_shared<State>();
    std::thread{[s = s_, w = text::widen(p)] {
        lower_thread_priority();
        HANDLE f = ::CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        Alg a5, a1, a256;
        if (f == INVALID_HANDLE_VALUE || !a5.open(BCRYPT_MD5_ALGORITHM) || !a1.open(BCRYPT_SHA1_ALGORITHM) || !a256.open(BCRYPT_SHA256_ALGORITHM)) {
            if (f != INVALID_HANDLE_VALUE) { ::CloseHandle(f); }
            s->failed.store(true);
            s->done.store(true);
            return;
        }
        LARGE_INTEGER total{};
        ::GetFileSizeEx(f, &total);
        std::vector<UCHAR> buf(1 << 20);
        std::uint64_t read_total = 0;
        DWORD n = 0;
        while (!s->cancel.load(std::memory_order_relaxed) && ::ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) {
            BCryptHashData(a5.hash, buf.data(), n, 0);
            BCryptHashData(a1.hash, buf.data(), n, 0);
            BCryptHashData(a256.hash, buf.data(), n, 0);
            read_total += n;
            if (total.QuadPart > 0) { s->progress.store(static_cast<int>(read_total * 1000 / static_cast<std::uint64_t>(total.QuadPart))); }
        }
        ::CloseHandle(f);
        if (!s->cancel.load()) {
            std::lock_guard g{s->lock};
            s->md5    = a5.finish();
            s->sha1   = a1.finish();
            s->sha256 = a256.finish();
            s->progress.store(1000);
        }
        s->done.store(true);
    }}.detach();
}

void HashJob::cancel() noexcept
{
    if (s_) { s_->cancel.store(true); }
    s_.reset();
}

std::string HashJob::md5() const { if (!s_) { return {}; } std::lock_guard g{s_->lock}; return s_->md5; }
std::string HashJob::sha1() const { if (!s_) { return {}; } std::lock_guard g{s_->lock}; return s_->sha1; }
std::string HashJob::sha256() const { if (!s_) { return {}; } std::lock_guard g{s_->lock}; return s_->sha256; }

} // namespace prism::core
