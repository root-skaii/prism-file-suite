#pragma once

#include <windows.h>

namespace prism::core {

// bulk workers (scans, hashing, thumbnails) step aside for the ui thread when the cpu is busy; not for work the user waits on
inline void lower_thread_priority() noexcept { ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

} // namespace prism::core
