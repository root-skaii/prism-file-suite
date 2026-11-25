#include "ui/properties_dialog.hpp"

#include "core/file_ops.hpp"
#include "core/path.hpp"
#include "core/shell_ops.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numeric>
#include <utility>

namespace prism::ui {

using namespace strata;

namespace {

constexpr std::uint32_t attr_readonly   = 0x1;
constexpr std::uint32_t attr_hidden     = 0x2;
constexpr std::uint32_t attr_system     = 0x4;
constexpr std::uint32_t attr_archive    = 0x20;
constexpr std::uint32_t attr_reparse    = 0x400;
constexpr std::uint32_t attr_compressed = 0x800;
constexpr std::uint32_t attr_encrypted  = 0x4000;

std::string bytes_text(std::uint64_t n)
{
    std::string digits = std::to_string(n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) { digits.insert(static_cast<std::size_t>(i), ","); }
    return digits + " " + std::string{tr_n("byte", "bytes", static_cast<long long>(n))};
}

[[nodiscard]] core::FileKind kind_of(const core::FileInfo& f)
{
    if (f.is_dir) { return core::FileKind::folder; }
    const std::size_t dot = f.name.rfind('.');
    if (dot == std::string::npos || dot == 0) { return core::FileKind::other; }
    return core::classify_extension(core::text::lower_ascii(std::string_view{f.name}.substr(dot + 1)));
}

[[nodiscard]] std::string upper_ext(const core::FileInfo& f)
{
    const std::size_t dot = f.name.rfind('.');
    if (f.is_dir || dot == std::string::npos || dot == 0 || dot + 1 >= f.name.size()) { return {}; }
    std::string e{std::string_view{f.name}.substr(dot + 1)};
    for (char& c : e) { if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 32); } }
    return e;
}

[[nodiscard]] char32_t icon_for_group(std::string_view title) noexcept
{
    if (title == "Image" || title == "Camera") { return glyph::picture; }
    if (title == "Video")    { return glyph::video; }
    if (title == "Audio")    { return glyph::music; }
    if (title == "Document") { return glyph::document; }
    if (title == "Shortcut") { return glyph::link; }
    return glyph::info;
}

[[nodiscard]] std::string aspect_text(std::uint32_t w, std::uint32_t h)
{
    if (w == 0 || h == 0) { return {}; }
    const std::uint32_t g = std::gcd(w, h);
    const std::uint32_t rw = w / g, rh = h / g;
    if (rw <= 32 && rh <= 32) { return std::format("{}:{}", rw, rh); }
    return std::format("{:.2f}:1", static_cast<double>(w) / static_cast<double>(h));
}

[[nodiscard]] std::string mp_text(std::uint32_t w, std::uint32_t h)
{
    const double mp = (static_cast<double>(w) * static_cast<double>(h)) / 1000000.0;
    if (mp >= 10.0) { return std::format("{:.1f} MP", mp); }
    if (mp >= 0.1)  { return std::format("{:.2f} MP", mp); }
    return std::format("{} px", static_cast<std::uint64_t>(w) * h);
}

} // namespace

void PropertiesDialog::open(std::vector<std::string> paths, int tab)
{
    if (paths.empty()) { return; }
    release();
    paths_       = std::move(paths);
    tab_         = std::clamp(tab, 0, 2);
    want_open_   = true;
    fade_frames_ = 0;
    reload();
}

void PropertiesDialog::release()
{
    info_job_.cancel();
    loading_ = false;
    scans_.clear();
    hash_.cancel();
    hash_started_ = false;
    renaming_     = false;
    compare_.clear();
}

void PropertiesDialog::reload(bool sync)
{
    bin_ = paths_.size() == 1 ? core::recycle::read(paths_.front()) : std::nullopt;
    if (sync) {
        infos_.clear();
        for (const std::string& p : paths_) { infos_.push_back(core::query_file_info(p)); }
        loading_ = false;
    } else {
        loading_ = true;
        info_job_.start(paths_, env_.window.waker());
    }
    if (scans_.empty() && !infos_.empty()) {
        for (const core::FileInfo& i : infos_) {
            auto scan = std::make_unique<core::FolderScan>();
            // never launch a full-drive crawl when inspecting a drive root (e.g. C:\): storage stats come from GetDiskFreeSpaceEx
            if (i.is_dir && !i.is_drive) { scan->start(i.path); }
            scans_.push_back(std::move(scan));
        }
    }
}

void PropertiesDialog::poll_info()
{
    if (!loading_ || !info_job_.ready()) { return; }
    infos_   = info_job_.take();
    loading_ = false;
    scans_.clear();
    for (const core::FileInfo& i : infos_) {
        auto scan = std::make_unique<core::FolderScan>();
        if (i.is_dir && !i.is_drive) { scan->start(i.path); }
        scans_.push_back(std::move(scan));
    }
    ++fade_gen_;
    fade_frames_ = 0;
}

void PropertiesDialog::note(Kit& kit, std::string_view id, std::string_view text)
{
    context& ui = kit.painter().ui();
    kit.card_begin(id, {}, 56.0f);
    const item_result r = ui.custom_item(std::string{id} + "_t", {ui.content_width(), 56.0f});
    kit.painter().text_center(r.bounds, text, kit.painter().pal().text_dim, kit.painter().fonts().body);
    kit.card_end(id);
}

// --- header -----------------------------------------------------------------------------------------------------

