#include "ui/compare_dialog.hpp"
#include "ui/tr.hpp"

#include "core/path.hpp"
#include "ui/tr.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace prism::ui {

using namespace strata;
using core::DiffKind;

namespace {

constexpr std::size_t k_cap = 1500; // rows listed (and applied)

[[nodiscard]] std::string_view kind_label(DiffKind k) noexcept
{
    switch (k) {
    case DiffKind::only_left:    return tr("Only on the left");
    case DiffKind::only_right:   return tr("Only on the right");
    case DiffKind::left_newer:   return tr("Newer on the left");
    case DiffKind::right_newer:  return tr("Newer on the right");
    case DiffKind::size_differs: return tr("Different size");
    case DiffKind::type_differs: return tr("File here, folder there");
    case DiffKind::same:         return tr("Identical");
    }
    return {};
}

} // namespace

CompareDialog::Act CompareDialog::default_act(const core::DiffItem& d)
{
    switch (d.kind) {
    case DiffKind::only_left:
    case DiffKind::left_newer:   return Act::to_right;
    case DiffKind::only_right:
    case DiffKind::right_newer:  return Act::to_left;
    default:                     return Act::skip; // (a guess would overwrite the wrong one)
    }
}

void CompareDialog::open(std::string left, std::string right)
{
    left_      = std::move(left);
    right_     = std::move(right);
    want_open_ = true;
    rescan();
}

void CompareDialog::rescan()
{
    items_.reset();
    acts_.clear();
    shown_.clear();
    scan_.start(left_, right_, recursive_, env_.window.waker());
}

void CompareDialog::apply()
{
    if (!items_) { return; }
    // one copy job per destination folder
    std::map<std::string, std::vector<std::string>> jobs;
    std::size_t count = 0;
    for (std::size_t n = 0; n < std::min(shown_.size(), k_cap); ++n) {
        const std::size_t i = shown_[n];
        if (acts_[i] == Act::skip) { continue; }
        const core::DiffItem& d = (*items_)[i];
        const bool ltr = acts_[i] == Act::to_right;
        const std::string& from_root = ltr ? left_ : right_;
        const std::string& to_root   = ltr ? right_ : left_;
        const std::size_t slash = d.rel.rfind('\\');
        const std::string parent = slash == std::string::npos ? std::string{} : d.rel.substr(0, slash);
        jobs[parent.empty() ? to_root : core::path::join(to_root, parent)].push_back(core::path::join(from_root, d.rel));
        ++count;
    }
    if (count == 0) { return; }
    for (auto& [dest, sources] : jobs) { env_.ops.start(core::OpKind::copy, std::move(sources), dest); }
    env_.cmd.toast(trf("Copying {} items ...", count), toast_kind::info);
}

