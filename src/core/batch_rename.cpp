#include "core/batch_rename.hpp"

#include "core/file_ops.hpp"
#include "core/path.hpp"
#include "core/text.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <format>

namespace prism::core {

namespace {

[[nodiscard]] char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

std::string replace_nocase(std::string_view s, std::string_view find, std::string_view with)
{
    if (find.empty()) { return std::string{s}; }
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        bool hit = i + find.size() <= s.size();
        for (std::size_t k = 0; hit && k < find.size(); ++k) { hit = lower(s[i + k]) == lower(find[k]); }
        if (hit) { out += with; i += find.size(); }
        else { out += s[i++]; }
    }
    return out;
}

std::string apply_case(std::string s, CaseMode mode)
{
    // ASCII only, like the rest of the app's case folding; other characters stay as they are
    bool word_start = true;
    for (char& c : s) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (mode == CaseMode::lower) { c = lower(c); }
        else if (mode == CaseMode::upper) { if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 32); } }
        else if (mode == CaseMode::title) {
            if (letter) { c = word_start ? (c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c) : lower(c); }
            word_start = !letter && c != '\'';
        }
    }
    return s;
}

} // namespace

std::vector<std::string> batch_names(const std::vector<RenameItem>& items, const RenameRule& rule)
{
    std::vector<std::string> out;
    out.reserve(items.size());
    long long counter = rule.start;
    const bool has_ext_token = rule.pattern.find("[E]") != std::string::npos;
    for (const RenameItem& it : items) {
        std::string stem = it.name, ext;
        const std::size_t dot = it.name.rfind('.');
        if (!it.is_dir && dot != std::string::npos && dot > 0) {
            stem = it.name.substr(0, dot);
            ext  = it.name.substr(dot + 1);
        }
        stem = apply_case(replace_nocase(stem, rule.find, rule.replace), rule.casing);
        const std::string number = std::format("{:0{}}", counter, static_cast<std::size_t>(std::clamp(rule.digits, 1, 12)));
        counter += rule.step;

        std::string name;
        const std::string_view pat = rule.pattern.empty() ? std::string_view{"[N]"} : std::string_view{rule.pattern};
        for (std::size_t i = 0; i < pat.size();) {
            if (pat.compare(i, 3, "[N]") == 0) { name += stem; i += 3; }
            else if (pat.compare(i, 3, "[E]") == 0) { name += ext; i += 3; }
            else if (pat.compare(i, 3, "[C]") == 0) { name += number; i += 3; }
            else { name += pat[i++]; }
        }
        if (!has_ext_token && !ext.empty()) { name += "." + ext; }
        out.push_back(std::move(name));
    }
    return out;
}

std::vector<std::string> batch_problems(const std::vector<RenameItem>& items, const std::vector<std::string>& result,
                                        const std::vector<std::string>& others)
{
    std::vector<std::string> bad(items.size());
    std::vector<std::string> keys;
    keys.reserve(result.size());
    for (const std::string& r : result) { keys.push_back(text::lower_ascii(r)); }
    for (std::size_t i = 0; i < result.size() && i < items.size(); ++i) {
        const std::string& n = result[i];
        if (n.empty()) { bad[i] = "empty"; }
        else if (n.find_first_of("\\/:*?\"<>|") != std::string::npos) { bad[i] = "invalid characters"; }
        else if (n.back() == '.' || n.back() == ' ') { bad[i] = "ends with a dot or space"; }
        else if (std::count(keys.begin(), keys.end(), keys[i]) > 1) { bad[i] = "same name twice"; }
        else if (std::ranges::any_of(others, [&](const std::string& o) { return text::lower_ascii(o) == keys[i]; })) { bad[i] = "already exists"; }
    }
    return bad;
}

bool apply_renames(const std::string& dir, const std::vector<std::pair<std::string, std::string>>& pairs)
{
    std::vector<std::pair<std::string, std::string>> todo;
    for (const auto& p : pairs) { if (p.first != p.second) { todo.push_back(p); } }
    if (todo.empty()) { return true; }

    // phase 1: everything moves out of the way under a name nobody uses; phase 2: onto the final names
    std::vector<std::string> temps;
    const auto rollback = [&](std::size_t done_phase1, std::size_t done_phase2) {
        for (std::size_t i = 0; i < done_phase2; ++i) { (void)FileOps::rename(path::join(dir, todo[i].second), temps[i]); }
        for (std::size_t i = 0; i < done_phase1; ++i) { (void)FileOps::rename(path::join(dir, temps[i]), todo[i].first); }
    };
    for (std::size_t i = 0; i < todo.size(); ++i) {
        temps.push_back(std::format("~prismfs-{}-{}.tmp", ::GetTickCount64(), i));
        if (!FileOps::rename(path::join(dir, todo[i].first), temps[i])) { rollback(i, 0); return false; }
    }
    for (std::size_t i = 0; i < todo.size(); ++i) {
        if (!FileOps::rename(path::join(dir, temps[i]), todo[i].second)) {
            rollback(todo.size(), i);
            return false;
        }
    }
    return true;
}

} // namespace prism::core
