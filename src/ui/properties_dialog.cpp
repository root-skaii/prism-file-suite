#include "ui/properties_dialog.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "ui/tr.hpp"
#include "core/shell_ops.hpp"
#include "ui/tr.hpp"
#include "core/text.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
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

// "1,468,006 bytes"
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
    scans_.clear();
    hash_.cancel();
    hash_started_ = false;
    compare_.clear();
}

void PropertiesDialog::reload()
{
    infos_.clear();
    for (const std::string& p : paths_) { infos_.push_back(core::query_file_info(p)); }
    bin_ = paths_.size() == 1 ? core::recycle::read(paths_.front()) : std::nullopt;
    if (scans_.empty()) {
        for (const core::FileInfo& i : infos_) {
            auto scan = std::make_unique<core::FolderScan>();
            if (i.is_dir) { scan->start(i.path); }
            scans_.push_back(std::move(scan));
        }
    }
}

void PropertiesDialog::note(Kit& kit, std::string_view id, std::string_view text)
{
    context& ui = kit.painter().ui();
    kit.card_begin(id, {}, 52.0f);
    const item_result r = ui.custom_item(std::string{id} + "_t", {ui.content_width(), 52.0f});
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
    for (std::size_t i = 0; i < infos_.size(); ++i) {
        if (infos_[i].is_dir) { ++folders; } else { ++files; size += infos_[i].size; }
    }

    const float close_w = 4.0f * 32.0f + 3.0f * 4.0f + 14.0f + 36.0f + 8.0f;
    const item_result r = ui.custom_item("##prop_head", {ui.content_width() - close_w, 64.0f});
    const rect tile = rect::from_size({r.bounds.min.x, r.bounds.min.y + 4.0f}, {56.0f, 56.0f});
    shape_style ts;
    ts.radius       = radii(14.0f);
    ts.fill_top     = ts.fill_bottom = pal.content;
    ts.border       = pal.divider;
    ts.border_width = 1.0f;
    p.dl().shape(tile, ts);
    const core::FileKind kind = single ? kind_of(first) : core::FileKind::other;
    bool drawn = false;
    if (single && !first.is_dir && (kind == core::FileKind::image || kind == core::FileKind::video || kind == core::FileKind::pdf)) {
        const platform::Image img = env_.images.get(first.path, static_cast<int>(std::ceil(56.0f * ui.scale())), platform::ImageKind::thumbnail);
        if (img.id != 0) {
            p.image_fit(tile.expanded(-3.0f), img, 11.0f);
            drawn = true;
        }
    }
    if (!drawn) {
        const rect icon = tile.expanded(-8.0f);
        if (!single) { p.kind_icon(icon, core::FileKind::folder); }
        else if (first.is_dir) { p.folder_icon(icon); }
        else { p.file_icon(icon, kind); }
    }
    const float tx = tile.max.x + 16.0f;
    const std::string title = bin_ ? core::path::leaf(bin_->original) : single ? first.name :trf_n("{} item", "{} items", static_cast<long long>(infos_.size()), infos_.size());
    p.text_left(rect{{tx, r.bounds.min.y + 6.0f}, {r.bounds.max.x, r.bounds.min.y + 34.0f}}, title, pal.text, f.title);
    std::string sub;
    if (single) {
        sub = first.type;
        if (!first.is_dir) { sub += "  \xc2\xb7  " + core::path::format_size(first.size); }
    } else {
        sub = trf_n("{} file", "{} files", static_cast<long long>(files), files) + ", " + trf_n("{} folder", "{} folders", static_cast<long long>(folders), folders);
        if (size > 0) { sub += "  \xc2\xb7  " + core::path::format_size(size); }
    }
    p.text_left(rect{{tx, r.bounds.min.y + 36.0f}, {r.bounds.max.x, r.bounds.min.y + 58.0f}}, sub, pal.text_dim, f.body);

    // actions sit beside the close button (no footer: the dialog stays compact)
    const std::string& path = first.path;
    ui.same_line();
    if (kit.icon_button("##prop_winprops", glyph::info, tr("Windows properties"), true, false, 32.0f)) { (void)core::shell::show_properties(path); }
    kit.hgap(4.0f);
    if (kit.icon_button("##prop_winopts", glyph::list, tr("Windows options"), true, false, 32.0f)) {
        std::vector<std::string> all;
        for (const core::FileInfo& i : infos_) { all.push_back(i.path); }
        (void)core::shell::show_native_menu(all, env_.window.handle());
    }
    kit.hgap(4.0f);
    if (kit.icon_button("##prop_show", glyph::open_new, tr("Show in File Explorer"), true, false, 32.0f) && !core::shell::reveal_in_explorer(path)) {
        env_.cmd.toast(tr("Could not open File Explorer"), toast_kind::error);
    }
    kit.hgap(4.0f);
    if (kit.icon_button("##prop_copy", glyph::copy, tr("Copy path"), true, false, 32.0f)) {
        env_.cmd.toast(core::shell::set_clipboard_text(path) ? tr("Path copied") : tr("Could not copy to the clipboard"), toast_kind::info);
    }
    kit.hgap(14.0f);
    if (kit.icon_button("##prop_close", glyph::close, tr("Close  (Esc)"), true, true, 32.0f)) {
        ui.close_modal();
    }

    // key chips
    if (single) {
        const std::string ext = upper_ext(first);
        bool any = false;
        const auto add = [&](std::string_view id, std::string_view label, Tone tone, char32_t icon = 0) {
            if (any) { kit.hgap(6.0f); } else { (void)ui.custom_item("##chips_lead", {72.0f, 24.0f}); ui.same_line(); } // (under the name)
            kit.badge(id, label, tone, icon);
            any = true;
        };
        if (!ext.empty()) { add("##chip_ext", ext, Tone::accent); }
        if (first.attributes & attr_readonly)   { add("##chip_ro", "Read-only", Tone::warning, glyph::lock); }
        if (first.attributes & attr_hidden)     { add("##chip_hid", "Hidden", Tone::neutral, glyph::eye); }
        if (first.attributes & attr_system)     { add("##chip_sys", "System", Tone::neutral); }
        if (first.attributes & attr_reparse)    { add("##chip_link", "Link", Tone::neutral, glyph::link); }
        if (first.attributes & attr_compressed) { add("##chip_cmp", "Compressed", Tone::neutral, glyph::zip); }
        if (first.attributes & attr_encrypted)  { add("##chip_enc", "Encrypted", Tone::success, glyph::lock); }
        if (!any) { (void)ui.custom_item("##chips_none", {1.0f, 2.0f}); }
    } else {
        (void)ui.custom_item("##chips_none", {1.0f, 2.0f});
    }
    kit.vgap(14.0f);
}

