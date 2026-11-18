#pragma once

// The Backups page's model, free of UI code: snapshots of the config tree
// (nss::ConfigStore), deleting unpinned ones, and a per-file diff of a
// snapshot against the current files.

#include "Diff.h"

#include "nss/ConfigStore.h"

#include <filesystem>
#include <string>
#include <vector>

namespace backups {

namespace fs = std::filesystem;

struct FileDiff
{
    enum Status
    {
        Same,
        Modified,
        Added,   // in the current tree, not in the snapshot
        Removed, // in the snapshot, not in the current tree
    };
    std::string path; // relative, '/' separated
    Status status = Same;
    diff::Unified diff; // snapshot (old) -> current (new); empty for a binary file
    // An image or another file that isn't text: compared byte for byte, not line by line.
    bool binary = false;
    uint64_t oldSize = 0, newSize = 0; // bytes in the snapshot and now (0 when absent)
};

struct SnapshotDiff
{
    std::vector<FileDiff> files; // changed files first, then unchanged, each by path
    int changed = 0;
    std::string error;
};

// Compares the snapshot's files with the current tree (text decoded, so a
// change of encoding alone shows as "modified" without line changes). A snapshot
// that kept every file (SnapshotInfo::allFiles) is compared with every file the
// next one would keep; an older one with the .nss files only.

SnapshotDiff diffSnapshot(const nss::ConfigStore &store, const nss::SnapshotInfo &snapshot);

// Deletes an unpinned snapshot folder. False (with a reason) otherwise.
bool deleteSnapshot(const nss::ConfigStore &store, const std::string &id, std::string *error = nullptr);

// Not text: a NUL byte in it, or it isn't UTF-8 or UTF-16 text (.nss files are
// always text).
bool isBinary(std::string_view path, std::string_view bytes);

// "12 KB", "1.5 MB", "320 bytes".
std::string sizeText(uint64_t bytes);

// True when every file of the snapshot matches the tree byte for byte.
bool matchesTree(const nss::ConfigStore &store, const nss::SnapshotInfo &snapshot);

// "Oct 12, 2026, 20:31" from "2026-10-12T20:31:07"; the id is used as a fallback.
std::string displayTime(const nss::SnapshotInfo &snapshot);
// "just now", "5 minutes ago", "yesterday", "3 days ago".
std::string relativeTime(const nss::SnapshotInfo &snapshot);

} // namespace backups