void PropertiesDialog::header(Kit& kit)
{
    Painter&       p   = kit.painter();
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet  f   = p.fonts();
    const core::FileInfo& first = infos_.front();
    const bool single = infos_.size() == 1;

    std::uint64_t files = 0, folders = 0, size = 0;
    for (const core::FileInfo& i : infos_) {
        if (i.is_dir) { ++folders; } else { ++files; size += i.size; }
    }

    const bool  dense   = kit.dense();
    const float head_h  = dense ? 54.0f : 68.0f;
    const float tile_s  = dense ? 46.0f : 60.0f;
    const float gap_x   = dense ? 12.0f : 16.0f;
    const float btn_s   = 32.0f;
    const float actions_w = 4.0f * btn_s + 3.0f * 4.0f + 16.0f + btn_s + 8.0f;

    const item_result r = ui.custom_item("##prop_head", {std::max(120.0f, ui.content_width() - actions_w), head_h});
    drag_zone_ = rect{{0.0f, 0.0f}, {r.bounds.max.x, r.bounds.max.y + 8.0f}};

    // elevated hero icon / thumbnail container
    const rect tile = rect::from_size({r.bounds.min.x, r.bounds.min.y + (head_h - tile_s) * 0.5f}, {tile_s, tile_s});
    const core::FileKind kind = single ? kind_of(first) : core::FileKind::other;
    shape_style ts;
    ts.radius       = radii(14.0f);
    ts.fill_top     = ts.fill_bottom = pal.content;
    ts.border       = pal.border.scaled_alpha(0.85f);
    ts.border_width = 1.0f;
    p.dl().shape(tile, ts);

    bool drawn = false;
    if (single && !first.is_dir && (kind == core::FileKind::image || kind == core::FileKind::video || kind == core::FileKind::pdf)) {
        const platform::Image img = env_.images.get(first.path, static_cast<int>(std::ceil(tile_s * ui.scale())), platform::ImageKind::thumbnail);
        if (img.id != 0) {
            p.image_fit(tile.expanded(-4.0f), img, 10.0f);
            drawn = true;
        }
    }
    if (!drawn) {
        const rect icon = tile.expanded(dense ? -8.0f : -10.0f);
        if (!single) { p.kind_icon(icon, core::FileKind::folder); }
        else if (first.is_drive) { p.glyph_center(icon, glyph::drive, pal.accent, f.big); }
        else if (first.is_dir) { p.folder_icon(icon); }
        else { p.file_icon(icon, kind); }
    }

    const float tx = tile.max.x + gap_x;
    const float title_h = p.line_height(f.title);
    const float sub_h   = p.line_height(f.body);
    const float stack_h = title_h + 4.0f + sub_h;
    const float top_y   = std::round(r.bounds.center().y - stack_h * 0.5f);
    const rect  title_r{{tx, top_y}, {r.bounds.max.x - 4.0f, top_y + title_h + 2.0f}};
    const rect  sub_r{{tx, top_y + title_h + 4.0f}, {r.bounds.max.x - 4.0f, top_y + title_h + 4.0f + sub_h + 2.0f}};

    const bool can_rename = single && !first.is_drive && !bin_ && first.exists;
    std::string title;
    if (bin_) { title = core::path::leaf(bin_->original); }
    else if (single && first.is_drive) {
        title = first.volume_label.empty() ? first.path : std::format("{} ({})", first.volume_label, first.path.substr(0, 2));
    } else if (single) {
        title = first.name;
    } else {
        title = trf_n("{} item", "{} items", static_cast<long long>(infos_.size()), infos_.size());
    }

    if (can_rename && !renaming_) {
        const bool title_hot = r.hovered && title_r.contains(ui.mouse_pos());
        if (title_hot) {
            p.dl().rect_filled(title_r.expanded(2.0f), pal.hover.scaled_alpha(0.65f), 6.0f);
            env_.cmd.set_cursor(cursor_kind::text);
            ui.tooltip(tr("Click to rename"));
            if (ui.mouse_released(mouse_button::left)) {
                renaming_    = true;
                rename_age_  = 0;
                rename_text_ = first.name;
            }
        }
    }
    if (!renaming_) {
        p.text_left(title_r, title, pal.text, f.title);
    }

    std::string sub;
    if (single && first.is_drive) {
        sub = first.type;
        if (first.drive_total > 0) { sub += "  \xc2\xb7  " + core::path::format_size(first.drive_total); }
    } else if (single) {
        sub = first.type;
        if (!first.is_dir) { sub += "  \xc2\xb7  " + core::path::format_size(first.size); }
    } else {
        sub = trf_n("{} file", "{} files", static_cast<long long>(files), files) + ", " + trf_n("{} folder", "{} folders", static_cast<long long>(folders), folders);
        if (size > 0) { sub += "  \xc2\xb7  " + core::path::format_size(size); }
    }
    p.text_left(sub_r, sub, pal.text_dim, f.body);

    // action buttons in the header, optically centred vertically against the icon + title
    const std::string& path = first.path;
    const auto header_btn = [&](std::string_view id, char32_t g, std::string_view tip, bool danger = false) {
        const float pad_y = std::max(0.0f, (head_h - btn_s) * 0.5f);
        const item_result slot = ui.custom_item(std::string{id} + "_s", {btn_s, head_h});
        const rect b = rect::from_size({slot.bounds.min.x, slot.bounds.min.y + pad_y}, {btn_s, btn_s});
        const bool hot = slot.hovered && b.contains(ui.mouse_pos());
        const float hk = ui.animate(std::string{id} + "h", hot ? 1.0f : 0.0f, 18.0f);
        if (hk > 0.01f) {
            const color fill = danger ? pal.close_hover : (slot.held ? pal.pressed : pal.hover);
            p.dl().rect_filled(b, fill.scaled_alpha(hk), 8.0f);
        }
        const color base = danger ? lerp(pal.text_dim, color{255, 255, 255, 255}, hk) : lerp(pal.text_dim, pal.text, hk);
        p.glyph_center(b, g, base);
        if (hot) {
            env_.cmd.set_cursor(cursor_kind::hand);
            ui.tooltip(tip);
        }
        return hot && ui.mouse_released(mouse_button::left);
    };
    ui.same_line();
    if (header_btn("##prop_winprops", glyph::info, tr("Windows properties"))) { (void)core::shell::show_properties(path); }
    kit.hgap(4.0f);
    if (header_btn("##prop_winopts", glyph::list, tr("Windows options"))) {
        std::vector<std::string> all;
        for (const core::FileInfo& i : infos_) { all.push_back(i.path); }
        (void)core::shell::show_native_menu(all, env_.window.handle());
    }
    kit.hgap(4.0f);
    if (header_btn("##prop_show", glyph::open_new, tr("Show in File Explorer")) && !core::shell::reveal_in_explorer(path)) {
        env_.cmd.toast(tr("Could not open File Explorer"), toast_kind::error);
    }
    kit.hgap(4.0f);
    if (header_btn("##prop_copy", glyph::copy, single ? tr("Copy path") : tr("Copy paths"))) {
        std::string all;
        for (const core::FileInfo& i : infos_) {
            if (!all.empty()) { all += "\r\n"; }
            all += i.path;
        }
        env_.cmd.toast(core::shell::set_clipboard_text(all) ? tr("Path copied") : tr("Could not copy to the clipboard"), toast_kind::info);
    }
    kit.hgap(14.0f);
    if (header_btn("##prop_close", glyph::close, tr("Close  (Esc)"), true)) { close(ui); }

    // inline rename field cleanly placed beneath/over the header when active
    if (renaming_) {
        kit.vgap(6.0f);
        const float field_h = 34.0f;
        const float fw = ui.content_width() - 88.0f;
        const bool bad = rename_text_.empty() || rename_text_.find_first_of("\\/:*?\"<>|") != std::string::npos;
        const item_result lead = ui.custom_item("##prop_rn_lead", {0.0f, field_h});
        shape_style fs;
        fs.radius       = radii(8.0f);
        fs.fill_top     = fs.fill_bottom = pal.raised;
        fs.border       = bad ? pal.danger : pal.accent;
        fs.border_width = 1.5f;
        p.dl().shape(rect::from_size({lead.bounds.min.x, lead.bounds.min.y}, {fw, field_h}), fs);
        ui.same_line();
        ui.set_next_item_width(fw);
        ui.push_var(style_var::frame_padding_y, (field_h - p.line_height(f.body)) * 0.5f);
        ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
        if (rename_age_ < 3) {
            std::size_t sel_end = rename_text_.size();
            if (env_.settings.rename_select_stem && !first.is_dir) {
                const std::size_t dot = rename_text_.rfind('.');
                if (dot != std::string::npos && dot > 0) { sel_end = dot; }
            }
            ui.request_text_focus("##prop_rn", 0, sel_end);
        }
        (void)ui.input_text("##prop_rn", rename_text_, tr("File name"));
        ui.pop_color();
        ui.pop_var();
        ++rename_age_;
        kit.hgap(8.0f);
        const bool apply_clicked = kit.button({"##prop_rn_ok", tr("Rename"), 0, Kit::ButtonKind::primary, !bad && rename_text_ != first.name, {}, 80.0f});
        const bool commit = apply_clicked || (ui.key_pressed(key::enter) && !bad);
        const bool cancel = ui.key_pressed(key::escape);
        if (commit && rename_text_ != first.name) {
            if (core::FileOps::rename(first.path, rename_text_)) {
                paths_[0] = core::path::join(core::path::parent(first.path), rename_text_);
                renaming_ = false;
                reload(true);
                if (env_.tabs.size() > 0) { env_.tabs.active().refresh(); }
                if (env_.right != nullptr) { env_.right->refresh(); }
                env_.cmd.toast(tr("Renamed"), toast_kind::success);
            } else {
                env_.cmd.toast(tr("Could not rename: the name is invalid or already used"), toast_kind::error);
            }
        } else if (commit || cancel) {
            renaming_ = false;
        }
    }

    // status & format chips aligned directly under the title text
    if (single) {
        const std::string ext = upper_ext(first);
        bool any = false;
        const float lead_w = tile_s + gap_x;
        const auto add = [&](std::string_view id, std::string_view label, Tone tone, char32_t icon = 0) {
            if (any) { kit.hgap(6.0f); } else { (void)ui.custom_item("##chips_lead", {lead_w, 24.0f}); ui.same_line(); }
            kit.badge(id, label, tone, icon);
            any = true;
        };
        if (!ext.empty()) { add("##chip_ext", ext, Tone::accent); }
        if (first.motw_blocked) { add("##chip_motw", tr("Blocked"), Tone::warning, glyph::shield); }
        if (first.attributes & attr_readonly)   { add("##chip_ro", tr("Read-only"), Tone::warning, glyph::lock); }
        if (first.attributes & attr_hidden)     { add("##chip_hid", tr("Hidden"), Tone::neutral, glyph::eye); }
        if (first.attributes & attr_system)     { add("##chip_sys", tr("System"), Tone::neutral); }
        if (first.attributes & attr_reparse)    { add("##chip_link", tr("Link"), Tone::neutral, glyph::link); }
        if (first.attributes & attr_compressed) { add("##chip_cmp", tr("Compressed"), Tone::neutral, glyph::zip); }
        if (first.attributes & attr_encrypted)  { add("##chip_enc", tr("Encrypted"), Tone::success, glyph::lock); }
        if (!any) { (void)ui.custom_item("##chips_none", {1.0f, 2.0f}); }
    } else {
        (void)ui.custom_item("##chips_none", {1.0f, 2.0f});
    }
    kit.vgap(dense ? 10.0f : 14.0f);
}