// --- general ----------------------------------------------------------------------------------------------------

void PropertiesDialog::general(Kit& kit)
{
    Painter&  p  = kit.painter();
    context&  ui = p.ui();
    const bool single = infos_.size() == 1;
    const core::FileInfo& first = infos_.front();

    std::uint64_t size = 0, disk = 0, files = 0, folders = 0;
    bool scanning = false;
    for (std::size_t i = 0; i < infos_.size(); ++i) {
        const core::FileInfo& f = infos_[i];
        if (f.is_dir) {
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
            size += f.size;
            disk += f.size_on_disk;
        }
    }
    const bool is_folder_item = !single || first.is_dir;

    // size breakdown
    std::array<Kit::Stat, 3> tiles;
    std::size_t n = 0;
    tiles[n++] = {tr("Size"), core::path::format_size(size), scanning ? tr("counting...") : std::string_view{}};
    const std::string size_sub = bytes_text(size);
    if (!scanning) { tiles[0].sub = size_sub; }
    const std::string slack = disk > size ? trf("+{} slack", core::path::format_size(disk - size)) : std::string{};
    tiles[n++] = {tr("On disk"), core::path::format_size(disk), slack};
    const std::string contains = trf_n("{} file", "{} files", static_cast<long long>(files), files);
    const std::string contains_sub = trf_n("{} folder", "{} folders", static_cast<long long>(single ? scans_[0]->folders() : folders), single ? scans_[0]->folders() : folders);
    if (is_folder_item) { tiles[n++] = {tr("Contains"), contains, contains_sub}; }
    kit.card_begin("##card_size", tr("Size"), kit.stat_tiles_height());
    kit.stat_tiles("##tiles", std::span<const Kit::Stat>{tiles.data(), n});
    kit.card_end("##card_size");

    // where and when
    const std::string where = single ? core::path::parent(first.path) : core::path::parent(first.path);
    const std::string location = where.empty() ? first.path : where;
    const std::string created = single ? core::path::format_time(first.created) : std::string{};
    const std::string modified = single ? core::path::format_time(first.modified) : std::string{};
    const std::string accessed = single ? core::path::format_time(first.accessed) : std::string{};
    const float rh = kit.row_h();
    if (bin_) {
        // a Recycle Bin item: the bin's own file name and times say nothing, its record says where and when
        const std::string deleted_from = core::path::parent(bin_->original);
        kit.card_begin("##card_where", tr("Recycle Bin"), rh * 4.0f);
        kit.key_value({"##kv_orig", tr("Original location"), deleted_from.empty() ? bin_->original : deleted_from, false, true, true});
        kit.key_value({"##kv_origname", tr("Original name"), core::path::leaf(bin_->original), false, true, true});
        kit.key_value({"##kv_deleted", tr("Deleted"), core::path::format_time(bin_->deleted), false, true, true});
        kit.key_value({"##kv_binpath", tr("Stored as"), first.path, false, true, false});
        kit.card_end("##card_where");
        return;
    }
    kit.card_begin("##card_where", single ? "Details" : "Selection", rh * (single ? (first.owner.empty() ? 4.0f : 5.0f) : 1.0f));
    kit.key_value({"##kv_loc", tr("Location"), location, false, true, single});
    if (single) {
        kit.key_value({"##kv_created", tr("Created"), created});
        kit.key_value({"##kv_modified", tr("Modified"), modified});
        kit.key_value({"##kv_accessed", tr("Accessed"), accessed, false, true, !first.owner.empty()});
        if (!first.owner.empty()) { kit.key_value({"##kv_owner", tr("Owner"), first.owner, false, true, false}); }
    }
    kit.card_end("##card_where");

    if (!single) { return; }

    // attributes: read-only and hidden can be flipped here, the rest are shown
    std::string flags;
    const auto flag = [&](std::uint32_t bit, std::string_view name) {
        if ((first.attributes & bit) != 0) { flags += flags.empty() ? std::string{name} : ", " + std::string{name}; }
    };
    flag(attr_system, tr("System"));
    flag(attr_archive, tr("Archive"));
    flag(attr_compressed, tr("Compressed"));
    flag(attr_encrypted, tr("Encrypted"));
    flag(attr_reparse, tr("Link"));
    const float body = kit.row_h() * 2.0f + 16.0f * 2.0f + (flags.empty() ? 0.0f : kit.row_h());
    kit.card_begin("##card_attr", tr("Attributes"), body);
    bool ro = (first.attributes & attr_readonly) != 0;
    bool hid = (first.attributes & attr_hidden) != 0;
    if (kit.switch_row("##sw_ro", tr("Read-only"), tr("The file cannot be changed or deleted by accident"), ro, true)) { attr_flag_ = attr_readonly; attr_on_ = ro; }
    if (kit.switch_row("##sw_hid", tr("Hidden"), tr("Not shown in listings unless hidden files are on"), hid, !flags.empty())) { attr_flag_ = attr_hidden; attr_on_ = hid; }
    if (!flags.empty()) { kit.key_value({"##kv_flags", tr("Also"), flags, false, false, false}); }
    kit.card_end("##card_attr");
    (void)ui;
}

