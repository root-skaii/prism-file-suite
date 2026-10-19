#include "ui/batch_rename_dialog.hpp"

#include "core/path.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace prism::ui {

using namespace strata;

void BatchRenameDialog::open(std::string dir, std::vector<core::RenameItem> items, std::vector<std::string> others)
{
    if (items.empty()) { return; }
    dir_       = std::move(dir);
    items_     = std::move(items);
    others_    = std::move(others);
    want_open_ = true;
    age_       = 0;
}

core::RenameRule BatchRenameDialog::rule() const
{
    core::RenameRule r;
    r.pattern = pattern_;
    r.find    = find_;
    r.replace = replace_;
    r.casing  = static_cast<core::CaseMode>(casing_);
    r.digits  = digits_ + 1;
    int start = 1;
    (void)std::from_chars(start_.data(), start_.data() + start_.size(), start);
    r.start = start;
    return r;
}

// label on the left, a text box on the right; strata fills text fields with a fixed darker colour that goes grey on the
// light theme, so the fill is made transparent and the real one drawn where the layout puts the field (as in Properties)
void BatchRenameDialog::field(Painter& p, std::string_view id, std::string_view label, std::string& text, std::string_view hint, float width)
{
    context&       ui  = p.ui();
    const Palette& pal = p.pal();
    const FontSet  f   = p.fonts();
    const float    h   = 36.0f;
    const float    lw  = 120.0f;
    const item_result lead = ui.custom_item(std::string{id} + "_l", {lw, h});
    p.text_left(lead.bounds, label, pal.text_dim, f.body);
    shape_style fs;
    fs.radius       = radii(8.0f);
    fs.fill_top     = fs.fill_bottom = pal.raised;
    fs.border       = pal.border;
    fs.border_width = 1.0f;
    p.dl().shape(rect::from_size({lead.bounds.max.x, lead.bounds.min.y}, {width, h}), fs);
    ui.same_line();
    ui.set_next_item_width(width);
    ui.push_var(style_var::frame_padding_y, (h - p.line_height(f.body)) * 0.5f);
    ui.push_color(style_color::widget_bg, color{0, 0, 0, 0});
    (void)ui.input_text(std::string{id}, text, hint);
    ui.pop_color();
    ui.pop_var();
}

void BatchRenameDialog::apply()
{
    const std::vector<std::string> names = core::batch_names(items_, rule());
    std::vector<std::pair<std::string, std::string>> pairs;
    for (std::size_t i = 0; i < items_.size(); ++i) { pairs.emplace_back(items_[i].name, names[i]); }
    if (!core::apply_renames(dir_, pairs)) {
        env_.cmd.toast("Could not rename: nothing was changed", toast_kind::warning);
        return;
    }
    std::vector<std::pair<std::string, std::string>> forward, backward;
    for (const auto& pr : pairs) {
        if (pr.first == pr.second) { continue; }
        forward.push_back(pr);
        backward.emplace_back(pr.second, pr.first);
    }
    const std::string dir = dir_;
    env_.history.push({std::format("Rename {} items", forward.size()),
                       [dir, backward] { return core::apply_renames(dir, backward); },
                       [dir, forward] { return core::apply_renames(dir, forward); }});
    env_.cmd.toast(std::format("Renamed {} items  (Ctrl+Z undoes it)", forward.size()), toast_kind::info);
    env_.tab().refresh();
    if (model::Tab* other = env_.other()) { other->refresh(); }
}

