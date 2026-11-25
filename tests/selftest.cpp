// prismfs_selftest: logic checks for core/ and model/ (no window, no gpu). exit code 0 = all passed

#include "core/archive.hpp"
#include "core/batch_rename.hpp"
#include "core/directory_loader.hpp"
#include "core/compare.hpp"
#include "core/disk_usage.hpp"
#include "core/duplicates.hpp"
#include "core/empty_folders.hpp"
#include "core/file_index.hpp"
#include "core/file_info.hpp"
#include "core/rclone.hpp"
#include "core/i18n.hpp"
#include "core/treemap.hpp"
#include "core/folder_sizes.hpp"
#include "core/image_tools.hpp"
#include "core/links.hpp"
#include "core/git_status.hpp"
#include "core/process.hpp"
#include "core/recycle.hpp"
#include "core/transfer.hpp"
#include "core/templates.hpp"
#include "core/file_ops.hpp"
#include "core/path.hpp"
#include "core/path_probe.hpp"
#include "core/default_manager.hpp"
#include "core/drive_watcher.hpp"
#include "core/shell_menu.hpp"
#include "core/shell_ops.hpp"
#include "core/syntax.hpp"
#include "core/terminal.hpp"
#include "core/text.hpp"
#include "core/virtual_files.hpp"
#include "model/folder_sorts.hpp"
#include "model/listing.hpp"
#include "model/sessions.hpp"
#include "model/sidebar_layout.hpp"
#include "model/tags.hpp"
#include "model/visit_log.hpp"
#include "strata/config.hpp"
#include "model/tab.hpp"
#include "model/tab_set.hpp"
#include "model/undo_stack.hpp"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#else
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <thread>

using namespace prism;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

#ifdef _WIN32

static void write_file(const std::string& p, const char* data)
{
    std::ofstream f{core::text::widen(p), std::ios::binary};
    f << data;
}

static void test_text()
{
    CHECK(core::text::compare_natural("file2", "file10") < 0);
    CHECK(core::text::compare_natural("File", "file") == 0);
    CHECK(core::text::compare_natural("a01", "a1") != 0); // total order
    CHECK(core::text::narrow(core::text::widen("h\xc3\xa9llo")) == "h\xc3\xa9llo");
}

static void test_path()
{
    CHECK(core::path::parent("C:\\a\\b") == "C:\\a");
    CHECK(core::path::parent("C:\\a") == "C:\\");
    CHECK(core::path::parent("C:\\").empty());
    CHECK(core::path::leaf("C:\\a\\b.txt") == "b.txt");
    CHECK(core::path::normalize("C:/a/b/") == "C:\\a\\b");
    CHECK(core::path::normalize("C:") == "C:\\");
    const auto segs = core::path::segments("C:\\Users\\me");
    CHECK(segs.size() == 3 && segs[2].path == "C:\\Users\\me" && segs[0].name == "C:\\");
}

static void fill(model::Listing& L)
{
    std::vector<core::FileEntry> v;
    const auto add = [&](const char* n, std::uint64_t size, bool dir) {
        core::FileEntry e;
        e.name = n;
        e.size = size;
        e.attributes = dir ? core::attr::directory : 0;
        e.kind = dir ? core::FileKind::folder : core::FileKind::other;
        e.ext_offset = static_cast<std::uint16_t>(e.name.size());
        v.push_back(std::move(e));
    };
    add("b.txt", 5, false);
    add("a10", 0, true);
    add("a2", 0, true);
    add("c.txt", 50, false);
    L.clear();
    L.append(std::move(v));
    L.finish();
}

static void test_listing()
{
    model::Listing L;
    fill(L);
    CHECK(L.rows() == 4);
    CHECK(L.at(0).name == "a2" && L.at(1).name == "a10"); // folders first, natural order
    CHECK(L.at(2).name == "b.txt");

    L.set_sort({model::SortKey::size, false});
    CHECK(L.at(2).name == "c.txt"); // biggest file first (folders stay on top)

    L.select_range(2, 3, false);
    CHECK(L.selected_count() == 2 && L.selected_bytes() == 55);
    L.set_sort({model::SortKey::name, true}); // selection survives re-sorting
    CHECK(L.selected_count() == 2 && L.selected(2) && L.selected(3));

    L.set_query(core::Query::parse("c."));
    CHECK(L.rows() == 1 && L.at(0).name == "c.txt");
    L.set_query(core::Query::parse("*.txt kind:file"));
    CHECK(L.rows() == 2);
    L.set_query(core::Query::parse("re:(oops"));
    CHECK(!L.query().valid() && L.rows() == 4); // an invalid query filters nothing
    L.set_query({});
    CHECK(L.rows() == 4);
    CHECK(L.find_prefix("B", 0).value_or(99) == 2);
    L.select_all();
    CHECK(L.selected_count() == 4);
    L.clear_selection();
    CHECK(L.selected_count() == 0);
}

static void test_grouping()
{
    model::Listing L;
    fill(L); // a10, a2 (folders), b.txt (5 B), c.txt (50 B)
    L.set_group(model::GroupBy::type);
    CHECK(L.group_starts().size() == 2 && L.group_starts()[0] == 0 && L.group_starts()[1] == 2);
    CHECK(L.group_title(0) == "Folders" && L.group_title(3) == "Other files");
    L.set_group(model::GroupBy::size);
    CHECK(L.group_starts().size() == 2 && L.group_title(2) == "Tiny (under 16 KB)"); // folders, then both files
    L.set_group(model::GroupBy::name);
    CHECK(L.group_title(0) == "A" && L.group_starts().size() == 3); // a, b, c
    L.set_sort({model::SortKey::name, false});
    CHECK(L.group_title(0) == "C"); // descending by name turns the letters round
    L.set_group(model::GroupBy::none);
    CHECK(L.group_starts().empty());
}

static void test_tags()
{
    model::Tags T;
    CHECK(!T.any() && T.get("C:\\a\\b.txt") == 0);
    CHECK(T.set("C:\\A\\b.txt", 0x5) && !T.set("c:/a/b.txt", 0x5)); // same item whatever the spelling: no change
    CHECK(T.get("c:\\a\\B.TXT") == 0x5);
    CHECK(T.set("C:\\a\\sub\\x", 0x2) && T.set("C:\\ab\\y", 0x1));
    T.rename_prefix("C:\\a", "D:\\z"); // an item and what is below it follow; "C:\\ab" is a different folder
    CHECK(T.get("D:\\z\\b.txt") == 0x5 && T.get("D:\\z\\sub\\x") == 0x2 && T.get("C:\\a\\b.txt") == 0 && T.get("C:\\ab\\y") == 0x1);
    T.remove_prefix("D:\\z");
    CHECK(T.size() == 1);
    CHECK(T.set("C:\\ab\\y", 0) && !T.any());

    CHECK(T.set("C:\\k", 0x81));
    const std::string file = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_tags_test.ini";
    CHECK(T.save_file(file));
    model::Tags U;
    CHECK(U.load_file(file) && U.get("C:\\k") == 0x81);
    ::DeleteFileW(core::text::widen(file).c_str());

    // the filter: tag:red,blue / tag:any / tag:none read FileEntry::tags
    core::FileEntry e;
    e.name = "f.txt";
    const auto hit = [&](const char* q) { return core::Query::parse(q).matches(e); };
    e.tags = 0;
    CHECK(hit("tag:none") && !hit("tag:any") && !hit("tag:red"));
    e.tags = 1u << 5; // blue
    CHECK(hit("tag:blue") && hit("tag:red,blue") && hit("tag:any") && !hit("tag:none") && !hit("tag:red") && hit("-tag:red"));
    CHECK(!core::Query::parse("tag:mauve").valid() && core::Query::parse("tag:blue").uses_tags());

    model::Listing L;
    fill(L); // b.txt, a10, a2, c.txt
    L.set_query(core::Query::parse("tag:red"));
    CHECK(L.rows() == 0);
    L.sync_tags(1, [](const core::FileEntry& en) { return static_cast<std::uint16_t>(en.name == "c.txt" ? 1 : 0); });
    CHECK(L.rows() == 1 && L.at(0).name == "c.txt"); // a `tag:` filter follows the labels
    L.sync_tags(1, [](const core::FileEntry&) { return std::uint16_t{0xff}; }); // same token: not asked again
    CHECK(L.rows() == 1);
    L.sync_tags(2, {});
    CHECK(L.rows() == 0);
}

