#include "ui/preview_pane.hpp"

#include "core/path.hpp"
#include "core/text.hpp"
#include "core/thread_priority.hpp"
#include "ui/panel.hpp"
#include "ui/tr.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace prism::ui {

using namespace strata;
using core::FileKind;

namespace {

[[nodiscard]] bool is_text_kind(FileKind k) noexcept
{
    return k == FileKind::text || k == FileKind::code || k == FileKind::config;
}

[[nodiscard]] bool is_pictured_kind(FileKind k) noexcept
{
    return k == FileKind::image || k == FileKind::video || k == FileKind::audio || k == FileKind::pdf ||
           k == FileKind::document || k == FileKind::presentation || k == FileKind::spreadsheet || k == FileKind::executable;
}

[[nodiscard]] std::vector<std::string> split_lines_utf8(std::string_view text, std::size_t max_lines)
{
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < text.size() && lines.size() < max_lines; ++i) {
        const char c = text[i];
        if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
        else if (c == '\t') { cur += "    "; }
        else if (c != '\r') { cur.push_back(c); }
    }
    if (!cur.empty() && lines.size() < max_lines) { lines.push_back(std::move(cur)); }
    return lines;
}

} // namespace

std::vector<unsigned char> read_raw_head(const std::string& path, std::size_t max_bytes)
{
    std::vector<unsigned char> buf;
    HANDLE f = ::CreateFileW(core::path::long_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return buf; }
    buf.resize(max_bytes);
    DWORD n = 0;
    const BOOL ok = ::ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr);
    ::CloseHandle(f);
    buf.resize(ok ? n : 0);
    return buf;
}

std::vector<std::string> read_text_head(const std::string& path, std::size_t max_bytes, std::size_t max_lines)
{
    const std::vector<unsigned char> buf = read_raw_head(path, max_bytes);
    if (buf.empty()) { return {}; }
    // handle UTF-16 LE / BE BOM text files instead of rejecting them as binary on NUL bytes
    if (buf.size() >= 2 && ((buf[0] == 0xff && buf[1] == 0xfe) || (buf[0] == 0xfe && buf[1] == 0xff))) {
        const bool be = buf[0] == 0xfe;
        const std::size_t count = (buf.size() - 2) / 2;
        std::wstring w(count, L'\0');
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char b0 = buf[2 + i * 2];
            const unsigned char b1 = buf[2 + i * 2 + 1];
            const wchar_t ch = be ? static_cast<wchar_t>((b0 << 8) | b1) : static_cast<wchar_t>((b1 << 8) | b0);
            if (ch == 0) { w.resize(i); break; }
            w[i] = ch;
        }
        return split_lines_utf8(core::text::narrow(w), max_lines);
    }
    for (const unsigned char b : buf) {
        if (b == 0) { return {}; }
    }
    std::size_t start = 0;
    if (buf.size() >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) { start = 3; } // strip UTF-8 BOM
    return split_lines_utf8(std::string_view{reinterpret_cast<const char*>(buf.data() + start), buf.size() - start}, max_lines);
}

color token_color(core::TokenKind kind, const Palette& pal, bool dark) noexcept
{
    switch (kind) {
    case core::TokenKind::keyword: return dark ? color{130, 170, 255, 255} : color{36, 82, 200, 255};
    case core::TokenKind::type:    return dark ? color{98, 210, 198, 255}  : color{14, 128, 120, 255};
    case core::TokenKind::string:  return dark ? color{152, 214, 134, 255} : color{34, 134, 58, 255};
    case core::TokenKind::comment: return pal.text_faint;
    case core::TokenKind::number:  return dark ? color{240, 184, 108, 255} : color{184, 96, 16, 255};
    case core::TokenKind::preproc: return dark ? color{206, 148, 246, 255} : color{136, 56, 184, 255};
    case core::TokenKind::plain:   return pal.text;
    }
    return pal.text;
}

void draw_highlighted_line(Painter& p, const rect& bounds, const core::HighlightedLine& line, font_id font, bool dark)
{
    if (line.text.empty()) { return; }
    const Palette& pal = p.pal();
    const float    y   = bounds.min.y + (bounds.height() - p.line_height(font)) * 0.5f;
    float          x   = bounds.min.x;
    for (const core::TokenSpan& s : line.spans) {
        if (x >= bounds.max.x) { break; }
        const std::string_view part = std::string_view{line.text}.substr(s.start, s.length);
        p.dl().text({x, y}, token_color(s.kind, pal, dark), part, font);
        x += p.measure(font, part).x;
    }
}

void PreviewPane::cancel_load() noexcept
{
    if (pending_) { pending_->cancel.store(true, std::memory_order_relaxed); }
    pending_.reset();
}

