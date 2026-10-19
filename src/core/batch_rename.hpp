#pragma once

// renaming many files by one rule, with a preview. pure string work (no ui): the dialog shows batch_names() live and
// only touches disk through apply_renames() once the user says so

#include <string>
#include <utility>
#include <vector>

namespace prism::core {

enum class CaseMode { keep, lower, upper, title };

struct RenameRule {
    std::string pattern{"[N]"};   // [N] the name, [E] the extension, [C] a counter; anything else is literal text
    std::string find;             // replaced (ASCII case-insensitive) in the name before the pattern is applied
    std::string replace;
    CaseMode    casing{CaseMode::keep};
    int         start{1};
    int         step{1};
    int         digits{1};        // counter padded with zeros to this width
};

struct RenameItem {
    std::string name;
    bool        is_dir{};         // folders have no extension to keep apart
};

// the new name of every item, in the order given (the counter follows that order). The extension is kept unless the
// pattern places it with [E]
[[nodiscard]] std::vector<std::string> batch_names(const std::vector<RenameItem>& items, const RenameRule& rule);

// why each new name cannot be used (empty = fine): "empty", "invalid characters", "ends with a dot or space",
// "same name twice", "already exists". `others` are the names in the folder that are not being renamed
[[nodiscard]] std::vector<std::string> batch_problems(const std::vector<RenameItem>& items, const std::vector<std::string>& result,
                                                      const std::vector<std::string>& others);

// renames dir\old -> dir\new for every pair whose names differ. Two phases (through temporary names), so swapping or
// shifting names inside the selection works; on a failure everything done so far is rolled back and false returned
[[nodiscard]] bool apply_renames(const std::string& dir, const std::vector<std::pair<std::string, std::string>>& pairs);

} // namespace prism::core
