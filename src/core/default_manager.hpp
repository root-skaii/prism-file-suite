#pragma once

// making PrismFS the app that opens folders (and drives) for other programs, and giving that back to Explorer. it is the
// per-user "open" verb of the Directory and Drive classes (HKCU, no admin rights); removing it restores what Windows had.
// Win+E, the taskbar button and "show in folder" in browsers call explorer.exe directly and stay as they are

namespace prism::core {

// folders are opened by a PrismFS (any copy) through the registry verb
[[nodiscard]] bool default_manager_active();
// switches it on (pointing at this exe) or off. false if the registry refused
bool set_default_manager(bool on);
// the Recycle Bin icon is not a folder, so it has a verb of its own (HKCU\Software\Classes\CLSID\{645FF040-...}\shell\open):
// double-click or "Open" starts PrismFS on the bin folder (see core/recycle). the same on / off / refresh rules
[[nodiscard]] bool recycle_bin_handler_active();
bool set_recycle_bin_handler(bool on);
// when it is on but points at another copy of the exe (the folder was moved), point it here again
void refresh_default_manager();

} // namespace prism::core
