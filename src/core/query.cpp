#include "core/query.hpp"

#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#endif

#include <algorithm>
#include <bitset>
#include <charconv>
#include <cmath>
#include <limits>

namespace prism::core {

namespace {

constexpr std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();

[[nodiscard]] constexpr char fold_char(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

// bytes of the utf-8 sequence starting at s[i]
[[nodiscard]] std::size_t utf8_len(std::string_view s, std::size_t i) noexcept
{
    std::size_t n = 1;
    while (i + n < s.size() && (static_cast<unsigned char>(s[i + n]) & 0xC0) == 0x80) { ++n; }
    return n;
}

[[nodiscard]] bool contains(std::string_view hay, std::string_view needle, bool fold) noexcept
{
    if (needle.empty()) { return true; }
    if (hay.size() < needle.size()) { return false; }
    if (!fold) { return hay.find(needle) != std::string_view::npos; }
    const char first = needle[0];
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (fold_char(hay[i]) != first) { continue; }
        std::size_t k = 1;
        while (k < needle.size() && fold_char(hay[i + k]) == needle[k]) { ++k; }
        if (k == needle.size()) { return true; }
    }
    return false;
}

[[nodiscard]] bool ends_with(std::string_view hay, std::string_view tail, bool fold) noexcept
{
    if (hay.size() < tail.size()) { return false; }
    hay.remove_prefix(hay.size() - tail.size());
    for (std::size_t i = 0; i < tail.size(); ++i) {
        if ((fold ? fold_char(hay[i]) : hay[i]) != tail[i]) { return false; }
    }
    return true;
}

// `*` any run, `?` one character; the whole of `s` must match. iterative with one backtrack point: no blow-up
[[nodiscard]] bool glob_match(std::string_view pat, std::string_view s, bool fold) noexcept
{
    constexpr std::size_t none = std::string_view::npos;
    std::size_t p = 0, i = 0, star = none, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (p < pat.size() && pat[p] == '?') {
            ++p;
            i += utf8_len(s, i);
        } else if (p < pat.size() && pat[p] == (fold ? fold_char(s[i]) : s[i])) {
            ++p;
            ++i;
        } else if (star != none) {
            p    = star + 1;
            mark += utf8_len(s, mark);
            i    = mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') { ++p; }
    return p == pat.size();
}

// --- regular expressions ----------------------------------------------------------------------------------------
// std::regex throws on a bad pattern and the project builds without exceptions, so this is a small own engine: literals,
// . [] [^] ^ $ | ( ) (?: ) * + ? {n,m} (and lazy forms), \d \w \s \D \W \S and escapes. no back-references or look-around.
// matching is a memoised backtracking walk over (instruction, position): O(program x name length), never exponential.

} // namespace

struct QueryRegex {
    enum class Op : std::uint8_t { byte, any, set, split, jmp, bol, eol, match };
    struct Inst {
        Op            op{};
        unsigned char c{};
        int           x{};
        int           y{};
    };
    std::vector<Inst>              prog;
    std::vector<std::bitset<256>>  sets;
    bool                           fold{};
    bool                           anchored{}; // starts with ^: only position 0 can begin a match

    [[nodiscard]] bool search(std::string_view s) const;
};

namespace {

constexpr std::size_t max_name_scan   = 1024;
constexpr std::size_t max_program     = 2000;
constexpr std::size_t max_pattern     = 256;
constexpr int         max_repeat      = 100;

bool QueryRegexSearch(const QueryRegex& re, std::string_view s)
{
    using Op = QueryRegex::Op;
    if (s.size() > max_name_scan) { s = s.substr(0, max_name_scan); }
    const std::size_t width = s.size() + 1;
    thread_local std::vector<std::uint64_t>                  visited;
    thread_local std::vector<std::pair<int, std::size_t>>    stack;
    visited.assign((re.prog.size() * width + 63) / 64, 0);

    const std::size_t last_start = re.anchored ? 0 : s.size();
    for (std::size_t start = 0; start <= last_start; ++start) {
        stack.clear();
        stack.emplace_back(0, start);
        while (!stack.empty()) {
            auto [pc, pos] = stack.back();
            stack.pop_back();
            for (;;) {
                const std::size_t bit = static_cast<std::size_t>(pc) * width + pos;
                std::uint64_t&    word = visited[bit >> 6];
                const std::uint64_t mask = std::uint64_t{1} << (bit & 63);
                if ((word & mask) != 0) { break; }
                word |= mask;
                const QueryRegex::Inst& in = re.prog[static_cast<std::size_t>(pc)];
                bool alive = true;
                switch (in.op) {
                case Op::match: return true;
                case Op::byte:
                    if (pos < s.size() && (re.fold ? fold_char(s[pos]) : s[pos]) == static_cast<char>(in.c)) { ++pos; ++pc; } else { alive = false; }
                    break;
                case Op::any:
                    if (pos < s.size()) { pos += utf8_len(s, pos); ++pc; } else { alive = false; }
                    break;
                case Op::set:
                    if (pos < s.size() && re.sets[static_cast<std::size_t>(in.x)].test(static_cast<unsigned char>(s[pos]))) {
                        pos += static_cast<unsigned char>(s[pos]) >= 0xC0 ? utf8_len(s, pos) : 1;
                        ++pc;
                    } else {
                        alive = false;
                    }
                    break;
                case Op::split:
                    stack.emplace_back(in.y, pos);
                    pc = in.x;
                    break;
                case Op::jmp: pc = in.x; break;
                case Op::bol: if (pos == 0) { ++pc; } else { alive = false; } break;
                case Op::eol: if (pos == s.size()) { ++pc; } else { alive = false; } break;
                }
                if (!alive) { break; }
            }
        }
    }
    return false;
}

struct RegexCompiler {
    enum class K : std::uint8_t { empty, byte, any, set, bol, eol, cat, alt, star, plus, quest, repeat };
    struct Node {
        K             k{};
        unsigned char c{};
        int           set{-1};
        int           a{-1};
        int           b{-1};
        int           lo{};
        int           hi{};   // repeat: -1 = unbounded
        bool          lazy{};
    };

