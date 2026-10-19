#include "ui/preview_pane.hpp"

#include "core/path.hpp"
#include "core/text.hpp"
#include "ui/panel.hpp"

#include <windows.h>

#include <algorithm>
#include <format>

namespace prism::ui {

using namespace strata;
using core::FileKind;

namespace {

[[nodiscard]] bool is_text_kind(FileKind k) noexcept
{
    return k == FileKind::text || k == FileKind::code || k == FileKind::config;
}

} // namespace

std::vector<std::string> read_text_head(const std::string& path, std::size_t max_bytes, std::size_t max_lines)
{
    std::vector<std::string> lines;
    HANDLE f = ::CreateFileW(core::text::widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return lines; }
    std::string buf(max_bytes, '\0');
    DWORD n = 0;
    const BOOL ok = ::ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr);
    ::CloseHandle(f);
    if (!ok || n == 0) { return lines; }
    for (DWORD i = 0; i < n; ++i) {
        if (buf[i] == '\0') { return {}; }
    }
    std::string cur;
    for (DWORD i = 0; i < n && lines.size() < max_lines; ++i) {
        const char c = buf[i];
        if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
        else if (c == '\t') { cur += "    "; }
        else if (c != '\r') { cur.push_back(c); }
    }
    if (!cur.empty() && lines.size() < max_lines) { lines.push_back(std::move(cur)); }
    return lines;
}

void PreviewPane::load(const std::string& path)
{
    shown_ = path;
    info_  = core::query_file_info(path);
    lines_.clear();
    scan_.reset();
    kind_ = FileKind::other;
    if (info_.is_dir) {
        kind_ = FileKind::folder;
        scan_ = std::make_unique<core::FolderScan>();
        scan_->start(path);
        return;
    }
    const std::size_t dot = info_.name.rfind('.');
    const std::string ext = dot == std::string::npos || dot == 0 ? std::string{} : core::text::lower_ascii(std::string_view{info_.name}.substr(dot + 1));
    kind_ = core::classify_extension(ext);
    if (is_text_kind(kind_) && info_.size < (16u << 20)) { lines_ = read_text_head(path, 6144, 60); }
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

    // what to show: the single selected item, a summary of several, or the folder itself
    const std::size_t sel = L.selected_count();
    if (sel > 1) {
        const rect center{{area.min.x, area.min.y + area.height() * 0.35f}, {area.max.x, area.min.y + area.height() * 0.35f + 28.0f}};
        p.text_center(center, std::format("{} items selected", sel), pal.text, f.bold);
        p.text_center(rect{{area.min.x, center.max.y}, {area.max.x, center.max.y + 24.0f}}, core::path::format_size(L.selected_bytes()), pal.text_dim, f.body);
        return;
    }
    std::string target = tab.path();
    if (sel == 1) { target = core::path::join(tab.path(), L.at(L.selected_rows().front()).name); }
    if (target.empty()) { return; }
    if (target != shown_) { load(target); }

    const float pad = 18.0f;
    const float w   = area.width() - pad * 2.0f;
    float y = area.min.y + pad;

    // the picture
    const float box_h = std::min(w * 0.8f, 280.0f);
    const rect  box{{area.min.x + pad, y}, {area.max.x - pad, y + box_h}};
    p.dl().rect_filled(box, pal.raised, 12.0f);
    bool drawn = false;
    if (!info_.is_dir) {
        const platform::Image img = env_.images.get(target, static_cast<int>(std::ceil(std::min(box.width(), box.height()) * ui.scale())), platform::ImageKind::thumbnail);
        if (img.id != 0 && (kind_ == FileKind::image || kind_ == FileKind::video || kind_ == FileKind::pdf || kind_ == FileKind::document || kind_ == FileKind::presentation || kind_ == FileKind::spreadsheet || kind_ == FileKind::executable)) {
            p.image_fit(box.expanded(-8.0f), img, 8.0f);
            drawn = true;
        }
    }
    if (!drawn && lines_.empty()) {
        const float s = std::min(box.width(), box.height()) * 0.5f;
        p.kind_icon(rect::from_size(box.center() - vec2{s * 0.5f, s * 0.5f}, {s, s}), kind_);
    } else if (!drawn) {
        // the head of a text file, set small
        p.dl().push_clip(box.expanded(-8.0f));
        float ty = box.min.y + 10.0f;
        const float lh = p.line_height(f.small) + 1.0f;
        for (const std::string& line : lines_) {
            if (ty + lh > box.max.y - 8.0f) { break; }
            (void)ui.label_clipped({box.min.x + 12.0f, ty}, box.width() - 24.0f, pal.text_dim, line, f.small);
            ty += lh;
        }
        p.dl().pop_clip();
    }
    y = box.max.y + 16.0f;

    // name, type
    const rect name_row{{area.min.x + pad, y}, {area.max.x - pad, y + 24.0f}};
    p.text_left(name_row, info_.name.empty() ? target : info_.name, pal.text, f.bold);
    y += 26.0f;
    p.text_left(rect{{area.min.x + pad, y}, {area.max.x - pad, y + 20.0f}}, info_.type, pal.text_dim, f.small);
    y += 28.0f;

    const auto row = [&](std::string_view k, const std::string& v) {
        if (v.empty()) { return; }
        p.text_left(rect{{area.min.x + pad, y}, {area.min.x + pad + 90.0f, y + 20.0f}}, k, pal.text_faint, f.small);
        p.text_left(rect{{area.min.x + pad + 90.0f, y}, {area.max.x - pad, y + 20.0f}}, v, pal.text, f.small);
        y += 22.0f;
    };
    if (info_.is_dir) {
        if (scan_) {
            row("Contains", std::format("{} files, {} folders{}", scan_->files(), scan_->folders(), scan_->done() ? "" : " ..."));
            row("Size", core::path::format_size(scan_->bytes()) + (scan_->done() ? "" : " ..."));
        }
    } else {
        row("Size", core::path::format_size(info_.size));
    }
    row("Modified", core::path::format_time(info_.modified));
    row("Created", core::path::format_time(info_.created));
    for (const auto& [k, v] : info_.version) {
        if (k == "FileDescription" || k == "FileVersion" || k == "CompanyName") { row(k == "FileDescription" ? "Description" : (k == "FileVersion" ? "Version" : "Company"), v); }
    }
    if (!info_.owner.empty()) { row("Owner", info_.owner); }
}

} // namespace prism::ui
