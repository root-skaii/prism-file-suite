#include "ui/widgets.hpp"

#include "core/shell_ops.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace prism::ui {

using namespace strata;

namespace {

constexpr char32_t glyph_copy = 0xe8c8;

// keyboard focus is shared by every kit widget: each frame's focusable widgets list themselves in order, Tab walks the
// previous frame's list
struct FocusState {
    std::vector<std::size_t> prev, cur;
    std::size_t              focus{};
    bool                     visible{}; // the ring shows only after a key moved it, never after a mouse click
};

FocusState& focus_state()
{
    static FocusState s;
    return s;
}

[[nodiscard]] color dim_if(color c, bool enabled) noexcept
{
    return enabled ? c : c.scaled_alpha(0.45f);
}

void draw_switch(Painter& p, const rect& r, float on_k, float hot_k, bool enabled)
{
    const Palette& pal = p.pal();
    const color off = pal.text_faint.scaled_alpha(0.55f);
    shape_style s;
    s.radius       = radii(r.height() * 0.5f);
    s.fill_top     = s.fill_bottom = dim_if(lerp(off, pal.accent, on_k), enabled);
    s.border       = lerp(pal.text_faint, pal.accent, on_k).scaled_alpha(enabled ? 0.8f : 0.35f);
    s.border_width = 1.0f;
    p.dl().shape(r, s);
    const float rad = r.height() * 0.5f - 4.0f + hot_k;
    const float x   = std::lerp(r.min.x + r.height() * 0.5f, r.max.x - r.height() * 0.5f, on_k);
    p.dl().circle_filled({x, r.center().y}, rad, dim_if(lerp(pal.text_dim, pal.on_accent, on_k), enabled));
}

} // namespace

void Kit::begin_frame()
{
    counter_ = 0;
    FocusState& f = focus_state();
    f.prev        = std::move(f.cur);
    f.cur.clear();
    context& ui = p_.ui();
    if (ui.mouse_clicked(mouse_button::left)) { f.visible = false; }
    if (ui.want_text_input() || f.prev.empty()) { return; }
    const bool fwd = ui.key_pressed(key::tab, false, false);
    const bool back = ui.key_pressed(key::tab, false, true);
    if (!fwd && !back) { return; }
    const auto   it = std::find(f.prev.begin(), f.prev.end(), f.focus);
    const auto   n  = static_cast<std::ptrdiff_t>(f.prev.size());
    std::ptrdiff_t i = 0;
    if (it == f.prev.end()) {
        i = fwd ? 0 : n - 1;
    } else {
        const std::ptrdiff_t at = it - f.prev.begin();
        i = fwd ? (at + 1) % n : (at + n - 1) % n;
    }
    f.focus   = f.prev[static_cast<std::size_t>(i)];
    f.visible = true;
}

void Kit::reset_focus() noexcept
{
    FocusState& f = focus_state();
    f.prev.clear();
    f.cur.clear();
    f.focus   = 0;
    f.visible = false;
}

std::string Kit::uid(const char* prefix)
{
    return std::string{prefix} + std::to_string(counter_++);
}

color Kit::tone_color(Tone t) const noexcept
{
    const Palette& pal = p_.pal();
    switch (t) {
    case Tone::accent:  return pal.accent;
    case Tone::success: return pal.success;
    case Tone::warning: return pal.warning;
    case Tone::danger:  return pal.danger;
    case Tone::neutral: break;
    }
    return pal.text_dim;
}

Kit::Hit Kit::hit(std::string_view id, vec2 size, bool enabled)
{
    context& ui = p_.ui();
    Hit h;
    h.r   = ui.custom_item(id, size);
    h.hot = enabled && h.r.hovered;
    if (h.hot && ui.mouse_released(mouse_button::left)) { h.clicked = true; }
    if (h.hot) { cmd_.set_cursor(cursor_kind::hand); }
    if (enabled) {
        FocusState& f = focus_state();
        const std::size_t key_hash = std::hash<std::string_view>{}(id);
        f.cur.push_back(key_hash);
        h.focused = f.visible && f.focus == key_hash;
        if (h.focused && !ui.want_text_input() && (ui.key_pressed(key::enter) || ui.key_pressed(key::space))) { h.clicked = true; }
    }
    return h;
}

