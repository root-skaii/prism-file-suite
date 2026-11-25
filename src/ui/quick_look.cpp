#include "ui/quick_look.hpp"

#include "core/path.hpp"
#include "core/text.hpp"
#include "core/thread_priority.hpp"
#include "ui/preview_pane.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

namespace prism::ui {

using namespace strata;
using core::FileKind;

void QuickLook::cancel_load() noexcept
{
    if (pending_) { pending_->cancel.store(true, std::memory_order_relaxed); }
    pending_.reset();
}

void QuickLook::load(const std::string& path)
{
    cancel_load();
    shown_  = path;
    info_   = {};
    info_.path = path;
    info_.name = core::path::leaf(path);
    lines_.clear();
    hex_lines_.clear();
    scroll_ = 0;
    zoom_   = 1.0f;
    pan_    = {};
    dragging_pan_ = false;
    kind_   = FileKind::other;

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
            const bool text = job->kind == FileKind::text || job->kind == FileKind::code || job->kind == FileKind::config;
            if (!cloud_only && text && job->info.size < (32u << 20)) {
                auto raw = read_text_head(job->path, 48 * 1024, 400);
                if (!raw.empty()) { job->lines = core::highlight_lines(std::move(raw), core::syntax_for_extension(ext)); }
            }
            const bool pictured = job->kind == FileKind::image || job->kind == FileKind::video || job->kind == FileKind::audio ||
                                  job->kind == FileKind::pdf || job->kind == FileKind::document || job->kind == FileKind::presentation ||
                                  job->kind == FileKind::spreadsheet;
            if (!cloud_only && job->lines.empty() && !pictured && job->info.size > 0) {
                const auto raw = read_raw_head(job->path, 4096);
                if (!raw.empty()) { job->hex_lines = core::format_hex_dump(raw, 256); }
            }
        }
        job->done.store(true, std::memory_order_release);
        if (wake) { wake(); }
    }}.detach();
}

void QuickLook::poll_load()
{
    if (!pending_ || !pending_->done.load(std::memory_order_acquire)) { return; }
    info_      = std::move(pending_->info);
    kind_      = pending_->kind;
    lines_     = std::move(pending_->lines);
    hex_lines_ = std::move(pending_->hex_lines);
    pending_.reset();
}

