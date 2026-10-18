#pragma once

// copy / move / delete through the shell's IFileOperation: undo records, the recycle bin, conflict prompts and the
// system progress dialog all come for free. each job runs on its own thread (COM apartment), the ui polls results.

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace prism::core {

enum class OpKind { copy, move, recycle, erase };

struct OpResult {
    OpKind      kind{};
    bool        ok{};
    bool        aborted{}; // the user cancelled in the shell dialog
    bool        silent{};  // started by undo / redo: no toast, not recorded again
    std::size_t count{};
    std::string dest;      // destination folder (copy / move), the source folder otherwise: what to refresh
    std::vector<std::string> sources; // what the job was given (undo needs it)
};

class FileOps {
public:
    // `wake` runs on the worker when a job finishes; `owner` is the HWND the shell dialogs attach to
    FileOps(std::function<void()> wake, void* owner) : wake_{std::move(wake)}, owner_{owner} {}
    ~FileOps();
    FileOps(const FileOps&)            = delete;
    FileOps& operator=(const FileOps&) = delete;

    void start(OpKind kind, std::vector<std::string> sources, std::string dest_dir, bool silent = false);
    [[nodiscard]] bool poll(OpResult& out);
    [[nodiscard]] bool busy() const;

    // quick, synchronous
    [[nodiscard]] static bool rename(const std::string& path, const std::string& new_name);
    // creates "New folder", "New folder (2)" ... returns its name, empty on failure
    [[nodiscard]] static std::string create_folder(const std::string& dir);
    // removes a folder only if it is empty (undoing "new folder")
    [[nodiscard]] static bool remove_empty_folder(const std::string& path);
    // puts items that were recycled back where they came from (the recycle bin's own "restore"). needs the original
    // paths; true if something was restored. runs on the calling (ui) thread
    [[nodiscard]] static bool restore_from_recycle(const std::vector<std::string>& original_paths);

private:
    void run(OpKind kind, std::vector<std::string> sources, std::string dest, bool silent);

    std::function<void()>    wake_;
    void*                    owner_{};
    mutable std::mutex       lock_;
    std::vector<std::thread> threads_;
    std::vector<OpResult>    results_;
    int                      running_{};
};

// files on the system clipboard (CF_HDROP + "Preferred DropEffect"), so copy / paste works with explorer and other apps
bool clipboard_set_files(const std::vector<std::string>& paths, bool cut);
bool clipboard_get_files(std::vector<std::string>& paths, bool& cut);
[[nodiscard]] bool clipboard_has_files();

} // namespace prism::core