    std::string_view               src;
    std::size_t                    i{};
    std::string                    error;
    std::vector<Node>              nodes;
    std::vector<std::bitset<256>>  sets;
    bool                           fold{};
    int                            depth{};
    QueryRegex*                    out{};

    int add(Node n)
    {
        nodes.push_back(n);
        return static_cast<int>(nodes.size()) - 1;
    }
    int fail(const char* why)
    {
        if (error.empty()) { error = why; }
        return -1;
    }
    [[nodiscard]] bool more() const noexcept { return i < src.size(); }

    static std::bitset<256> class_bits(char cls)
    {
        std::bitset<256> b;
        const char lower = fold_char(cls);
        if (lower == 'd') {
            for (int c = '0'; c <= '9'; ++c) { b.set(static_cast<std::size_t>(c)); }
        } else if (lower == 'w') {
            for (int c = 0; c < 256; ++c) {
                const bool word = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
                if (word) { b.set(static_cast<std::size_t>(c)); }
            }
        } else {
            for (const char c : {' ', '\t', '\r', '\n', '\f', '\v'}) { b.set(static_cast<unsigned char>(c)); }
        }
        if (cls >= 'A' && cls <= 'Z') { b.flip(); }
        return b;
    }

    // an escaped single character; -1 = not one (a letter that means something else, or nothing)
    int escaped_char(char e)
    {
        switch (e) {
        case 't': return '\t';
        case 'n': return '\n';
        case 'r': return '\r';
        default: break;
        }
        const bool alnum = (e >= '0' && e <= '9') || (e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z');
        if (alnum) { return fail("unsupported escape"); }
        return static_cast<unsigned char>(e);
    }

    int literal(unsigned char c)
    {
        Node n;
        n.k = K::byte;
        n.c = static_cast<unsigned char>(fold ? fold_char(static_cast<char>(c)) : static_cast<char>(c));
        return add(n);
    }

    int parse_set()
    {
        ++i; // [
        bool negate = false;
        if (more() && src[i] == '^') { negate = true; ++i; }
        std::bitset<256> bits;
        bool first = true, closed = false;
        const auto one = [&](int& v) -> bool {
            const unsigned char c = static_cast<unsigned char>(src[i]);
            if (c == '\\') {
                ++i;
                if (!more()) { fail("trailing \\"); return false; }
                v = escaped_char(src[i++]);
                return v >= 0;
            }
            if (c >= 0x80) { fail("non-ascii characters in [] are not supported"); return false; }
            v = c;
            ++i;
            return true;
        };
        while (more()) {
            const char c = src[i];
            if (c == ']' && !first) { closed = true; ++i; break; }
            first = false;
            if (c == '\\' && i + 1 < src.size() && std::string_view{"dDwWsS"}.find(src[i + 1]) != std::string_view::npos) {
                const char cls = src[i + 1];
                if (cls >= 'A' && cls <= 'Z') { fail("negated class inside [] is not supported"); return -1; }
                bits |= class_bits(cls);
                i += 2;
                continue;
            }
            int lo = 0;
            if (!one(lo)) { return -1; }
            if (i + 1 < src.size() && src[i] == '-' && src[i + 1] != ']') {
                ++i;
                int hi = 0;
                if (!one(hi)) { return -1; }
                if (hi < lo) { return fail("bad range in []"); }
                for (int v = lo; v <= hi; ++v) { bits.set(static_cast<std::size_t>(v)); }
            } else {
                bits.set(static_cast<std::size_t>(lo));
            }
        }
        if (!closed) { return fail("missing ]"); }
        if (fold) {
            for (int c = 'a'; c <= 'z'; ++c) {
                if (bits.test(static_cast<std::size_t>(c)) || bits.test(static_cast<std::size_t>(c - 32))) {
                    bits.set(static_cast<std::size_t>(c));
                    bits.set(static_cast<std::size_t>(c - 32));
                }
            }
        }
        if (negate) { bits.flip(); }
        sets.push_back(bits);
        Node n;
        n.k   = K::set;
        n.set = static_cast<int>(sets.size()) - 1;
        return add(n);
    }

    int parse_atom()
    {
        const char c = src[i];
        switch (c) {
        case '(': {
            ++i;
            if (src.substr(i, 2) == "?:") { i += 2; }
            if (++depth > 16) { return fail("groups nested too deep"); }
            const int inner = parse_alt();
            if (inner < 0) { return -1; }
            if (!more() || src[i] != ')') { return fail("missing )"); }
            ++i;
            --depth;
            return inner;
        }
        case '[': return parse_set();
        case '.': { ++i; Node n; n.k = K::any; return add(n); }
        case '^': { ++i; Node n; n.k = K::bol; return add(n); }
        case '$': { ++i; Node n; n.k = K::eol; return add(n); }
        case '*': case '+': case '?': return fail("nothing to repeat");
        case '\\': {
            ++i;
            if (!more()) { return fail("trailing \\"); }
            const char e = src[i++];
            if (std::string_view{"dDwWsS"}.find(e) != std::string_view::npos) {
                sets.push_back(class_bits(e));
                Node n;
                n.k   = K::set;
                n.set = static_cast<int>(sets.size()) - 1;
                return add(n);
            }
            const int v = escaped_char(e);
            return v < 0 ? -1 : literal(static_cast<unsigned char>(v));
        }
        default: break;
        }
        // a literal; a multi-byte character is one unit so a quantifier covers all its bytes
        const std::size_t len = utf8_len(src, i);
        int seq = -1;
        for (std::size_t k = 0; k < len; ++k) {
            const int b = literal(static_cast<unsigned char>(src[i + k]));
            if (seq < 0) {
                seq = b;
            } else {
                Node cat;
                cat.k = K::cat;
                cat.a = seq;
                cat.b = b;
                seq   = add(cat);
            }
        }
        i += len;
        return seq;
    }

    // `{n}`, `{n,}`, `{n,m}` at src[i] == '{'; false (and i unchanged) when it is not a quantifier: then '{' is a literal
    bool parse_braces(int& lo, int& hi)
    {
        std::size_t j = i + 1;
        const auto number = [&](int& v) {
            const std::size_t from = j;
            v = 0;
            while (j < src.size() && src[j] >= '0' && src[j] <= '9' && v < 10000) { v = v * 10 + (src[j] - '0'); ++j; }
            return j > from;
        };
        if (!number(lo)) { return false; }
        hi = lo;
        if (j < src.size() && src[j] == ',') {
            ++j;
            if (j < src.size() && src[j] == '}') { hi = -1; } else if (!number(hi)) { return false; }
        }
        if (j >= src.size() || src[j] != '}') { return false; }
        i = j + 1;
        return true;
    }

    int parse_repeat()
    {
        int atom = parse_atom();
        if (atom < 0) { return -1; }
        while (more()) {
            Node q;
            q.a = atom;
            const char c = src[i];
            if (c == '*') { q.k = K::star; ++i; }
            else if (c == '+') { q.k = K::plus; ++i; }
            else if (c == '?') { q.k = K::quest; ++i; }
            else if (c == '{') {
                int lo = 0, hi = 0;
                if (!parse_braces(lo, hi)) { break; }
                if (lo > max_repeat || hi > max_repeat || (hi >= 0 && hi < lo)) { return fail("bad repeat count"); }
                q.k  = K::repeat;
                q.lo = lo;
                q.hi = hi;
            } else {
                break;
            }
            if (more() && src[i] == '?') { q.lazy = true; ++i; }
            atom = add(q);
        }
        return atom;
    }

    int parse_cat()
    {
        int seq = -1;
        while (more() && src[i] != '|' && src[i] != ')') {
            const int r = parse_repeat();
            if (r < 0) { return -1; }
            if (seq < 0) {
                seq = r;
            } else {
                Node cat;
                cat.k = K::cat;
                cat.a = seq;
                cat.b = r;
                seq   = add(cat);
            }
        }
        if (seq < 0) { Node e; e.k = K::empty; seq = add(e); }
        return seq;
    }

    int parse_alt()
    {
        int left = parse_cat();
        if (left < 0) { return -1; }
        while (more() && src[i] == '|') {
            ++i;
            const int right = parse_cat();
            if (right < 0) { return -1; }
            Node alt;
            alt.k = K::alt;
            alt.a = left;
            alt.b = right;
            left  = add(alt);
        }
        return left;
    }

    // --- code generation
    int emit(QueryRegex::Op op, unsigned char c = 0, int x = 0, int y = 0)
    {
        if (out->prog.size() >= max_program) { fail("pattern too complex"); return -1; }
        out->prog.push_back({op, c, x, y});
        return static_cast<int>(out->prog.size()) - 1;
    }
    [[nodiscard]] int here() const noexcept { return static_cast<int>(out->prog.size()); }

    bool gen_star(int body, bool lazy)
    {
        using Op = QueryRegex::Op;
        const int s = emit(Op::split);
        if (s < 0) { return false; }
        const int start = here();
        if (!gen(body)) { return false; }
        if (emit(Op::jmp, 0, s) < 0) { return false; }
        const int end = here();
        out->prog[static_cast<std::size_t>(s)].x = lazy ? end : start;
        out->prog[static_cast<std::size_t>(s)].y = lazy ? start : end;
        return true;
    }

    bool gen(int n)
    {
        using Op = QueryRegex::Op;
        if (n < 0 || !error.empty()) { return false; }
        const Node& nd = nodes[static_cast<std::size_t>(n)];
        switch (nd.k) {
        case K::empty: return true;
        case K::byte: return emit(Op::byte, nd.c) >= 0;
        case K::any:  return emit(Op::any) >= 0;
        case K::set:  return emit(Op::set, 0, nd.set) >= 0;
        case K::bol:  return emit(Op::bol) >= 0;
        case K::eol:  return emit(Op::eol) >= 0;
        case K::cat:  return gen(nd.a) && gen(nd.b);
        case K::alt: {
            const int s = emit(Op::split);
            if (s < 0) { return false; }
            out->prog[static_cast<std::size_t>(s)].x = here();
            if (!gen(nd.a)) { return false; }
            const int j = emit(Op::jmp);
            if (j < 0) { return false; }
            out->prog[static_cast<std::size_t>(s)].y = here();
            if (!gen(nd.b)) { return false; }
            out->prog[static_cast<std::size_t>(j)].x = here();
            return true;
        }
        case K::star: return gen_star(nd.a, nd.lazy);
        case K::plus: {
            const int start = here();
            if (!gen(nd.a)) { return false; }
            const int s = emit(Op::split);
            if (s < 0) { return false; }
            out->prog[static_cast<std::size_t>(s)].x = nd.lazy ? s + 1 : start;
            out->prog[static_cast<std::size_t>(s)].y = nd.lazy ? start : s + 1;
            return true;
        }
        case K::quest: {
            const int s = emit(Op::split);
            if (s < 0) { return false; }
            if (!gen(nd.a)) { return false; }
            const int end = here();
            out->prog[static_cast<std::size_t>(s)].x = nd.lazy ? end : s + 1;
            out->prog[static_cast<std::size_t>(s)].y = nd.lazy ? s + 1 : end;
            return true;
        }
        case K::repeat: {
            for (int k = 0; k < nd.lo; ++k) { if (!gen(nd.a)) { return false; } }
            if (nd.hi < 0) { return gen_star(nd.a, nd.lazy); }
            std::vector<int> splits;
            for (int k = nd.lo; k < nd.hi; ++k) {
                const int s = emit(Op::split);
                if (s < 0) { return false; }
                splits.push_back(s);
                if (!gen(nd.a)) { return false; }
            }
            const int end = here();
            for (const int s : splits) {
                QueryRegex::Inst& in = out->prog[static_cast<std::size_t>(s)];
                in.x = nd.lazy ? end : s + 1;
                in.y = nd.lazy ? s + 1 : end;
            }
            return true;
        }
        }
        return false;
    }
};

[[nodiscard]] std::shared_ptr<const QueryRegex> compile_regex(std::string_view pattern, bool fold, std::string& error)
{
    if (pattern.size() > max_pattern) {
        error = "pattern too long";
        return nullptr;
    }
    auto re  = std::make_shared<QueryRegex>();
    re->fold = fold;
    RegexCompiler c;
    c.src  = pattern;
    c.fold = fold;
    c.out  = re.get();
    const int root = c.parse_alt();
    if (root >= 0 && c.more()) { c.fail("unmatched )"); }
    if (!c.error.empty()) {
        error = c.error;
        return nullptr;
    }
    re->sets = std::move(c.sets);
    if (!c.gen(root) || c.emit(QueryRegex::Op::match) < 0) {
        error = c.error.empty() ? "pattern too complex" : c.error;
        return nullptr;
    }
    re->anchored = !re->prog.empty() && re->prog[0].op == QueryRegex::Op::bol;
    return re;
}

// --- time --------------------------------------------------------------------------------------------------------

constexpr std::uint64_t ticks_per_day = 864'000'000'000ULL;
constexpr std::int64_t  days_1601_to_1970 = 134774;

// days since 1970-01-01 of a proleptic gregorian date (Howard Hinnant's algorithm)
[[nodiscard]] constexpr std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept
{
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] constexpr unsigned days_in_month(int y, unsigned m) noexcept
{
    constexpr unsigned table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29u : table[m - 1];
}

struct Range {
    std::uint64_t lo{0};
    std::uint64_t hi{u64_max};
};

struct Clock {
    std::uint64_t now{};
    std::int64_t  offset{}; // local = utc + offset, in ticks
};

[[nodiscard]] Clock make_clock(const QueryOptions& o)
{
    Clock c;
    c.now = o.now;
    if (c.now == 0) {
#ifdef _WIN32
        FILETIME ft;
        ::GetSystemTimeAsFileTime(&ft);
        c.now = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#else
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        c.now = (static_cast<std::uint64_t>(secs) + 11644473600ULL) * 10000000ULL;
#endif
    }
    int minutes = 0;
    if (o.utc_offset_minutes) {
        minutes = *o.utc_offset_minutes;
    } else {
#ifdef _WIN32
        TIME_ZONE_INFORMATION tz{};
        const DWORD id = ::GetTimeZoneInformation(&tz);
        minutes = -(tz.Bias + (id == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias));
#else
        const std::time_t now_t = std::time(nullptr);
        std::tm local_tm{};
        ::localtime_r(&now_t, &local_tm);
        minutes = static_cast<int>(local_tm.tm_gmtoff / 60);
#endif
    }
    c.offset = static_cast<std::int64_t>(minutes) * 600'000'000LL;
    return c;
}

// the utc tick a local civil day starts at
[[nodiscard]] std::uint64_t local_midnight(const Clock& ck, int y, unsigned m, unsigned d) noexcept
{
    const std::int64_t t = (days_from_civil(y, m, d) + days_1601_to_1970) * static_cast<std::int64_t>(ticks_per_day) - ck.offset;
    return t < 0 ? 0 : static_cast<std::uint64_t>(t);
}

[[nodiscard]] bool parse_uint(std::string_view s, int& out) noexcept
{
    if (s.empty() || s.size() > 9) { return false; }
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

[[nodiscard]] std::string lower(std::string_view s) { return text::lower_ascii(s); }

// one date: a keyword, "3d" (within the last 3 days), "2026", "2026-10" or "2026-10-19". the range is the whole span
[[nodiscard]] bool date_scalar(std::string_view v, const Clock& ck, Range& out)
{
    const std::string w = lower(v);
    const std::int64_t local_now = static_cast<std::int64_t>(ck.now) + ck.offset;
    const auto today_start = static_cast<std::uint64_t>(local_now - local_now % static_cast<std::int64_t>(ticks_per_day) - ck.offset);
    const auto within = [&](std::uint64_t span) {
        out.lo = ck.now > span ? ck.now - span : 0;
        out.hi = u64_max;
        return true;
    };
    if (w == "today")     { out = {today_start, today_start + ticks_per_day - 1}; return true; }
    if (w == "yesterday") { out = {today_start - ticks_per_day, today_start - 1}; return true; }
    if (w == "week")      { return within(7 * ticks_per_day); }
    if (w == "month")     { return within(30 * ticks_per_day); }
    if (w == "year")      { return within(365 * ticks_per_day); }
    if (w.size() >= 2 && (w.back() == 'd' || w.back() == 'h' || w.back() == 'w')) {
        int n = 0;
        if (parse_uint(std::string_view{w}.substr(0, w.size() - 1), n)) {
            const std::uint64_t unit = w.back() == 'h' ? ticks_per_day / 24 : (w.back() == 'w' ? 7 * ticks_per_day : ticks_per_day);
            return within(static_cast<std::uint64_t>(n) * unit);
        }
    }
    // y[-m[-d]]
    int part[3]{};
    int count = 0;
    std::size_t from = 0;
    while (count < 3) {
        std::size_t to = w.find_first_of("-/.", from);
        if (to == std::string::npos) { to = w.size(); }
        if (!parse_uint(std::string_view{w}.substr(from, to - from), part[count])) { return false; }
        ++count;
        if (to >= w.size()) { from = w.size(); break; }
        from = to + 1;
    }
    if (from < w.size() || count == 0) { return false; }
    const int y = part[0];
    if (y < 1601 || y > 9999) { return false; }
    if (count == 1) {
        out = {local_midnight(ck, y, 1, 1), local_midnight(ck, y + 1, 1, 1) - 1};
        return true;
    }
    const auto m = static_cast<unsigned>(part[1]);
    if (m < 1 || m > 12) { return false; }
    if (count == 2) {
        out = {local_midnight(ck, y, m, 1), (m == 12 ? local_midnight(ck, y + 1, 1, 1) : local_midnight(ck, y, m + 1, 1)) - 1};
        return true;
    }
    const auto d = static_cast<unsigned>(part[2]);
    if (d < 1 || d > days_in_month(y, m)) { return false; }
    out = {local_midnight(ck, y, m, d), local_midnight(ck, y, m, d) + ticks_per_day - 1};
    return true;
}

// "10mb", "1.5 gb", "300": bytes. 1024-based
[[nodiscard]] bool size_scalar(std::string_view v, Range& out)
{
    double number = 0.0;
    const auto r  = std::from_chars(v.data(), v.data() + v.size(), number);
    if (r.ec != std::errc{} || !(number >= 0.0)) { return false; }
    const std::string unit = lower(std::string_view{r.ptr, static_cast<std::size_t>(v.data() + v.size() - r.ptr)});
    double mult = 1.0;
    if (unit.empty() || unit == "b") { mult = 1.0; }
    else if (unit == "k" || unit == "kb" || unit == "kib") { mult = 1024.0; }
    else if (unit == "m" || unit == "mb" || unit == "mib") { mult = 1048576.0; }
    else if (unit == "g" || unit == "gb" || unit == "gib") { mult = 1073741824.0; }
    else if (unit == "t" || unit == "tb" || unit == "tib") { mult = 1099511627776.0; }
    else { return false; }
    const double bytes = std::floor(number * mult);
    if (bytes >= 1.8e19) { return false; }
    out = {static_cast<std::uint64_t>(bytes), static_cast<std::uint64_t>(bytes)};
    return true;
}

// ">x" ">=x" "<x" "<=x" "=x" "x" "a..b" "a.." "..b" over a scalar parser that yields the span one value covers
template <class Scalar>
[[nodiscard]] bool parse_range(std::string_view v, Scalar scalar, Range& out)
{
    out = {};
    if (const std::size_t dots = v.find(".."); dots != std::string_view::npos) {
        const std::string_view a = v.substr(0, dots), b = v.substr(dots + 2);
        if (a.empty() && b.empty()) { return false; }
        Range r;
        if (!a.empty()) { if (!scalar(a, r)) { return false; } out.lo = r.lo; }
        if (!b.empty()) { if (!scalar(b, r)) { return false; } out.hi = r.hi; }
        return true;
    }
    std::string_view op;
    for (const std::string_view candidate : {">=", "<=", ">", "<", "="}) {
        if (v.starts_with(candidate)) { op = candidate; break; }
    }
    v.remove_prefix(op.size());
    Range r;
    if (v.empty() || !scalar(v, r)) { return false; }
    if (op == ">")       { out.lo = r.hi == u64_max ? u64_max : r.hi + 1; }
    else if (op == ">=") { out.lo = r.lo; }
    else if (op == "<")  { if (r.lo == 0) { out = {1, 0}; } else { out.hi = r.lo - 1; } }
    else if (op == "<=") { out.hi = r.hi; }
    else                 { out = r; }
    return true;
}

[[nodiscard]] std::uint32_t kind_bit(FileKind k) noexcept
{
    return std::uint32_t{1} << static_cast<unsigned>(k);
}

// "image" -> the kinds it stands for; 0 = unknown
[[nodiscard]] std::uint32_t type_mask(std::string_view name)
{
    const std::string n = lower(name);
    using K = FileKind;
    if (n == "image" || n == "images" || n == "picture" || n == "pictures" || n == "photo") { return kind_bit(K::image); }
    if (n == "video" || n == "videos" || n == "movie") { return kind_bit(K::video); }
    if (n == "audio" || n == "music" || n == "sound") { return kind_bit(K::audio); }
    if (n == "archive" || n == "archives" || n == "zip" || n == "compressed") { return kind_bit(K::archive); }
    if (n == "doc" || n == "docs" || n == "document" || n == "documents") {
        return kind_bit(K::document) | kind_bit(K::spreadsheet) | kind_bit(K::presentation) | kind_bit(K::pdf) | kind_bit(K::text);
    }
    if (n == "sheet" || n == "spreadsheet") { return kind_bit(K::spreadsheet); }
    if (n == "slides" || n == "presentation") { return kind_bit(K::presentation); }
    if (n == "pdf") { return kind_bit(K::pdf); }
    if (n == "text") { return kind_bit(K::text); }
    if (n == "code" || n == "source") { return kind_bit(K::code); }
    if (n == "config") { return kind_bit(K::config); }
    if (n == "exe" || n == "executable" || n == "app" || n == "program") { return kind_bit(K::executable); }
    if (n == "lib" || n == "library") { return kind_bit(K::library); }
    if (n == "font" || n == "fonts") { return kind_bit(K::font); }
    if (n == "disk" || n == "iso" || n == "diskimage") { return kind_bit(K::disk_image); }
    if (n == "shortcut" || n == "link") { return kind_bit(K::shortcut); }
    if (n == "folder" || n == "folders" || n == "dir") { return kind_bit(K::folder); }
    if (n == "other") { return kind_bit(K::other); }
    return 0;
}

[[nodiscard]] std::string_view last_component(std::string_view name) noexcept
{
    const std::size_t slash = name.rfind('\\');
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

[[nodiscard]] bool holds(const QueryTerm& t, const FileEntryView& e, std::string_view leaf) noexcept
{
    using Kind = QueryTerm::Kind;
    const std::string_view subject = t.on_path ? e.name : leaf;
    switch (t.kind) {
    case Kind::substring: return contains(subject, t.text, t.fold);
    case Kind::glob:      return glob_match(t.text, subject, t.fold);
    case Kind::suffix:    return ends_with(subject, t.text, t.fold);
    case Kind::extension: {
        const std::string_view ext = e.ext;
        for (const std::string& want : t.list) {
            if (ext.size() == want.size() && ends_with(ext, want, t.fold)) { return true; }
        }
        return false;
    }
    case Kind::type:     return (t.mask & kind_bit(e.kind)) != 0;
    case Kind::folder:   return e.is_dir() == t.flag;
    case Kind::size:     return !e.is_dir() && e.size >= t.lo && e.size <= t.hi;
    case Kind::modified: return e.modified >= t.lo && e.modified <= t.hi;
    case Kind::hidden:   return e.is_hidden() == t.flag;
    case Kind::tag:      return t.flag ? e.tags == 0 : (e.tags & t.mask) != 0;
    case Kind::regex:    return t.re != nullptr && QueryRegexSearch(*t.re, leaf);
    }
    return false;
}

[[nodiscard]] std::string dequote(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) { if (c != '"') { out.push_back(c); } }
    return out;
}

[[nodiscard]] bool is_key(std::string_view k) noexcept
{
    return k == "ext" || k == "type" || k == "kind" || k == "size" || k == "modified" || k == "date" || k == "hidden" || k == "tag" || k == "label" || k == "re";
}

} // namespace

bool QueryRegex::search(std::string_view s) const { return QueryRegexSearch(*this, s); }

struct QueryBuilder {
    const QueryOptions& opt;
    Clock               clock;
    std::string         error;