static void test_treemap_and_usage()
{
    const std::vector<double> v{60, 25, 10, 3, 2};
    const auto tiles = core::squarify(v, 10.0f, 20.0f, 200.0f, 100.0f);
    CHECK(tiles.size() == v.size());
    double area = 0.0;
    bool inside = true;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        area += static_cast<double>(tiles[i].w) * tiles[i].h;
        inside = inside && tiles[i].x >= 9.99f && tiles[i].y >= 19.99f && tiles[i].x + tiles[i].w <= 210.01f && tiles[i].y + tiles[i].h <= 120.01f;
        CHECK(std::abs(static_cast<double>(tiles[i].w) * tiles[i].h - 200.0 * 100.0 * v[i] / 100.0) < 1.0); // area follows the value
    }
    CHECK(inside && std::abs(area - 20000.0) < 1.0);
    CHECK(core::squarify({}, 0, 0, 10, 10).empty() && core::squarify({0, 0}, 0, 0, 10, 10)[0].w == 0.0f);

    // a real scan: two files in a folder, one in a subfolder
    const std::string root = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_usage_test";
    ::CreateDirectoryW(core::text::widen(root).c_str(), nullptr);
    ::CreateDirectoryW(core::text::widen(root + "\\sub").c_str(), nullptr);
    write_file(root + "\\a.bin", "0123456789");
    write_file(root + "\\b.bin", "01234");
    write_file(root + "\\sub\\c.bin", "0123456789012345678901234");
    core::DiskUsage usage;
    usage.start(root);
    for (int i = 0; i < 400 && !usage.result(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const auto tree = usage.result();
    CHECK(tree && tree->bytes == 40 && tree->files == 3);
    if (tree) {
        CHECK(tree->children.size() == 3 && tree->children[0].name == "sub" && tree->children[0].bytes == 25 && tree->children[0].is_dir); // biggest first
        CHECK(tree->children[1].name == "a.bin" && tree->children[2].name == "b.bin");
        CHECK(tree->children[0].children.size() == 1 && tree->children[0].children[0].name == "c.bin");
    }
    ::DeleteFileW(core::text::widen(root + "\\sub\\c.bin").c_str());
    ::DeleteFileW(core::text::widen(root + "\\a.bin").c_str());
    ::DeleteFileW(core::text::widen(root + "\\b.bin").c_str());
    ::RemoveDirectoryW(core::text::widen(root + "\\sub").c_str());
    ::RemoveDirectoryW(core::text::widen(root).c_str());
}

static void test_folder_compare()
{
    const std::string base = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_compare_test";
    const auto mk = [](const std::string& p) { ::CreateDirectoryW(core::text::widen(p).c_str(), nullptr); };
    const auto rm = [](const std::string& p) { ::DeleteFileW(core::text::widen(p).c_str()); };
    mk(base); mk(base + "\\L"); mk(base + "\\R"); mk(base + "\\L\\sub"); mk(base + "\\R\\sub"); mk(base + "\\L\\onlydir");
    write_file(base + "\\L\\same.txt", "abc");      write_file(base + "\\R\\same.txt", "abc");
    write_file(base + "\\L\\left.txt", "x");        // only left
    write_file(base + "\\R\\right.txt", "y");       // only right
    write_file(base + "\\L\\grows.txt", "12345");   write_file(base + "\\R\\grows.txt", "1"); // same second, different size
    write_file(base + "\\L\\sub\\deep.txt", "d");   // only left, inside a folder both sides have
    write_file(base + "\\L\\onlydir\\f.txt", "f");
    // make both "same" files and the "grows" pair share one modified time
    {
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        for (const char* n : {"\\L\\same.txt", "\\R\\same.txt", "\\L\\grows.txt", "\\R\\grows.txt"}) {
            HANDLE h = ::CreateFileW(core::text::widen(base + n).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { ::SetFileTime(h, nullptr, nullptr, &ft); ::CloseHandle(h); }
        }
        // the right "left.txt"-style newer file: touch one file an hour into the future
        write_file(base + "\\L\\newer.txt", "n"); write_file(base + "\\R\\newer.txt", "n");
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        u.QuadPart += 36000000000ull; // +1 h
        FILETIME later{u.LowPart, static_cast<DWORD>(u.HighPart)};
        HANDLE h = ::CreateFileW(core::text::widen(base + "\\R\\newer.txt").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { ::SetFileTime(h, nullptr, nullptr, &later); ::CloseHandle(h); }
    }

    core::FolderCompare cmp;
    cmp.start(base + "\\L", base + "\\R", true);
    for (int i = 0; i < 400 && !cmp.result(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const auto items = cmp.result();
    CHECK(items != nullptr);
    if (items) {
        const auto kind_of = [&](const char* rel) {
            for (const auto& d : *items) { if (d.rel == rel) { return static_cast<int>(d.kind); } }
            return -1;
        };
        using K = core::DiffKind;
        CHECK(kind_of("same.txt") == static_cast<int>(K::same));
        CHECK(kind_of("left.txt") == static_cast<int>(K::only_left));
        CHECK(kind_of("right.txt") == static_cast<int>(K::only_right));
        CHECK(kind_of("grows.txt") == static_cast<int>(K::size_differs));
        CHECK(kind_of("newer.txt") == static_cast<int>(K::right_newer));
        CHECK(kind_of("sub\\deep.txt") == static_cast<int>(K::only_left)); // recursion into a folder both have
        CHECK(kind_of("onlydir") == static_cast<int>(K::only_left) && kind_of("onlydir\\f.txt") == -1); // a one-sided folder is one entry
    }
    cmp.start(base + "\\L", base + "\\R", false);
    for (int i = 0; i < 400 && !cmp.result(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    if (const auto flat = cmp.result()) {
        bool deep = false;
        for (const auto& d : *flat) { deep = deep || d.rel == "sub\\deep.txt"; }
        CHECK(!deep); // not recursive: "sub" is listed, not what is in it
    }
    cmp.cancel();
    for (const char* n : {"\\L\\same.txt", "\\R\\same.txt", "\\L\\left.txt", "\\R\\right.txt", "\\L\\grows.txt", "\\R\\grows.txt", "\\L\\sub\\deep.txt", "\\L\\onlydir\\f.txt", "\\L\\newer.txt", "\\R\\newer.txt"}) { rm(base + n); }
    for (const char* n : {"\\L\\sub", "\\R\\sub", "\\L\\onlydir", "\\L", "\\R", ""}) { ::RemoveDirectoryW(core::text::widen(base + n).c_str()); }
}

static std::string slurp(const std::string& p)
{
    std::ifstream f{core::text::widen(p), std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}

static void rm_rf(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") { continue; }
            const std::wstring full = dir + L"\\" + n;
            ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) { rm_rf(full); } else { ::DeleteFileW(full.c_str()); }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    ::RemoveDirectoryW(dir.c_str());
}

static void test_transfer()
{
    const std::string base = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_transfer_test";
    rm_rf(core::text::widen(base));
    const auto mk = [](const std::string& p) { ::CreateDirectoryW(core::text::widen(p).c_str(), nullptr); };
    const auto at = [&](const char* rel) { return base + "\\" + rel; };
    mk(base); mk(at("src")); mk(at("src\\dir")); mk(at("src\\dir\\sub")); mk(at("dst")); mk(at("other"));
    write_file(at("src\\a.txt"), "AAA");
    write_file(at("src\\dir\\b.txt"), "BBBB");
    write_file(at("src\\dir\\sub\\c.txt"), "C");
    write_file(at("src\\ro.txt"), "read only");
    ::SetFileAttributesW(core::text::widen(at("src\\ro.txt")).c_str(), FILE_ATTRIBUTE_READONLY);

    core::TransferQueue q{[] {}};
    int asked = 0;
    // runs the queue until the job's result arrives, answering every question the same way
    const auto run = [&](core::OpKind kind, std::vector<std::string> sources, const std::string& dest, core::ConflictChoice answer, bool for_all) {
        asked = 0;
        q.add(kind, std::move(sources), dest, false);
        core::OpResult r;
        for (int i = 0; i < 1500; ++i) {
            if (const auto c = q.conflict()) { ++asked; q.resolve(c->serial, answer, for_all); }
            if (q.poll(r)) { return r; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        r.message = "timeout";
        return r;
    };
    using Ch = core::ConflictChoice;

    // 1. a file and a folder tree
    auto r = run(core::OpKind::copy, {at("src\\a.txt"), at("src\\dir"), at("src\\ro.txt")}, at("dst"), Ch::replace, true);
    CHECK(r.ok && !r.aborted && asked == 0);
    CHECK(r.made.size() == 3 && r.made[0] == at("dst\\a.txt") && r.made[1] == at("dst\\dir") && r.made[2] == at("dst\\ro.txt"));
    CHECK(slurp(at("dst\\a.txt")) == "AAA" && slurp(at("dst\\dir\\b.txt")) == "BBBB" && slurp(at("dst\\dir\\sub\\c.txt")) == "C");
    CHECK((::GetFileAttributesW(core::text::widen(at("dst\\ro.txt")).c_str()) & FILE_ATTRIBUTE_READONLY) != 0); // attributes come along

    // 2. everything exists now: replace for all asks once
    write_file(at("src\\a.txt"), "NEW-A");
    write_file(at("src\\dir\\b.txt"), "NEW-B");
    r = run(core::OpKind::copy, {at("src\\a.txt"), at("src\\dir"), at("src\\ro.txt")}, at("dst"), Ch::replace, true);
    CHECK(r.ok && asked == 1 && slurp(at("dst\\a.txt")) == "NEW-A" && slurp(at("dst\\dir\\b.txt")) == "NEW-B"); // (a read-only target is replaced too)

    // 3. skip: asked per file, nothing changes
    write_file(at("src\\a.txt"), "OTHER");
    r = run(core::OpKind::copy, {at("src\\a.txt")}, at("dst"), Ch::skip, false);
    CHECK(r.ok && asked == 1 && slurp(at("dst\\a.txt")) == "NEW-A" && r.made.size() == 1 && r.made[0].empty()); // skipped: nothing to undo

    // 4. keep both
    r = run(core::OpKind::copy, {at("src\\a.txt")}, at("dst"), Ch::keep_both, false);
    CHECK(r.ok && r.made[0] == at("dst\\a (2).txt") && slurp(at("dst\\a (2).txt")) == "OTHER" && slurp(at("dst\\a.txt")) == "NEW-A");

    // 5. a copy into its own folder is "name - Copy"
    r = run(core::OpKind::copy, {at("src\\a.txt")}, at("src"), Ch::cancel, false);
    CHECK(r.ok && asked == 0 && r.made[0] == at("src\\a - Copy.txt") && slurp(at("src\\a - Copy.txt")) == "OTHER");

    // 6. a move on the same volume renames the whole folder
    r = run(core::OpKind::move, {at("dst\\dir")}, at("other"), Ch::cancel, false);
    CHECK(r.ok && r.made[0] == at("other\\dir") && !core::path::exists(at("dst\\dir")) && slurp(at("other\\dir\\sub\\c.txt")) == "C");

    // 7. a move onto a folder with the same name merges, asks about the files that clash and tidies up what it emptied
    mk(at("dst\\dir"));
    write_file(at("dst\\dir\\b.txt"), "OLD");
    write_file(at("dst\\dir\\keep.txt"), "kept");
    r = run(core::OpKind::move, {at("src\\dir")}, at("dst"), Ch::replace, true);
    CHECK(r.ok && asked == 1 && slurp(at("dst\\dir\\b.txt")) == "NEW-B" && slurp(at("dst\\dir\\keep.txt")) == "kept" && slurp(at("dst\\dir\\sub\\c.txt")) == "C");
    CHECK(!core::path::exists(at("src\\dir")));

    // 8. stopping at a question cancels the job
    r = run(core::OpKind::copy, {at("src\\a.txt")}, at("dst"), Ch::cancel, false);
    CHECK(r.aborted && !r.ok && slurp(at("dst\\a.txt")) == "NEW-A");

    // 9. a folder cannot go inside itself
    r = run(core::OpKind::copy, {at("dst")}, at("dst\\dir"), Ch::cancel, false);
    CHECK(!r.ok && !r.aborted && !r.message.empty() && !core::path::exists(at("dst\\dir\\dst")));

    // 10. a missing destination
    r = run(core::OpKind::copy, {at("src\\a.txt")}, at("nowhere"), Ch::cancel, false);
    CHECK(!r.ok && !r.message.empty());

    // 11. cancel() while queued: the job that never started reports itself aborted
    q.add(core::OpKind::copy, {at("src\\a.txt")}, at("other"), false);
    q.cancel();
    core::OpResult first;
    bool got = false;
    for (int i = 0; i < 300 && !got; ++i) { got = q.poll(first); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(got); // (either it ran before the cancel landed, or it was cancelled: both finish with a result)

    rm_rf(core::text::widen(base));
}

static void test_file_index()
{
    const std::string base = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_index_test";
    rm_rf(core::text::widen(base));
    const auto mk = [](const std::string& p) { ::CreateDirectoryW(core::text::widen(p).c_str(), nullptr); };
    mk(base); mk(base + "\\one"); mk(base + "\\one\\deep");
    write_file(base + "\\alpha_report.txt", "1");
    write_file(base + "\\one\\Beta Report.pdf", "22");
    write_file(base + "\\one\\deep\\notes.md", "333");

    core::FileIndex idx{[] {}};
    CHECK(!idx.ready() && idx.age_seconds() > 1e9);
    idx.start({base});
    for (int i = 0; i < 500 && (idx.building() || !idx.ready()); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(!idx.building() && idx.entries() == 6 && idx.age_seconds() < 60.0); // the root, two folders, three files

    // asks and waits for the answer to that very query
    const auto ask = [&](const char* q) {
        idx.search(core::Query::parse(q));
        std::shared_ptr<const core::SearchResult> r;
        for (int i = 0; i < 500; ++i) {
            r = idx.result();
            if (r && r->text == q && !idx.searching()) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return r;
    };
    auto r = ask("report");
    CHECK(r && r->total == 2 && r->hits.size() == 2 && r->hits[0].path == base + "\\alpha_report.txt" && r->hits[1].path == base + "\\one\\Beta Report.pdf");
    CHECK(r && !r->hits[0].is_dir && r->hits[0].size == 1 && r->hits[1].size == 2);
    r = ask("ext:md");
    CHECK(r && r->hits.size() == 1 && r->hits[0].path == base + "\\one\\deep\\notes.md");
    r = ask("one\\deep"); // a word with a backslash is a path: the folder and what is in it
    CHECK(r && r->hits.size() == 2);
    r = ask("kind:folder one");
    CHECK(r && r->hits.size() == 1 && r->hits[0].is_dir && r->hits[0].path == base + "\\one");
    r = ask("size:>1");
    CHECK(r && r->hits.size() == 2); // the 2 and 3 byte files (folders have no size)
    r = ask("type:image");
    CHECK(r && r->hits.empty());
    r = ask("");
    CHECK(r && r->hits.empty() && r->total == 0); // nothing typed: nothing listed
    for (int i = 0; i < 30; ++i) { idx.search(core::Query::parse("notes" + std::string(static_cast<std::size_t>(i % 3), 'x'))); }
    r = ask("notes");
    CHECK(r && r->hits.size() == 1); // the newest query wins

    // a rebuild keeps the old data searchable until the new one is ready
    idx.start({base});
    CHECK(idx.ready());
    for (int i = 0; i < 500 && idx.building(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(idx.entries() == 6);
    rm_rf(core::text::widen(base));
}

static void test_image_tools()
{
    CHECK(core::is_image_name("a.JPG") && core::is_image_name("b.png") && !core::is_image_name("c.txt") && !core::is_image_name("jpg"));
    const std::string base = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_image_test";
    rm_rf(core::text::widen(base));
    ::CreateDirectoryW(core::text::widen(base).c_str(), nullptr);

    // a 64 x 32 24-bit bitmap, written by hand: a 54-byte header and 32 rows of 192 bytes
    std::string bmp(54 + 64 * 32 * 3, '\0');
    const auto put32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) { bmp[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xff); } };
    bmp[0] = 'B'; bmp[1] = 'M';
    put32(2, static_cast<std::uint32_t>(bmp.size())); put32(10, 54); put32(14, 40); put32(18, 64); put32(22, 32);
    bmp[26] = 1; bmp[28] = 24; put32(34, 64 * 32 * 3);
    for (std::size_t i = 54; i < bmp.size(); i += 3) { bmp[i] = static_cast<char>(i % 251); bmp[i + 1] = static_cast<char>(i % 13); bmp[i + 2] = static_cast<char>(200); }
    {
        std::ofstream f{core::text::widen(base + "\\pic.bmp"), std::ios::binary};
        f.write(bmp.data(), static_cast<std::streamsize>(bmp.size()));
    }

    const auto half = core::transform_image(base + "\\pic.bmp", core::ImageOp::half);
    CHECK(half.ok && half.path == base + "\\pic - 50%.bmp" && core::path::exists(half.path));
    const auto png = core::transform_image(half.path, core::ImageOp::to_png);
    CHECK(png.ok && png.path == base + "\\pic - 50%.png" && slurp(png.path).substr(1, 3) == "PNG"); // a real PNG, made from the half-size copy
    const auto jpg = core::transform_image(base + "\\pic.bmp", core::ImageOp::to_jpeg);
    CHECK(jpg.ok && jpg.path == base + "\\pic.jpg" && static_cast<unsigned char>(slurp(jpg.path)[0]) == 0xff);
    const auto turned = core::transform_image(jpg.path, core::ImageOp::rotate_right);
    CHECK(turned.ok && turned.path == base + "\\pic - rotated right.jpg");
    const auto again = core::transform_image(jpg.path, core::ImageOp::to_jpeg);
    CHECK(!again.ok && !again.error.empty()); // already a JPEG
    const auto bad = core::transform_image(base + "\\nothing.png", core::ImageOp::half);
    CHECK(!bad.ok && !bad.error.empty());
    const auto twice = core::transform_image(base + "\\pic.bmp", core::ImageOp::half);
    CHECK(twice.ok && twice.path == base + "\\pic - 50% (2).bmp"); // never overwrites

    std::atomic<int> woke{0};
    core::ImageJobs jobs{[&] { ++woke; }};
    jobs.start(core::ImageOp::quarter, {base + "\\pic.bmp", base + "\\missing.bmp"});
    core::ImageJobs::Result jr;
    bool got = false;
    for (int i = 0; i < 300 && !got; ++i) { got = jobs.poll(jr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(got && jr.ok && jr.done == 1 && jr.dir == base && jr.made == "pic - 25%.bmp" && jr.message.find("1 of 2") != std::string::npos);
    rm_rf(core::text::widen(base));
}

static void test_empty_folders()
{
    const std::string base = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_empty_test";
    rm_rf(core::text::widen(base));
    const auto mk = [&](const char* rel) { ::CreateDirectoryW(core::text::widen(base + (rel[0] ? std::string{"\\"} + rel : std::string{})).c_str(), nullptr); };
    mk(""); mk("empty"); mk("tree"); mk("tree\\a"); mk("tree\\a\\b"); mk("tree\\c");
    mk("mixed"); mk("mixed\\hasfile"); mk("mixed\\hollow"); mk("mixed\\hollow\\deeper");
    write_file(base + "\\mixed\\hasfile\\f.txt", "x");
    write_file(base + "\\root.txt", "x");

    core::EmptyFolderScan scan;
    scan.start(base);
    for (int i = 0; i < 300 && !scan.done(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(scan.done());
    const auto found = scan.result();
    // the outermost empty ones: "empty", the whole "tree", and "hollow" (with what is inside it); not "hasfile", not "mixed"
    CHECK(found.size() == 3 && found[0] == base + "\\empty" && found[1] == base + "\\mixed\\hollow" && found[2] == base + "\\tree");
    rm_rf(core::text::widen(base));
}

static void test_links()
{
    const std::string base = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_links_test";
    rm_rf(core::text::widen(base));
    ::CreateDirectoryW(core::text::widen(base).c_str(), nullptr);
    ::CreateDirectoryW(core::text::widen(base + "\\folder").c_str(), nullptr);
    write_file(base + "\\folder\\inside.txt", "inside");
    write_file(base + "\\file.txt", "content");

    // a junction shows what is in its target; removing it leaves the target alone
    CHECK(core::make_link(core::LinkKind::junction, base + "\\folder", true, base + "\\jn").empty());
    CHECK(slurp(base + "\\jn\\inside.txt") == "inside");
    CHECK(!core::make_link(core::LinkKind::junction, base + "\\folder", true, base + "\\jn").empty()); // already there
    CHECK(!core::make_link(core::LinkKind::junction, base + "\\file.txt", false, base + "\\jn2").empty()); // a file is no junction target
    CHECK(core::remove_link(base + "\\jn") && !core::path::exists(base + "\\jn") && slurp(base + "\\folder\\inside.txt") == "inside");
    CHECK(!core::remove_link(base + "\\folder") && core::path::exists(base + "\\folder")); // a real folder is never removed

    // a hard link is the same file under another name
    CHECK(core::make_link(core::LinkKind::hard, base + "\\file.txt", false, base + "\\hard.txt").empty());
    CHECK(slurp(base + "\\hard.txt") == "content");
    write_file(base + "\\file.txt", "changed");
    CHECK(slurp(base + "\\hard.txt") == "changed");
    CHECK(!core::make_link(core::LinkKind::hard, base + "\\folder", true, base + "\\hardf").empty()); // not for folders

    // a symbolic link needs a privilege: it either works or says why
    const std::string sym = core::make_link(core::LinkKind::symbolic, base + "\\file.txt", false, base + "\\sym.txt");
    CHECK(sym.empty() || sym.find("Developer Mode") != std::string::npos);
    if (sym.empty()) { CHECK(slurp(base + "\\sym.txt") == "changed" && core::remove_link(base + "\\sym.txt")); }
    rm_rf(core::text::widen(base));
}

static void test_sessions()
{
    model::Sessions S;
    CHECK(!S.save({"", {"C:\\a"}, 0, ""}) && !S.save({"empty", {}, 0, ""}));
    CHECK(S.save({"Work", {"C:\\a", "C:\\b"}, 5, "C:\\c"}));
    CHECK(S.find("work")->active == 1); // clamped, name is case-insensitive
    CHECK(S.save({"WORK", {"C:\\z"}, 0, ""}) && S.all().size() == 1 && S.find("Work")->tabs.size() == 1);

    CHECK(S.save({"Other", {"D:\\x", "D:\\y"}, 1, "D:\\r"}));
    const std::string file = core::text::narrow(std::wstring{_wgetenv(L"TEMP")}) + "\\prismfs_sessions_test.ini";
    CHECK(S.save_file(file));
    model::Sessions T;
    CHECK(T.load_file(file) && T.all().size() == 2);
    CHECK(T.find("Other") && T.find("Other")->right == "D:\\r" && T.find("Other")->tabs[1] == "D:\\y" && T.find("Other")->active == 1);
    CHECK(T.remove("work") && !T.remove("work") && T.all().size() == 1);
    ::DeleteFileW(core::text::widen(file).c_str());
}

// --- query language -----------------------------------------------------------------------------------------------

static std::uint64_t filetime(int y, unsigned m, unsigned d, int hour = 12)
{
    using namespace std::chrono;
    const sys_days date{year{y} / month{m} / std::chrono::day{d}};
    const std::int64_t days = date.time_since_epoch().count() + 134774; // 1601 -> 1970
    return static_cast<std::uint64_t>(days) * 864'000'000'000ULL + static_cast<std::uint64_t>(hour) * 36'000'000'000ULL;
}

static core::FileEntry entry(const char* name, std::uint64_t size = 0, core::FileKind kind = core::FileKind::other, std::uint64_t modified = 0, std::uint32_t attrs = 0)
{
    core::FileEntry e;
    e.name       = name;
    e.size       = size;
    e.modified   = modified;
    e.attributes = attrs;
    e.kind       = kind;
    const std::size_t dot = e.name.rfind('.');
    const std::size_t slash = e.name.rfind('\\');
    const std::size_t base = slash == std::string::npos ? 0 : slash + 1;
    const bool has_ext = dot != std::string::npos && dot > base && dot + 1 < e.name.size(); // (".gitignore" is a name)
    e.ext_offset = static_cast<std::uint16_t>(has_ext ? dot + 1 : e.name.size());
    if (kind == core::FileKind::folder) { e.attributes |= core::attr::directory; e.ext_offset = static_cast<std::uint16_t>(e.name.size()); }
    return e;
}

static bool hit(const char* query, const core::FileEntry& e, core::QueryOptions o = {})
{
    return core::Query::parse(query, o).matches(e);
}

static void test_query()
{
    using core::FileKind;
    core::QueryOptions at;
    at.now                = filetime(2026, 10, 19);
    at.utc_offset_minutes = 0;

    // words, phrases, case
    CHECK(core::Query::parse("").empty() && core::Query::parse("   ").valid());
    CHECK(hit("", entry("anything")));
    CHECK(hit("rep 2026", entry("report_2026.txt")) && !hit("rep 2026", entry("report_2025.txt")));
    CHECK(hit("REPORT", entry("annual report.txt")));
    core::QueryOptions cs;
    cs.case_sensitive = true;
    CHECK(!hit("REPORT", entry("report.txt"), cs) && hit("report", entry("report.txt"), cs));
    CHECK(hit("\"my file\"", entry("my file.txt")) && !hit("\"my file\"", entry("my_file.txt")));
    CHECK(hit("\"a*b\"", entry("xa*by")) && !hit("\"a*b\"", entry("xaZby"))); // quoted: literal, no wildcards
    CHECK(hit("caf\xc3\xa9", entry("caf\xc3\xa9.txt")));

    // wildcards match the whole name
    CHECK(hit("*.cpp", entry("main.cpp")) && !hit("*.cpp", entry("main.cpp.bak")) && !hit("*.cpp", entry("main.hpp")));
    CHECK(hit("*.CPP", entry("main.cpp")));
    CHECK(hit("report?.txt", entry("report1.txt")) && !hit("report?.txt", entry("report10.txt")));
    CHECK(hit("ma*", entry("main.cpp")) && !hit("ma*", entry("domain.cpp")));
    CHECK(hit("*a*b*c*", entry("xxaxxbxxcxx")) && !hit("*a*b*c*", entry("xxcxxbxxaxx")));
    CHECK(hit("caf?.txt", entry("caf\xc3\xa9.txt"))); // ? is one character, not one byte
    CHECK(hit("*.txt", entry("sub\\deep\\a.txt"))); // results carry relative paths: the last component is tested
    CHECK(!hit("deep", entry("deep\\x.txt")) && hit("deep", entry("deep", 0, FileKind::folder)));
    CHECK(hit("sub\\*.h", entry("sub\\x.h")) && hit("sub/x", entry("sub\\x.h")) && !hit("sub\\*.h", entry("x.h")));

    // extension shorthand
    CHECK(hit(".cpp", entry("a.cpp")) && !hit(".cpp", entry("a.cpps")) && hit(".gitignore", entry(".gitignore")));
    CHECK(hit("ext:cpp,h", entry("a.h")) && hit("ext:.CPP", entry("a.cpp")) && !hit("ext:cpp,h", entry("a.c")));
    CHECK(!hit("ext:cpp", entry("cpp", 0, FileKind::folder)));

    // not / or
    CHECK(!hit("report -draft", entry("report_draft.txt")) && hit("report -draft", entry("report_final.txt")));
    CHECK(hit("-ext:obj", entry("a.cpp")) && !hit("-ext:obj", entry("a.obj")));
    CHECK(hit("cat | dog", entry("dog.png")) && hit("cat OR dog", entry("cat.png")) && !hit("cat | dog", entry("cow.png")));
    CHECK(hit("a b | c", entry("c")) && hit("a b | c", entry("ab")) && !hit("a b | c", entry("a")));
    CHECK(hit("\"-x\"", entry("-x.txt")));

    // type / kind
    CHECK(hit("type:image", entry("a.png", 1, FileKind::image)) && !hit("type:image", entry("a.txt", 1, FileKind::text)));
    CHECK(hit("type:image,video", entry("a.mp4", 1, FileKind::video)));
    CHECK(hit("type:doc", entry("a.pdf", 1, FileKind::pdf)) && hit("type:docs", entry("a.xlsx", 1, FileKind::spreadsheet)));
    CHECK(hit("type:folder", entry("d", 0, FileKind::folder)));
    CHECK(hit("kind:folder", entry("d", 0, FileKind::folder)) && !hit("kind:folder", entry("d.txt")) && hit("kind:file", entry("d.txt")));
    CHECK(!core::Query::parse("type:nonsense").valid() && hit("type:nonsense", entry("x")));

    // size
    const std::uint64_t mb = 1048576;
    CHECK(hit("size:>10mb", entry("a", 11 * mb)) && !hit("size:>10mb", entry("a", 10 * mb)) && hit("size:>=10mb", entry("a", 10 * mb)));
    CHECK(hit("size:1kb..1mb", entry("a", 5000)) && !hit("size:1kb..1mb", entry("a", 10)) && !hit("size:1kb..1mb", entry("a", 2 * mb)));
    CHECK(hit("size:..1kb", entry("a", 100)) && hit("size:1mb..", entry("a", 2 * mb)));
    CHECK(hit("size:<500", entry("a", 499)) && !hit("size:<500", entry("a", 500)) && hit("size:1.5kb", entry("a", 1536)));
    CHECK(!hit("size:>1", entry("folder", 0, FileKind::folder)) && !hit("size:<1mb", entry("folder", 0, FileKind::folder)));
    CHECK(!core::Query::parse("size:big").valid() && core::Query::parse("size:").valid());

    // modified (now = 2026-10-19 12:00 utc)
    const auto at_day = [&](unsigned y, unsigned m, unsigned d, int h = 12) { return entry("f", 1, FileKind::other, filetime(static_cast<int>(y), m, d, h)); };
    CHECK(hit("modified:today", at_day(2026, 10, 19, 1), at) && !hit("modified:today", at_day(2026, 10, 18, 23), at));
    CHECK(hit("modified:yesterday", at_day(2026, 10, 18, 23), at) && !hit("modified:yesterday", at_day(2026, 10, 19, 1), at));
    CHECK(hit("modified:week", at_day(2026, 10, 14), at) && !hit("modified:week", at_day(2026, 10, 10), at));
    CHECK(hit("modified:3d", at_day(2026, 10, 17), at) && !hit("modified:3d", at_day(2026, 10, 15), at));
    CHECK(hit("modified:2026-10", at_day(2026, 10, 1, 0), at) && hit("modified:2026-10", at_day(2026, 10, 31, 23), at) && !hit("modified:2026-10", at_day(2026, 11, 1, 0), at));
    CHECK(hit("modified:2026", at_day(2026, 1, 1, 0), at) && !hit("modified:2026", at_day(2025, 12, 31, 23), at));
    CHECK(hit("modified:>2026-01-01", at_day(2026, 3, 1), at) && !hit("modified:>2026-01-01", at_day(2026, 1, 1), at));
    CHECK(hit("modified:>=2026-01-01", at_day(2026, 1, 1, 0), at) && hit("modified:<2026-01-01", at_day(2025, 12, 31), at));
    CHECK(hit("modified:2026-01-01..2026-03-31", at_day(2026, 3, 31, 23), at) && !hit("modified:2026-01-01..2026-03-31", at_day(2026, 4, 1, 0), at));
    CHECK(!core::Query::parse("date:2026-02-29", at).valid()); // 2026 is no leap year
    CHECK(!core::Query::parse("modified:2026-13", at).valid());
    core::QueryOptions east = at;
    east.utc_offset_minutes = 120; // local midnight is two hours earlier in utc
    CHECK(hit("modified:today", at_day(2026, 10, 18, 23), east) && !hit("modified:today", at_day(2026, 10, 18, 21), east));

    // hidden
    const core::FileEntry secret = entry(".secret", 1, FileKind::other, 0, core::attr::hidden);
    CHECK(hit("hidden:yes", secret) && !hit("hidden:yes", entry("open")) && hit("hidden:no", entry("open")));
    CHECK(core::Query::parse("hidden:yes").includes_hidden() && !core::Query::parse("hidden:no").includes_hidden() && core::Query::parse("-hidden:no").includes_hidden());

    // regular expressions
    CHECK(hit("re:^IMG_\\d+\\.(jpg|png)$", entry("IMG_001.png")) && !hit("re:^IMG_\\d+\\.(jpg|png)$", entry("IMG_x1.png")));
    CHECK(hit("re:^img_", entry("IMG_1.png")) && !hit("re:^img_", entry("IMG_1.png"), cs));
    CHECK(hit("re:a{2,3}b", entry("xaab")) && !hit("re:^a{2,3}b", entry("ab")) && !hit("re:^a{2}$", entry("aaa")));
    CHECK(hit("re:[a-c]+x$", entry("zzabcx")) && !hit("re:^[^a-c]+x$", entry("zzabcx")));
    CHECK(hit("re:\\w+@\\d", entry("me@5")) && hit("re:^(?:ab)+$", entry("ababab")) && !hit("re:^(?:ab)+$", entry("aba")));
    CHECK(hit("re:a.c", entry("a\xc3\xa9" "c")) && hit("re:^caf.$", entry("caf\xc3\xa9"))); // . is a whole utf-8 character
    CHECK(hit("re:colou?r", entry("color")) && hit("re:^a.*?b", entry("aXb")));
    CHECK(!hit("re:(a*)*b", entry("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))); // memoised: no blow-up
    CHECK(!core::Query::parse("re:(abc").valid() && !core::Query::parse("re:abc)").valid() && !core::Query::parse("re:[abc").valid());
    CHECK(!core::Query::parse("re:*a").valid() && !core::Query::parse("re:a{3,1}").valid() && !core::Query::parse("re:\\q").valid());
    core::QueryOptions rx;
    rx.regex = true;
    CHECK(hit("^re.*t$", entry("report"), rx) && !hit("^re.*t$", entry("reports"), rx) && hit("a|b", entry("xb"), rx)); // | is the regex's own
    CHECK(!core::Query::parse("*.cpp", rx).valid() && hit("size:>1kb \"a.b\"", entry("a.b", 5000), rx));

    // an invalid query never filters
    const core::Query bad = core::Query::parse("re:(abc");
    CHECK(!bad.valid() && !bad.error().empty() && bad.matches(entry("anything")));
}

static std::string make_temp_dir()
{
    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::string dir = core::path::join(core::text::narrow(tmp), "prismfs_selftest_" + std::to_string(::GetCurrentProcessId()));
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    return dir;
}

static void test_loader_and_ops()
{
    const std::string dir = make_temp_dir();
    write_file(core::path::join(dir, "one.txt"), "1");
    write_file(core::path::join(dir, "two.png"), "22");
    ::CreateDirectoryW(core::text::widen(core::path::join(dir, "sub")).c_str(), nullptr);

    // async listing
    model::Tab tab{[] {}};
    tab.navigate(dir);
    for (int i = 0; i < 200 && !tab.listing().finished(); ++i) {
        (void)tab.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(tab.listing().finished() && tab.listing().rows() == 3);
    CHECK(tab.listing().at(0).name == "sub" && tab.listing().at(0).is_dir());
    CHECK(tab.listing().at(2).kind == core::FileKind::image);

    // history
    tab.navigate(core::path::join(dir, "sub"));
    CHECK(tab.can_back() && tab.back() && tab.path() == dir);
    CHECK(tab.can_forward());

    // recursive search finds names below the folder, as relative paths
    write_file(core::path::join(core::path::join(dir, "sub"), "deep_two.txt"), "x");
    const auto search = [&](const char* q) {
        tab.search(core::Query::parse(q));
        for (int i = 0; i < 200 && !tab.listing().finished(); ++i) {
            (void)tab.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return tab.listing().rows();
    };
    CHECK(search("TWO") == 2 && tab.searching());
    CHECK(tab.listing().find_name("sub\\deep_two.txt").has_value());
    CHECK(search("*.txt") == 2 && tab.listing().find_name("one.txt").has_value());       // wildcards, below the folder too
    CHECK(search("ext:png") == 1 && tab.listing().find_name("two.png").has_value());
    CHECK(search("kind:folder") == 1 && tab.listing().find_name("sub").has_value());
    CHECK(search("-two") == 2);                                                           // one.txt and sub
    CHECK(search("sub\\*") == 1 && tab.listing().find_name("sub\\deep_two.txt").has_value());
    CHECK(search("type:image") == 1);                                                     // (kinds come from the extension)
    CHECK(search("re:^deep_\\w+\\.txt$") == 1);
    CHECK(search("size:>0 kind:file") == 3);
    tab.search(core::Query::parse("re:(bad")); // invalid: nothing starts, the previous results stay
    CHECK(tab.searching() && tab.listing().rows() == 3);
    tab.clear_search();
    for (int i = 0; i < 200 && !tab.listing().finished(); ++i) {
        (void)tab.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!tab.searching() && tab.listing().rows() == 3);

    // rename, new folder
    CHECK(core::FileOps::rename(core::path::join(dir, "one.txt"), "uno.txt"));
    CHECK(!core::FileOps::rename(core::path::join(dir, "uno.txt"), "bad:name"));
    CHECK(core::FileOps::create_folder(dir) == "New folder");
    CHECK(core::FileOps::create_folder(dir) == "New folder (2)");
    CHECK(core::FileOps::create_file(dir, "New file", "") == "New file");
    CHECK(core::FileOps::create_file(dir, "New file", "") == "New file (2)");
    CHECK(core::FileOps::create_file(dir, "t", ".txt") == "t.txt");
    CHECK(core::FileOps::remove_empty_file(core::path::join(dir, "t.txt")));

    // copy / move / delete through the shell, waiting for the worker
    core::FileOps ops{[] {}, nullptr};
    const auto run = [&](core::OpKind k, std::vector<std::string> src, std::string dst) {
        ops.start(k, std::move(src), std::move(dst));
        core::OpResult r;
        for (int i = 0; i < 500; ++i) {
            if (ops.poll(r)) { return r; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return core::OpResult{};
    };
    const std::string sub = core::path::join(dir, "sub");
    auto r = run(core::OpKind::copy, {core::path::join(dir, "uno.txt")}, sub);
    CHECK(r.ok && core::path::exists(core::path::join(sub, "uno.txt")));
    r = run(core::OpKind::move, {core::path::join(dir, "two.png")}, sub);
    CHECK(r.ok && core::path::exists(core::path::join(sub, "two.png")) && !core::path::exists(core::path::join(dir, "two.png")));
    r = run(core::OpKind::erase, {sub}, {});
    CHECK(r.ok && !core::path::exists(sub));

    // recycle + restore (what undoing a delete does)
    {
        const std::string victim = core::path::join(dir, "restore_me.txt");
        write_file(victim, "keep");
        r = run(core::OpKind::recycle, {victim}, {});
        CHECK(r.ok && !core::path::exists(victim));
        CHECK(core::FileOps::restore_from_recycle({victim}));
        CHECK(core::path::exists(victim));
    }

    // the undo history: inverse closures run newest first, redo walks forward again
    {
        model::UndoStack h;
        int state = 0;
        h.push({"inc", [&] { --state; return true; }, [&] { ++state; return true; }});
        h.push({"inc2", [&] { --state; return true; }, [&] { ++state; return true; }});
        state = 2;
        CHECK(h.can_undo() && h.undo_label() == "inc2");
        CHECK(h.undo() && state == 1 && h.can_redo());
        CHECK(h.redo() && state == 2);
        CHECK(h.undo() && h.undo() && state == 0 && !h.can_undo());
        h.push({"new", [&] { return true; }, [&] { return true; }}); // a new action ends the redo branch
        CHECK(!h.can_redo());
    }

    // clipboard round trip
    CHECK(core::clipboard_set_files({core::path::join(dir, "uno.txt")}, true));
    std::vector<std::string> got;
    bool cut = false;
    CHECK(core::clipboard_get_files(got, cut) && got.size() == 1 && cut);

    (void)run(core::OpKind::erase, {dir}, {});
}

static void test_sidebar_layout()
{
    using model::SidebarLayout;
    using model::ItemPos;

    SidebarLayout l = SidebarLayout::defaults();
    CHECK(l.sections().size() == 2 && !l.sections()[0].drives && l.sections()[1].drives);
    CHECK(!l.sections()[0].items.empty() && l.sections()[0].items[0].name == "Home" && l.sections()[0].items[0].known == "home");

    // pin: a drive section refuses, duplicates are refused (case and slash insensitive)
    const std::string a = "C:\\Prism Test\\Alpha", b = "C:\\Prism Test\\Beta", c = "D:\\Gamma";
    const std::size_t n0 = l.sections()[0].items.size();
    CHECK(!l.pin(1, 0, a));
    CHECK(l.pin(0, 0, a) && l.sections()[0].items.size() == n0 + 1 && l.sections()[0].items[0].path == a);
    CHECK(!l.pin(0, 0, "c:/prism test/alpha/"));
    CHECK(l.pin(0, 99, b) && l.sections()[0].items.back().path == b);       // past the end = the end
    CHECK(l.find(a) && l.find(a)->index == 0 && !l.find(c));

    // move_item: slots count the item itself, so moving next to itself changes nothing
    const std::size_t last = l.sections()[0].items.size() - 1;
    CHECK(!l.move_item({0, 0}, 0, 0) && !l.move_item({0, 0}, 0, 1));
    CHECK(l.move_item({0, 0}, 0, 3) && l.sections()[0].items[2].path == a); // slot 3 before removal = index 2 after
    CHECK(l.move_item({0, last}, 0, 0) && l.sections()[0].items[0].path == b);
    CHECK(!l.move_item({0, 0}, 1, 0));                                       // not onto the drive list

    // sections: new, move items there, remove hands the pins to the neighbour
    const std::size_t s = l.add_section(1, "Work");
    CHECK(s == 1 && l.sections().size() == 3 && l.sections()[1].title == "Work");
    CHECK(l.move_item({0, 0}, 1, 0) && l.sections()[1].items.size() == 1 && l.sections()[1].items[0].path == b);
    CHECK(l.move_section(1, 0) && l.sections()[0].title == "Work");         // (above Quick access)
    CHECK(l.remove_section(0) && l.sections().size() == 2 && l.find(b) && l.find(b)->section == 0);
    CHECK(l.pin_section() == 0);

    // rename: an empty name or the folder's own name goes back to the default
    const auto at = *l.find(a);
    CHECK(l.rename_item(at, "Alpha!") && l.sections()[at.section].items[at.index].label() == "Alpha!");
    CHECK(l.rename_item(at, "") && l.sections()[at.section].items[at.index].label() == "Alpha");
    CHECK(!l.rename_item(at, "Alpha"));

    // persistence: same shape back, including collapsed state and a custom icon
    l.set_collapsed(0, true);
    l.set_item_style(*l.find(a), 0xe80f, 0xff3366cc);
    strata::config cfg;
    l.store(cfg);
    SidebarLayout back;
    CHECK(back.load(cfg) && back.sections().size() == l.sections().size());
    CHECK(back.sections()[0].collapsed && back.sections().back().drives);
    const auto bat = back.find(a);
    CHECK(bat && back.sections()[bat->section].items[bat->index].glyph == 0xe80f && back.sections()[bat->section].items[bat->index].color == 0xff3366cc);
    CHECK(back.sections()[0].items.size() == l.sections()[0].items.size());
    SidebarLayout none;
    CHECK(!none.load(strata::config{}));                                     // nothing stored: the caller keeps its defaults

    // undo / redo restore the shape but not what is expanded
    SidebarLayout before = l;
    CHECK(l.unpin(a) && !l.find(a));
    l.set_collapsed(0, false);
    l.restore(before);
    CHECK(l.find(a) && !l.sections()[0].collapsed);

    CHECK(!l.remove_item({9, 0}) && !l.pin(9, 0, c));
}

static void test_sidebar_phase3()
{
    using model::SidebarLayout;
    using model::SidebarSection;

    SidebarLayout l = SidebarLayout::defaults();
    // hidden drives: case-insensitive, once, and they survive undo snapshots and the file
    CHECK(l.hide_drive("E:\\") && !l.hide_drive("e:/") && l.drive_hidden("E:\\") && !l.drive_hidden("C:\\"));
    SidebarLayout with_hidden = l;
    CHECK(l.show_drive("e:\\") && !l.drive_hidden("E:\\") && !l.show_drive("E:\\"));
    l.restore(with_hidden);
    CHECK(l.drive_hidden("E:\\"));
    CHECK(l.hide_drive("F:\\"));
    strata::config cfg;
    l.store(cfg);
    SidebarLayout back;
    CHECK(back.load(cfg) && back.hidden_drives().size() == 2 && back.drive_hidden("F:\\"));

    // automatic sections: one of each kind, they take no pins and no moved items, and they round-trip
    CHECK(l.add_auto_section(1, SidebarSection::Auto::recent) && !l.add_auto_section(0, SidebarSection::Auto::recent));
    CHECK(l.has_auto(SidebarSection::Auto::recent) && !l.has_auto(SidebarSection::Auto::frequent));
    CHECK(!l.pin(1, 0, "C:\\Prism Test\\Gamma") && l.sections()[1].dynamic());
    CHECK(!l.move_item({0, 0}, 1, 0));
    CHECK(l.pin_section() == 0);
    strata::config cfg2;
    l.store(cfg2);
    SidebarLayout again;
    CHECK(again.load(cfg2) && again.sections()[1].auto_kind == SidebarSection::Auto::recent && again.sections()[1].title == "Recent");
    CHECK(l.remove_section(1) && !l.has_auto(SidebarSection::Auto::recent));

    // replace takes a whole layout (a pasted one)
    SidebarLayout other = SidebarLayout::defaults();
    l.replace(other);
    CHECK(l.hidden_drives().empty() && l.sections().size() == 2);
    l.hide_drive("E:\\");
    l.reset();
    CHECK(l.hidden_drives().empty());

    // the visit log: recent is newest first, frequent needs a second visit, roots are not listed, forget works
    model::VisitLog v;
    v.note("C:\\a\\one");
    v.note("C:\\a\\two");
    v.note("C:\\a\\one");
    v.note("C:\\a\\three");
    v.note("C:\\");
    CHECK(v.size() == 3);
    const auto rc = v.recent(2);
    CHECK(rc.size() == 2 && rc[0] == "C:\\a\\three" && rc[1] == "C:\\a\\one");
    const auto fq = v.frequent(5);
    CHECK(fq.size() == 1 && fq[0] == "C:\\a\\one");
    const std::string file = make_temp_dir() + "\\visits_test.ini";
    CHECK(v.save_file(file));
    model::VisitLog loaded;
    CHECK(loaded.load_file(file) && loaded.size() == 3 && loaded.recent(1)[0] == "C:\\a\\three" && loaded.frequent(1)[0] == "C:\\a\\one");
    loaded.note("C:\\a\\newest");
    CHECK(loaded.recent(1)[0] == "C:\\a\\newest"); // (the clock continued after loading)
    v.forget("c:\\A\\ONE");
    CHECK(v.size() == 2 && v.frequent(5).empty());
    ::DeleteFileW(core::text::widen(file).c_str());
}

// the worker must answer for a real folder and a real file (what the entries are depends on what is installed)
static void test_shell_menu()
{
    core::ShellMenu m;
    for (const bool background : {true, false}) {
        m.query({background ? "C:\\Windows" : "C:\\Windows\\notepad.exe"}, background);
        for (int i = 0; i < 200 && !m.ready(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        CHECK(m.ready());
        std::printf("shell menu %s: %zu entries\n", background ? "folder" : "file", m.items().size());
    }
}

// switches the per-user folder verb on and off again (skipped when the user has it on: never touch their setup)
static void test_recycle_bin_handler()
{
    const bool before = core::recycle_bin_handler_active();
    CHECK(core::set_recycle_bin_handler(true));
    CHECK(core::recycle_bin_handler_active());
    CHECK(core::set_recycle_bin_handler(false));
    CHECK(!core::recycle_bin_handler_active());
    if (before) { (void)core::set_recycle_bin_handler(true); }
}

static void test_default_manager()
{
    if (core::default_manager_active()) { return; }
    CHECK(core::set_default_manager(true));
    CHECK(core::default_manager_active());
    CHECK(core::set_default_manager(false));
    CHECK(!core::default_manager_active());
}

// a file sent to the real Recycle Bin comes back under its own name, and its `$I` record goes with it
static void test_recycle_restore()
{
    const std::string dir = make_temp_dir();
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    const std::string file = core::path::join(dir, "bin test \xC3\xBC.txt");
    { std::ofstream(core::text::widen(file), std::ios::binary) << "x"; }

    std::wstring from = core::text::widen(file);
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (::SHFileOperationW(&op) != 0 || core::recycle::folder().empty()) { ::RemoveDirectoryW(core::text::widen(dir).c_str()); return; } // (no bin on this machine: nothing to test)

    std::string item;
    WIN32_FIND_DATAW fd{};
    const std::string bin = core::recycle::folder();
    HANDLE h = ::FindFirstFileW(core::text::widen(core::path::join(bin, "$R*")).c_str(), &fd);
    for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = ::FindNextFileW(h, &fd)) {
        const std::string candidate = core::path::join(bin, core::text::narrow(fd.cFileName));
        if (const auto rec = core::recycle::read(candidate); rec && core::text::lower_ascii(rec->original) == core::text::lower_ascii(file)) { item = candidate; break; }
    }
    if (h != INVALID_HANDLE_VALUE) { ::FindClose(h); }
    CHECK(!item.empty());
    if (item.empty()) { ::RemoveDirectoryW(core::text::widen(dir).c_str()); return; }

    std::string error;
    CHECK(core::recycle::restore(item, error));
    CHECK(::GetFileAttributesW(core::text::widen(file).c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(!core::recycle::read(item).has_value()); // (the record was removed)
    ::DeleteFileW(core::text::widen(file).c_str());
    ::RemoveDirectoryW(core::text::widen(dir).c_str());
}

static void test_drive_watcher()
{
    core::DriveWatcher w;
    w.interval = std::chrono::milliseconds{50};
    const auto first = w.list();
    CHECK(!first.empty() && first.front().path.size() >= 2 && w.revision() >= 1); // (a machine with no drive would not run this)
    w.poke();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const auto drives_only = [](const std::vector<core::Place>& l) { return std::ranges::count_if(l, [](const core::Place& p) { return !p.device && !p.recycle; }); };
    CHECK(drives_only(w.list()) == drives_only(first)); // (phones and the bin are only looked for on the worker: they may turn up after `first`)
}

static void test_path_probe()
{
    const std::string dir = make_temp_dir();
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    core::PathProbe probe;
    probe.interval = std::chrono::milliseconds{50};
    probe.watch({dir});
    CHECK(!probe.missing(dir));
    ::RemoveDirectoryW(core::text::widen(dir).c_str());
    bool seen = false;
    for (int i = 0; i < 100 && !seen; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        seen = probe.missing(dir);
    }
    CHECK(seen);
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    seen = false;
    for (int i = 0; i < 100 && !seen; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        seen = !probe.missing(dir);
    }
    CHECK(seen);
    ::RemoveDirectoryW(core::text::widen(dir).c_str());
}

static void test_batch_rename()
{
    using core::RenameItem;
    const std::vector<RenameItem> items{{"IMG_0001.JPG", false}, {"holiday photo.png", false}, {"notes", true}};
    core::RenameRule r;
    CHECK(core::batch_names(items, r) == std::vector<std::string>({"IMG_0001.JPG", "holiday photo.png", "notes"}));
    r.pattern = "trip-[C]";
    r.digits  = 3;
    r.start   = 7;
    CHECK(core::batch_names(items, r) == std::vector<std::string>({"trip-007.JPG", "trip-008.png", "trip-009"}));
    r = {};
    r.find    = "img_";
    r.replace = "photo ";
    r.casing  = core::CaseMode::title;
    CHECK(core::batch_names(items, r)[0] == "Photo 0001.JPG");
    CHECK(core::batch_names(items, r)[1] == "Holiday Photo.png");
    r = {};
    r.pattern = "[N].[E].bak";
    CHECK(core::batch_names(items, r)[0] == "IMG_0001.JPG.bak");
    const std::vector<RenameItem> twins{{"a.txt", false}, {"A.txt", false}};
    r = {};
    const auto bad = core::batch_problems(twins, core::batch_names(twins, r), {});
    CHECK(bad[0] == "same name twice" && bad[1] == "same name twice");
    r.pattern = "a:b";
    CHECK(core::batch_problems(items, core::batch_names(items, r), {})[2] == "invalid characters");
    CHECK(core::batch_problems({items[2]}, {"Taken"}, {"taken"})[0] == "already exists");

    // on disk: a swap inside the selection works, a clash rolls everything back
    const std::string dir = core::path::join(make_temp_dir(), "ren");
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    write_file(core::path::join(dir, "a.txt"), "A");
    write_file(core::path::join(dir, "b.txt"), "BB");
    CHECK(core::apply_renames(dir, {{"a.txt", "b.txt"}, {"b.txt", "a.txt"}}));
    CHECK(core::path::exists(core::path::join(dir, "a.txt")) && core::path::exists(core::path::join(dir, "b.txt")));
    write_file(core::path::join(dir, "c.txt"), "C");
    CHECK(!core::apply_renames(dir, {{"a.txt", "x.txt"}, {"b.txt", "c.txt"}}));
    CHECK(core::path::exists(core::path::join(dir, "a.txt")) && core::path::exists(core::path::join(dir, "b.txt")) && !core::path::exists(core::path::join(dir, "x.txt")));
}

static void test_duplicates_and_sizes()
{
    const std::string dir = core::path::join(make_temp_dir(), "dups");
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    const std::string sub = core::path::join(dir, "sub");
    ::CreateDirectoryW(core::text::widen(sub).c_str(), nullptr);
    write_file(core::path::join(dir, "one.txt"), "same content");
    write_file(core::path::join(sub, "copy.txt"), "same content");
    write_file(core::path::join(dir, "other.txt"), "same lenght!"); // same size, other bytes
    write_file(core::path::join(dir, "lone.txt"), "unique");
    core::DuplicateScan scan;
    scan.start(dir);
    for (int i = 0; i < 500 && scan.stage() != core::DuplicateScan::Stage::done; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const auto groups = scan.groups();
    CHECK(groups.size() == 1 && groups[0].paths.size() == 2 && groups[0].size == 12);
    CHECK(scan.reclaimable() == 12);

    CHECK(core::measure_folder(dir, std::stop_token{}) == 12 * 3 + 6);
    core::FolderSizes sizes{[] {}};
    CHECK(!sizes.lookup(dir).has_value() || true);
    for (int i = 0; i < 500 && !sizes.lookup(dir); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(sizes.lookup(dir).value_or(0) == 12 * 3 + 6);
}

static void test_git_status()
{
    using namespace std::string_literals;
    const std::string out = "## main...origin/main [ahead 2, behind 1]\0"
                            " M src/a.cpp\0A  src/new.cpp\0?? notes/\0?? Readme.md\0 D old.txt\0UU conflict.txt\0R  moved.txt\0was.txt\0"s;
    const core::GitRepo r = core::parse_git_status(out);
    CHECK(r.branch == "main" && r.ahead == 2 && r.behind == 1);
    CHECK(r.code_for("src\\a.cpp", false) == 'M');
    CHECK(r.code_for("src\\new.cpp", false) == 'A');
    CHECK(r.code_for("readme.md", false) == '?');
    CHECK(r.code_for("old.txt", false) == 'D');
    CHECK(r.code_for("conflict.txt", false) == '!');
    CHECK(r.code_for("moved.txt", false) == 'R' && r.code_for("was.txt", false) == 0); // (the old name is not a change)
    CHECK(r.code_for("notes\\inside.txt", false) == '?'); // inside an untracked folder
    CHECK(r.code_for("src", true) == 'A' || r.code_for("src", true) == 'M');
    CHECK(r.code_for("clean.txt", false) == 0);
    CHECK(core::parse_git_status("## No commits yet on trunk\0"s).branch == "trunk");

    // the real thing, when git is installed
    const std::string dir = core::path::join(make_temp_dir(), "repo");
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    const core::ProcessResult init = core::run_process(L"git init -q", core::text::widen(dir), 15000);
    if (init.started && init.exit_code == 0) {
        write_file(core::path::join(dir, "tracked.txt"), "x");
        write_file(core::path::join(dir, "fresh.txt"), "y");
        (void)core::run_process(L"git add tracked.txt", core::text::widen(dir), 15000);
        core::GitStatus git{[] {}};
        std::shared_ptr<const core::GitRepo> repo;
        for (int i = 0; i < 300 && !repo; ++i) {
            repo = git.lookup(dir);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(repo != nullptr);
        if (repo) {
            CHECK(repo->code_for("tracked.txt", false) == 'A');
            CHECK(repo->code_for("fresh.txt", false) == '?');
            CHECK(repo->relative(core::path::join(dir, "sub\\file.txt")) == "sub\\file.txt");
        }
    }
}

static void test_templates_and_archives()
{
    const std::string dir = core::path::join(make_temp_dir(), "tpl");
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    CHECK(core::builtin_templates().empty()); // (none shipped: the Templates folder is the user's)
    CHECK(core::user_templates(core::path::join(dir, "nope")).empty());
    const std::string tpl = core::path::join(dir, "Templates");
    ::CreateDirectoryW(core::text::widen(tpl).c_str(), nullptr);
    write_file(core::path::join(tpl, "Note.md"), "# note\n");
    const auto found = core::user_templates(tpl);
    CHECK(found.size() == 1 && found[0].label == "Note" && found[0].ext == ".md");
    const std::string a = core::create_from_template(dir, found[0]), b = core::create_from_template(dir, found[0]);
    CHECK(a == "Note.md" && b == "Note (2).md");

    CHECK(core::is_archive_name("a.ZIP") && core::is_archive_name("x.tar.gz") && !core::is_archive_name("zip") && !core::is_archive_name("notes.txt"));
    CHECK(core::archive_stem("photos.tar.gz") == "photos" && core::archive_stem("a.b.zip") == "a.b");

    // pack and unpack through the system's tar.exe
    const std::string work = core::path::join(make_temp_dir(), "arc");
    ::CreateDirectoryW(core::text::widen(work).c_str(), nullptr);
    write_file(core::path::join(work, "one.txt"), "hello archive");
    std::atomic<int> woke{0};
    core::ArchiveOps ops{[&] { ++woke; }};
    ops.compress(work, {"one.txt"}, "pack");
    core::ArchiveOps::Result res;
    for (int i = 0; i < 500 && !ops.poll(res); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(res.ok && res.made == "pack.zip");
    if (res.ok) {
        ops.extract(core::path::join(work, "pack.zip"), work, true);
        core::ArchiveOps::Result res2;
        for (int i = 0; i < 500 && !ops.poll(res2); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(res2.ok && res2.made == "pack");
        CHECK(core::path::exists(core::path::join(core::path::join(work, "pack"), "one.txt")));

        // an archive opened as a folder: unpacked into its cache folder with the tag coming back
        const std::string zip   = core::path::join(work, "pack.zip");
        const std::string cache = core::archive_cache_dir(zip);
        CHECK(cache == core::archive_cache_dir(zip) && cache != core::archive_cache_dir(core::path::join(work, "other.zip"))); // stable per archive
        CHECK(core::archive_cache_dir(zip, true) == cache && core::path::is_directory(cache));
        ops.extract(zip, cache, false, "view\n" + zip);
        core::ArchiveOps::Result res3;
        for (int i = 0; i < 500 && !ops.poll(res3); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(res3.ok && res3.tag == "view\n" + zip && core::path::exists(core::path::join(cache, "one.txt")));
        core::ArchiveViews views;
        views.add(cache, zip);
        CHECK(views.has_root(cache) && views.root_for(zip).has_value());
        CHECK(views.find(cache) && views.find(cache)->inner.empty() && views.find(cache)->archive == zip);
        const auto inner = views.find(core::path::join(cache, "sub\\deeper"));
        CHECK(inner && inner->inner == "sub\\deeper");
        CHECK(!views.find(cache + "x\\y") && !views.find("C:\\Windows")); // a folder that only starts with the same letters is not inside
        CHECK(views.leave_target(cache) == work && !views.leave_target(core::path::join(cache, "sub")));
        core::clear_archive_cache();
        CHECK(!core::path::exists(cache));
    }
}

static void test_sort_options()
{
    model::Listing L;
    std::vector<core::FileEntry> v;
    const auto add = [&](const char* n, bool dir, bool hidden) {
        core::FileEntry e;
        e.name       = n;
        e.attributes = (dir ? core::attr::directory : 0u) | (hidden ? core::attr::hidden : 0u);
        e.kind       = dir ? core::FileKind::folder : core::FileKind::other;
        e.ext_offset = static_cast<std::uint16_t>(e.name.size());
        v.push_back(std::move(e));
    };
    add("file10", false, false);
    add("file2", false, false);
    add("Docs", true, false);
    add(".git", true, true);
    add(".env", false, true);
    L.set_show_hidden(true);
    L.clear();
    L.append(std::move(v));
    L.finish();
    const auto names = [&] {
        std::string s;
        for (std::size_t i = 0; i < L.rows(); ++i) { s += L.at(i).name + (i + 1 < L.rows() ? "," : ""); }
        return s;
    };
    model::SortSpec s;
    L.set_sort(s);
    CHECK(names() == ".git,Docs,.env,file2,file10"); // folders on top, numbers in order
    s.folders = model::FolderPlacement::last;
    L.set_sort(s);
    CHECK(names() == ".env,file2,file10,.git,Docs");
    s.folders = model::FolderPlacement::mixed;
    L.set_sort(s);
    CHECK(names() == ".env,.git,Docs,file2,file10");
    s.hidden_last = true;
    L.set_sort(s);
    CHECK(names() == "Docs,file2,file10,.env,.git");
    s.natural = false;
    L.set_sort(s);
    CHECK(names() == "Docs,file10,file2,.env,.git");
    s.ascending = false; // the folder grouping does not flip with the direction
    s.folders   = model::FolderPlacement::first;
    s.hidden_last = false;
    L.set_sort(s);
    CHECK(L.at(0).is_dir() && L.at(1).is_dir() && !L.at(2).is_dir());

    model::FolderSorts fs;
    CHECK(!fs.get("C:\\Work").has_value());
    CHECK(fs.set("C:\\Work", {model::SortKey::modified, false}));
    CHECK(!fs.set("c:\\work\\", {model::SortKey::modified, false})); // same folder, same order
    CHECK(fs.get("C:\\WORK")->key == model::SortKey::modified && !fs.get("C:\\WORK")->ascending);
    strata::config c;
    fs.store(c);
    model::FolderSorts back;
    CHECK(back.load(c) && back.get("c:\\work").has_value() && back.size() == 1);
}

static void test_saved_search()
{
    model::SidebarLayout l;
    const std::size_t s = l.pin_section();
    CHECK(l.pin(s, 0, "C:\\Work"));
    CHECK(l.pin_search(s, "C:\\Work", "*.cpp size:>10kb"));
    CHECK(!l.pin_search(s, "c:\\work", "*.cpp size:>10kb")); // same search twice
    CHECK(l.pin_search(s, "C:\\Work", "ext:h"));
    CHECK(l.find("C:\\Work") && l.find("C:\\Work")->index == 0); // the folder pin is not confused with the searches
    strata::config c;
    l.store(c);
    model::SidebarLayout back;
    CHECK(back.load(c));
    std::size_t searches = 0;
    for (const auto& sec : back.sections()) { for (const auto& it : sec.items) { searches += it.query.empty() ? 0 : 1; } }
    CHECK(searches == 2);
}

static void test_i18n()
{
    namespace i18n = prism::core::i18n;
    CHECK(!i18n::languages().empty() && i18n::languages()[0].code == "en");
    CHECK(i18n::set_language("xx") == "en"); // unknown: English
    CHECK(i18n::tr("Compact density") == "Compact density");
    CHECK(i18n::set_language("en") == "en");

    // a language file in %TEMP%
    char tmp[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, tmp);
    const std::string dir = std::string{tmp} + "prismfs_i18n_test";
    ::CreateDirectoryA(dir.c_str(), nullptr);
    {
        std::ofstream f{dir + "\\xx.lang", std::ios::binary};
        f << "# test\n@name = Xx Lang\n@plural = slavic\n\n"
             "\"Compact density\" = \"Dense\"\n"
             "\"Say \\\"hi\\\"\\nnow\" = \"A\\\\B\\n\"\n"
             "\"Not done\" = \"\"\n"
             "\"{} item|{} items\" = \"{} one|{} few|{} many\"\n"
             "\"{} left\" = \"{1} bad\"\n"
             "\"{} of {}\" = \"{1} von {0}\"\n"
             "\"{} done\" = \"{ broken\"\n";
    }
    const std::string dirs[]{dir};
    i18n::scan(dirs);
    CHECK(i18n::languages().size() == 2 && i18n::languages()[1].code == "xx" && i18n::languages()[1].name == "Xx Lang");
    CHECK(i18n::set_language("xx") == "xx");
    CHECK(i18n::tr("Compact density") == "Dense");
    CHECK(i18n::tr("Say \"hi\"\nnow") == "A\\B\n");
    CHECK(i18n::tr("Not done") == "Not done");   // empty = untranslated
    CHECK(i18n::tr("Unknown") == "Unknown");
    CHECK(i18n::tr_n("{} item", "{} items", 1) == "{} one");
    CHECK(i18n::tr_n("{} item", "{} items", 3) == "{} few");
    CHECK(i18n::tr_n("{} item", "{} items", 11) == "{} many");
    CHECK(i18n::tr_n("{} item", "{} items", 21) == "{} one");
    CHECK(i18n::tr_n("{} file", "{} files", 2) == "{} files"); // no entry: english rule
    CHECK(i18n::trf("{} of {}", 3, 9) == "9 von 3");
    CHECK(i18n::trf("{} left", 5) == "5 left");   // index out of range: entry dropped
    CHECK(i18n::trf("{} done", 5) == "5 done");   // malformed braces: entry dropped
    CHECK(i18n::trf_n("{} item", "{} items", 22, 22) == "22 few");
    CHECK(i18n::set_language("en") == "en" && i18n::tr("Compact density") == "Compact density");
    ::DeleteFileA((dir + "\\xx.lang").c_str());
    ::RemoveDirectoryA(dir.c_str());
    i18n::scan({});
}

// a drag source that offers files without paths on disk, like MobaXterm's SFTP browser
class FakeVirtualSource final : public IDataObject {
public:
    struct Item {
        std::wstring name;
        std::string  data;
        bool         dir{};
    };
    explicit FakeVirtualSource(std::vector<Item> items) : items_{std::move(items)} {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (id == IID_IUnknown || id == IID_IDataObject) { *out = static_cast<IDataObject*>(this); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override
    {
        *m = {};
        if (f->cfFormat == group_cf() && (f->tymed & TYMED_HGLOBAL) != 0) {
            const std::size_t bytes = sizeof(UINT) + items_.size() * sizeof(FILEDESCRIPTORW);
            HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
            auto* g = static_cast<FILEGROUPDESCRIPTORW*>(::GlobalLock(mem));
            g->cItems = static_cast<UINT>(items_.size());
            for (std::size_t i = 0; i < items_.size(); ++i) {
                g->fgd[i].dwFlags = FD_ATTRIBUTES;
                g->fgd[i].dwFileAttributes = items_[i].dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                items_[i].name.copy(g->fgd[i].cFileName, MAX_PATH - 1);
            }
            ::GlobalUnlock(mem);
            m->tymed   = TYMED_HGLOBAL;
            m->hGlobal = mem;
            return S_OK;
        }
        if (f->cfFormat == contents_cf() && (f->tymed & TYMED_ISTREAM) != 0 && f->lindex >= 0 && static_cast<std::size_t>(f->lindex) < items_.size()) {
            const std::string& d = items_[static_cast<std::size_t>(f->lindex)].data;
            m->tymed = TYMED_ISTREAM;
            m->pstm  = ::SHCreateMemStream(reinterpret_cast<const BYTE*>(d.data()), static_cast<UINT>(d.size()));
            return m->pstm != nullptr ? S_OK : E_OUTOFMEMORY;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override { return f->cfFormat == group_cf() ? S_OK : DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }

private:
    static CLIPFORMAT group_cf() { return static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)); }
    static CLIPFORMAT contents_cf() { return static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_FILECONTENTS)); }
    std::vector<Item> items_;
};

static void test_virtual_files()
{
    CHECK(core::safe_relative_path(L"a.txt") == "a.txt");
    CHECK(core::safe_relative_path(L"dir/sub\\f.txt") == "dir\\sub\\f.txt");
    CHECK(!core::safe_relative_path(L"..\\x"));
    CHECK(!core::safe_relative_path(L"a\\..\\..\\x"));
    CHECK(!core::safe_relative_path(L"C:\\x"));
    CHECK(!core::safe_relative_path(L"nul.txt"));
    CHECK(!core::safe_relative_path(L"a?.txt"));
    CHECK(!core::safe_relative_path(L"trailing."));
    CHECK(!core::safe_relative_path(L""));

    wchar_t tmp[MAX_PATH + 1]{};
    const DWORD n = ::GetTempPathW(MAX_PATH, tmp);
    std::string root = core::text::narrow(std::wstring{tmp, n}) + "pfs_vf_test";
    FakeVirtualSource source{{{L"dir", "", true}, {L"dir\\inner.txt", "world", false}, {L"top.txt", "hello", false}, {L"..\\evil.txt", "x", false}}};
    CHECK(core::offers_virtual_files(&source));
    const core::ReceivedFiles got = core::receive_virtual_files(&source, root);
    CHECK(got.failed == 1);          // (the path leaving the staging folder)
    CHECK(got.items.size() == 2);    // dir, top.txt
    if (got.items.size() == 2) {
        CHECK(core::path::is_directory(got.items[0]));
        CHECK(slurp(got.items[1]) == "hello");
        CHECK(slurp(got.items[0] + "\\inner.txt") == "world");
        CHECK(::GetFileAttributesW(core::text::widen(core::path::parent(got.items[0]) + "\\..\\evil.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
    }
    CHECK(!core::offers_virtual_files(nullptr));
    rm_rf(core::text::widen(root));
}

static void test_terminal()
{
    using core::terminal::wt_arguments;
    CHECK(wt_arguments(L"C:\\Users\\a", L"cmd", L"") == LR"(-d "C:\Users\a" cmd)");
    CHECK(wt_arguments(L"C:\\", L"powershell", L"-NoExit") == LR"(-d "C:\\" powershell -NoExit)");
    CHECK(wt_arguments(L"C:\\a;b", L"", L"") == LR"(-d "C:\a\;b")");
    CHECK(wt_arguments(L"", L"wsl", L"") == L"wsl");
    CHECK(core::terminal::from_program(L"pwsh") == core::terminal::Shell::pwsh);
    CHECK(!core::terminal::from_program(L"code"));
}

static void test_relaunch_and_regressions()
{
    // 1. admin/non-admin relaunch preserves every tab (including drive roots and spaces), active tab and right pane
    {
        core::shell::RelaunchState st;
        st.tabs       = {"C:\\", "D:\\My Folder\\Sub", "E:\\"};
        st.active_tab = 1;
        st.right      = "F:\\Split Folder";
        const std::wstring cmd = L"\"C:\\Program Files\\PrismFS\\prismfs.exe\" " + core::shell::relaunch_arguments(st);
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(cmd.c_str(), &argc);
        CHECK(argv != nullptr);
        std::vector<std::string> tabs;
        int active = -1;
        std::string right;
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view a = argv[i];
            if (a == L"--tab" && i + 1 < argc) { tabs.push_back(core::text::narrow(argv[++i])); }
            else if (a == L"--active-tab" && i + 1 < argc) { active = _wtoi(argv[++i]); }
            else if (a == L"--right" && i + 1 < argc) { right = core::text::narrow(argv[++i]); }
        }
        ::LocalFree(argv);
        CHECK(tabs.size() == 3);
        if (tabs.size() == 3) {
            CHECK(tabs[0] == "C:\\");
            CHECK(tabs[1] == "D:\\My Folder\\Sub");
            CHECK(tabs[2] == "E:\\");
        }
        CHECK(active == 1);
        CHECK(right == "F:\\Split Folder");
    }

    // 2. UNC path handling (root, parent, leaf, segments) and long_wide extended prefix
    {
        CHECK(core::path::is_root("\\\\server\\share"));
        CHECK(core::path::is_root("\\\\server\\share\\"));
        CHECK(!core::path::is_root("\\\\server\\share\\dir"));
        CHECK(core::path::parent("\\\\server\\share").empty());
        CHECK(core::path::parent("\\\\server\\share\\dir") == "\\\\server\\share");
        CHECK(core::path::leaf("\\\\server\\share") == "\\\\server\\share");
        CHECK(core::path::leaf("\\\\server\\share\\dir") == "dir");
        const auto segs = core::path::segments("\\\\server\\share\\a\\b");
        CHECK(segs.size() == 3);
        if (segs.size() == 3) {
            CHECK(segs[0].name == "\\\\server\\share" && segs[0].path == "\\\\server\\share");
            CHECK(segs[1].name == "a" && segs[1].path == "\\\\server\\share\\a");
            CHECK(segs[2].name == "b" && segs[2].path == "\\\\server\\share\\a\\b");
        }
        const std::string long_local = "C:\\" + std::string(260, 'a');
        CHECK(core::path::long_wide(long_local).starts_with(L"\\\\?\\C:\\"));
        const std::string long_unc = "\\\\srv\\shr\\" + std::string(260, 'b');
        CHECK(core::path::long_wide(long_unc).starts_with(L"\\\\?\\UNC\\srv\\shr\\"));
    }

    // 3. Listing incremental O(1) selection and O(N) invert_selection (exact counts & bytes)
    {
        model::Listing L;
        std::vector<core::FileEntry> entries;
        entries.reserve(20000);
        for (int i = 0; i < 20000; ++i) {
            core::FileEntry e;
            e.name       = "file_" + std::to_string(i) + ".bin";
            e.size       = static_cast<std::uint64_t>(i + 1);
            e.ext_offset = static_cast<std::uint16_t>(e.name.size() - 3);
            entries.push_back(std::move(e));
        }
        L.append(std::move(entries));
        L.finish();

        L.select_only(0);
        CHECK(L.selected_count() == 1 && L.selected_bytes() == L.at(0).size);
        L.toggle(1);
        CHECK(L.selected_count() == 2 && L.selected_bytes() == L.at(0).size + L.at(1).size);
        L.set_selected(1, true); // idempotent
        CHECK(L.selected_count() == 2);
        L.set_selected(0, false);
        CHECK(L.selected_count() == 1 && L.selected_bytes() == L.at(1).size);
        L.select_range(0, 99, false);
        CHECK(L.selected_count() == 100);
        L.invert_selection();
        CHECK(L.selected_count() == 19900);
        L.invert_selection();
        CHECK(L.selected_count() == 100);
    }

    // 4. Compact FileEntry size & RecycleInfo out-of-line storage
    {
        CHECK(sizeof(core::FileEntry) <= 80);
        core::FileEntry e;
        e.name = "$R12345.txt";
        CHECK(!e.has_display_override());
        CHECK(e.display() == "$R12345.txt");
        e.set_recycle("notes.txt", "C:\\Docs\\notes.txt");
        CHECK(e.has_display_override());
        CHECK(e.display() == "notes.txt");
        CHECK(e.original_path() == "C:\\Docs\\notes.txt");
        core::FileEntry copy = e;
        CHECK(copy.display() == "notes.txt" && copy.original_path() == "C:\\Docs\\notes.txt");
    }

    // 5. FolderWatcher does not block or spin if watched folder is deleted
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::string dir = core::text::narrow(tmp) + "pfs_watch_test_" + std::to_string(::GetCurrentProcessId());
        ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
        std::atomic<int> wakes{0};
        {
            core::FolderWatcher w{[&] { wakes.fetch_add(1, std::memory_order_relaxed); }};
            w.watch(dir);
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            write_file(dir + "\\a.txt", "hi");
            std::this_thread::sleep_for(std::chrono::milliseconds{40});
            ::DeleteFileW(core::text::widen(dir + "\\a.txt").c_str());
            ::RemoveDirectoryW(core::text::widen(dir).c_str());
            const int before = wakes.load();
            std::this_thread::sleep_for(std::chrono::milliseconds{60});
            CHECK(wakes.load() - before < 20); // must not spin infinitely
        }
        ::RemoveDirectoryW(core::text::widen(dir).c_str());
    }
}

static void test_phase4()
{
    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::string base = core::text::narrow(tmp) + "pfs_phase4_" + std::to_string(::GetCurrentProcessId());
    ::CreateDirectoryW(core::text::widen(base).c_str(), nullptr);

    // 1. Persistent FileIndex cache save/load + corruption rejection
    {
        write_file(base + "\\alpha.cpp", "int main(){}");
        write_file(base + "\\beta.txt", "hello world");
        const std::string cache = base + "\\index.bin";
        {
            core::FileIndex idx{[] {}};
            idx.start({base}, true);
            for (int i = 0; i < 200 && idx.building(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds{10}); }
            CHECK(idx.entries() >= 3);
            CHECK(idx.save_to(cache));
        }
        {
            core::FileIndex idx2{[] {}};
            CHECK(idx2.load_from(cache));
            CHECK(idx2.entries() >= 3);
            CHECK(idx2.age_seconds() < 60.0);
            idx2.search(core::Query::parse("alpha"));
            for (int i = 0; i < 200 && idx2.searching(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds{5}); }
            const auto r = idx2.result();
            CHECK(r != nullptr && r->total == 1);
        }
        // corrupted file must fail cleanly
        write_file(base + "\\bad_index.bin", "NOT_A_VALID_INDEX_FILE");
        core::FileIndex idx3{[] {}};
        CHECK(!idx3.load_from(base + "\\bad_index.bin"));
    }

    // 2. Cross-window TabSet::detach / attach preserves history, scroll, and listing
    {
        model::TabSet a{[] {}}, b{[] {}};
        model::Tab& t1 = a.open(base);
        t1.scroll_y    = 142.5f;
        (void)a.open("C:\\", false);
        CHECK(a.size() == 2);
        auto moved = a.detach(0);
        CHECK(moved != nullptr && a.size() == 1);
        model::Tab& adopted = b.attach(std::move(moved), true);
        CHECK(b.size() == 1);
        CHECK(adopted.path() == base);
        CHECK(adopted.scroll_y == 142.5f);
    }

    // 3. Syntax highlighter & hex dump formatter
    {
        CHECK(core::syntax_for_extension("cpp") == core::SyntaxLang::c_like);
        CHECK(core::syntax_for_extension("py") == core::SyntaxLang::hash);
        CHECK(core::syntax_for_extension("json") == core::SyntaxLang::json);
        CHECK(core::syntax_for_extension("xml") == core::SyntaxLang::markup);

        const auto hl = core::highlight_lines({"#include <vector>", "/* block", "comment */ int x = 42; // end", "const char* s = \"hi\";"}, core::SyntaxLang::c_like);
        CHECK(hl.size() == 4);
        CHECK(!hl[0].spans.empty() && hl[0].spans[0].kind == core::TokenKind::preproc);
        CHECK(!hl[1].spans.empty() && hl[1].spans[0].kind == core::TokenKind::comment);
        CHECK(!hl[2].spans.empty() && hl[2].spans[0].kind == core::TokenKind::comment);
        bool has_num = false, has_str = false;
        for (const auto& sp : hl[2].spans) { if (sp.kind == core::TokenKind::number) { has_num = true; } }
        for (const auto& sp : hl[3].spans) { if (sp.kind == core::TokenKind::string) { has_str = true; } }
        CHECK(has_num && has_str);

        const unsigned char bytes[] = {'M', 'Z', 0x90, 0x00, 0x03, 0x00};
        const auto hex = core::format_hex_dump(bytes, 4);
        CHECK(hex.size() == 1);
        CHECK(hex[0].starts_with("00000000  4D 5A 90 00"));
        CHECK(hex[0].find("|MZ....|") != std::string::npos);
    }

    // 4. Incremental Listing::upsert_entry / remove_name & live ReadDirectoryChangesW deltas
    {
        model::Listing L;
        auto mk = [](std::string n, std::uint64_t sz) {
            core::FileEntry e;
            e.name       = std::move(n);
            e.size       = sz;
            e.ext_offset = static_cast<std::uint16_t>(e.name.size());
            return e;
        };
        L.append({mk("a.txt", 10), mk("c.txt", 30)});
        L.finish();
        L.select_only(1); // "c.txt"
        CHECK(L.selected_bytes() == 30);
        // insert in sorted middle
        CHECK(L.upsert_entry(mk("b.txt", 20)));
        CHECK(L.rows() == 3 && L.at(0).name == "a.txt" && L.at(1).name == "b.txt" && L.at(2).name == "c.txt");
        CHECK(L.selected(2) && L.selected_bytes() == 30);
        // update c.txt size in place (keeps selection, updates selected_bytes)
        CHECK(L.upsert_entry(mk("c.txt", 99)));
        CHECK(L.rows() == 3 && L.selected(2) && L.selected_bytes() == 99);
        // remove b.txt
        CHECK(L.remove_name("b.txt"));
        CHECK(L.rows() == 2 && L.at(0).name == "a.txt" && L.at(1).name == "c.txt");
        CHECK(L.selected(1) && L.selected_bytes() == 99);

        // live watcher delta delivery
        const std::string wdir = base + "\\watch";
        ::CreateDirectoryW(core::text::widen(wdir).c_str(), nullptr);
        std::atomic<int> wakes{0};
        core::FolderWatcher fw{[&] { wakes.fetch_add(1, std::memory_order_relaxed); }};
        fw.watch(wdir);
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        write_file(wdir + "\\live.txt", "12345");
        bool saw_upsert = false;
        for (int i = 0; i < 60 && !saw_upsert; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{15});
            const core::FolderChanges fc = fw.take_changes();
            for (const auto& d : fc.deltas) {
                if (d.kind == core::FolderDelta::Kind::upsert && d.name == "live.txt" && d.entry.size == 5) { saw_upsert = true; }
            }
        }
        CHECK(saw_upsert);
        ::DeleteFileW(core::text::widen(wdir + "\\live.txt").c_str());
        bool saw_remove = false;
        for (int i = 0; i < 60 && !saw_remove; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{15});
            const core::FolderChanges fc = fw.take_changes();
            for (const auto& d : fc.deltas) {
                if (d.kind == core::FolderDelta::Kind::remove && d.name == "live.txt") { saw_remove = true; }
            }
        }
        CHECK(saw_remove);
        ::RemoveDirectoryW(core::text::widen(wdir).c_str());
    }

    ::DeleteFileW(core::text::widen(base + "\\alpha.cpp").c_str());
    ::DeleteFileW(core::text::widen(base + "\\beta.txt").c_str());
    ::DeleteFileW(core::text::widen(base + "\\index.bin").c_str());
    ::DeleteFileW(core::text::widen(base + "\\bad_index.bin").c_str());
    ::RemoveDirectoryW(core::text::widen(base).c_str());
}

static void test_explorer_upgrades()
{
    // 1. cloud reparse tags vs filesystem links
    CHECK(core::path::is_link_reparse_tag(IO_REPARSE_TAG_SYMLINK));
    CHECK(core::path::is_link_reparse_tag(IO_REPARSE_TAG_MOUNT_POINT));
    CHECK(!core::path::is_link_reparse_tag(0x9000001Au)); // IO_REPARSE_TAG_CLOUD
    CHECK(!core::path::is_link_reparse_tag(0x9000101Au)); // IO_REPARSE_TAG_CLOUD_1

    // 2. path leaf trailing slash + resolve_input (%ENV%, quotes, relative)
    CHECK(core::path::leaf("C:\\foo\\") == "foo");
    CHECK(core::path::leaf("C:\\") == "C:\\");
    const std::string resolved_tmp = core::path::resolve_input("\"%TEMP%\"");
    CHECK(!resolved_tmp.empty() && resolved_tmp.find('%') == std::string::npos && core::path::is_directory(resolved_tmp));
    const std::string resolved_dot = core::path::resolve_input(".");
    CHECK(resolved_dot.size() >= 3 && resolved_dot[1] == ':');

    // 3. Unicode case-insensitive prefix matching
    CHECK(core::text::starts_with_icase("Привет мир", "прив"));
    CHECK(core::text::starts_with_icase("Überraschung", "üb"));
    CHECK(!core::text::starts_with_icase("Привет", "пока"));

    // 4. Recycle Bin Original location sorting (SortKey::type sorts by parent of original_path)
    {
        model::Listing L;
        std::vector<core::FileEntry> items;
        core::FileEntry a, b;
        a.name = "$R1.png"; a.kind = core::FileKind::image; a.set_recycle("photo.png", "Z:\\zeta\\photo.png");
        b.name = "$R2.txt"; b.kind = core::FileKind::text;  b.set_recycle("notes.txt", "A:\\alpha\\notes.txt");
        items.push_back(std::move(a));
        items.push_back(std::move(b));
        L.append(std::move(items));
        L.finish();
        L.set_sort({model::SortKey::type, true});
        CHECK(L.rows() == 2 && L.at(0).name == "$R2.txt" && L.at(1).name == "$R1.png");
    }

    // 5. Rich FileInfo: image resolution via WIC, drive capacity, MOTW detection + unblock, async FileInfoJob
    const std::string dir = make_temp_dir();
    ::CreateDirectoryW(core::text::widen(dir).c_str(), nullptr);
    // write a minimal valid 2x3 24-bit BMP (54-byte header + 3 rows of 8 padded bytes = 78 bytes)
    const std::string bmp_path = core::path::join(dir, "sample.bmp");
    {
        unsigned char bmp[78]{};
        bmp[0] = 'B'; bmp[1] = 'M';
        bmp[2] = 78;  bmp[10] = 54;
        bmp[14] = 40; // BITMAPINFOHEADER
        bmp[18] = 2;  // width = 2
        bmp[22] = 3;  // height = 3
        bmp[26] = 1;  // planes
        bmp[28] = 24; // 24 bpp
        std::ofstream out(core::text::widen(bmp_path), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bmp), sizeof bmp);
    }
    const core::FileInfo img_info = core::query_file_info(bmp_path);
    CHECK(img_info.exists && img_info.img_width == 2 && img_info.img_height == 3 && img_info.img_bpp == 24);
    CHECK(!img_info.meta_groups.empty() && img_info.meta_groups.front().title == "Image");
    CHECK(img_info.media_summary.find("2 \xc3\x97 3") != std::string::npos);

    // drive root info
    const core::FileInfo drv = core::query_file_info("C:\\");
    CHECK(drv.exists && drv.is_drive && drv.drive_total > 0);

    // MOTW Zone.Identifier detection & unblock
    const std::string dl_path = core::path::join(dir, "downloaded.txt");
    { std::ofstream(core::text::widen(dl_path), std::ios::binary) << "hello"; }
    {
        const std::wstring ads = core::text::widen(dl_path) + L":Zone.Identifier";
        HANDLE h = ::CreateFileW(ads.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const char data[] = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://example.com/file.zip\r\n";
            DWORD written = 0;
            ::WriteFile(h, data, sizeof(data) - 1, &written, nullptr);
            ::CloseHandle(h);
            const core::FileInfo blocked = core::query_file_info(dl_path);
            CHECK(blocked.motw_blocked && blocked.motw_zone == 3 && blocked.motw_host == "https://example.com/file.zip");
            CHECK(core::unblock_file(dl_path));
            const core::FileInfo unblocked = core::query_file_info(dl_path);
            CHECK(!unblocked.motw_blocked);
        }
    }

    // async FileInfoJob
    {
        core::FileInfoJob job;
        std::atomic<bool> woken{false};
        job.start({bmp_path, dl_path}, [&] { woken.store(true); });
        for (int i = 0; i < 200 && !job.ready(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds{10}); }
        CHECK(job.ready() && woken.load());
        const auto res = job.take();
        CHECK(res.size() == 2 && res[0].img_width == 2 && res[1].exists);
    }

    ::DeleteFileW(core::text::widen(bmp_path).c_str());
    ::DeleteFileW(core::text::widen(dl_path).c_str());

    // UNC server root detection
    CHECK(core::path::is_unc_server("\\\\server"));
    CHECK(core::path::is_unc_server("\\\\wsl.localhost"));
    CHECK(!core::path::is_unc_server("\\\\server\\share"));
    CHECK(!core::path::is_unc_server("C:\\"));
    CHECK(core::path::is_root("\\\\server"));
    CHECK(core::path::kind("\\\\server") == core::path::Kind::folder);
    {
        const auto segs = core::path::segments("\\\\server");
        CHECK(segs.size() == 1 && segs[0].name == "\\\\server" && segs[0].path == "\\\\server");
    }

    // Tab history dropdowns & jump_history
    {
        model::Tab tab{[] {}};
        tab.navigate("C:\\");
        tab.navigate("C:\\Windows");
        tab.navigate("C:\\Windows\\System32");
        const auto back = tab.back_history();
        CHECK(back.size() == 2 && back[0].second == "C:\\Windows" && back[1].second == "C:\\");
        CHECK(tab.forward_history().empty());
        CHECK(tab.jump_history(back[1].first) && tab.path() == "C:\\");
        const auto fwd = tab.forward_history();
        CHECK(fwd.size() == 2 && fwd[0].second == "C:\\Windows" && fwd[1].second == "C:\\Windows\\System32");
        CHECK(tab.jump_history(fwd[1].first) && tab.path() == "C:\\Windows\\System32");
    }

    // WSL & Google Drive discovery (must never crash or hang)
    {
        const auto wsl = core::wsl_distros();
        const auto gd  = core::google_drive_places();
        for (const auto& p : wsl) { CHECK(p.wsl && p.path.starts_with("\\\\wsl.localhost\\")); }
        for (const auto& p : gd)  { CHECK(p.cloud && !p.path.empty()); }
    }

    // .lnk shortcut creation
    {
        const std::string lnk = core::path::join(dir, "test_shortcut.lnk");
        CHECK(core::shell::create_shortcut("C:\\Windows\\notepad.exe", lnk));
        CHECK(core::path::exists(lnk));
        ::DeleteFileW(core::text::widen(lnk).c_str());
    }

    // rclone virtual path helpers & safe discovery
    {
        CHECK(core::rclone::is_path("rclone:gdrive:"));
        CHECK(core::rclone::is_path("rclone:gdrive:\\Photos\\2026"));
        CHECK(!core::rclone::is_path("C:\\Users"));
        CHECK(core::rclone::normalize_path("rclone:gdrive:/Photos/2026/") == "rclone:gdrive:\\Photos\\2026");
        CHECK(core::rclone::normalize_path("rclone:gdrive:/") == "rclone:gdrive:");
        const auto sp = core::rclone::split("rclone:gdrive:\\Photos\\2026");
        CHECK(sp.remote == "gdrive" && sp.rel_unix == "Photos/2026");
        CHECK(core::path::is_root("rclone:gdrive:"));
        CHECK(!core::path::is_root("rclone:gdrive:\\Photos"));
        CHECK(core::path::parent("rclone:gdrive:\\Photos\\2026") == "rclone:gdrive:\\Photos");
        CHECK(core::path::parent("rclone:gdrive:\\Photos") == "rclone:gdrive:");
        CHECK(core::path::parent("rclone:gdrive:").empty());
        CHECK(core::path::leaf("rclone:gdrive:\\Photos\\cat.png") == "cat.png");
        CHECK(core::path::leaf("rclone:gdrive:") == "gdrive");
        CHECK(core::path::join("rclone:gdrive:", "Photos") == "rclone:gdrive:\\Photos");
        CHECK(core::path::same_volume("rclone:gdrive:\\a", "rclone:gdrive:\\b"));
        CHECK(!core::path::same_volume("rclone:gdrive:\\a", "C:\\temp"));
        const auto segs = core::path::segments("rclone:gdrive:\\Photos\\2026");
        CHECK(segs.size() == 3 && segs[0].name == "gdrive" && segs[0].path == "rclone:gdrive:" && segs[2].path == "rclone:gdrive:\\Photos\\2026");
        (void)core::rclone::available();
        (void)core::rclone::list_remotes();
        core::rclone::shutdown();
    }

    ::RemoveDirectoryW(core::text::widen(dir).c_str());
}

int main()
{
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    test_virtual_files();
    test_terminal();
    test_i18n();
    test_sidebar_layout();
    test_sidebar_phase3();
    test_path_probe();
    test_recycle_restore();
    test_drive_watcher();
    test_shell_menu();
    test_default_manager();
    test_recycle_bin_handler();
    test_text();
    test_path();
    test_query();
    test_listing();
    test_grouping();
    test_sessions();
    test_tags();
    test_treemap_and_usage();
    test_folder_compare();
    test_transfer();
    test_file_index();
    test_links();
    test_empty_folders();
    test_image_tools();
    test_loader_and_ops();
    test_batch_rename();
    test_duplicates_and_sizes();
    test_sort_options();
    test_saved_search();
    test_git_status();
    test_templates_and_archives();
    test_relaunch_and_regressions();
    test_phase4();
    test_explorer_upgrades();
    std::printf(g_fail == 0 ? "all passed\n" : "%d failed\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
#else
static void test_linux_core()
{
    // text & natural sort
    CHECK(core::text::compare_natural("file2.txt", "file10.txt") < 0);
    CHECK(core::text::starts_with_icase("PrismFS", "prism"));

    // POSIX path helpers
    CHECK(core::path::is_root("/"));
    CHECK(!core::path::is_root("/home/user"));
    CHECK(core::path::normalize("/home/user/docs/") == "/home/user/docs");
    CHECK(core::path::parent("/home/user/docs") == "/home/user");
    CHECK(core::path::parent("/home") == "/");
    CHECK(core::path::parent("/").empty());
    CHECK(core::path::leaf("/home/user/docs") == "docs");
    CHECK(core::path::join("/home/user", "docs") == "/home/user/docs");
    const auto segs = core::path::segments("/home/user/docs");
    CHECK(segs.size() == 4 && segs[0].path == "/" && segs[3].name == "docs");

    // rclone virtual paths
    CHECK(core::rclone::is_path("rclone:gdrive:\\Photos\\2026"));
    CHECK(core::path::is_root("rclone:gdrive:"));
    CHECK(core::path::parent("rclone:gdrive:\\Photos\\2026") == "rclone:gdrive:\\Photos");
    CHECK(core::path::leaf("rclone:gdrive:\\Photos\\cat.png") == "cat.png");

    // real temp folder: FileOps, DirectoryLoader, FileInfo, Links
    const std::string dir = "/tmp/prismfs_selftest_" + std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0755);
    const std::string made_dir = core::FileOps::create_folder(dir);
    CHECK(!made_dir.empty() && core::path::is_directory(core::path::join(dir, made_dir)));
    const std::string made_file = core::FileOps::create_file(dir, "hello", ".txt");
    CHECK(!made_file.empty());
    const std::string fp = core::path::join(dir, made_file);
    const core::FileInfo fi = core::query_file_info(fp);
    CHECK(fi.exists && !fi.is_dir && fi.name == made_file);
    CHECK(core::FileOps::rename(fp, "renamed.txt"));
    const std::string rfp = core::path::join(dir, "renamed.txt");
    CHECK(core::path::exists(rfp));

    // DirectoryLoader
    {
        core::DirectoryLoader loader{[] {}};
        loader.start(dir);
        std::vector<core::FileEntry> entries;
        for (int i = 0; i < 200; ++i) {
            const auto prog = loader.poll(entries);
            if (prog.done) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        CHECK(entries.size() >= 2);
    }

    // places & process
    CHECK(!core::drives().empty() || !core::home_folder().empty());
    const core::ProcessResult pr = core::run_process(L"echo prism", L"", 5000);
    CHECK(pr.started && pr.exit_code == 0 && pr.output.find("prism") != std::string::npos);

    // cleanup
    (void)core::FileOps::remove_empty_file(rfp);
    (void)core::FileOps::remove_empty_folder(core::path::join(dir, made_dir));
    ::rmdir(dir.c_str());
}

int main()
{
    test_linux_core();
    std::printf(g_fail == 0 ? "all passed\n" : "%d failed\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
#endif