// --- details ----------------------------------------------------------------------------------------------------

void PropertiesDialog::details(Kit& kit)
{
    if (infos_.size() != 1) {
        note(kit, "##det_note", "Select a single item to see its details.");
        return;
    }
    const core::FileInfo& f = infos_.front();
    const float rh = kit.row_h();

    const std::string volume = f.volume + (f.filesystem.empty() ? "" : "  (" + f.filesystem + ")");
    const std::string file_id = std::format("0x{:016X}", f.file_id);
    const std::string links = std::to_string(f.hard_links);
    const std::string attrs = std::format("0x{:X}", f.attributes);
    struct Row { std::string_view key; const std::string* value; bool mono; };
    std::vector<Row> rows;
    rows.push_back({tr("Full path"), &f.path, false});
    if (!f.volume.empty()) { rows.push_back({tr("Volume"), &volume, false}); }
    if (f.file_id != 0) { rows.push_back({tr("File ID"), &file_id, true}); }
    if (f.hard_links > 1) { rows.push_back({tr("Hard links"), &links, false}); }
    rows.push_back({tr("Attributes"), &attrs, true});
    if (!f.link_target.empty()) { rows.push_back({tr("Link target"), &f.link_target, false}); }

    kit.card_begin("##card_file", tr("File"), rh * static_cast<float>(rows.size()));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        kit.key_value({"##dr" + std::to_string(i), rows[i].key, *rows[i].value, rows[i].mono, true, i + 1 < rows.size()});
    }
    kit.card_end("##card_file");

    if (!f.version.empty()) {
        kit.card_begin("##card_ver", tr("Version information"), rh * static_cast<float>(f.version.size()));
        for (std::size_t i = 0; i < f.version.size(); ++i) {
            kit.key_value({"##vr" + std::to_string(i), f.version[i].first, f.version[i].second, false, true, i + 1 < f.version.size(), 170.0f});
        }
        kit.card_end("##card_ver");
    }
    if (!f.streams.empty()) {
        kit.card_begin("##card_ads", tr("Alternate data streams"), rh * static_cast<float>(f.streams.size()));
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
        note(kit, "##hash_note", "Hashes are computed for a single file.");
        return;
    }
    if (!hash_started_) {
        hash_started_ = true;
        hash_.start(infos_.front().path);
    }
    const float rh = kit.row_h();
    if (hash_.failed()) {
        note(kit, "##hash_fail", "The file could not be read.");
        return;
    }
    if (hash_.running()) {
        kit.card_begin("##card_hash", tr("Checksums"), rh + 20.0f);
        const item_result r = ui.custom_item("##hash_run", {ui.content_width(), rh});
        p.text_left(r.bounds, tr("Computing MD5, SHA-1 and SHA-256 ..."), pal.text_dim, f.body, Kit::pad());
        p.text_right(r.bounds, std::format("{:.0f} %", hash_.progress() * 100.0f), pal.text, f.bold, Kit::pad());
        kit.bar("##hash_bar", hash_.progress(), Tone::accent, 6.0f, Kit::pad());
        kit.card_end("##card_hash");
        return;
    }
    const std::string md5 = hash_.md5(), sha1 = hash_.sha1(), sha256 = hash_.sha256();
    kit.card_begin("##card_hash", tr("Checksums"), rh * 3.0f);
    kit.key_value({"##h_md5", tr("MD5"), md5, true, true, true, 74.0f});
    kit.key_value({"##h_sha1", tr("SHA-1"), sha1, true, true, true, 74.0f});
    kit.key_value({"##h_sha256", tr("SHA-256"), sha256, true, true, false, 74.0f});
    kit.card_end("##card_hash");

    // compare with a checksum from a download page
    const float field_h = 36.0f;
    const float badge_w = 120.0f;
    kit.card_begin("##card_cmp", tr("Compare with"), 4.0f + field_h);
    kit.vgap(4.0f);
    const item_result lead = ui.custom_item("##cmp_lead", {Kit::pad(), field_h});
    const float fw = ui.content_width() - Kit::pad() * 2.0f - badge_w;
    shape_style fs;
    fs.radius       = radii(8.0f);
    fs.fill_top     = fs.fill_bottom = pal.raised;
    fs.border       = pal.border;
    fs.border_width = 1.0f;
    p.dl().shape(rect::from_size({lead.bounds.max.x, lead.bounds.min.y}, {fw, field_h}), fs);
    ui.same_line();
    ui.set_next_item_width(fw);
    ui.push_var(style_var::frame_padding_y, (field_h - p.line_height(f.body)) * 0.5f);
    ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
    (void)ui.input_text("##compare", compare_, tr("Paste a checksum"));
    ui.pop_color();
    ui.pop_var();
    ui.same_line();
    if (compare_.empty()) {
        (void)ui.custom_item("##cmp_none", {badge_w, field_h});
    } else {
        std::string c = core::text::lower_ascii(compare_);
        c.erase(std::remove_if(c.begin(), c.end(), [](char ch) { return ch <= ' '; }), c.end());
        const char* algo = c == md5 ? "MD5" : (c == sha1 ? "SHA-1" : (c == sha256 ? "SHA-256" : nullptr));
        const std::string label = algo != nullptr ? trf("Match \xc2\xb7 {}", algo) : std::string{tr("No match")};
        const float bw = kit.chip_width(label, true);
        (void)ui.custom_item("##cmp_pad", {std::max(8.0f, badge_w - bw), field_h});
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
        ui.open_modal("Properties");
    }
    if (!shown_) { return; }
    const DialogStyle style{ui, p.pal()};
    const float width = std::min(640.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Properties", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        release();
        Kit::reset_focus();
        return;
    }
    if (infos_.empty()) { return; }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    header(kit);

    const std::array<std::string_view, 3> tabs{tr("General"), tr("Details"), tr("Hashes")};
    const int before = tab_;
    if (kit.pill_tabs("##prop_tabs", tabs, tab_, 330.0f) && before != tab_) {
        ++fade_gen_;
        fade_frames_ = 0;
    }
    kit.vgap(14.0f);

    // the body: fixed height so the dialog does not jump between tabs, scrolls when a tab is long; fades in on a switch
    // (the height follows the tab's content, one frame behind, and glides)
    const float max_h  = std::clamp(ui.display_size().y - 262.0f, 240.0f, 640.0f);
    const float body_h = ui.animate("prop_bodyh", std::clamp(content_h_ + 2.0f, 120.0f, max_h), 16.0f);
    const std::string fade_key = "prop_fade" + std::to_string(fade_gen_);
    const float fade = ui.animate(fade_key, fade_frames_ == 0 ? 0.0f : 1.0f, 16.0f);
    if (fade_frames_ < 2) { ++fade_frames_; }
    {
        auto body = ui.child("##prop_body", {ui.content_width() + (content_h_ + 2.0f > max_h ? 16.0f : 0.0f), body_h}, child_flags::no_padding);
        if (body) {
            p.dl().push_alpha(0.15f + 0.85f * fade);
            const float slide = (1.0f - fade) * 10.0f;
            if (slide > 0.5f) { kit.vgap(slide); }
            switch (tab_) {
            case 0:  general(kit); break;
            case 1:  details(kit); break;
            default: hashes(kit); break;
            }
            if (slide > 0.5f) { kit.vgap(10.0f - slide); }
            p.dl().pop_alpha();
            content_h_ = kit.cards_height() + (slide > 0.5f ? 10.0f : 0.0f);
        }
    }

    // an attribute switch was flipped: apply it now that nothing is half drawn
    if (attr_flag_ != 0) {
        const std::uint32_t flag = std::exchange(attr_flag_, 0);
        (void)core::set_file_attribute(infos_.front().path, flag, attr_on_);
        reload();
        if (flag == attr_hidden) { env_.tabs.active().refresh(); }
    }
}

} // namespace prism::ui
