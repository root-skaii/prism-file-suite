// prismfs_selftest: logic checks for core/ and model/ (no window, no gpu). exit code 0 = all passed

#include "core/directory_loader.hpp"
#include "core/file_ops.hpp"
#include "core/path.hpp"
#include "core/text.hpp"
#include "model/listing.hpp"
#include "model/tab.hpp"
#include "model/undo_stack.hpp"

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace prism;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

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

int main()
{
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    test_text();
    test_path();
    test_query();
    test_listing();
    test_loader_and_ops();
    std::printf(g_fail == 0 ? "all passed\n" : "%d failed\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