// --- general ----------------------------------------------------------------------------------------------------

void PropertiesDialog::general(Kit& kit)
{
    Painter&       p   = kit.painter();
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet  f   = p.fonts();
    const bool single = infos_.size() == 1;
    const core::FileInfo& first = infos_.front();

    // Mark-of-the-Web security banner (downloaded file) with one-click Unblock
    if (single && first.motw_blocked) {
        const float banner_h = first.motw_host.empty() ? 58.0f : 74.0f;
        kit.card_begin("##card_motw", tr("Security"), banner_h, glyph::shield);
        const float pad = Kit::pad();
        const float btn_w = kit.button_width(tr("Unblock"), true, Kit::ButtonKind::primary);
        const float text_w = ui.content_width() - pad * 2.0f - btn_w - 14.0f;
        const item_result r = ui.custom_item("##motw_info", {pad + text_w, banner_h});
        const float lh = p.line_height(f.small);
        const float top = r.bounds.min.y + (first.motw_host.empty() ? (banner_h - lh * 2.0f) * 0.5f : 6.0f);
        p.text_left(rect{{r.bounds.min.x + pad, top}, {r.bounds.max.x, top + lh + 2.0f}}, tr("This file came from another computer and may be blocked."), pal.warning, f.bold);
        if (!first.motw_host.empty()) {
            p.text_left(rect{{r.bounds.min.x + pad, top + lh + 4.0f}, {r.bounds.max.x, top + lh * 2.0f + 6.0f}}, first.motw_host, pal.text_dim, f.small);
        } else {
            p.text_left(rect{{r.bounds.min.x + pad, top + lh + 4.0f}, {r.bounds.max.x, top + lh * 2.0f + 6.0f}}, tr("Unblock removes the Zone.Identifier download tag."), pal.text_dim, f.small);
        }
        ui.same_line();
        if (kit.button({"##btn_unblock", tr("Unblock"), glyph::check, Kit::ButtonKind::primary, true, tr("Remove Mark-of-the-Web (Zone.Identifier)"), btn_w})) {
            want_unblock_ = true;
        }
        kit.card_end("##card_motw");
    }

    // Drive storage visualization (used/free bar + 3 balanced capacity tiles)
    if (single && first.is_drive && first.drive_total > 0) {
        const std::uint64_t used = first.drive_total > first.drive_free ? first.drive_total - first.drive_free : 0;
        const float frac = static_cast<float>(static_cast<double>(used) / static_cast<double>(first.drive_total));
        const Tone bar_tone = frac >= 0.92f ? Tone::danger : (frac >= 0.80f ? Tone::warning : Tone::accent);
        const std::string pct_used = std::format("{:.1f}% used", frac * 100.0f);
        const std::string pct_free = std::format("{:.1f}% free", (1.0f - frac) * 100.0f);
        const std::array<Kit::Stat, 3> tiles{{
            {tr("Used space"), core::path::format_size(used), pct_used, bar_tone, true},
            {tr("Free space"), core::path::format_size(first.drive_free), pct_free, Tone::success, true},
            {tr("Capacity"),   core::path::format_size(first.drive_total), first.filesystem, Tone::neutral, true},
        }};
        kit.card_begin("##card_drive", tr("Storage"), kit.stat_tiles_height() + 26.0f, glyph::drive);
        kit.stat_tiles("##drive_tiles", tiles);
        kit.bar("##drive_bar", frac, bar_tone, 10.0f, Kit::pad());
        kit.card_end("##card_drive");
    }

    // Image / Video / Audio quick-specs hero strip right on General
    const core::FileKind kind = single ? kind_of(first) : core::FileKind::other;
    if (single && !first.is_dir && kind == core::FileKind::image && first.img_width > 0 && first.img_height > 0) {
        const std::string res = std::format("{} \xc3\x97 {}", first.img_width, first.img_height);
        const std::string mp  = mp_text(first.img_width, first.img_height);
        const std::string ar  = aspect_text(first.img_width, first.img_height);
        const std::string col = first.img_bpp > 0 ? std::format("{}-bit", first.img_bpp) : std::string{"—"};
        const std::string dpi = first.img_dpi_x > 0.5 ? std::format("{:.0f} DPI", first.img_dpi_x) : std::string{};
        const std::array<Kit::Stat, 3> tiles{{
            {tr("Resolution"), res, tr("pixels"), Tone::accent, true},
            {tr("Aspect & Size"), ar, mp, Tone::neutral, true},
            {tr("Color depth"), col, dpi, Tone::neutral, true},
        }};
        kit.card_begin("##card_img", tr("Image"), kit.stat_tiles_height(), glyph::picture);
        kit.stat_tiles("##img_tiles", tiles);
        kit.card_end("##card_img");
    } else if (single && !first.is_dir && (kind == core::FileKind::video || kind == core::FileKind::audio) && (!first.media_duration.empty() || first.img_width > 0)) {
        std::array<Kit::Stat, 3> tiles;
        std::size_t n = 0;
        if (first.img_width > 0 && first.img_height > 0) {
            tiles[n++] = {tr("Resolution"), std::format("{} \xc3\x97 {}", first.img_width, first.img_height), aspect_text(first.img_width, first.img_height), Tone::accent, true};
        }
        if (!first.media_duration.empty()) {
            tiles[n++] = {tr("Duration"), first.media_duration, {}, Tone::neutral, true};
        }
        if (!first.media_bitrate.empty()) {
            tiles[n++] = {tr("Bitrate"), first.media_bitrate, {}, Tone::neutral, true};
        }
        if (n > 0) {
            kit.card_begin("##card_media", kind == core::FileKind::video ? tr("Video") : tr("Audio"), kit.stat_tiles_height(), kind == core::FileKind::video ? glyph::video : glyph::music);
            kit.stat_tiles("##media_tiles", std::span<const Kit::Stat>{tiles.data(), n});
            kit.card_end("##card_media");
        }
    }

    // Size breakdown (for files, regular folders, and multi-selections; skipped for drives)
    if (!(single && first.is_drive)) {
        std::uint64_t size = 0, disk = 0, files = 0, folders = 0;
        bool scanning = false;
        for (std::size_t i = 0; i < infos_.size(); ++i) {
            const core::FileInfo& info = infos_[i];
            if (info.is_dir) {
                ++folders;
                if (i < scans_.size()) {
                    size += scans_[i]->bytes();
                    disk += scans_[i]->on_disk();
                    files += scans_[i]->files();
                    folders += scans_[i]->folders();
                    scanning |= !scans_[i]->done();
                }
            } else {
                ++files;
                size += info.size;
                disk += info.size_on_disk;
            }
        }
        const bool is_folder_item = !single || first.is_dir;
        std::array<Kit::Stat, 3> tiles;
        std::size_t n = 0;
        const std::string size_sub = bytes_text(size);
        tiles[n++] = {tr("Size"), core::path::format_size(size), scanning ? tr("counting...") : std::string_view{size_sub}, Tone::neutral, true};
        const std::string slack = disk > size ? trf("+{} slack", core::path::format_size(disk - size)) : std::string{};
        tiles[n++] = {tr("On disk"), core::path::format_size(disk), slack, Tone::neutral, true};
        const std::uint64_t sub_folders = single && !scans_.empty() ? scans_[0]->folders() : folders;
        const std::string contains = trf_n("{} file", "{} files", static_cast<long long>(files), files);
        const std::string contains_sub = trf_n("{} folder", "{} folders", static_cast<long long>(sub_folders), sub_folders);
        if (is_folder_item) { tiles[n++] = {tr("Contains"), contains, contains_sub, Tone::neutral, false}; }
        kit.card_begin("##card_size", tr("Size"), kit.stat_tiles_height());
        kit.stat_tiles("##tiles", std::span<const Kit::Stat>{tiles.data(), n});
        kit.card_end("##card_size");
    }

    // Location and timestamps
    std::string location;
    if (single) {
        const std::string where = core::path::parent(first.path);
        location = where.empty() ? first.path : where;
    } else {
        const std::string p0 = core::path::parent(first.path);
        const std::string p0_l = core::text::lower_ascii(p0);
        const bool same_parent = std::ranges::all_of(infos_, [&](const core::FileInfo& info) {
            return core::text::lower_ascii(core::path::parent(info.path)) == p0_l;
        });
        location = same_parent ? (p0.empty() ? first.path : p0) : std::string{tr("Multiple folders")};
    }

    const float rh = kit.row_h();
    if (bin_) {
        const std::string deleted_from = core::path::parent(bin_->original);
        kit.card_begin("##card_where", tr("Recycle Bin"), rh * 4.0f, glyph::delete_);
        kit.key_value({"##kv_orig", tr("Original location"), deleted_from.empty() ? bin_->original : deleted_from, false, true, true});
        kit.key_value({"##kv_origname", tr("Original name"), core::path::leaf(bin_->original), false, true, true});
        kit.key_value({"##kv_deleted", tr("Deleted"), core::path::format_time(bin_->deleted), false, true, true});
        kit.key_value({"##kv_binpath", tr("Stored as"), first.path, false, true, false});
        kit.card_end("##card_where");
        return;
    }

    const bool show_loc = !(single && first.is_drive);
    const std::string created  = single ? core::path::format_time(first.created) : std::string{};
    const std::string modified = single ? core::path::format_time(first.modified) : std::string{};
    const std::string accessed = single ? core::path::format_time(first.accessed) : std::string{};
    int where_rows = show_loc ? 1 : 0;
    if (single) {
        if (!created.empty())  { ++where_rows; }
        if (!modified.empty()) { ++where_rows; }
        if (!accessed.empty()) { ++where_rows; }
        if (!first.owner.empty()) { ++where_rows; }
        if (!first.link_target.empty()) { ++where_rows; }
    }
    if (where_rows > 0) {
        kit.card_begin("##card_where", single ? tr("Overview") : tr("Selection"), rh * static_cast<float>(where_rows), glyph::info);
        int drawn_rows = 0;
        const auto kv_row = [&](std::string_view id, std::string_view k, std::string_view v, bool copyable = true) {
            ++drawn_rows;
            kit.key_value({id, k, v, false, copyable, drawn_rows < where_rows});
        };
        if (show_loc) { kv_row("##kv_loc", tr("Location"), location, single || location != tr("Multiple folders")); }
        if (single) {
            if (!created.empty())  { kv_row("##kv_created", tr("Created"), created); }
            if (!modified.empty()) { kv_row("##kv_modified", tr("Modified"), modified); }
            if (!accessed.empty()) { kv_row("##kv_accessed", tr("Accessed"), accessed); }
            if (!first.owner.empty()) { kv_row("##kv_owner", tr("Owner"), first.owner); }
            if (!first.link_target.empty()) { kv_row("##kv_target", tr("Target"), first.link_target); }
        }
        kit.card_end("##card_where");
    }

    if (single && first.is_drive) { return; }

    // Attributes: works for both single and multi-file selections
    bool all_ro = true, all_hid = true;
    for (const core::FileInfo& info : infos_) {
        if ((info.attributes & attr_readonly) == 0) { all_ro = false; }
        if ((info.attributes & attr_hidden) == 0)   { all_hid = false; }
    }
    std::string flags;
    if (single) {
        const auto flag = [&](std::uint32_t bit, std::string_view name) {
            if ((first.attributes & bit) != 0) { flags += flags.empty() ? std::string{name} : ", " + std::string{name}; }
        };
        flag(attr_system, tr("System"));
        flag(attr_archive, tr("Archive"));
        flag(attr_compressed, tr("Compressed"));
        flag(attr_encrypted, tr("Encrypted"));
        flag(attr_reparse, tr("Link"));
    }
    const float body = kit.row_h() * 2.0f + 16.0f * 2.0f + (flags.empty() ? 0.0f : kit.row_h());
    kit.card_begin("##card_attr", tr("Attributes"), body, glyph::lock);
    bool ro = all_ro;
    bool hid = all_hid;
    if (kit.switch_row("##sw_ro", tr("Read-only"), tr("Prevent accidental modification or deletion"), ro, true)) { attr_flag_ = attr_readonly; attr_on_ = ro; }
    if (kit.switch_row("##sw_hid", tr("Hidden"), tr("Not shown in listings unless hidden files are on"), hid, !flags.empty())) { attr_flag_ = attr_hidden; attr_on_ = hid; }
    if (!flags.empty()) { kit.key_value({"##kv_flags", tr("Also"), flags, false, false, false}); }
    kit.card_end("##card_attr");
    (void)ui;
}