    bool fail(std::string message)
    {
        if (error.empty()) { error = std::move(message); }
        return false;
    }

    bool key_term(std::string_view key, std::string_view value, QueryTerm& t)
    {
        using Kind = QueryTerm::Kind;
        t.fold = !opt.case_sensitive;
        if (key == "ext") {
            t.kind = Kind::extension;
            std::size_t from = 0;
            while (from <= value.size()) {
                std::size_t to = value.find_first_of(",;", from);
                if (to == std::string_view::npos) { to = value.size(); }
                std::string_view one = value.substr(from, to - from);
                while (one.starts_with('.')) { one.remove_prefix(1); }
                if (!one.empty()) { t.list.push_back(t.fold ? lower(one) : std::string{one}); }
                from = to + 1;
            }
            return !t.list.empty() || fail("ext: needs an extension");
        }
        if (key == "type") {
            t.kind = Kind::type;
            std::size_t from = 0;
            while (from <= value.size()) {
                std::size_t to = value.find(',', from);
                if (to == std::string_view::npos) { to = value.size(); }
                const std::string_view one = value.substr(from, to - from);
                if (!one.empty()) {
                    const std::uint32_t m = type_mask(one);
                    if (m == 0) { return fail("unknown type '" + std::string{one} + "' (image video audio doc code archive ...)"); }
                    t.mask |= m;
                }
                from = to + 1;
            }
            return t.mask != 0 || fail("type: needs a value");
        }
        if (key == "kind") {
            t.kind = Kind::folder;
            const std::string v = lower(value);
            if (v == "folder" || v == "folders" || v == "dir" || v == "directory") { t.flag = true; return true; }
            if (v == "file" || v == "files") { t.flag = false; return true; }
            return fail("kind: is folder or file");
        }
        if (key == "size") {
            t.kind = Kind::size;
            Range r;
            if (!parse_range(value, size_scalar, r)) { return fail("bad size '" + std::string{value} + "' (try >10mb or 1kb..1mb)"); }
            t.lo = r.lo;
            t.hi = r.hi;
            return true;
        }
        if (key == "modified" || key == "date") {
            t.kind = Kind::modified;
            Range r;
            const auto scalar = [this](std::string_view v, Range& out) { return date_scalar(v, clock, out); };
            if (!parse_range(value, scalar, r)) { return fail("bad date '" + std::string{value} + "' (try today, 7d, 2026-10 or >2026-01-01)"); }
            t.lo = r.lo;
            t.hi = r.hi;
            return true;
        }
        if (key == "hidden") {
            t.kind = Kind::hidden;
            const std::string v = lower(value);
            if (v == "yes" || v == "true" || v == "1" || v == "on") { t.flag = true; return true; }
            if (v == "no" || v == "false" || v == "0" || v == "off") { t.flag = false; return true; }
            return fail("hidden: is yes or no");
        }
        if (key == "tag" || key == "label") {
            t.kind = Kind::tag;
            std::size_t from = 0;
            while (from <= value.size()) {
                std::size_t to = value.find_first_of(",;", from);
                if (to == std::string_view::npos) { to = value.size(); }
                const std::string one = lower(value.substr(from, to - from));
                from = to + 1;
                if (one.empty()) { continue; }
                if (one == "any") { t.mask |= tag::all; continue; }
                if (one == "none") { t.flag = true; continue; }
                bool known = false;
                for (int i = 0; i < tag::count; ++i) {
                    if (tag::names[i] == one) { t.mask |= 1u << i; known = true; }
                }
                if (!known) { return fail("unknown label '" + one + "' (red orange yellow green teal blue purple pink any none)"); }
            }
            if (t.flag && t.mask != 0) { return fail("tag: none cannot be mixed with colours"); }
            return t.flag || t.mask != 0 || fail("tag: needs a colour");
        }
        return regex_term(value, t);
    }

