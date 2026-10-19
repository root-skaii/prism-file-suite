#pragma once

// copy / move / delete through the shell's IFileOperation: undo records, the recycle bin, conflict prompts and the
// system progress dialog all come for free. each job runs on its own thread (COM apartment), the ui polls results.

#include "core/op_types.hpp"
#include "core/transfer.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace prism::core {

class FileOps {
public:
    // `wake` runs on the worker when a job finishes; `owner` is the HWND the shell dialogs attach to
    FileOps(std::function<void()> wake, void* owner) : wake_{wake}, owner_{owner}, transfers_{std::move(wake)} {}
    ~FileOps();
    FileOps(const FileOps&)            = delete;
    FileOps& operator=(const FileOps&) = delete;

    // copy and move go through the built-in queue (one at a time, with progress and its own conflict prompt) unless
    // `use_builtin(false)`: then the shell's IFileOperation does them, with Windows' own progress window
    void start(OpKind kind, std::vector<std::string> sources, std::string dest_dir, bool silent = false);
    void use_builtin(bool on) noexcept { builtin_ = on; }
    [[nodiscard]] TransferQueue& transfers() noexcept { return transfers_; }
    [[nodiscard]] bool poll(OpResult& out);
    [[nodiscard]] bool busy() const;

    // quick, synchronous
    [[nodiscard]] static bool rename(const std::string& path, const std::string& new_name);
    // called (on the calling thread) after a rename succeeded, with the old and new full paths: things keyed by path follow.
    // every window registers one (keyed by itself), so all of them are told
    using RenameHook = std::function<void(const std::string& from, const std::string& to)>;
    static void add_rename_hook(const void* owner, RenameHook hook) { rename_hooks().emplace_back(owner, std::move(hook)); }
    static void remove_rename_hook(const void* owner) { std::erase_if(rename_hooks(), [owner](const auto& h) { return h.first == owner; }); }
    // creates "New folder", "New folder (2)" ... returns its name, empty on failure
    [[nodiscard]] static std::string create_folder(const std::string& dir);
    // creates an empty file "<stem><ext>", "<stem> (2)<ext>" ... returns its name, empty on failure
    [[nodiscard]] static std::string create_file(const std::string& dir, const std::string& stem, const std::string& ext);
    // removes a file only if it is still empty (undoing "new file" must never throw away what was typed into it)
    [[nodiscard]] static bool remove_empty_file(const std::string& path);
    // removes a folder only if it is empty (undoing "new folder")
    [[nodiscard]] static bool remove_empty_folder(const std::string& path);
    // puts items that were recycled back where they came from (the recycle bin's own "restore"). needs the original
    // paths; true if something was restored. runs on the calling (ui) thread
    [[nodiscard]] static bool restore_from_recycle(const std::vector<std::string>& original_paths);

private:
    [[nodiscard]] static std::vector<std::pair<const void*, RenameHook>>& rename_hooks()
    {
        static std::vector<std::pair<const void*, RenameHook>> hooks;
        return hooks;
    }

    void run(OpKind kind, std::vector<std::string> sources, std::string dest, bool silent);

    std::function<void()>    wake_;
    void*                    owner_{};
    TransferQueue            transfers_;
    bool                     builtin_{true};
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