// --- details ----------------------------------------------------------------------------------------------------

void PropertiesDialog::details(Kit& kit)
{
    if (infos_.size() != 1) {
        note(kit, "##det_note", tr("Select a single item to see its details."));
        return;
    }
    const core::FileInfo& f = infos_.front();
    const float rh = kit.row_h();

    // Format-specific cards: skip rows already shown in General's hero card so Details adds new information instead of repeating it
    const auto shown_on_general = [](std::string_view grp, std::string_view key) noexcept {
        if (grp == "Image") { return key == "Dimensions" || key == "Megapixels" || key == "Aspect ratio" || key == "DPI"; }
        if (grp == "Video") { return key == "Resolution" || key == "Duration" || key == "Total bitrate"; }
        if (grp == "Audio") { return key == "Duration" || key == "Bitrate"; }
        return false;
    };
    for (std::size_t g = 0; g < f.meta_groups.size(); ++g) {
        const core::MetaGroup& grp = f.meta_groups[g];
        std::vector<std::pair<std::string_view, std::string_view>> extra;
        for (const auto& [k, v] : grp.rows) {
            if (!shown_on_general(grp.title, k)) { extra.emplace_back(k, v); }
        }
        if (extra.empty()) { continue; }
        const std::string cid = "##card_mg" + std::to_string(g);
        kit.card_begin(cid, tr(grp.title), rh * static_cast<float>(extra.size()), icon_for_group(grp.title));
        for (std::size_t i = 0; i < extra.size(); ++i) {
            kit.key_value({"##mg" + std::to_string(g) + "_" + std::to_string(i), tr(extra[i].first), extra[i].second, false, true, i + 1 < extra.size(), 155.0f});
        }
        kit.card_end(cid);
    }

    if (!f.version.empty()) {
        kit.card_begin("##card_ver", tr("Version information"), rh * static_cast<float>(f.version.size()), glyph::app);
        for (std::size_t i = 0; i < f.version.size(); ++i) {
            kit.key_value({"##vr" + std::to_string(i), f.version[i].first, f.version[i].second, false, true, i + 1 < f.version.size(), 165.0f});
        }
        kit.card_end("##card_ver");
    }

    const std::string volume  = f.volume + (f.filesystem.empty() ? "" : "  (" + f.filesystem + ")");
    const std::string file_id = std::format("0x{:016X}", f.file_id);
    const std::string links   = std::to_string(f.hard_links);
    const std::string attrs   = std::format("0x{:X}", f.attributes);
    struct Row { std::string_view key; const std::string* value; bool mono; };
    std::vector<Row> rows;
    rows.push_back({tr("Full path"), &f.path, false});
    if (!f.volume.empty()) { rows.push_back({tr("Volume"), &volume, false}); }
    if (f.file_id != 0) { rows.push_back({tr("File ID"), &file_id, true}); }
    if (f.hard_links > 1) { rows.push_back({tr("Hard links"), &links, false}); }
    rows.push_back({tr("Attributes"), &attrs, true});

    kit.card_begin("##card_file", tr("File"), rh * static_cast<float>(rows.size()), glyph::document);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        kit.key_value({"##dr" + std::to_string(i), rows[i].key, *rows[i].value, rows[i].mono, true, i + 1 < rows.size()});
    }
    kit.card_end("##card_file");

    if (!f.streams.empty()) {
        kit.card_begin("##card_ads", tr("Alternate data streams"), rh * static_cast<float>(f.streams.size()), glyph::shield);
        for (std::size_t i = 0; i < f.streams.size(); ++i) {
            kit.key_value({"##ads" + std::to_string(i), core::path::format_size(f.streams[i].second), f.streams[i].first, false, true, i + 1 < f.streams.size(), 90.0f});
        }
        kit.card_end("##card_ads");
    }
}