    bool regex_term(std::string_view pattern, QueryTerm& t)
    {
        t.kind = QueryTerm::Kind::regex;
        t.fold = !opt.case_sensitive;
        std::string why;
        t.re = compile_regex(pattern, t.fold, why);
        return t.re != nullptr || fail("bad regex: " + why);
    }

    // one whitespace-delimited token. an empty value (`size:` still being typed) adds nothing
    bool add(std::vector<QueryTerm>& group, std::string_view raw)
    {
        QueryTerm t;
        if (raw.size() > 1 && raw[0] == '-') {
            t.negate = true;
            raw.remove_prefix(1);
        }
        const std::size_t colon = raw.find(':');
        if (colon != std::string_view::npos && colon > 0 && colon <= 8) {
            const std::string key = lower(raw.substr(0, colon));
            if (is_key(key)) {
                const std::string value = dequote(raw.substr(colon + 1));
                if (value.empty()) { return true; }
                if (!key_term(key, value, t)) { return false; }
                group.push_back(std::move(t));
                return true;
            }
        }
        const bool literal = raw.starts_with('"');
        std::string word   = dequote(raw);
        if (word.empty()) { return true; }
        t.fold = !opt.case_sensitive;
        if (opt.regex && !literal) {
            if (!regex_term(word, t)) { return false; }
            group.push_back(std::move(t));
            return true;
        }
        std::replace(word.begin(), word.end(), '/', '\\');
        t.on_path = word.find('\\') != std::string::npos;
        if (t.fold) { word = lower(word); }
        if (!literal && word.find_first_of("*?") != std::string::npos) {
            t.kind = QueryTerm::Kind::glob;
        } else if (!literal && word.size() > 1 && word[0] == '.') {
            t.kind = QueryTerm::Kind::suffix;
        } else {
            t.kind = QueryTerm::Kind::substring;
        }
        t.text = std::move(word);
        group.push_back(std::move(t));
        return true;
    }
};

Query Query::parse(std::string_view text, const QueryOptions& options)
{
    Query q;
    q.text_ = std::string{text};
    QueryBuilder b{options, make_clock(options), {}};

    std::vector<QueryTerm> group;
    const auto close_group = [&] {
        if (!group.empty()) { q.groups_.push_back(std::move(group)); }
        group.clear();
    };
    const auto blank = [](char c) { return static_cast<unsigned char>(c) <= ' '; };
    std::size_t i = 0;
    while (i < text.size()) {
        if (blank(text[i])) { ++i; continue; }
        if (!options.regex && text[i] == '|') { ++i; close_group(); continue; }
        const std::size_t start = i;
        // a regex keeps its own `|`: in regex mode, and in a re: term
        const std::string_view head = text.substr(start, 4);
        const bool own_pipe = options.regex || lower(head).starts_with("re:") || lower(head).starts_with("-re:");
        bool quoted = false;
        while (i < text.size()) {
            const char c = text[i];
            if (c == '"') { quoted = !quoted; }
            else if (!quoted && (blank(c) || (!own_pipe && c == '|'))) { break; }
            ++i;
        }
        const std::string_view raw = text.substr(start, i - start);
        if (raw == "OR") { close_group(); continue; }
        if (!b.add(group, raw)) { break; }
    }
    close_group();
    if (!b.error.empty()) {
        q.error_ = std::move(b.error);
        q.groups_.clear();
        return q;
    }
    for (const auto& g : q.groups_) {
        for (const QueryTerm& t : g) {
            if (t.kind == QueryTerm::Kind::hidden && t.flag != t.negate) { q.wants_hidden_ = true; }
            if (t.kind == QueryTerm::Kind::tag) { q.uses_tags_ = true; }
            if (t.on_path) { q.uses_path_ = true; }
        }
    }
    return q;
}

bool Query::matches(const FileEntryView& e) const noexcept
{
    if (groups_.empty()) { return true; }
    const std::string_view leaf = last_component(e.display);
    for (const auto& group : groups_) {
        bool all = true;
        for (const QueryTerm& t : group) {
            if (holds(t, e, leaf) == t.negate) { all = false; break; }
        }
        if (all) { return true; }
    }
    return false;
}

} // namespace prism::core