void BatchRenameDialog::frame(Painter& p)
{
    context& ui = p.ui();
    if (want_open_) {
        want_open_ = false;
        shown_     = true;
        Kit::reset_focus();
        ui.open_modal("Rename items");
    }
    if (!shown_) { return; }
    const Palette&    pal = p.pal();
    const FontSet     f   = p.fonts();
    const DialogStyle style{ui, pal};
    const float width = std::min(700.0f, ui.display_size().x - 40.0f);
    auto modal = ui.modal("Rename items", {width, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!modal) {
        shown_ = false;
        Kit::reset_focus();
        return;
    }

    Kit kit{p, env_.cmd};
    kit.begin_frame();
    {
        const item_result r = ui.custom_item("##br_head", {ui.content_width() - 36.0f, 48.0f});
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 2.0f}, {r.bounds.max.x, r.bounds.min.y + 30.0f}}, std::format("Rename {} items", items_.size()), pal.text, f.title);
        p.text_left(rect{{r.bounds.min.x, r.bounds.min.y + 28.0f}, {r.bounds.max.x, r.bounds.max.y}}, "[N] name   [E] extension   [C] counter", pal.text_dim, f.body);
        ui.same_line();
        if (kit.icon_button("##br_close", glyph::close, "Close  (Esc)", true, true, 32.0f)) { ui.close_modal(); }
    }
    kit.vgap(10.0f);

    if (age_ < 3) { ui.request_text_focus("##br_pattern"); }
    ++age_;
    const float fw = ui.content_width() - 120.0f;
    field(p, "##br_pattern", "Name", pattern_, "[N]", fw);
    kit.vgap(6.0f);
    field(p, "##br_find", "Find", find_, "text to replace in the name", fw);
    kit.vgap(6.0f);
    field(p, "##br_repl", "Replace with", replace_, "", fw);
    kit.vgap(8.0f);
    {
        const item_result lead = ui.custom_item("##br_case_l", {120.0f, 36.0f});
        p.text_left(lead.bounds, "Letters", pal.text_dim, f.body);
        ui.same_line();
        static constexpr std::array<std::string_view, 4> cases{"Keep", "lower", "UPPER", "Title Case"};
        (void)kit.pill_tabs("##br_case", cases, casing_, 340.0f, 36.0f);
    }
    kit.vgap(6.0f);
    {
        std::string_view none;
        (void)none;
        field(p, "##br_start", "Counter starts", start_, "1", 110.0f);
        ui.same_line();
        (void)ui.custom_item("##br_dgap", {16.0f, 36.0f});
        ui.same_line();
        static constexpr std::array<std::string_view, 4> digits{"1", "01", "001", "0001"};
        (void)kit.pill_tabs("##br_digits", digits, digits_, 240.0f, 36.0f);
    }
    kit.vgap(12.0f);

    // before / after
    const core::RenameRule rl = rule();
    const std::vector<std::string> names = core::batch_names(items_, rl);
    std::vector<core::RenameItem> moved;
    std::vector<std::string>      result;
    std::vector<std::string>      outside = others_;
    for (std::size_t i = 0; i < items_.size(); ++i) { moved.push_back(items_[i]); result.push_back(names[i]); }
    const std::vector<std::string> bad = core::batch_problems(moved, result, outside);
    std::size_t changed = 0, problems = 0;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (names[i] != items_[i].name) { ++changed; }
        if (!bad[i].empty()) { ++problems; }
    }

    const float rowh   = 28.0f;
    const std::size_t shown_rows = std::min<std::size_t>(items_.size(), 200);
    const float list_h = std::min(rowh * static_cast<float>(shown_rows) + 8.0f, std::clamp(ui.display_size().y - 520.0f, 120.0f, 300.0f));
    {
        auto list = ui.child("##br_list", {ui.content_width() + (rowh * static_cast<float>(shown_rows + (items_.size() > shown_rows ? 1 : 0)) + 8.0f > list_h ? 16.0f : 0.0f), list_h}, child_flags::no_padding);
        if (list) {
            for (std::size_t i = 0; i < shown_rows; ++i) {
                const item_result r = ui.custom_item("##br_row" + std::to_string(i), {ui.content_width(), rowh});
                const float half = r.bounds.width() * 0.5f;
                p.text_left(rect{r.bounds.min, {r.bounds.min.x + half - 14.0f, r.bounds.max.y}}, items_[i].name, pal.text_dim, f.body, 8.0f);
                const bool same = names[i] == items_[i].name;
                const color nc  = !bad[i].empty() ? pal.danger : (same ? pal.text_faint : pal.text);
                p.glyph_center(rect{{r.bounds.min.x + half - 14.0f, r.bounds.min.y}, {r.bounds.min.x + half + 6.0f, r.bounds.max.y}}, glyph::chevron_r, pal.text_faint, f.tile);
                const std::string right = bad[i].empty() ? names[i] : names[i] + "   (" + bad[i] + ")";
                p.text_left(rect{{r.bounds.min.x + half + 8.0f, r.bounds.min.y}, r.bounds.max}, right, nc, f.body);
            }
            if (items_.size() > shown_rows) {
                const item_result r = ui.custom_item("##br_more", {ui.content_width(), rowh});
                p.text_left(r.bounds, std::format("... and {} more", items_.size() - shown_rows), pal.text_faint, f.small, 8.0f);
            }
        }
    }
    kit.vgap(12.0f);

    const std::string summary = problems > 0 ? std::format("{} names cannot be used", problems)
                              : changed == 0 ? std::string{"Nothing would change"}
                                             : std::format("{} of {} will be renamed", changed, items_.size());
    const float bw_cancel = kit.button_width("Cancel", false);
    const float bw_ok     = kit.button_width("Rename", false, Kit::ButtonKind::primary);
    const item_result sr  = ui.custom_item("##br_sum", {ui.content_width() - bw_cancel - bw_ok - 16.0f, kit.control_h()});
    p.text_left(sr.bounds, summary, problems > 0 ? pal.danger : pal.text_dim, f.body);
    ui.same_line();
    if (kit.button({"##br_cancel", "Cancel"})) { ui.close_modal(); }
    ui.same_line();
    (void)ui.custom_item("##br_bgap", {8.0f, kit.control_h()});
    ui.same_line();
    const bool ok = problems == 0 && changed > 0;
    if (kit.button({"##br_go", "Rename", 0, Kit::ButtonKind::primary, ok}) && ok) {
        apply();
        ui.close_modal();
    }
}

} // namespace prism::ui
