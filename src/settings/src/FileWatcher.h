#pragma once

// Watches a folder (optionally recursively) with ReadDirectoryChangesW and
// reports changed files after a quiet period (debounce).
//
// The app's own saves are not reported: call noteOwnWrite() with the bytes
// written, and a change whose file content hashes the same is dropped.
//
// The callback runs on the watcher's thread; marshal to the UI thread with
// slint::invoke_from_event_loop.

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <latch>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class FileWatcher
{
public:
    using Callback = std::function<void(std::vector<std::filesystem::path> changed)>;
    using Filter = std::function<bool(const std::filesystem::path &)>; // true: report this path

    FileWatcher() = default;
    ~FileWatcher();
    FileWatcher(const FileWatcher &) = delete;
    FileWatcher &operator=(const FileWatcher &) = delete;

    // Starts watching `dir` (stopping any previous watch). False if the folder
    // cannot be opened.
    bool start(const std::filesystem::path &dir, Callback callback, Filter filter = {}, bool recursive = true,
               std::chrono::milliseconds debounce = std::chrono::milliseconds(300));
    void stop();
    bool watching() const { return thread_.joinable(); }
    const std::filesystem::path &dir() const { return dir_; }

    // Remembers the content the app itself wrote to `file`.
    void noteOwnWrite(const std::filesystem::path &file, std::string_view bytes);
    // Same, reading the file's current bytes.
    void noteOwnWrite(const std::filesystem::path &file);

    // True when the file's current content is what the app last wrote there
    // (also true when both are "missing").
    bool isOwnContent(const std::filesystem::path &file) const;

    static std::string hashOf(std::string_view bytes);
    static std::string hashFile(const std::filesystem::path &file); // "" when unreadable

private:
    void run(std::stop_token stop, HANDLE dirHandle, std::latch *ready);

    std::filesystem::path dir_;
    Callback callback_;
    Filter filter_;
    bool recursive_ = true;
    std::chrono::milliseconds debounce_{ 300 };
    HANDLE stopEvent_ = nullptr;

    mutable std::mutex mutex_;
    std::map<std::string, std::string> ownHashes_; // pathKey -> sha256 of our last write
    std::jthread thread_;
};
