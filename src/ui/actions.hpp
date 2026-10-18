#pragma once

// every command the user can trigger, by name: the shortcuts, the command palette and the rebinding editor all work from
// this one list (strata::keybinds holds the chords). names are what settings.ini stores, so they never change

#include "strata/keybinds.hpp"

namespace prism::ui {

void register_actions(strata::keybinds& binds);

} // namespace prism::ui
