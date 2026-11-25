#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace prism::core {

// bulk workers (scans, hashing, thumbnails) step aside for the ui thread when the cpu is busy; not for work the user waits on
inline void lower_thread_priority() noexcept
{
#ifdef _WIN32
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    (void)::nice(5);
#endif
}

} // namespace prism::core