void Kit::focus_ring(const rect& r, float radius) const
{
    p_.dl().rect_outline(r.expanded(2.0f), p_.pal().accent, radius + 2.0f, 2.0f);
}

void Kit::vgap(float h)
{
    (void)p_.ui().custom_item(uid("##vg"), {1.0f, h});
}

void Kit::hgap(float w)
{
    context& ui = p_.ui();
    ui.same_line();
    (void)ui.custom_item(uid("##hg"), {w, 1.0f});
    ui.same_line();
}

// --- buttons ------------------------------------------------------------------------------------------------------

bool Kit::button(const ButtonSpec& s)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const FontSet  f   = p_.fonts();
    const font_id  font = s.kind == ButtonKind::primary ? f.bold : f.body;

    const float label_w = p_.measure(font, s.label).x;
    const float h = control_h();
    const float w = s.width > 0.0f ? s.width : label_w + 28.0f + (s.icon != 0 ? 22.0f : 0.0f);
    const Hit ht = hit(s.id, {w, h}, s.enabled);

    const std::string key{s.id};
    const float hk = ui.animate(key + "h", ht.hot ? 1.0f : 0.0f, 18.0f);
    const float pk = ui.animate(key + "p", ht.hot && ht.r.held ? 1.0f : 0.0f, 30.0f);
    rect b = ht.r.bounds;
    if (pk > 0.5f) { b = b.expanded(-0.5f); }
    const float radius = 8.0f;

    color text = pal.text;
    if (s.kind == ButtonKind::primary) {
        p_.dl().push_alpha(s.enabled ? 1.0f : 0.45f);
        shape_style st;
        st.radius   = radii(radius);
        st.fill_top = st.fill_bottom = lerp(lerp(pal.accent, pal.accent_hover, hk), color{0, 0, 0, 255}, 0.16f * pk);
        p_.dl().shape(b, st);
        text = pal.on_accent;
    } else {
        p_.dl().push_alpha(s.enabled ? 1.0f : 0.45f);
        if (s.kind == ButtonKind::secondary) {
            shape_style st;
            st.radius       = radii(radius);
            st.fill_top     = st.fill_bottom = pal.raised;
            st.border       = pal.border.scaled_alpha(0.85f);
            st.border_width = 1.0f;
            p_.dl().shape(b, st);
        }
        if (hk > 0.01f) { p_.dl().rect_filled(b, pal.hover.scaled_alpha(hk), radius); }
        if (pk > 0.01f) { p_.dl().rect_filled(b, pal.pressed.scaled_alpha(pk), radius); }
        text = s.kind == ButtonKind::ghost ? lerp(pal.text_dim, pal.text, hk) : pal.text;
    }
    const float group = label_w + (s.icon != 0 ? 22.0f : 0.0f);
    float x = std::round(b.center().x - group * 0.5f);
    if (s.icon != 0) {
        p_.glyph_center(rect{{x, b.min.y}, {x + 16.0f, b.max.y}}, s.icon, text);
        x += 22.0f;
    }
    p_.text_left(rect{{x, b.min.y}, {b.max.x, b.max.y}}, s.label, text, font);
    p_.dl().pop_alpha();
    if (ht.focused) { focus_ring(ht.r.bounds, radius); }
    if (ht.r.hovered && !s.tip.empty()) { ui.tooltip(s.tip); }
    return ht.clicked;
}