void PreviewPane::load(const std::string& path)
{
    cancel_load();
    shown_ = path;
    info_  = {};
    info_.path = path;
    info_.name = core::path::leaf(path);
    lines_.clear();
    hex_lines_.clear();
    scan_.reset();
    kind_ = FileKind::other;

    auto job = std::make_shared<LoadResult>();
    job->path = path;
    pending_  = job;
    std::thread{[job, wake = env_.window.waker()] {
        core::lower_thread_priority();
        if (job->cancel.load(std::memory_order_relaxed)) { return; }
        job->info = core::query_file_info(job->path);
        if (job->cancel.load(std::memory_order_relaxed)) { return; }
        if (job->info.is_dir) {
            job->kind = FileKind::folder;
        } else {
            const std::size_t dot = job->info.name.rfind('.');
            const std::string ext = dot == std::string::npos || dot == 0 ? std::string{} : core::text::lower_ascii(std::string_view{job->info.name}.substr(dot + 1));
            job->kind = core::classify_extension(ext);
            const bool cloud_only = (job->info.attributes & (0x00400000u | 0x00040000u | FILE_ATTRIBUTE_OFFLINE)) != 0;
            if (!cloud_only && is_text_kind(job->kind) && job->info.size < (16u << 20)) {
                auto raw = read_text_head(job->path, 8192, 80);
                if (!raw.empty()) { job->lines = core::highlight_lines(std::move(raw), core::syntax_for_extension(ext)); }
            }
            if (!cloud_only && job->lines.empty() && !is_pictured_kind(job->kind) && job->info.size > 0) {
                const auto raw = read_raw_head(job->path, 512);
                if (!raw.empty()) { job->hex_lines = core::format_hex_dump(raw, 20); }
            }
        }
        job->done.store(true, std::memory_order_release);
        if (wake) { wake(); }
    }}.detach();
}

void PreviewPane::poll_load()
{
    if (!pending_ || !pending_->done.load(std::memory_order_acquire)) { return; }
    info_      = std::move(pending_->info);
    kind_      = pending_->kind;
    lines_     = std::move(pending_->lines);
    hex_lines_ = std::move(pending_->hex_lines);
    pending_.reset();
    if (info_.is_dir && !info_.is_drive) {
        scan_ = std::make_unique<core::FolderScan>();
        scan_->start(info_.path);
    }
}