void CompareDialog::frame(Painter& p)
{
    context& ui = p.ui();
    if (want_open_) {
        want_open_ = false;
        open_      = true;
        Kit::reset_focus();
        ui.open_modal("Compare folders");
    }
    if (!open_) { return; }
    const Palette&    pal = p.pal();
    const FontSet     f   = p.fonts();
    const DialogStyle style{ui, pal};
    const float width = std::min(960.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Compare folders", {width, 0.0f}, modal_flags::esc_closes | modal_flags::no_title_bar);
    if (!modal) {
        open_ = false;
        scan_.cancel();
        items_.reset();
        Kit::reset_focus();
        return;
    }
    if (!items_) {
        if (auto r = scan_.result()) {
            items_ = std::move(r);
            acts_.resize(items_->size());
            for (std::size_t i = 0; i < items_->size(); ++i) { acts_[i] = default_act((*items_)[i]); }
        }
    }
    if (items_ && shown_.empty()) {
        for (std::size_t i = 0; i < items_->size(); ++i) {
            const core::DiffItem& d = (*items_)[i];
            if (d.kind == DiffKind::same && (d.is_dir || !show_same_)) { continue; }
            shown_.push_back(i);
        }
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    {
        const item_result r = ui.custom_item("##cmp_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, tr("Compare folders"), pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, left_ + "   <>   " + right_, pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##cmp_close", glyph::close, tr("Close  (Esc)"), true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(10.0f);

    if (!items_) {
        const item_result r = ui.custom_item("##cmp_run", {ui.content_width(), 34.0f});
        p.text_left(r.bounds, trf("Comparing ... {} entries looked at", scan_.seen()), pal.text_dim, f.body, 4.0f);
        kit.bar("##cmp_bar", 0.0f, Tone::accent, 6.0f, 4.0f);
        kit.vgap(14.0f);
        if (kit.button({"##cmp_cancel", tr("Cancel")})) { ui.close_modal(); }
        return;
    }

    // options
    {
        const item_result lr = ui.custom_item("##cmp_opts_l", {ui.content_width() - 2.0f * 190.0f - 24.0f, 34.0f});
        p.text_left(lr.bounds, trf("{} differences", std::ranges::count_if(shown_, [&](std::size_t i) { return (*items_)[i].kind != DiffKind::same; })), pal.text_dim, f.body, 4.0f);
        ui.same_line();
        bool changed = false;
        bool sub = recursive_;
        if (kit.toggle_switch("##cmp_sub", sub)) { recursive_ = sub; changed = true; }
        ui.same_line();
        const item_result t1 = ui.custom_item("##cmp_sub_t", {130.0f, 34.0f});
        p.text_left(t1.bounds, tr("Subfolders"), pal.text_dim, f.body, 6.0f);
        ui.same_line();
        bool same = show_same_;
        if (kit.toggle_switch("##cmp_same", same)) { show_same_ = same; shown_.clear(); }
        ui.same_line();
        const item_result t2 = ui.custom_item("##cmp_same_t", {100.0f, 34.0f});
        p.text_left(t2.bounds, tr("Identical"), pal.text_dim, f.body, 6.0f);
        if (changed) { rescan(); return; }
    }
    kit.vgap(6.0f);

    if (shown_.empty()) {
        const item_result r = ui.custom_item("##cmp_none", {ui.content_width(), 80.0f});
        p.text_center(r.bounds, tr("The two folders match"), pal.text_dim, f.body);
        return;
    }

    const float rowh   = 34.0f;
    const std::size_t rows = std::min(shown_.size(), k_cap);
    const float list_h = std::min(rowh * static_cast<float>(rows + 1), std::clamp(ui.display_size().y - 330.0f, 160.0f, 460.0f));
    std::size_t flip = shown_.size();
    {
        auto list = ui.child("##cmp_list", {ui.content_width() + (rowh * static_cast<float>(rows) > list_h ? 16.0f : 0.0f), list_h}, child_flags::no_padding);
        if (list) {
            for (std::size_t n = 0; n < rows; ++n) {
                const std::size_t i = shown_[n];
                const core::DiffItem& d = (*items_)[i];
                const item_result r = ui.custom_item("##cmp_row" + std::to_string(n), {ui.content_width(), rowh});
                const float w = r.bounds.width();
                const color kc = d.kind == DiffKind::only_left || d.kind == DiffKind::left_newer ? pal.accent
                               : d.kind == DiffKind::only_right || d.kind == DiffKind::right_newer ? pal.success
                               : pal.warning;
                std::string name = d.rel + (d.is_dir ? "\\" : "");
                p.text_left(rect{{r.bounds.min.x, r.bounds.min.y}, {r.bounds.min.x + w * 0.46f, r.bounds.max.y}}, name, pal.text, f.body, 8.0f);
                p.text_left(rect{{r.bounds.min.x + w * 0.46f, r.bounds.min.y}, {r.bounds.min.x + w * 0.72f, r.bounds.max.y}}, kind_label(d.kind), kc, f.small);
                const std::string info = d.kind == DiffKind::only_left ? core::path::format_size(d.left_size)
                                       : d.kind == DiffKind::only_right ? core::path::format_size(d.right_size)
                                       : core::path::format_size(d.left_size) + " / " + core::path::format_size(d.right_size);
                if (!d.is_dir) { p.text_left(rect{{r.bounds.min.x + w * 0.72f, r.bounds.min.y}, {r.bounds.max.x - 70.0f, r.bounds.max.y}}, info, pal.text_faint, f.small); }
                // the direction: click cycles  ->  <-  skip
                const rect btn = rect::from_size({r.bounds.max.x - 62.0f, r.bounds.min.y + 4.0f}, {54.0f, rowh - 8.0f});
                const bool over = r.hovered && btn.contains(ui.mouse_pos());
                const bool on   = acts_[i] != Act::skip;
                p.dl().rect_filled(btn, on ? pal.accent.scaled_alpha(over ? 0.38f : 0.25f) : (over ? pal.hover : pal.raised), 8.0f);
                p.text_center(btn, acts_[i] == Act::to_right ? "->" : acts_[i] == Act::to_left ? "<-" : tr("skip"), on ? pal.text : pal.text_faint, f.bold);
                if (over) { env_.cmd.set_cursor(cursor_kind::hand); }
                if (over && ui.mouse_released(mouse_button::left)) { flip = i; }
            }
            if (shown_.size() > rows) {
                const item_result r = ui.custom_item("##cmp_more", {ui.content_width(), rowh});
                p.text_left(r.bounds, trf("... and {} more differences (not shown, they are not copied)", shown_.size() - rows), pal.text_faint, f.small, 8.0f);
            }
        }
    }
    if (flip != shown_.size()) {
        Act& a = acts_[flip];
        a = a == Act::to_right ? Act::to_left : a == Act::to_left ? Act::skip : Act::to_right;
        ui.invalidate();
    }
    kit.vgap(12.0f);

    std::size_t todo = 0;
    for (std::size_t n = 0; n < rows; ++n) { todo += acts_[shown_[n]] != Act::skip ? 1 : 0; }
    const float bw_none = kit.button_width(tr("Skip all"), false);
    const float bw_ok   = kit.button_width(tr("Copy"), false, Kit::ButtonKind::primary) + 70.0f;
    const item_result sr = ui.custom_item("##cmp_sum", {ui.content_width() - bw_none - bw_ok - 24.0f, kit.control_h()});
    p.text_left(sr.bounds, tr("Nothing is deleted - click an arrow to change a row's direction"), pal.text_dim, f.small, 4.0f);
    ui.same_line();
    if (kit.button({"##cmp_none_b", tr("Skip all")})) {
        for (Act& a : acts_) { a = Act::skip; }
        ui.invalidate();
    }
    ui.same_line();
    (void)ui.custom_item("##cmp_gap", {8.0f, kit.control_h()});
    ui.same_line();
    const std::string label = trf("Copy {}", todo);
    if (kit.button({"##cmp_go", label, 0, Kit::ButtonKind::primary, todo > 0, {}, bw_ok}) && todo > 0) {
        apply();
        ui.close_modal();
    }
}

} // namespace prism::ui
