#pragma once

// crash handling (win32): a fatal fault writes a report (text + minidump) and shows a notice from a fresh process;
// `guard` wraps calls into code we do not own (shell extensions, thumbnail handlers): a fault there is logged with the
// module that caused it and the call is abandoned, instead of taking the whole window down

#include <string>
#include <type_traits>

namespace prism::core::crash {

// top-level handler for faults, abort(), pure virtual calls and invalid CRT parameters. call first thing in main
void install();
// where reports go (created on demand); until set, %TEMP%\PrismFS-crashes
void set_directory(const std::wstring& dir);

// the `guard` implementation (SEH, so it cannot live in a template). false: the call faulted
[[nodiscard]] bool guarded(const char* what, void (*fn)(void*), void* context) noexcept;

// runs f(); returns false if it hit a hardware fault (access violation, illegal instruction, divide by zero ...).
// the state f touched may be half done: use it only around calls whose results are optional (a menu, a thumbnail)
template <class F>
[[nodiscard]] bool guard(const char* what, F&& f)
{
    return guarded(what, [](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); }, &f);
}

} // namespace prism::core::crash
