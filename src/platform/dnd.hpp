#pragma once

// ole drag and drop. out: a drag of real file objects (explorer, the desktop and other apps accept them). in: the main
// window accepts file drops from anywhere, our own drags included, and tells the ui which folder is under the pointer

#include "strata/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace prism::platform {

enum class DropAction { copy, move, link };

struct DropCallbacks {
    // the folder a drop at this logical point would land in; "" = nowhere
    std::function<std::string(strata::vec2)>                                             resolve;
    // the pointer while a drag is over the window (nullopt when it leaves or ends)
    std::function<void(std::optional<strata::vec2>)>                                     hover;
    std::function<void(std::vector<std::string>, std::string dest, DropAction action)>   dropped;
    // true when this destination only records the paths (a pin): the pointer shows "link" and nothing is moved
    std::function<bool(const std::string&)>                                        link;
    // files offered without a path (remote files from MobaXterm ...) that could not be received: how many
    std::function<void(std::size_t)>                                               failed;
    // draw a frame now: the drag has its own message loop, so the app loop is not running
    std::function<void()>                                                          redraw;
    // physical pixels per logical pixel of the ui
    std::function<float()>                                                         scale;
};

// registers `hwnd` as a drop target (needs OleInitialize on this thread). false on failure
bool attach_drop_target(void* hwnd, DropCallbacks callbacks);
void detach_drop_target(void* hwnd);

// runs the drag of these files until it ends; blocks (it pumps messages itself). true if the drop happened elsewhere
bool run_drag(const std::vector<std::string>& paths);

} // namespace prism::platform
