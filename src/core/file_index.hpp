#pragma once

// a name index of whole drives for instant search: a background crawl lists every folder once, a search then scans the
// compact in-memory table (millions of names in well under a second) on its own thread, newest query wins. the index is
// rebuilt on demand (it is a snapshot: files created after the crawl show up after the next one). links are not followed

#include "core/query.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace prism::core {

struct IndexHit {
    std::string   path;
    bool          is_dir{};
    std::uint64_t size{};
    std::uint64_t modified{};  // FILETIME
};

struct SearchResult {
    std::string            text;       // the query this answers
    std::vector<IndexHit>  hits;       // sorted by name, at most `FileIndex::max_hits`
    std::size_t            total{};    // how many entries matched
    double                 milliseconds{};
    std::uint64_t          generation{};
};

class FileIndex {
public:
    static constexpr std::size_t max_hits = 20000;

    explicit FileIndex(std::function<void()> wake) : wake_{std::move(wake)} {}
    ~FileIndex();
    FileIndex(const FileIndex&)            = delete;
    FileIndex& operator=(const FileIndex&) = delete;

    // (re)builds the index of these folders (drive roots, usually) in the background; the old one stays searchable meanwhile
    void start(std::vector<std::string> roots);
    [[nodiscard]] bool          building() const noexcept { return building_.load(); }
    [[nodiscard]] std::uint64_t scanned() const noexcept { return scanned_.load(); }   // entries seen by the running crawl
    [[nodiscard]] std::uint64_t entries() const;                                       // entries in the searchable index
    [[nodiscard]] bool          ready() const { return entries() > 0; }
    // seconds since the searchable index was finished (a very large number when there is none)
    [[nodiscard]] double        age_seconds() const;

    // starts a search on the worker; a newer one replaces it. an empty query finds nothing
    void search(Query query);
    [[nodiscard]] std::shared_ptr<const SearchResult> result() const;
    // true while the worker is still on the latest query
    [[nodiscard]] bool          searching() const noexcept { return searched_gen_.load() != asked_gen_.load(); }

private:
    struct Node {
        std::uint32_t parent{};     // npos for a root
        std::uint32_t name_off{};
        std::uint16_t name_len{};
        std::uint16_t ext_off{};    // within the name; == name_len: none
        std::uint8_t  flags{};      // bit 0 directory, 1 hidden, 2 system, 3 read-only
        std::uint8_t  kind{};       // FileKind
        std::uint64_t size{};
        std::uint64_t modified{};
    };
    struct Data {
        std::vector<Node> nodes;
        std::string       pool;     // the names, back to back
        std::chrono::steady_clock::time_point built;
    };
    static constexpr std::uint32_t npos = 0xffffffffu;

    void crawl(std::vector<std::string> roots, std::uint64_t generation);
    void worker();
    [[nodiscard]] std::shared_ptr<const Data> data() const;
    static void path_of(const Data& d, std::uint32_t id, std::string& out);

    std::function<void()>        wake_;
    mutable std::mutex           lock_;
    std::shared_ptr<const Data>  data_;
    std::atomic<bool>            building_{};
    std::atomic<std::uint64_t>   scanned_{};
    std::atomic<std::uint64_t>   crawl_gen_{};
    std::thread                  crawler_;

    std::condition_variable      cv_;
    std::optional<Query>         pending_;
    std::atomic<std::uint64_t>   asked_gen_{};
    std::atomic<std::uint64_t>   searched_gen_{};
    std::shared_ptr<const SearchResult> result_;
    std::atomic<bool>            quit_{};
    std::thread                  searcher_;
};

} // namespace prism::core