void PreviewPane::frame(Painter& p, const rect& area)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const Metrics& m   = p.metrics();
    const FontSet& f   = env_.fonts;
    model::Tab&    tab = env_.tab();
    const model::Listing& L = tab.listing();

    PanelScope panel{ui, "##preview", area};
    shape_style card;
    card.radius       = radii(m.pane_radius);
    card.fill_top     = card.fill_bottom = pal.content;
    card.border       = pal.border;
    card.border_width = 1.0f;
    p.dl().shape(area, card);

    const std::size_t sel = L.selected_count();
    if (sel > 1) {
        const rect center{{area.min.x, area.min.y + area.height() * 0.35f}, {area.max.x, area.min.y + area.height() * 0.35f + 28.0f}};
        p.text_center(center, trf("{} items selected", sel), pal.text, f.bold);
        p.text_center(rect{{area.min.x, center.max.y}, {area.max.x, center.max.y + 24.0f}}, core::path::format_size(L.selected_bytes()), pal.text_dim, f.body);
        return;
    }
    std::string target = tab.path();
    if (sel == 1) { target = core::path::join(tab.path(), L.at(L.selected_rows().front()).name); }
    if (target.empty()) { return; }
    if (target != shown_) { load(target); }
    poll_load();

    const float pad = 16.0f;
    const float w   = area.width() - pad * 2.0f;
    float y = area.min.y + pad;

    // preview stage
    const float box_h = std::min(w * 0.78f, 260.0f);
    const rect  box{{area.min.x + pad, y}, {area.max.x - pad, y + box_h}};
    shape_style stage;
    stage.radius       = radii(12.0f);
    stage.fill_top     = stage.fill_bottom = pal.raised;
    stage.border       = pal.divider;
    stage.border_width = 1.0f;
    p.dl().shape(box, stage);

    bool drawn = false;
    if (!info_.is_dir && !pending_) {
        const platform::Image img = env_.images.get(target, static_cast<int>(std::ceil(std::min(box.width(), box.height()) * ui.scale())), platform::ImageKind::thumbnail);
        if (img.id != 0 && is_pictured_kind(kind_)) {
            p.image_fit(box.expanded(-8.0f), img, 8.0f);
            drawn = true;
        }
    }
    if (!drawn && !lines_.empty()) {
        p.dl().push_clip(box.expanded(-8.0f));
        float ty = box.min.y + 10.0f;
        const float lh = p.line_height(f.mono) + 1.0f;
        const bool dark = env_.theme.dark();
        for (const core::HighlightedLine& line : lines_) {
            if (ty + lh > box.max.y - 8.0f) { break; }
            draw_highlighted_line(p, rect{{box.min.x + 12.0f, ty}, {box.max.x - 10.0f, ty + lh}}, line, f.mono, dark);
            ty += lh;
        }
        p.dl().pop_clip();
        drawn = true;
    } else if (!drawn && !hex_lines_.empty()) {
        p.dl().push_clip(box.expanded(-8.0f));
        float ty = box.min.y + 10.0f;
        const float lh = p.line_height(f.mono) + 1.0f;
        for (const std::string& line : hex_lines_) {
            if (ty + lh > box.max.y - 8.0f) { break; }
            p.text_left(rect{{box.min.x + 10.0f, ty}, {box.max.x - 10.0f, ty + lh}}, line, pal.text_dim, f.mono);
            ty += lh;
        }
        p.dl().pop_clip();
        drawn = true;
    }
    if (!drawn) {
        const float s = std::min(box.width(), box.height()) * 0.45f;
        p.kind_icon(rect::from_size(box.center() - vec2{s * 0.5f, s * 0.5f}, {s, s}), kind_);
    }
    y = box.max.y + 14.0f;

    // header name & type (format details live in the metadata rows below, so keep the subtitle clean)
    p.text_left(rect{{area.min.x + pad, y}, {area.max.x - pad, y + 22.0f}}, info_.name.empty() ? target : info_.name, pal.text, f.bold);
    y += 23.0f;
    p.text_left(rect{{area.min.x + pad, y}, {area.max.x - pad, y + 18.0f}}, info_.type, pal.text_dim, f.small);
    y += 24.0f;
    p.dl().rect_filled({{area.min.x + pad, y}, {area.max.x - pad, y + 1.0f}}, pal.divider);
    y += 8.0f;

    // collect metadata rows and render inside a scrollable region so info never clips off the bottom
    std::vector<std::pair<std::string, std::string>> rows;
    const auto add_row = [&](std::string_view k, std::string v) {
        if (!v.empty()) { rows.emplace_back(std::string{k}, std::move(v)); }
    };
    if (info_.is_drive && info_.drive_total > 0) {
        add_row(tr("Free"), core::path::format_size(info_.drive_free));
        add_row(tr("Capacity"), core::path::format_size(info_.drive_total));
        add_row(tr("Filesystem"), info_.filesystem);
    } else if (info_.is_dir) {
        if (scan_) {
            add_row(tr("Contains"), trf("{} files, {} folders{}", scan_->files(), scan_->folders(), scan_->done() ? "" : " ..."));
            add_row(tr("Size"), core::path::format_size(scan_->bytes()) + (scan_->done() ? "" : " ..."));
        }
    } else {
        add_row(tr("Size"), core::path::format_size(info_.size));
    }
    // highlight key format metadata (dimensions, duration, bitrate, camera, artist, target)
    for (const core::MetaGroup& grp : info_.meta_groups) {
        for (const auto& [k, v] : grp.rows) {
            if (rows.size() >= 12) { break; }
            add_row(tr(k), v);
        }
    }
    add_row(tr("Modified"), core::path::format_time(info_.modified));
    add_row(tr("Created"), core::path::format_time(info_.created));
    for (const auto& [k, v] : info_.version) {
        if (k == "FileDescription" || k == "FileVersion" || k == "CompanyName") {
            add_row(k == "FileDescription" ? tr("Description") : (k == "FileVersion" ? tr("Version") : tr("Company")), v);
        }
    }
    add_row(tr("Owner"), info_.owner);

    const float avail_h = std::max(0.0f, area.max.y - pad - y);
    if (avail_h > 20.0f && !rows.empty()) {
        (void)ui.custom_item("##prev_spacer", {1.0f, y - area.min.y});
        auto sc = ui.child("##prev_meta", {area.width(), avail_h}, child_flags::no_padding);
        if (sc) {
            const float rh = 23.0f;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const item_result r = ui.custom_item("##pm" + std::to_string(i), {area.width(), rh});
                p.text_left(rect{{r.bounds.min.x + pad, r.bounds.min.y}, {r.bounds.min.x + pad + 96.0f, r.bounds.max.y}}, rows[i].first, pal.text_faint, f.small);
                p.text_left(rect{{r.bounds.min.x + pad + 96.0f, r.bounds.min.y}, {r.bounds.max.x - pad, r.bounds.max.y}}, rows[i].second, pal.text, f.small);
                if (r.hovered && p.measure(f.small, rows[i].second).x > r.bounds.width() - pad * 2.0f - 96.0f) { ui.tooltip(rows[i].second); }
            }
        }
    }
}

} // namespace prism::ui
