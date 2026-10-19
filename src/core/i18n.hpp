#pragma once

// UI translations, loaded from text files (see lang/README.md). The English text is the key: `tr("Compact density")`
// returns it unchanged for English or for any text a language does not translate, so strings are wrapped one at a time.
// Ui thread only: the language is a plain global, and returned views stay valid for the whole run.

#include <format>
#include <span>
#include <string>
#include <string_view>

namespace prism::core::i18n {

struct LanguageInfo {
    std::string code; // file name without ".lang" ("de"); "en" is built in
    std::string name; // in its own language, shown in Settings
    std::string path; // empty for English
};

// looks for *.lang in each folder (later folders override earlier ones with the same code); English is always first
void scan(std::span<const std::string> dirs);
[[nodiscard]] std::span<const LanguageInfo> languages();
// loads the language; unknown or unreadable falls back to English. returns the code in use
std::string_view set_language(std::string_view code);
std::string_view reload(); // re-reads the current file (after editing it)
[[nodiscard]] std::string_view language();
[[nodiscard]] unsigned revision() noexcept; // changes whenever the texts change (rebuild what was translated once, e.g. the action list)

[[nodiscard]] std::string_view tr(std::string_view english);
// plural: the key is "one|other"; a file lists one form per plural rule of the language, separated by '|'
[[nodiscard]] std::string_view tr_n(std::string_view one, std::string_view other, long long n);

// translated text with std::format placeholders ("{} items selected"). Entries whose placeholders do not match the English ones
// are dropped when the file loads, so a typo in a .lang file can never make the format call fail
template <class... A>
[[nodiscard]] std::string trf(std::string_view english, A&&... args)
{
    return std::vformat(tr(english), std::make_format_args(args...));
}
template <class... A>
[[nodiscard]] std::string trf_n(std::string_view one, std::string_view other, long long n, A&&... args)
{
    return std::vformat(tr_n(one, other, n), std::make_format_args(args...));
}

} // namespace prism::core::i18n
