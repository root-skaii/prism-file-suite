#pragma once

// what a finished file operation reports (shared by the shell-based jobs in file_ops and the built-in transfer queue)

#include <cstddef>
#include <string>
#include <vector>

namespace prism::core {

enum class OpKind { copy, move, recycle, erase };

struct OpResult {
    OpKind      kind{};
    bool        ok{};
    bool        aborted{}; // the user cancelled
    bool        access_denied{}; // built-in transfer hit ERROR_ACCESS_DENIED (caller may retry via elevated shell)
    bool        silent{};  // started by undo / redo: no toast, not recorded again
    std::size_t count{};
    std::string dest;      // destination folder (copy / move), the source folder otherwise: what to refresh
    std::vector<std::string> sources; // what the job was given (undo needs it)
    // copy / move by the built-in queue: where each source ended up (same order as `sources`; "" = not transferred).
    // empty when the shell did it: the name is then the source's own
    std::vector<std::string> made;
    std::string message;   // why it failed or only partly worked (for a toast); empty when all went well
};

} // namespace prism::core
