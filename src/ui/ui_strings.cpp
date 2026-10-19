#include "ui/ui_strings.hpp"

#include "ui/tr.hpp"

namespace prism::ui {

void apply_ui_strings(strata::context& ui)
{
    strata::ui_strings s;
    s.months   = {tr("January"), tr("February"), tr("March"), tr("April"), tr("May"), tr("June"), tr("July"), tr("August"),
                  tr("September"), tr("October"), tr("November"), tr("December")};
    s.weekdays = {tr("Mo"), tr("Tu"), tr("We"), tr("Th"), tr("Fr"), tr("Sa"), tr("Su")};
    s.today = tr("Today");
    s.now   = tr("Now");
    s.done  = tr("Done");
    s.filter_hint      = tr("type to filter");
    s.no_match         = tr("no match");
    s.nothing_selected = tr("none");
    s.all_selected     = tr("all");
    s.select_all       = tr("select all");
    s.select_none      = tr("clear");
    s.press_a_key      = tr("press a key...");
    s.unbound          = tr("None");
    s.action           = tr("Action");
    s.key              = tr("Key");
    s.reset            = tr("reset");
    s.key_shared_with  = tr("the same key is used by ");
    s.palette_title    = tr("Command palette");
    s.palette_hint     = tr("Type a command...");
    s.no_commands      = tr("no matching commands");
    s.commands         = tr("commands");
    s.confirm          = tr("Confirm");
    s.toast_success    = {}; // toasts are one line: the accent bar already says done / warning / error
    s.toast_warning    = {};
    s.toast_error      = {};
    ui.set_strings(s);
}

} // namespace prism::ui