bool Kit::icon_button(std::string_view id, char32_t glyph, std::string_view tip, bool enabled, bool danger, float size)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const float    s   = size > 0.0f ? size : control_h();
    const Hit      ht  = hit(id, {s, s}, enabled);
    const std::string key{id};
    const float hk = ui.animate(key + "h", ht.hot ? 1.0f : 0.0f, 18.0f);
    const rect  b  = ht.r.bounds;
    if (hk > 0.01f) {
        const color fill = danger ? pal.close_hover : (ht.r.held ? pal.pressed : pal.hover);
        p_.dl().rect_filled(b, fill.scaled_alpha(hk), 8.0f);
    }
    const color base = danger ? lerp(pal.text_dim, color{255, 255, 255, 255}, hk) : lerp(pal.text_dim, pal.text, hk);
    p_.glyph_center(b, glyph, dim_if(base, enabled));
    if (ht.focused) { focus_ring(b, 8.0f); }
    if (ht.r.hovered && !tip.empty()) { ui.tooltip(tip); }
    return ht.clicked;
}

bool Kit::pill_tabs(std::string_view id, std::span<const std::string_view> labels, int& selected, float width, float line_h)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const FontSet  f   = p_.fonts();
    const float h = control_h();
    const std::size_t n = labels.size();
    if (n == 0) { return false; }

    std::vector<float> w(n);
    float sum = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = p_.measure(f.bold, labels[i]).x + 32.0f;
        sum += w[i];
    }
    const float total = width > 0.0f ? width : sum + 6.0f;
    if (width > 0.0f) {
        const float scale = (width - 6.0f) / sum;
        for (float& v : w) { v *= scale; }
    }
    const Hit ht = hit(id, {total, std::max(h, line_h)}, true);
    const rect box = rect::from_size({ht.r.bounds.min.x, ht.r.bounds.center().y - h * 0.5f}, {total, h});

    std::vector<float> left(n);
    float x = box.min.x + 3.0f;
    for (std::size_t i = 0; i < n; ++i) { left[i] = x; x += w[i]; }

    int over = -1;
    if (ht.r.hovered) {
        const float mx = ui.mouse_pos().x;
        for (std::size_t i = 0; i < n; ++i) { if (mx >= left[i] && mx < left[i] + w[i]) { over = static_cast<int>(i); } }
    }
    bool changed = false;
    if (ht.clicked && over >= 0 && over != selected) {
        selected = over;
        changed  = true;
    }
    if (ht.focused && !ui.want_text_input()) {
        if (ui.key_pressed(key::right) && selected + 1 < static_cast<int>(n)) { ++selected; changed = true; }
        if (ui.key_pressed(key::left) && selected > 0) { --selected; changed = true; }
    }
    selected = std::clamp(selected, 0, static_cast<int>(n) - 1);

    const std::string key{id};
    const auto sel = static_cast<std::size_t>(selected);
    const float tx = ui.animate(key + "x", left[sel], 22.0f);
    const float tw = ui.animate(key + "w", w[sel], 22.0f);

    shape_style track;
    track.radius   = radii(h * 0.5f);
    track.fill_top = track.fill_bottom = pal.window;
    p_.dl().shape(box, track);
    shape_style thumb;
    thumb.radius       = radii((h - 6.0f) * 0.5f);
    thumb.fill_top     = thumb.fill_bottom = pal.raised;
    thumb.border       = pal.border.scaled_alpha(0.8f);
    thumb.border_width = 1.0f;
    p_.dl().shape(rect{{tx, box.min.y + 3.0f}, {tx + tw, box.max.y - 3.0f}}, thumb);
    for (std::size_t i = 0; i < n; ++i) {
        const bool on = i == sel;
        const color c = on ? pal.text : (static_cast<int>(i) == over ? pal.text : pal.text_dim);
        p_.text_center(rect{{left[i], box.min.y}, {left[i] + w[i], box.max.y}}, labels[i], c, on ? f.bold : f.body);
    }
    if (ht.focused) { focus_ring(box, h * 0.5f); }
    return changed;
}

