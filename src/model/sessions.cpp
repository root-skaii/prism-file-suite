#include "model/sessions.hpp"

#include "core/text.hpp"
#include "strata/config.hpp"

#include <algorithm>
#include <format>

namespace prism::model {

namespace {

[[nodiscard]] bool same_name(std::string_view a, std::string_view b)
{
    return core::text::lower_ascii(a) == core::text::lower_ascii(b);
}

} // namespace

bool Sessions::save(Session s)
{
    if (s.name.empty() || s.tabs.empty()) { return false; }
    if (s.tabs.size() > max_tabs) { s.tabs.resize(max_tabs); }
    s.active = std::clamp(s.active, 0, static_cast<int>(s.tabs.size()) - 1);
    const auto it = std::ranges::find_if(list_, [&](const Session& o) { return same_name(o.name, s.name); });
    if (it != list_.end()) {
        *it = std::move(s);
    } else if (list_.size() < max_sessions) {
        list_.push_back(std::move(s));
    } else {
        return false;
    }
    ++revision_;
    return true;
}

bool Sessions::remove(std::string_view name)
{
    const auto it = std::ranges::find_if(list_, [&](const Session& o) { return same_name(o.name, name); });
    if (it == list_.end()) { return false; }
    list_.erase(it);
    ++revision_;
    return true;
}

std::optional<Session> Sessions::find(std::string_view name) const
{
    for (const Session& s : list_) {
        if (same_name(s.name, name)) { return s; }
    }
    return std::nullopt;
}

void Sessions::store(strata::config& c) const
{
    c.set_int("sessions", "count", static_cast<int>(list_.size()));
    for (std::size_t i = 0; i < list_.size(); ++i) {
        const Session& s = list_[i];
        const std::string sec = std::format("session{}", i);
        c.set(sec, "name", s.name);
        c.set_int(sec, "active", s.active);
        c.set(sec, "right", s.right);
        c.set_int(sec, "tabs", static_cast<int>(s.tabs.size()));
        for (std::size_t t = 0; t < s.tabs.size(); ++t) { c.set(sec, std::format("t{}", t), s.tabs[t]); }
    }
}

bool Sessions::load(const strata::config& c)
{
    if (!c.has("sessions", "count")) { return false; }
    list_.clear();
    const int n = std::clamp(c.get_int("sessions", "count", 0), 0, static_cast<int>(max_sessions));
    for (int i = 0; i < n; ++i) {
        const std::string sec = std::format("session{}", i);
        Session s;
        s.name   = std::string{c.get(sec, "name")};
        s.active = c.get_int(sec, "active", 0);
        s.right  = std::string{c.get(sec, "right")};
        const int tabs = std::clamp(c.get_int(sec, "tabs", 0), 0, static_cast<int>(max_tabs));
        for (int t = 0; t < tabs; ++t) {
            const std::string_view p = c.get(sec, std::format("t{}", t));
            if (!p.empty()) { s.tabs.emplace_back(p); }
        }
        if (!s.name.empty() && !s.tabs.empty()) {
            s.active = std::clamp(s.active, 0, static_cast<int>(s.tabs.size()) - 1);
            list_.push_back(std::move(s));
        }
    }
    ++revision_;
    return true;
}

bool Sessions::load_file(const std::string& path)
{
    strata::config c;
    return c.load_file(path) && load(c);
}

bool Sessions::save_file(const std::string& path) const
{
    strata::config c;
    store(c);
    return c.save_file(path);
}

} // namespace prism::model
