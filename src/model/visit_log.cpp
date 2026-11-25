#include "model/visit_log.hpp"

#include "core/path.hpp"
#include "core/text.hpp"
#include "strata/config.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <format>

namespace prism::model {

namespace {

[[nodiscard]] bool same(std::string_view a, std::string_view b)
{
#ifdef _WIN32
    return a.size() == b.size() && ::CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size())) == CSTR_EQUAL;
#else
    return a == b;
#endif
}

} // namespace

void VisitLog::note(std::string_view path)
{
    const std::string p = core::path::normalize(path);
    if (p.empty() || core::path::is_root(p)) { return; }
    ++clock_;
    ++revision_;
    for (Visit& v : visits_) {
        if (same(v.path, p)) {
            ++v.count;
            v.last = clock_;
            return;
        }
    }
    visits_.push_back({p, 1, clock_});
    if (visits_.size() > k_max) {
        // drop the least useful: fewest visits, then oldest
        const auto worst = std::ranges::min_element(visits_, [](const Visit& a, const Visit& b) { return a.count != b.count ? a.count < b.count : a.last < b.last; });
        visits_.erase(worst);
    }
}

void VisitLog::forget(std::string_view path)
{
    const auto it = std::ranges::find_if(visits_, [&](const Visit& v) { return same(v.path, core::path::normalize(path)); });
    if (it == visits_.end()) { return; }
    visits_.erase(it);
    ++revision_;
}

void VisitLog::clear()
{
    if (visits_.empty()) { return; }
    visits_.clear();
    ++revision_;
}

std::vector<std::string> VisitLog::recent(std::size_t n) const
{
    std::vector<const Visit*> v;
    for (const Visit& x : visits_) { v.push_back(&x); }
    std::ranges::sort(v, [](const Visit* a, const Visit* b) { return a->last > b->last; });
    std::vector<std::string> out;
    for (std::size_t i = 0; i < v.size() && i < n; ++i) { out.push_back(v[i]->path); }
    return out;
}

std::vector<std::string> VisitLog::frequent(std::size_t n) const
{
    std::vector<const Visit*> v;
    for (const Visit& x : visits_) {
        if (x.count > 1) { v.push_back(&x); } // (one visit is not a habit)
    }
    std::ranges::sort(v, [](const Visit* a, const Visit* b) { return a->count != b->count ? a->count > b->count : a->last > b->last; });
    std::vector<std::string> out;
    for (std::size_t i = 0; i < v.size() && i < n; ++i) { out.push_back(v[i]->path); }
    return out;
}

bool VisitLog::load_file(const std::string& path)
{
    strata::config c;
    if (!c.load_file(path) || !c.has("visits", "count")) { return false; }
    visits_.clear();
    const int n = std::clamp(c.get_int("visits", "count", 0), 0, static_cast<int>(k_max));
    for (int i = 0; i < n; ++i) {
        Visit v;
        v.path  = core::path::normalize(c.get("visits", std::format("v{}.path", i)));
        v.count = static_cast<std::uint32_t>(std::max(1, c.get_int("visits", std::format("v{}.count", i), 1)));
        v.last  = static_cast<std::uint64_t>(std::max(0, c.get_int("visits", std::format("v{}.last", i), 0)));
        if (!v.path.empty()) { visits_.push_back(std::move(v)); }
    }
    clock_ = 0;
    for (const Visit& v : visits_) { clock_ = std::max(clock_, v.last); }
    ++revision_;
    return true;
}

bool VisitLog::save_file(const std::string& path) const
{
    strata::config c;
    c.set_int("visits", "count", static_cast<int>(visits_.size()));
    for (std::size_t i = 0; i < visits_.size(); ++i) {
        c.set("visits", std::format("v{}.path", i), visits_[i].path);
        c.set_int("visits", std::format("v{}.count", i), static_cast<int>(visits_[i].count));
        c.set_int("visits", std::format("v{}.last", i), static_cast<int>(visits_[i].last));
    }
    return c.save_file(path);
}

} // namespace prism::model