bool Kit::swatch(std::string_view id, color c, bool selected, float line_h)
{
    context& ui = p_.ui();
    const Palette& pal = p_.pal();
    const Hit ht = hit(id, {28.0f, std::max(28.0f, line_h)}, true);
    const std::string key{id};
    const float hk = ui.animate(key + "h", ht.hot ? 1.0f : 0.0f, 18.0f);
    const float sk = ui.animate(key + "s", selected ? 1.0f : 0.0f, 20.0f);
    const vec2 m = ht.r.bounds.center();
    p_.dl().circle_filled(m, 9.0f + hk, c);
    if (sk > 0.01f) {
        p_.dl().circle(m, 12.5f, pal.text.scaled_alpha(sk), 2.0f);
        const int lum = (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
        p_.glyph_center(rect::from_size({m.x - 8.0f, m.y - 8.0f}, {16.0f, 16.0f}), glyph::check, (lum > 150 ? color{11, 13, 17, 255} : color{255, 255, 255, 255}).scaled_alpha(sk), p_.fonts().tile);
    }
    if (ht.focused) { focus_ring(rect::from_size({m.x - 11.0f, m.y - 11.0f}, {22.0f, 22.0f}), 11.0f); }
    return ht.clicked;
}

bool Kit::toggle_switch(std::string_view id, bool& on, bool enabled)
{
    context& ui = p_.ui();
    const Hit ht = hit(id, {42.0f, 22.0f}, enabled);
    const std::string key{id};
    const float ok = ui.animate(key + "o", on ? 1.0f : 0.0f, 22.0f);
    const float hk = ui.animate(key + "h", ht.hot ? 1.0f : 0.0f, 18.0f);
    draw_switch(p_, ht.r.bounds, ok, hk, enabled);
    if (ht.focused) { focus_ring(ht.r.bounds, 11.0f); }
    if (ht.clicked) { on = !on; }
    return ht.clicked;
}

// --- chips ----------------------------------------------------------------------------------------------------------

float Kit::chip_width(std::string_view label, bool icon) const noexcept
{
    return p_.measure(p_.fonts().small, label).x + 20.0f + (icon ? 17.0f : 0.0f);
}

float Kit::button_width(std::string_view label, bool icon, ButtonKind kind) const noexcept
{
    const FontSet f = p_.fonts();
    return p_.measure(kind == ButtonKind::primary ? f.bold : f.body, label).x + 28.0f + (icon ? 22.0f : 0.0f);
}

bool Kit::chip(std::string_view id, std::string_view label, char32_t icon, Tone tone, bool interactive, std::string_view tip, float line_h)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const FontSet  f   = p_.fonts();
    const float h  = dense() ? 22.0f : 24.0f;
    const float w  = chip_width(label, icon != 0);
    const Hit ht = hit(id, {w, std::max(h, line_h)}, interactive);
    const std::string key{id};
    const float hk = ui.animate(key + "h", ht.hot ? 1.0f : 0.0f, 18.0f);
    const rect  b  = rect::from_size({ht.r.bounds.min.x, ht.r.bounds.center().y - h * 0.5f}, {w, h});

    const color tc = tone_color(tone);
    shape_style st;
    st.radius = radii(h * 0.5f);
    if (tone == Tone::neutral) {
        st.fill_top = st.fill_bottom = pal.raised;
        st.border       = pal.border.scaled_alpha(0.7f);
        st.border_width = 1.0f;
    } else {
        st.fill_top = st.fill_bottom = tc.scaled_alpha(0.16f);
    }
    p_.dl().shape(b, st);
    if (hk > 0.01f) { p_.dl().rect_filled(b, pal.hover.scaled_alpha(hk), h * 0.5f); }
    const color text = tone == Tone::neutral ? pal.text : tc;
    float x = b.min.x + 10.0f;
    if (icon != 0) {
        p_.glyph_center(rect{{x - 2.0f, b.min.y}, {x + 12.0f, b.max.y}}, icon, text, f.tile);
        x += 17.0f;
    }
    p_.text_left(rect{{x, b.min.y}, {b.max.x, b.max.y}}, label, text, f.small);
    if (ht.focused) { focus_ring(b, h * 0.5f); }
    if (ht.r.hovered && !tip.empty()) { ui.tooltip(tip); }
    return ht.clicked;
}

void Kit::badge(std::string_view id, std::string_view label, Tone tone, char32_t icon, float line_h)
{
    (void)chip(id, label, icon, tone, false, {}, line_h);
}

// --- cards and rows ---------------------------------------------------------------------------------------------

