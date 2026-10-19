#include "ui/duplicates_dialog.hpp"

#include "core/path.hpp"

#include <algorithm>
#include <format>

namespace prism::ui {

using namespace strata;

namespace {

constexpr std::uint64_t min_size = 1024; // smaller files are mostly noise (empty markers, tiny configs)

[[nodiscard]] std::string relative(const std::string& folder, const std::string& path)
{
    if (path.size() > folder.size() + 1 && path.compare(0, folder.size(), folder) == 0) { return path.substr(folder.size() + (folder.back() == '\\' ? 0 : 1)); }
    return path;
}

} // namespace

void DuplicatesDialog::open(std::string folder)
{
    if (folder.empty()) { return; }
    folder_    = std::move(folder);
    loaded_    = false;
    want_open_ = true;
    groups_.clear();
    marked_.clear();
    scan_.start(folder_, min_size);
}

void DuplicatesDialog::load_groups()
{
    groups_ = scan_.groups();
    marked_.clear();
    for (const core::DuplicateGroup& g : groups_) {
        for (std::size_t i = 1; i < g.paths.size(); ++i) { marked_.insert(g.paths[i]); } // (the oldest stays)
    }
    loaded_ = true;
}

void DuplicatesDialog::frame(Painter& p)
{
    context& ui = p.ui();
    if (want_open_) {
        want_open_ = false;
        shown_     = true;
        Kit::reset_focus();
        ui.open_modal("Duplicate files");
    }
    if (!shown_) { return; }
    const Palette&    pal = p.pal();
    const FontSet     f   = p.fonts();
    const DialogStyle style{ui, pal};
    const float width = std::min(760.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Duplicate files", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        scan_.cancel();
        Kit::reset_focus();
        return;
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    const bool done = scan_.stage() == core::DuplicateScan::Stage::done;
    if (done && !loaded_) { load_groups(); }
    {
        const item_result r = ui.custom_item("##dup_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, "Duplicate files", pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, folder_ + "  (files under 1 KB are ignored)", pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##dup_close", glyph::close, "Close  (Esc)", true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(10.0f);

    if (!done) {
        const bool comparing = scan_.stage() == core::DuplicateScan::Stage::comparing;
        const item_result r  = ui.custom_item("##dup_run", {ui.content_width(), 34.0f});
        p.text_left(r.bounds, comparing ? "Comparing files with the same size ..." : std::format("Looking at files ... {} so far", scan_.files_seen()), pal.text_dim, f.body, 4.0f);
        if (comparing) { p.text_right(r.bounds, std::format("{:.0f} %", scan_.progress() * 100.0f), pal.text, f.bold, 4.0f); }
        kit.bar("##dup_bar", comparing ? scan_.progress() : 0.0f, Tone::accent, 6.0f, 4.0f);
        kit.vgap(14.0f);
        if (kit.button({"##dup_cancel", "Cancel"})) { ui.close_modal(); }
        return;
    }
    if (groups_.empty()) {
        const item_result r = ui.custom_item("##dup_none", {ui.content_width(), 64.0f});
        p.text_center(r.bounds, "No duplicates found here.", pal.text_dim, f.body);
        return;
    }

    std::uint64_t free_bytes = 0;
    for (const core::DuplicateGroup& g : groups_) {
        for (const std::string& path : g.paths) { if (marked_.contains(path)) { free_bytes += g.size; } }
    }
    {
        const item_result r = ui.custom_item("##dup_sum", {ui.content_width(), 30.0f});
        p.text_left(r.bounds, std::format("{} sets of identical files. {} can be freed. Click a file to keep or remove it.", groups_.size(), core::path::format_size(scan_.reclaimable())), pal.text_dim, f.body, 4.0f);
    }

    const float rowh = 30.0f;
    const float list_h = std::clamp(ui.display_size().y - 360.0f, 160.0f, 420.0f);
    std::string toggle;
    {
        std::size_t rows = 0;
        for (std::size_t gi = 0; gi < std::min<std::size_t>(groups_.size(), 150); ++gi) { rows += 1 + groups_[gi].paths.size(); }
        auto list = ui.child("##dup_list", {ui.content_width() + (rowh * static_cast<float>(rows) > list_h ? 16.0f : 0.0f), list_h}, child_flags::no_padding);
        if (list) {
            const std::size_t max_groups = 150;
            for (std::size_t gi = 0; gi < std::min(groups_.size(), max_groups); ++gi) {
                const core::DuplicateGroup& g = groups_[gi];
                const item_result h = ui.custom_item("##dup_g" + std::to_string(gi), {ui.content_width(), rowh});
                p.text_left(h.bounds, core::path::leaf(g.paths.front()), pal.text, f.bold, 8.0f);
                p.text_right(h.bounds, std::format("{} x {}", g.paths.size(), core::path::format_size(g.size)), pal.text_dim, f.body, 12.0f);
                for (std::size_t i = 0; i < g.paths.size(); ++i) {
                    const std::string& path = g.paths[i];
                    const bool         mark = marked_.contains(path);
                    const item_result  r    = ui.custom_item("##dup_f" + std::to_string(gi) + "_" + std::to_string(i), {ui.content_width(), rowh});
                    if (r.hovered) { p.dl().rect_filled(r.bounds, pal.hover, 6.0f); env_.cmd.set_cursor(cursor_kind::hand); ui.tooltip(path); }
                    const rect box = rect::from_size({r.bounds.min.x + 14.0f, r.bounds.center().y - 9.0f}, {18.0f, 18.0f});
                    if (mark) { p.dl().rect_filled(box, pal.danger, 4.0f); p.glyph_center(box, glyph::close, pal.window, f.tile); }
                    else { p.dl().rect_outline(box, pal.text_faint, 4.0f, 1.5f); }
                    p.text_left(rect{{box.max.x + 10.0f, r.bounds.min.y}, {r.bounds.max.x - 110.0f, r.bounds.max.y}}, relative(folder_, path), mark ? pal.text_dim : pal.text, f.body);
                    p.text_right(r.bounds, mark ? "remove" : (i == 0 ? "keep (oldest)" : "keep"), mark ? pal.danger : pal.text_faint, f.small, 12.0f);
                    if (r.hovered && ui.mouse_released(mouse_button::left)) { toggle = path; }
                }
            }
            if (groups_.size() > max_groups) {
                const item_result r = ui.custom_item("##dup_more", {ui.content_width(), rowh});
                p.text_left(r.bounds, std::format("... and {} more sets (the biggest are shown first)", groups_.size() - max_groups), pal.text_faint, f.small, 8.0f);
            }
        }
    }
    if (!toggle.empty()) {
        // never mark the last file of a set that is still kept: the content would be gone
        for (const core::DuplicateGroup& g : groups_) {
            if (std::find(g.paths.begin(), g.paths.end(), toggle) == g.paths.end()) { continue; }
            if (marked_.contains(toggle)) { marked_.erase(toggle); }
            else {
                const auto kept = std::count_if(g.paths.begin(), g.paths.end(), [&](const std::string& s) { return !marked_.contains(s); });
                if (kept > 1) { marked_.insert(toggle); }
                else { env_.cmd.toast("Keep at least one copy of each file", toast_kind::info); }
            }
            break;
        }
    }
    kit.vgap(12.0f);

    const std::string label = marked_.empty() ? std::string{"Nothing marked"} : std::format("Move {} files to the Recycle Bin  ({})", marked_.size(), core::path::format_size(free_bytes));
    const float bw_cancel = kit.button_width("Close", false);
    const float bw_ok     = kit.button_width(label, false, Kit::ButtonKind::primary);
    (void)ui.custom_item("##dup_pad", {std::max(0.0f, ui.content_width() - bw_cancel - bw_ok - 8.0f), kit.control_h()});
    ui.same_line();
    if (kit.button({"##dup_close2", "Close"})) { ui.close_modal(); }
    ui.same_line();
    (void)ui.custom_item("##dup_gap", {8.0f, kit.control_h()});
    ui.same_line();
    if (kit.button({"##dup_go", label, 0, Kit::ButtonKind::primary, !marked_.empty()}) && !marked_.empty()) {
        env_.ops.start(core::OpKind::recycle, std::vector<std::string>(marked_.begin(), marked_.end()), {});
        ui.close_modal();
    }
}

} // namespace prism::ui
