#include "core/duplicates.hpp"

#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <map>
#include <thread>

namespace prism::core {

namespace {

struct Item {
    std::string   path;
    std::uint64_t size{};
    std::uint64_t modified{};
};

constexpr std::uint64_t head_bytes = 64 * 1024;

// SHA-256 of the first `limit` bytes of a file (all of it when limit == 0); empty when it cannot be read.
// what it read is added to `read`
std::string hash_file(BCRYPT_ALG_HANDLE alg, const std::string& path, std::uint64_t limit, std::atomic<std::uint64_t>& read, const std::atomic<bool>& cancel)
{
    const HANDLE f = ::CreateFileW(text::widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return {}; }
    BCRYPT_HASH_HANDLE h{};
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) {
        ::CloseHandle(f);
        return {};
    }
    std::vector<unsigned char> buf(1 << 20);
    std::uint64_t total = 0;
    bool          ok    = true;
    while (!cancel.load()) {
        const DWORD want = limit == 0 ? static_cast<DWORD>(buf.size()) : static_cast<DWORD>(std::min<std::uint64_t>(buf.size(), limit - total));
        if (want == 0) { break; }
        DWORD got = 0;
        if (!::ReadFile(f, buf.data(), want, &got, nullptr)) { ok = false; break; }
        if (got == 0) { break; }
        if (BCryptHashData(h, buf.data(), got, 0) != 0) { ok = false; break; }
        total += got;
        read += got;
    }
    std::array<unsigned char, 32> digest{};
    ok = ok && !cancel.load() && BCryptFinishHash(h, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    BCryptDestroyHash(h);
    ::CloseHandle(f);
    if (!ok) { return {}; }
    return std::string{reinterpret_cast<const char*>(digest.data()), digest.size()};
}

[[nodiscard]] bool is_dots(const wchar_t* n) noexcept { return n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0)); }

} // namespace

float DuplicateScan::progress() const noexcept
{
    if (!s_) { return 0.0f; }
    const std::uint64_t all = s_->to_read.load();
    if (all == 0) { return stage() == Stage::done ? 1.0f : 0.0f; }
    return std::min(1.0f, static_cast<float>(s_->read.load()) / static_cast<float>(all));
}

std::vector<DuplicateGroup> DuplicateScan::groups() const
{
    if (!s_) { return {}; }
    const std::lock_guard lock{s_->lock};
    return s_->groups;
}

std::uint64_t DuplicateScan::reclaimable() const
{
    std::uint64_t n = 0;
    for (const DuplicateGroup& g : groups()) { n += g.size * (g.paths.size() - 1); }
    return n;
}

void DuplicateScan::cancel() noexcept
{
    if (s_) { s_->cancel = true; }
    s_.reset();
}

void DuplicateScan::start(const std::string& folder, std::uint64_t min_size)
{
    cancel();
    s_        = std::make_shared<State>();
    s_->stage = static_cast<int>(Stage::listing);
    std::thread{[s = s_, folder, min_size] {
        // 1. every file, by size
        std::map<std::uint64_t, std::vector<Item>> by_size;
        std::vector<std::wstring>                  todo{text::widen(folder)};
        while (!todo.empty() && !s->cancel.load()) {
            const std::wstring cur = std::move(todo.back());
            todo.pop_back();
            WIN32_FIND_DATAW fd{};
            const HANDLE     h = ::FindFirstFileExW((cur + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) { continue; }
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (is_dots(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { continue; }
                    todo.push_back(cur + L"\\" + fd.cFileName);
                    continue;
                }
                const std::uint64_t size = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                ++s->files;
                if (size < min_size) { continue; }
                const std::uint64_t mod = (static_cast<std::uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                by_size[size].push_back({text::narrow(cur + L"\\" + fd.cFileName), size, mod});
            } while (!s->cancel.load() && ::FindNextFileW(h, &fd));
            ::FindClose(h);
        }

        // 2. same size and company: hash the head, then (bigger files) the whole thing
        s->stage = static_cast<int>(Stage::comparing);
        std::vector<std::vector<Item>> candidates;
        std::uint64_t                  to_read = 0;
        for (auto& [size, items] : by_size) {
            if (items.size() < 2) { continue; }
            to_read += std::min(size, head_bytes) * items.size() + (size > head_bytes ? size * items.size() : 0);
            candidates.push_back(std::move(items));
        }
        s->to_read = to_read;

        BCRYPT_ALG_HANDLE           alg{};
        std::vector<DuplicateGroup> found;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
            const auto split = [&](std::vector<Item>& items, std::uint64_t limit) {
                std::map<std::string, std::vector<Item>> groups;
                for (Item& it : items) {
                    if (s->cancel.load()) { break; }
                    std::string d = hash_file(alg, it.path, limit, s->read, s->cancel);
                    if (!d.empty()) { groups[std::move(d)].push_back(std::move(it)); }
                }
                return groups;
            };
            for (std::vector<Item>& bucket : candidates) {
                if (s->cancel.load()) { break; }
                const std::uint64_t size = bucket.front().size;
                for (auto& head : split(bucket, size > head_bytes ? head_bytes : 0)) {
                    std::vector<Item>& same_head = head.second;
                    if (same_head.size() < 2) { continue; }
                    std::vector<std::vector<Item>> finals;
                    if (size > head_bytes) {
                        for (auto& whole : split(same_head, 0)) { finals.push_back(std::move(whole.second)); }
                    } else {
                        finals.push_back(std::move(same_head));
                    }
                    for (std::vector<Item>& g : finals) {
                        if (g.size() < 2) { continue; }
                        std::ranges::sort(g, [](const Item& a, const Item& b) { return a.modified != b.modified ? a.modified < b.modified : a.path < b.path; });
                        DuplicateGroup out;
                        out.size = size;
                        for (Item& it : g) { out.paths.push_back(std::move(it.path)); }
                        found.push_back(std::move(out));
                    }
                }
            }
            BCryptCloseAlgorithmProvider(alg, 0);
        }
        std::ranges::sort(found, [](const DuplicateGroup& a, const DuplicateGroup& b) {
            return a.size * (a.paths.size() - 1) > b.size * (b.paths.size() - 1);
        });
        if (!s->cancel.load()) {
            const std::lock_guard lock{s->lock};
            s->groups = std::move(found);
        }
        s->stage = static_cast<int>(Stage::done);
    }}.detach();
}

} // namespace prism::core
