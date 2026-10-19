#include "ui/quick_look.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "ui/tr.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"
#include "ui/preview_pane.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <cmath>

namespace prism::ui {

using namespace strata;
using core::FileKind;

void QuickLook::load(const std::string& path)
{
    shown_ = path;
    info_  = core::query_file_info(path);
    lines_.clear();
    kind_ = info_.is_dir ? FileKind::folder : FileKind::other;
    if (info_.is_dir) { return; }
    const std::size_t dot = info_.name.rfind('.');
    const std::string ext = dot == std::string::npos || dot == 0 ? std::string{} : core::text::lower_ascii(std::string_view{info_.name}.substr(dot + 1));
    kind_ = core::classify_extension(ext);
    const bool text = kind_ == FileKind::text || kind_ == FileKind::code || kind_ == FileKind::config;
    if (text && info_.size < (32u << 20)) { lines_ = read_text_head(path, 48 * 1024, 400); }
}

void QuickLook::frame(Painter& p, const rect& area)
{
    if (!open_) { return; }
    context&          ui  = p.ui();
    const Palette&    pal = p.pal();
    const FontSet&    f   = env_.fonts;
    model::Tab&       tab = env_.tab();
    const model::Listing& L = tab.listing();

    if (L.selected_count() != 1) { open_ = false; return; }
    const std::string target = core::path::join(tab.path(), L.at(L.selected_rows().front()).name);
    if (target != shown_) { load(target); }

    const float w = std::min(area.width() - 48.0f, 1000.0f);
    const float h = std::min(area.height() - 32.0f, 760.0f);
    if (w < 240.0f || h < 200.0f) { return; }
    const rect card = rect::from_size(area.center() - vec2{w * 0.5f, h * 0.5f}, {w, h});

    auto fg = ui.layer(layer::foreground);
    p.dl().rect_filled(area, color{0, 0, 0, 120}, 0.0f);
    p.dl().rect_filled(card.expanded(3.0f), color{0, 0, 0, 80}, 16.0f);
    shape_style bg;
    bg.radius       = radii(14.0f);
    bg.fill_top     = bg.fill_bottom = pal.content;
    bg.border       = pal.border;
    bg.border_width = 1.0f;
    p.dl().shape(card, bg);

    const float pad = 18.0f;
    const rect  head{{card.min.x + pad, card.min.y + 12.0f}, {card.max.x - pad, card.min.y + 36.0f}};
    p.text_left(head, info_.name.empty() ? target : info_.name, pal.text, f.bold);
    std::string sub = info_.type;
    if (!info_.is_dir) { sub += "  -  " + core::path::format_size(info_.size); }
    sub += "  -  " + core::path::format_time(info_.modified);
    p.text_left(rect{{head.min.x, head.max.y}, {card.max.x - pad - 150.0f, head.max.y + 20.0f}}, sub, pal.text_dim, f.small);
    p.text_left(rect{{card.max.x - pad - 150.0f, head.min.y + 4.0f}, {card.max.x - pad, head.min.y + 24.0f}}, trf("{} or Esc to close", key_hint(env_.binds, "quick_look")), pal.text_faint, f.small);

    const rect body{{card.min.x + pad, card.min.y + 66.0f}, {card.max.x - pad, card.max.y - pad}};
    p.dl().rect_filled(body, pal.raised, 10.0f);

    bool drawn = false;
    if (!info_.is_dir) {
        const int px = static_cast<int>(std::ceil(std::min(body.width(), body.height()) * ui.scale()));
        const platform::Image img = env_.images.get(target, px, platform::ImageKind::thumbnail);
        const bool pictured = kind_ == FileKind::image || kind_ == FileKind::video || kind_ == FileKind::pdf || kind_ == FileKind::document ||
                              kind_ == FileKind::presentation || kind_ == FileKind::spreadsheet;
        if (img.id != 0 && pictured) {
            p.image_fit(body.expanded(-8.0f), img, 8.0f);
            drawn = true;
        }
    }
    if (drawn) { return; }
    if (lines_.empty()) {
        const float s = std::min(body.width(), body.height()) * 0.4f;
        p.kind_icon(rect::from_size(body.center() - vec2{s * 0.5f, s * 0.5f}, {s, s}), kind_);
        return;
    }
    p.dl().push_clip(body.expanded(-6.0f));
    const float lh = p.line_height(f.mono) + 2.0f;
    float       y  = body.min.y + 10.0f;
    for (const std::string& line : lines_) {
        if (y + lh > body.max.y - 6.0f) { break; }
        p.text_left(rect{{body.min.x + 14.0f, y}, {body.max.x - 14.0f, y + lh}}, line, pal.text_dim, f.mono);
        y += lh;
    }
    p.dl().pop_clip();
}

} // namespace prism::ui