void Kit::card_begin(std::string_view id, std::string_view title, float body_h)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const float w  = ui.content_width();
    const float th = title.empty() ? 8.0f : (dense() ? 32.0f : 38.0f);
    const item_result r = ui.custom_item(id, {w, th});
    const rect full{r.bounds.min, {r.bounds.max.x, r.bounds.min.y + th + body_h + 8.0f}};
    card_h_ = th + body_h + 22.0f;
    shape_style st;
    st.radius       = radii(12.0f);
    st.fill_top     = st.fill_bottom = pal.content;
    st.border       = pal.divider;
    st.border_width = 1.0f;
    p_.dl().shape(full, st);
    if (!title.empty()) { p_.text_left(rect{{r.bounds.min.x + pad(), r.bounds.min.y + 4.0f}, {r.bounds.max.x - pad(), r.bounds.max.y}}, title, pal.text, p_.fonts().bold); }
}

void Kit::card_end(std::string_view id)
{
    (void)p_.ui().custom_item(std::string{id} + "_end", {1.0f, 8.0f + 14.0f});
    cards_h_ += card_h_;
}

void Kit::key_value(const KeyValue& kv)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const FontSet  f   = p_.fonts();
    const float w  = ui.content_width();
    const float h  = row_h();
    const item_result r = ui.custom_item(kv.id, {w, h});
    const bool hot = kv.copyable && r.hovered;
    const std::string key{kv.id};
    const float hk = ui.animate(key + "h", hot ? 1.0f : 0.0f, 18.0f);
    const rect  b  = r.bounds;
    if (hk > 0.01f) { p_.dl().rect_filled(rect{{b.min.x + 6.0f, b.min.y + 2.0f}, {b.max.x - 6.0f, b.max.y - 2.0f}}, pal.hover.scaled_alpha(hk), 6.0f); }

    const float room = kv.copyable ? 24.0f : 0.0f;
    const float vx   = b.min.x + pad() + kv.key_w;
    const float avail = b.max.x - pad() - room - vx;
    const font_id vf = kv.mono ? f.mono : f.body;
    p_.text_left(rect{{b.min.x + pad(), b.min.y}, {vx - 8.0f, b.max.y}}, kv.key, pal.text_dim, f.body);
    p_.text_left(rect{{vx, b.min.y}, {vx + std::max(0.0f, avail), b.max.y}}, kv.value, pal.text, vf);
    if (kv.copyable) { p_.glyph_center(rect{{b.max.x - pad() - 16.0f, b.min.y}, {b.max.x - pad(), b.max.y}}, glyph_copy, pal.text_dim.scaled_alpha(hk), f.tile); }
    if (kv.separator) { p_.dl().rect_filled(rect{{b.min.x + pad(), b.max.y - 1.0f}, {b.max.x - pad(), b.max.y}}, pal.divider); }

    if (r.hovered) {
        const bool cut = p_.measure(vf, kv.value).x > avail;
        if (cut) {
            ui.tooltip(kv.value);
        } else if (kv.copyable) {
            ui.tooltip("Click to copy");
        }
        if (kv.copyable) { cmd_.set_cursor(cursor_kind::hand); }
    }
    if (hot && ui.mouse_released(mouse_button::left)) {
        (void)core::shell::set_clipboard_text(std::string{kv.value});
        cmd_.toast("Copied", toast_kind::info);
    }
}

