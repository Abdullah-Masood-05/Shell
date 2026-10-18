#pragma once

// The Config Editor's model, free of UI code so it can be tested on its own:
// the import tree, file buffers, the live `-config:check` on a temp copy of
// the edited tree, and the save through nss::ConfigStore.

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace editor {

namespace fs = std::filesystem;

struct FileEntry
{
    fs::path path;        // absolute; empty for an unresolved import
    std::string label;    // relative to the root's folder ("imports/theme.nss")
    std::string name;     // file name, or the import expression when unresolved
    int depth = 0;        // 0 for the root
    bool managed = false; // app-owned (imports/settings-app*.nss): read-only
    bool missing = false; // could not be loaded
    bool unresolved = false;
    bool lang = false;    // import lang
    std::string note;     // load error or unresolved reason
};

// The root and everything it imports, depth first in import order, with
// unresolved imports listed under the importing file.
std::vector<FileEntry> loadTree(const fs::path &root, const fs::path &appDir = {});  // appDir: app.dir (the install folder)

// imports/settings-app.nss, imports/settings-app-menus.nss and imports/settings-app-snippets.nss
// under the root's folder.
bool isManaged(const fs::path &rootDir, const fs::path &file);

// A path the app must never write: under Program Files.
bool isProtectedPath(const fs::path &file);

// Text in the editor uses '\n' only; files keep their own line endings.
std::string toEditorText(std::string_view text);
std::string fromEditorText(std::string_view text, nss::Eol eol);

struct Buffer
{
    fs::path path;
    std::string original;  // editor text ('\n') of the file on disk
    std::string text;      // editor text, possibly edited
    nss::TextFormat format;
    std::string diskHash;  // sha256 of the bytes on disk when loaded

    bool dirty() const { return text != original; }
};

std::optional<Buffer> loadBuffer(const fs::path &path, std::string *error = nullptr);

struct Issue
{
    bool error = true;
    fs::path file;
    int line = 0;
    int column = 0;
    std::string message;
};

struct CheckOutcome
{
    bool ran = false;    // shell.exe ran and wrote a result
    bool ok = false;
    std::vector<Issue> issues; // errors first
    std::string failure; // why the check could not run
};

// Parses the -config:check JSON. Paths under `tempDir` (default: the folder of
// the checked root) are mapped to `realDir` when it is given.
CheckOutcome parseCheck(std::string_view json, const fs::path &tempDir = {}, const fs::path &realDir = {});

struct SaveRequest
{
    fs::path path;
    std::string text;      // editor text
    nss::TextFormat format;
};

// Runs `shell.exe -silent -config:check`. With overlays (edited buffers), the
// tree is copied to a temp folder first and the overlays are written there in
// their own format; the real files are never touched.
CheckOutcome checkTree(const fs::path &shellExe, const fs::path &root, const std::vector<SaveRequest> &overlays = {});

// Saves through ConfigStore: validator unless `force`, snapshot
// "Config Editor: <files>", .bak, atomic writes keeping encoding/BOM/EOL.
nss::SaveResult save(const fs::path &shellExe, const fs::path &root, const std::vector<SaveRequest> &files, bool force,
                     const nss::RetentionPolicy &retention = {});

std::string saveReason(const fs::path &root, const std::vector<SaveRequest> &files);

// Byte offsets [begin, end) of 1-based `line` in editor text.
std::pair<size_t, size_t> lineOffsets(std::string_view text, int line);

} // namespace editor