// --- hashes -----------------------------------------------------------------------------------------------------

void PropertiesDialog::hashes(Kit& kit)
{
    Painter&       p   = kit.painter();
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet  f   = p.fonts();
    if (infos_.size() != 1 || infos_.front().is_dir) {
        note(kit, "##hash_note", tr("Hashes are computed for a single file."));
        return;
    }
    if (!hash_started_) {
        hash_started_ = true;
        hash_.start(infos_.front().path);
    }
    const float rh = kit.row_h();
    if (hash_.failed()) {
        note(kit, "##hash_fail", tr("The file could not be read."));
        return;
    }
    if (hash_.running()) {
        kit.card_begin("##card_hash", tr("Checksums"), rh + 24.0f, glyph::shield);
        const item_result r = ui.custom_item("##hash_run", {ui.content_width(), rh});
        p.text_left(r.bounds, tr("Computing MD5, SHA-1 and SHA-256 ..."), pal.text_dim, f.body, Kit::pad());
        p.text_right(r.bounds, std::format("{:.0f} %", hash_.progress() * 100.0f), pal.text, f.bold, Kit::pad());
        kit.bar("##hash_bar", hash_.progress(), Tone::accent, 8.0f, Kit::pad());
        kit.card_end("##card_hash");
        ui.invalidate();
        return;
    }

    const std::string md5 = hash_.md5(), sha1 = hash_.sha1(), sha256 = hash_.sha256();
    std::string cleaned = core::text::lower_ascii(compare_);
    cleaned.erase(std::remove_if(cleaned.begin(), cleaned.end(), [](char ch) { return ch <= ' '; }), cleaned.end());
    const bool m_md5 = !cleaned.empty() && cleaned == md5;
    const bool m_s1  = !cleaned.empty() && cleaned == sha1;
    const bool m_256 = !cleaned.empty() && cleaned == sha256;
    const char* algo = m_md5 ? "MD5" : (m_s1 ? "SHA-1" : (m_256 ? "SHA-256" : nullptr));

    kit.card_begin("##card_hash", tr("Checksums"), rh * 3.0f, glyph::shield);
    kit.key_value({"##h_md5", tr("MD5"), md5, true, true, true, 78.0f, m_md5 ? Tone::success : Tone::neutral});
    kit.key_value({"##h_sha1", tr("SHA-1"), sha1, true, true, true, 78.0f, m_s1 ? Tone::success : Tone::neutral});
    kit.key_value({"##h_sha256", tr("SHA-256"), sha256, true, true, false, 78.0f, m_256 ? Tone::success : Tone::neutral});
    kit.card_end("##card_hash");

    // compare with a checksum from a download page
    const float field_h  = 36.0f;
    const float paste_w  = field_h;
    const float badge_w  = cleaned.empty() ? 0.0f : 118.0f;
    kit.card_begin("##card_cmp", tr("Verify checksum"), 6.0f + field_h);
    kit.vgap(4.0f);
    const item_result lead = ui.custom_item("##cmp_lead", {Kit::pad(), field_h});
    const float fw = std::max(80.0f, ui.content_width() - Kit::pad() * 2.0f - paste_w - (badge_w > 0.0f ? badge_w + 14.0f : 8.0f));
    shape_style fs;
    fs.radius       = radii(8.0f);
    fs.fill_top     = fs.fill_bottom = pal.raised;
    fs.border       = cleaned.empty() ? pal.border : (algo != nullptr ? pal.success : pal.danger);
    fs.border_width = cleaned.empty() ? 1.0f : 1.5f;
    p.dl().shape(rect::from_size({lead.bounds.max.x, lead.bounds.min.y}, {fw, field_h}), fs);
    ui.same_line();
    ui.set_next_item_width(fw);
    ui.push_var(style_var::frame_padding_y, (field_h - p.line_height(f.body)) * 0.5f);
    ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
    (void)ui.input_text("##compare", compare_, tr("Paste a checksum to verify"));
    ui.pop_color();
    ui.pop_var();
    kit.hgap(8.0f);
    if (kit.icon_button("##cmp_paste", glyph::copy, tr("Paste from clipboard"), true, false, paste_w)) {
        if (std::string clip = core::shell::get_clipboard_text(); !clip.empty()) { compare_ = std::move(clip); }
    }
    if (!cleaned.empty()) {
        kit.hgap(6.0f);
        const std::string label = algo != nullptr ? trf("Match \xc2\xb7 {}", algo) : std::string{tr("No match")};
        const float bw = kit.chip_width(label, true);
        (void)ui.custom_item("##cmp_pad", {std::max(4.0f, badge_w - bw), field_h});
        ui.same_line();
        kit.badge("##cmp_badge", label, algo != nullptr ? Tone::success : Tone::danger, algo != nullptr ? glyph::check : glyph::close, field_h);
    }
    kit.card_end("##card_cmp");
}