bool Kit::switch_row(std::string_view id, std::string_view label, std::string_view hint, bool& on, bool separator)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const FontSet  f   = p_.fonts();
    const float h = row_h() + (hint.empty() ? 0.0f : 16.0f);
    const Hit ht = hit(id, {ui.content_width(), h}, true);
    const std::string key{id};
    const float ok = ui.animate(key + "o", on ? 1.0f : 0.0f, 22.0f);
    const float hk = ui.animate(key + "h", ht.hot ? 1.0f : 0.0f, 18.0f);
    const rect  b  = ht.r.bounds;
    if (hk > 0.01f) { p_.dl().rect_filled(rect{{b.min.x + 6.0f, b.min.y + 2.0f}, {b.max.x - 6.0f, b.max.y - 2.0f}}, pal.hover.scaled_alpha(hk), 6.0f); }
    const float sw_w = 42.0f;
    const rect  label_box{{b.min.x + pad(), b.min.y}, {b.max.x - pad() - sw_w - 12.0f, hint.empty() ? b.max.y : b.min.y + row_h()}};
    p_.text_left(label_box, label, pal.text, f.body);
    if (!hint.empty()) { p_.text_left(rect{{b.min.x + pad(), b.min.y + row_h() - 8.0f}, {label_box.max.x, b.max.y - 2.0f}}, hint, pal.text_dim, f.small); }
    draw_switch(p_, rect::from_size({b.max.x - pad() - sw_w, b.center().y - 11.0f}, {sw_w, 22.0f}), ok, hk, true);
    if (separator) { p_.dl().rect_filled(rect{{b.min.x + pad(), b.max.y - 1.0f}, {b.max.x - pad(), b.max.y}}, pal.divider); }
    if (ht.focused) { focus_ring(b.expanded(-4.0f), 8.0f); }
    if (ht.clicked) { on = !on; }
    return ht.clicked;
}

float Kit::stat_tiles_height() const noexcept
{
    const FontSet f = p_.fonts();
    return 8.0f + 10.0f + p_.line_height(f.small) + 2.0f + p_.line_height(f.title) + 2.0f + p_.line_height(f.small) + 10.0f;
}

void Kit::stat_tiles(std::string_view id, std::span<const Stat> stats)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const FontSet  f   = p_.fonts();
    const float    h   = stat_tiles_height();
    const float    w   = ui.content_width();
    const item_result r = ui.custom_item(id, {w, h});
    const std::size_t n = stats.size();
    if (n == 0) { return; }
    // lines follow the fonts' real heights (a larger interface size or font must not make them run into each other)
    const float lh_small = p_.line_height(f.small);
    const float lh_title = p_.line_height(f.title);
    const float gap = 10.0f;
    const float tw  = (w - 2.0f * pad() - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float x = r.bounds.min.x + pad() + static_cast<float>(i) * (tw + gap);
        const rect  t{{x, r.bounds.min.y + 4.0f}, {x + tw, r.bounds.max.y - 4.0f}};
        shape_style st;
        st.radius       = radii(10.0f);
        st.fill_top     = st.fill_bottom = pal.raised;
        st.border       = pal.divider;
        st.border_width = 1.0f;
        p_.dl().shape(t, st);
        float y = t.min.y + 10.0f;
        p_.text_left(rect{{t.min.x, y}, {t.max.x, y + lh_small}}, stats[i].label, pal.text_dim, f.small, 12.0f);
        y += lh_small + 2.0f;
        p_.text_left(rect{{t.min.x, y}, {t.max.x, y + lh_title}}, stats[i].value, pal.text, f.title, 12.0f);
        y += lh_title + 2.0f;
        if (!stats[i].sub.empty()) { p_.text_left(rect{{t.min.x, y}, {t.max.x, y + lh_small}}, stats[i].sub, pal.text_dim, f.small, 12.0f); }
    }
}

void Kit::bar(std::string_view id, float fraction, Tone tone, float height, float inset)
{
    context&       ui  = p_.ui();
    const Palette& pal = p_.pal();
    const item_result r = ui.custom_item(id, {ui.content_width(), height + 14.0f});
    const float k = ui.animate(std::string{id}, std::clamp(fraction, 0.0f, 1.0f), 12.0f);
    const rect track{{r.bounds.min.x + inset, r.bounds.min.y + 7.0f}, {r.bounds.max.x - inset, r.bounds.min.y + 7.0f + height}};
    p_.dl().rect_filled(track, pal.raised, height * 0.5f);
    if (k > 0.001f) {
        const float fw = std::max(height, track.width() * k);
        p_.dl().rect_filled(rect{track.min, {track.min.x + fw, track.max.y}}, tone_color(tone), height * 0.5f);
    }
}

} // namespace prism::ui
