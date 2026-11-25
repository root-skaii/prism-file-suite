#include "core/git_status.hpp"

#include "core/path.hpp"
#include "core/process.hpp"
#include "core/rclone.hpp"
#include "core/text.hpp"

#include <windows.h>

#include <algorithm>

namespace prism::core {

namespace {

[[nodiscard]] std::string key_of(std::string_view p)
{
    std::string s = text::lower_ascii(p);
    for (char& c : s) { if (c == '/') { c = '\\'; } }
    while (!s.empty() && s.back() == '\\') { s.pop_back(); }
    return s;
}

[[nodiscard]] int rank(char c) noexcept
{
    switch (c) {
    case '!': return 6;
    case 'D': return 5;
    case 'M': return 4;
    case 'R': return 3;
    case 'A': return 2;
    case '?': return 1;
    default: return 0;
    }
}

void note_dir(GitRepo& r, const std::string& file_key, char code)
{
    for (std::size_t cut = file_key.find('\\'); cut != std::string::npos; cut = file_key.find('\\', cut + 1)) {
        char& d = r.dirs[file_key.substr(0, cut)];
        if (rank(code) > rank(d)) { d = code; }
    }
}

[[nodiscard]] bool has_dot_git(const std::string& dir)
{
    return ::GetFileAttributesW(text::widen(path::join(dir, ".git")).c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

char GitRepo::code_for(std::string_view rel, bool is_dir) const
{
    if (const auto f = files.find(rel); f != files.end()) { return f->second; }
    if (!untracked_dirs.empty()) {
        for (std::size_t cut = rel.find('\\'); cut != std::string_view::npos; cut = rel.find('\\', cut + 1)) {
            if (untracked_dirs.contains(rel.substr(0, cut))) { return '?'; }
        }
        if (is_dir && untracked_dirs.contains(rel)) { return '?'; }
    }
    if (is_dir) {
        if (const auto d = dirs.find(rel); d != dirs.end()) { return d->second; }
    }
    return 0;
}

std::string GitRepo::relative(std::string_view p) const
{
    const std::string k = key_of(p), r = key_of(root);
    if (k == r) { return {}; }
    if (k.size() > r.size() && k.compare(0, r.size(), r) == 0 && k[r.size()] == '\\') { return k.substr(r.size() + 1); }
    return {};
}

GitRepo parse_git_status(std::string_view out)
{
    GitRepo repo;
    std::size_t i = 0;
    while (i < out.size()) {
        const std::size_t end = std::min(out.find('\0', i), out.size());
        const std::string_view rec = out.substr(i, end - i);
        i = end + 1;
        if (rec.size() < 3) { continue; }
        if (rec.starts_with("## ")) {
            std::string_view b = rec.substr(3);
            if (b.starts_with("No commits yet on ")) { b.remove_prefix(18); }
            else if (b.starts_with("Initial commit on ")) { b.remove_prefix(18); }
            const std::size_t cut = std::min(b.find("..."), b.find(" ["));
            repo.branch = std::string{b.substr(0, cut)};
            if (repo.branch.starts_with("HEAD (no branch)")) { repo.branch = "HEAD (detached)"; }
            const std::size_t a = rec.find("ahead "), bh = rec.find("behind ");
            if (a != std::string_view::npos) { repo.ahead = std::atoi(std::string{rec.substr(a + 6, 8)}.c_str()); }
            if (bh != std::string_view::npos) { repo.behind = std::atoi(std::string{rec.substr(bh + 7, 8)}.c_str()); }
            continue;
        }
        const char x = rec[0], y = rec[1];
        std::string_view p = rec.substr(3);
        if (x == 'R' || x == 'C') { i = std::min(out.find('\0', i), out.size()) + 1; } // (the old name follows: not shown)
        char code = 'M';
        if (x == '?') { code = '?'; }
        else if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) { code = '!'; }
        else if (x == 'D' || y == 'D') { code = 'D'; }
        else if (x == 'A') { code = 'A'; }
        else if (x == 'R') { code = 'R'; }
        const bool dir = !p.empty() && p.back() == '/';
        const std::string k = key_of(p);
        if (k.empty()) { continue; }
        if (dir) { repo.untracked_dirs.insert(k); }
        else { repo.files[k] = code; }
        note_dir(repo, k + "\\x", code); // (every folder above it)
        if (dir) { char& d = repo.dirs[k]; if (rank(code) > rank(d)) { d = code; } }
    }
    return repo;
}

GitStatus::GitStatus(std::function<void()> wake) : wake_{std::move(wake)}, thread_{[this](std::stop_token st) { run(st); }} {}

GitStatus::~GitStatus()
{
    quit_ = true;
    thread_.request_stop();
    cv_.notify_all();
}

std::shared_ptr<const GitRepo> GitStatus::lookup(const std::string& folder)
{
    if (folder.empty() || rclone::is_path(folder)) { return nullptr; }
    const std::lock_guard lock{mutex_};
    FolderInfo& f = folders_[folder];
    if (!f.resolved) {
        if (!f.queued) { f.queued = true; queue_.push_back(folder); cv_.notify_one(); }
        return nullptr;
    }
    if (f.root.empty()) { return nullptr; }
    RepoInfo& r = repos_[f.root];
    const bool never = r.at.time_since_epoch().count() == 0; // (a failed run is still a run: no retry loop when git is missing)
    const bool stale = never || std::chrono::steady_clock::now() - r.at > max_age;
    if (stale && !r.queued) {
        r.queued = true;
        queue_.push_back(folder);
        cv_.notify_one();
    }
    return r.repo;
}

void GitStatus::invalidate()
{
    const std::lock_guard lock{mutex_};
    for (auto& [root, r] : repos_) { r.at = {}; }
    for (auto& [folder, f] : folders_) { if (f.root.empty() && !f.queued) { f.resolved = false; } } // (a `git init` since then)
}

void GitStatus::run(std::stop_token st)
{
    while (!st.stop_requested()) {
        std::string folder;
        {
            std::unique_lock lock{mutex_};
            if (!cv_.wait(lock, st, [this] { return !queue_.empty(); })) { return; }
            folder = std::move(queue_.front());
            queue_.pop_front();
        }
        // 1. which repository (a walk up looking for .git: stat calls, off the ui thread)
        std::string root;
        {
            std::string cur = path::normalize(folder);
            for (int depth = 0; depth < 64 && !cur.empty(); ++depth) {
                if (has_dot_git(cur)) { root = cur; break; }
                cur = path::parent(cur);
            }
        }
        {
            const std::lock_guard lock{mutex_};
            FolderInfo& f = folders_[folder];
            f.root     = root;
            f.resolved = true;
            f.queued   = false;
        }
        if (root.empty()) { if (wake_) { wake_(); } continue; }

        // 2. its status (skipped when another folder of the same repo just asked)
        {
            const std::lock_guard lock{mutex_};
            const RepoInfo& r = repos_[root];
            if (r.repo && std::chrono::steady_clock::now() - r.at < std::chrono::milliseconds{300}) { repos_[root].queued = false; continue; }
        }
        const ProcessResult p = run_process(L"git --no-optional-locks -c core.quotepath=off status --porcelain=v1 -z --branch --untracked-files=normal",
                                            text::widen(root), 20000, &quit_);
        std::shared_ptr<GitRepo> repo;
        if (p.started && !p.timed_out && p.exit_code == 0) {
            repo = std::make_shared<GitRepo>(parse_git_status(p.output));
            repo->root = root;
        }
        {
            const std::lock_guard lock{mutex_};
            RepoInfo& r = repos_[root];
            r.repo   = repo; // (null when git is missing or failed: no badges)
            r.at     = std::chrono::steady_clock::now();
            r.queued = false;
        }
        if (wake_) { wake_(); }
    }
}

} // namespace prism::core