// --- frame ------------------------------------------------------------------------------------------------------

void PropertiesDialog::frame(Painter& p)
{
    context& ui = p.ui();
    if (want_open_) {
        want_open_ = false;
        shown_     = true;
        Kit::reset_focus();
        if (!embedded_) { ui.open_modal("Properties"); }
    }
    if (!shown_) { return; }
    poll_info();
    if (embedded_) {
        const DialogStyle style{ui, p.pal()};
        const bool open = ui.begin_window("##prop_win", {0.0f, 0.0f}, ui.display_size(),
                                          window_flags::placed | window_flags::no_title_bar | window_flags::no_background | window_flags::no_collapse);
        if (open) {
            if (ui.key_pressed(key::escape) && !ui.want_text_input()) { close(ui); }
            else { content(p); }
        }
        ui.end_window();
        return;
    }
    const DialogStyle style{ui, p.pal()};
    const float width = std::min(680.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Properties", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        release();
        Kit::reset_focus();
        return;
    }
    content(p);
}

void PropertiesDialog::close(context& ui)
{
    if (!embedded_) { ui.close_modal(); return; }
    shown_ = false;
    release();
    Kit::reset_focus();
}

void PropertiesDialog::content(Painter& p)
{
    context& ui = p.ui();
    Kit kit{p, env_.cmd};
    kit.begin_frame();

    if (infos_.empty()) {
        if (loading_) {
            const item_result r = ui.custom_item("##prop_loading", {ui.content_width(), 140.0f});
            p.text_center(r.bounds, tr("Loading properties ..."), p.pal().text_dim, p.fonts().body);
            ui.invalidate();
        }
        return;
    }

    header(kit);

    // full-width centered pill tab bar
    const std::array<std::string_view, 3> tabs{tr("General"), tr("Details"), tr("Hashes")};
    const int before = tab_;
    const float tab_w = std::min(380.0f, ui.content_width());
    const float pad_l = std::max(0.0f, (ui.content_width() - tab_w) * 0.5f);
    if (pad_l > 1.0f) {
        (void)ui.custom_item("##tab_center", {pad_l, 1.0f});
        ui.same_line();
    }
    if (kit.pill_tabs("##prop_tabs", tabs, tab_, tab_w) && before != tab_) {
        ++fade_gen_;
        fade_frames_ = 0;
    }
    kit.vgap(kit.dense() ? 10.0f : 14.0f);

    float max_h, body_h;
    if (embedded_) {
        const float top = ui.custom_item("##prop_body_top", {1.0f, 0.0f}).bounds.min.y;
        body_h = max_h = std::max(60.0f, ui.display_size().y - top - 18.0f);
    } else {
        const float chrome = 266.0f - (kit.dense() ? 36.0f : 0.0f);
        max_h  = std::clamp(ui.display_size().y - chrome, 260.0f, 660.0f);
        body_h = ui.animate("prop_bodyh", std::clamp(content_h_ + 2.0f, 160.0f, max_h), 18.0f);
    }
    const std::string fade_key = "prop_fade" + std::to_string(fade_gen_);
    const float fade = ui.animate(fade_key, fade_frames_ == 0 ? 0.0f : 1.0f, 18.0f);
    if (fade_frames_ < 2) { ++fade_frames_; }
    {
        auto body = ui.child("##prop_body", {ui.content_width() + (content_h_ + 2.0f > max_h ? 16.0f : 0.0f), body_h}, child_flags::no_padding);
        if (body) {
            p.dl().push_alpha(0.15f + 0.85f * fade);
            const float slide = embedded_ ? 0.0f : (1.0f - fade) * 8.0f;
            if (slide > 0.5f) { kit.vgap(slide); }
            switch (tab_) {
            case 0:  general(kit); break;
            case 1:  details(kit); break;
            default: hashes(kit); break;
            }
            if (slide > 0.5f) { kit.vgap(8.0f - slide); }
            p.dl().pop_alpha();
            content_h_ = kit.cards_height() + (slide > 0.5f ? 8.0f : 0.0f);
        }
    }

    // apply deferred mutations outside the drawing flow
    if (want_unblock_) {
        want_unblock_ = false;
        if (core::unblock_file(infos_.front().path)) {
            env_.cmd.toast(tr("File unblocked"), toast_kind::success);
            reload(true);
        } else {
            env_.cmd.toast(tr("Could not unblock file"), toast_kind::error);
        }
    }
    if (attr_flag_ != 0) {
        const std::uint32_t flag = std::exchange(attr_flag_, 0);
        for (const core::FileInfo& info : infos_) { (void)core::set_file_attribute(info.path, flag, attr_on_); }
        reload(true);
        if (flag == attr_hidden) {
            if (env_.tabs.size() > 0) { env_.tabs.active().refresh(); }
            if (env_.right != nullptr) { env_.right->refresh(); }
        }
    }
}

} // namespace prism::ui