void QuickLook::frame(Painter& p, const rect& area)
{
    if (!open_) { return; }
    context&              ui  = p.ui();
    const Palette&        pal = p.pal();
    const FontSet&        f   = env_.fonts;
    model::Tab&           tab = env_.tab();
    const model::Listing& L   = tab.listing();

    if (L.selected_count() != 1) { open_ = false; return; }
    const std::string target = core::path::join(tab.path(), L.at(L.selected_rows().front()).name);
    if (target != shown_) { load(target); }
    poll_load();

    const float w = std::min(area.width() - 48.0f, 1000.0f);
    const float h = std::min(area.height() - 32.0f, 760.0f);
    if (w < 240.0f || h < 200.0f) { return; }
    const rect card = rect::from_size(area.center() - vec2{w * 0.5f, h * 0.5f}, {w, h});

    // clicking the dimmed backdrop outside the card closes Quick Look cleanly
    if (ui.mouse_clicked(mouse_button::left) && area.contains(ui.mouse_pos()) && !card.contains(ui.mouse_pos())) {
        open_ = false;
        return;
    }

    auto fg = ui.layer(layer::foreground);
    p.dl().rect_filled(area, color{0, 0, 0, 128}, 0.0f);
    p.dl().rect_filled(card.expanded(4.0f), color{0, 0, 0, 85}, 16.0f);
    shape_style bg;
    bg.radius       = radii(14.0f);
    bg.fill_top     = bg.fill_bottom = pal.content;
    bg.border       = pal.border;
    bg.border_width = 1.0f;
    p.dl().shape(card, bg);

    const float pad = 18.0f;
    const rect  head{{card.min.x + pad, card.min.y + 12.0f}, {card.max.x - pad - 150.0f, card.min.y + 36.0f}};
    p.text_left(head, info_.name.empty() ? target : info_.name, pal.text, f.bold);
    std::string sub = info_.type;
    if (!info_.media_summary.empty()) { sub += "  \xc2\xb7  " + info_.media_summary; }
    if (!info_.is_dir && info_.exists) { sub += "  \xc2\xb7  " + core::path::format_size(info_.size); }
    if (info_.modified != 0) { sub += "  \xc2\xb7  " + core::path::format_time(info_.modified); }
    if (!hex_lines_.empty()) { sub += "  \xc2\xb7  Hex"; }
    p.text_left(rect{{head.min.x, head.max.y}, {card.max.x - pad - 150.0f, head.max.y + 20.0f}}, sub, pal.text_dim, f.small);
    p.text_right(rect{{card.max.x - pad - 160.0f, head.min.y + 4.0f}, {card.max.x - pad, head.min.y + 24.0f}}, trf("{} or Esc to close", key_hint(env_.binds, "quick_look")), pal.text_faint, f.small);

    const rect body{{card.min.x + pad, card.min.y + 66.0f}, {card.max.x - pad, card.max.y - pad}};
    p.dl().rect_filled(body, pal.raised, 10.0f);

    bool drawn = false;
    if (!info_.is_dir && !pending_) {
        const bool is_img = kind_ == FileKind::image;
        const float req_scale = is_img ? std::min(zoom_, 3.0f) : 1.0f;
        const int px = static_cast<int>(std::ceil(std::min(body.width(), body.height()) * ui.scale() * req_scale));
        const platform::Image img = env_.images.get(target, px, platform::ImageKind::thumbnail);
        const bool pictured = is_img || kind_ == FileKind::video || kind_ == FileKind::audio ||
                              kind_ == FileKind::pdf || kind_ == FileKind::document || kind_ == FileKind::presentation ||
                              kind_ == FileKind::spreadsheet;
        if (img.id != 0 && pictured) {
            const rect view = body.expanded(-8.0f);
            if (is_img) {
                const vec2 mp = ui.mouse_pos();
                if (view.contains(mp)) {
                    const float wheel = ui.take_wheel();
                    if (wheel != 0.0f) {
                        const float factor = wheel > 0.0f ? 1.18f : (1.0f / 1.18f);
                        zoom_ = std::clamp(zoom_ * factor, 1.0f, 6.0f);
                        if (zoom_ <= 1.01f) { zoom_ = 1.0f; pan_ = {}; }
                    }
                    if (ui.mouse_clicked(mouse_button::left)) {
                        if (zoom_ > 1.01f) {
                            dragging_pan_ = true;
                            pan_anchor_   = mp;
                            pan_start_    = pan_;
                        }
                    }
                }
                if (dragging_pan_) {
                    if (ui.mouse_down(mouse_button::left)) {
                        pan_ = pan_start_ + (mp - pan_anchor_);
                    } else {
                        dragging_pan_ = false;
                    }
                }
                // compact zoom pill in top-right of the image viewport (Fit / zoom %)
                const std::string zlabel = zoom_ <= 1.01f ? std::string{tr("Fit")} : std::format("{}%", static_cast<int>(std::lround(zoom_ * 100.0f)));
                const float zw = p.measure(f.small, zlabel).x + 20.0f;
                const rect zpill = rect::from_size({view.max.x - zw - 6.0f, view.min.y + 6.0f}, {zw, 24.0f});

                // compute fitted base rect, then scale by zoom_ + offset by pan_
                const float iw = static_cast<float>(std::max(1, img.w));
                const float ih = static_cast<float>(std::max(1, img.h));
                const float fit = std::min(view.width() / iw, view.height() / ih);
                const float dw = iw * fit * zoom_;
                const float dh = ih * fit * zoom_;
                const float max_px = std::max(0.0f, (dw - view.width()) * 0.5f);
                const float max_py = std::max(0.0f, (dh - view.height()) * 0.5f);
                pan_.x = std::clamp(pan_.x, -max_px, max_px);
                pan_.y = std::clamp(pan_.y, -max_py, max_py);
                const vec2 center = view.center() + pan_;
                const rect dst = rect::from_size({center.x - dw * 0.5f, center.y - dh * 0.5f}, {dw, dh});

                p.dl().push_clip(view);
                p.dl().image(dst, img.id, {0.0f, 0.0f}, {1.0f, 1.0f}, color{255, 255, 255, 255}, radii(zoom_ <= 1.01f ? 8.0f : 0.0f));
                p.dl().pop_clip();

                const bool zhot = zpill.contains(mp);
                p.dl().rect_filled(zpill, (zhot ? pal.hover : pal.content).scaled_alpha(0.88f), 12.0f);
                p.dl().rect_outline(zpill, pal.border, 12.0f, 1.0f);
                p.text_center(zpill, zlabel, pal.text, f.small);
                if (zhot && ui.mouse_clicked(mouse_button::left)) {
                    zoom_ = zoom_ <= 1.01f ? 2.0f : 1.0f;
                    pan_  = {};
                }
            } else {
                p.image_fit(view, img, 8.0f);
            }
            drawn = true;
        }
    }
    if (drawn) { return; }

    const std::size_t total = !lines_.empty() ? lines_.size() : hex_lines_.size();
    if (total == 0) {
        const float s = std::min(body.width(), body.height()) * 0.36f;
        p.kind_icon(rect::from_size(body.center() - vec2{s * 0.5f, s * 0.5f}, {s, s}), kind_);
        return;
    }

    const float lh = p.line_height(f.mono) + 2.0f;
    const int   vis = std::max(1, static_cast<int>((body.height() - 16.0f) / lh));
    const int   max_scroll = std::max(0, static_cast<int>(total) - vis);
    if (body.contains(ui.mouse_pos())) {
        const float wheel = ui.take_wheel();
        if (wheel != 0.0f) { scroll_ = std::clamp(scroll_ - static_cast<int>(std::lround(wheel * 3.0f)), 0, max_scroll); }
    }
    scroll_ = std::clamp(scroll_, 0, max_scroll);

    p.dl().push_clip(body.expanded(-6.0f));
    float y = body.min.y + 10.0f;
    if (!lines_.empty()) {
        const float gutter_w = 42.0f;
        const bool  dark     = env_.theme.dark();
        p.dl().rect_filled({{body.min.x + gutter_w + 8.0f, body.min.y + 6.0f}, {body.min.x + gutter_w + 9.0f, body.max.y - 6.0f}}, pal.divider);
        for (std::size_t i = static_cast<std::size_t>(scroll_); i < lines_.size() && y + lh <= body.max.y - 6.0f; ++i) {
            p.text_right(rect{{body.min.x + 6.0f, y}, {body.min.x + gutter_w + 2.0f, y + lh}}, std::format("{}", i + 1), pal.text_faint, f.mono);
            draw_highlighted_line(p, rect{{body.min.x + gutter_w + 18.0f, y}, {body.max.x - 14.0f, y + lh}}, lines_[i], f.mono, dark);
            y += lh;
        }
    } else {
        for (std::size_t i = static_cast<std::size_t>(scroll_); i < hex_lines_.size() && y + lh <= body.max.y - 6.0f; ++i) {
            const std::string& line = hex_lines_[i];
            const float        x0   = body.min.x + 14.0f;
            if (line.size() > 60) {
                p.text_left(rect{{x0, y}, {x0 + 80.0f, y + lh}}, std::string_view{line}.substr(0, 8), pal.text_faint, f.mono);
                const float hx = x0 + p.measure(f.mono, "00000000  ").x;
                p.text_left(rect{{hx, y}, {body.max.x - 14.0f, y + lh}}, std::string_view{line}.substr(10, 49), pal.text, f.mono);
                const float ax = hx + p.measure(f.mono, std::string_view{line}.substr(10, 50)).x;
                p.text_left(rect{{ax, y}, {body.max.x - 14.0f, y + lh}}, std::string_view{line}.substr(60), pal.accent, f.mono);
            } else {
                p.text_left(rect{{x0, y}, {body.max.x - 14.0f, y + lh}}, line, pal.text_dim, f.mono);
            }
            y += lh;
        }
    }
    p.dl().pop_clip();
}

} // namespace prism::ui
