#include "FileWatcher.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <latch>
#include <set>

namespace fs = std::filesystem;

FileWatcher::~FileWatcher() { stop(); }

std::string FileWatcher::hashOf(std::string_view bytes) { return nss::ConfigStore::sha256(bytes); }

std::string FileWatcher::hashFile(const fs::path &file)
{
    std::string bytes;
    if (!nss::readFile(file, bytes))
        return {};
    return hashOf(bytes);
}

void FileWatcher::noteOwnWrite(const fs::path &file, std::string_view bytes)
{
    std::lock_guard lock(mutex_);
    ownHashes_[nss::pathKey(file)] = hashOf(bytes);
}

void FileWatcher::noteOwnWrite(const fs::path &file)
{
    auto h = hashFile(file);
    std::lock_guard lock(mutex_);
    ownHashes_[nss::pathKey(file)] = h;
}

bool FileWatcher::isOwnContent(const fs::path &file) const
{
    std::string own;
    {
        std::lock_guard lock(mutex_);
        auto it = ownHashes_.find(nss::pathKey(file));
        if (it == ownHashes_.end())
            return false;
        own = it->second;
    }
    return hashFile(file) == own;
}

bool FileWatcher::start(const fs::path &dir, Callback callback, Filter filter, bool recursive,
                        std::chrono::milliseconds debounce)
{
    stop();
    HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    dir_ = dir;
    callback_ = std::move(callback);
    filter_ = std::move(filter);
    recursive_ = recursive;
    debounce_ = debounce;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // Wait until the first read is queued, so no change right after start() is missed.
    std::latch ready(1);
    thread_ = std::jthread([this, h, &ready](std::stop_token st) { run(st, h, &ready); });
    ready.wait();
    return true;
}

void FileWatcher::stop()
{
    if (thread_.joinable()) {
        thread_.request_stop();
        SetEvent(stopEvent_);
        thread_.join();
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void FileWatcher::run(std::stop_token stop, HANDLE dirHandle, std::latch *ready)
{
    std::vector<DWORD> storage(16 * 1024); // 64 KB, DWORD-aligned
    auto *buffer = reinterpret_cast<BYTE *>(storage.data());
    const DWORD bufferSize = DWORD(storage.size() * sizeof(DWORD));
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const DWORD notify = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                         FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;

    std::set<fs::path> pending;
    auto lastEvent = std::chrono::steady_clock::now();
    auto issue = [&] {
        ResetEvent(ov.hEvent);
        return ReadDirectoryChangesW(dirHandle, buffer, bufferSize, recursive_, notify, nullptr, &ov, nullptr) != 0;
    };
    bool reading = issue();
    ready->count_down();

    while (!stop.stop_requested() && reading) {
        DWORD timeout = INFINITE;
        if (!pending.empty()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastEvent);
            timeout = elapsed >= debounce_ ? 0 : DWORD((debounce_ - elapsed).count());
        }
        HANDLE handles[2] = { ov.hEvent, stopEvent_ };
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, timeout);
        if (w == WAIT_OBJECT_0 + 1)
            break;
        if (w == WAIT_OBJECT_0) {
            DWORD bytes = 0;
            if (!GetOverlappedResult(dirHandle, &ov, &bytes, FALSE)) {
                reading = false;
                break;
            }
            if (bytes == 0) {
                pending.insert(dir_); // the buffer overflowed: "something changed"
            } else {
                auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buffer);
                for (;;) {
                    std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                    pending.insert((dir_ / name).lexically_normal());
                    if (!info->NextEntryOffset)
                        break;
                    info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(reinterpret_cast<BYTE *>(info) + info->NextEntryOffset);
                }
            }
            lastEvent = std::chrono::steady_clock::now();
            reading = issue();
            continue;
        }
        if (w == WAIT_TIMEOUT && !pending.empty()) {
            std::vector<fs::path> changed;
            for (const auto &p : pending) {
                if (filter_ && p != dir_ && !filter_(p))
                    continue;
                if (p != dir_ && isOwnContent(p))
                    continue;
                changed.push_back(p);
            }
            pending.clear();
            if (!changed.empty() && callback_)
                callback_(std::move(changed));
            continue;
        }
        if (w == WAIT_FAILED)
            break;
    }

    if (reading) {
        CancelIoEx(dirHandle, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(dirHandle, &ov, &ignored, TRUE);
    }
    CloseHandle(ov.hEvent);
    CloseHandle(dirHandle);
}
