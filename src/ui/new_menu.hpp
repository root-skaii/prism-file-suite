#pragma once

// the document entries both "New" menus share (toolbar button, right click on empty space): built-in templates, whatever
// sits in the Templates folder beside settings.ini, and a shortcut to that folder

#include "ui/env.hpp"

namespace prism::ui {

// call inside an open menu; a pick becomes the action "new_template:<key>" (handled by FileList)
void new_template_items(strata::context& ui, Env& env);

} // namespace prism::ui
