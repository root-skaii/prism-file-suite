#pragma once

// tr / trf (format) / tr_n / trf_n (plural): the ui's strings go through the language files (core/i18n)

#include "core/i18n.hpp"
#include "strata/keybinds.hpp"

// marks a constant table entry for tools/lang.ps1; show it with tr() when drawing
#define N_(s) s

namespace prism::ui {

using core::i18n::tr;
using core::i18n::tr_n;
using core::i18n::trf;
using core::i18n::trf_n;

// the chord an action is bound to right now, for sentences ("press {}"): rebinding in Settings shows up here
[[nodiscard]] inline std::string key_hint(const strata::keybinds& binds, std::string_view action)
{
    std::string k = binds.text(action);
    return k.empty() ? std::string{tr("unbound")} : k;
}

// display name of colour label `i` (the English word stays the query word in "tag:red")
[[nodiscard]] inline std::string_view tag_label(int i)
{
    static constexpr const char* k_names[] = {N_("red"), N_("orange"), N_("yellow"), N_("green"), N_("teal"), N_("blue"), N_("purple"), N_("pink")};
    return tr(k_names[i]);
}

} // namespace prism::ui
