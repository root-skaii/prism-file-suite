#pragma once

// the search / filter language, parsed once into a matcher. the live filter of a folder and the recursive search run the
// same Query; matches() is const and allocation-free for the common terms, so a worker thread can run its own copy.
//
//   words are ANDed:   report 2026        "exact phrase"       -draft (not)       a | b  or  a OR b (either)
//   wildcards:         *.cpp  report?.txt (match the whole name)       .cpp  (name ends with)       sub\name (path)
//   filters:           ext:cpp,h  type:image  kind:folder  size:>10mb  size:1kb..1mb  modified:today  hidden:yes  tag:red,blue  tag:any  tag:none  re:^IMG_\d+
//
// a query never fails to build: an invalid one reports error() and matches everything (the ui shows the error)

#include "core/file_entry.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::core {

struct QueryOptions {
    bool case_sensitive{};
    bool regex{};                          // plain words are regular expressions (`|` is then part of the pattern)
    std::uint64_t now{};                   // FILETIME ticks for modified: today / week ...; 0 = the system clock
    std::optional<int> utc_offset_minutes; // time zone for dates; unset = the system's
};

struct QueryRegex; // (query.cpp: a small backtracking-free matcher, std::regex needs exceptions)

// one compiled condition; the fields in use depend on `kind`
struct QueryTerm {
    enum class Kind : std::uint8_t { substring, glob, suffix, extension, type, folder, size, modified, hidden, tag, regex };
    Kind          kind{Kind::substring};
    bool          negate{};
    bool          on_path{};   // test the whole relative path, not the last component
    bool          fold{true};  // ascii case-insensitive; `text` and `list` are already folded
    bool          flag{};      // folder: wants a folder; hidden: wants hidden; tag: wants no label at all
    std::string   text;        // substring / glob / suffix
    std::vector<std::string> list; // extensions
    std::uint32_t mask{};      // FileKind bits; tag: label bits (any of them)
    std::uint64_t lo{};        // size or FILETIME range, inclusive
    std::uint64_t hi{~std::uint64_t{0}};
    std::shared_ptr<const QueryRegex> re;
};

class Query {
public:
    Query() = default; // matches everything

    [[nodiscard]] static Query parse(std::string_view text, const QueryOptions& options = {});

    [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    // no terms (or invalid): every entry matches
    [[nodiscard]] bool empty() const noexcept { return groups_.empty(); }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    // `hidden:yes` asks for hidden entries: the listing must not hide them first
    [[nodiscard]] bool includes_hidden() const noexcept { return wants_hidden_; }
    // `tag:red` reads FileEntry::tags: the listing must refresh them before it filters
    [[nodiscard]] bool uses_tags() const noexcept { return uses_tags_; }
    // a word with a backslash tests the whole path, so the caller must put the full path in FileEntry::name
    [[nodiscard]] bool uses_path() const noexcept { return uses_path_; }

    // `name` of a recursive result is a relative path; wildcards and words test the last component
    [[nodiscard]] bool matches(const FileEntryView& e) const noexcept;
    [[nodiscard]] bool matches(const FileEntry& e) const noexcept { return matches(e.view()); }

private:
    std::string                          text_;
    std::string                          error_;
    std::vector<std::vector<QueryTerm>>  groups_; // OR of ANDs
    bool                                 wants_hidden_{};
    bool                                 uses_tags_{};
    bool                                 uses_path_{};
};

} // namespace prism::core
