#pragma once

// Snippets: small .nss files that each add one thing to the menu, turned on and off on
// the Snippets page (SnippetsController). No Slint here (tests/test_snippets.cpp).
//
// Where snippets come from is a Source (below). Both are embedded in the exe by
// cmake/Snippets.cmake, so NilesoftShellPro.exe needs no files beside it and nothing is
// downloaded:
//   - BundledSource: the app's own snippets, src/bin/snippets/catalog.json and <id>.nss;
//   - RubicSource: a checked selection from RubicBG's collection (CC0-1.0, shared with
//     his permission), src/bin/snippets/rubicbg/catalog.json and <id>.nss, copied
//     unchanged from his repository at the commit the catalog names.
//
// Turning one on, in the user's config folder:
//   imports/snippets/<id>.nss          a copy of the snippet
//   imports/settings-app-snippets.nss  app-owned; one `import 'snippets/<id>.nss'` per
//                                      snippet that is on (imports resolve against the
//                                      importing file, so the path is relative to imports/),
//                                      each after a `// snippet <id> sha256=<hash>` line with
//                                      the hash of the copy as the app wrote it
//   shell.nss                          `import 'imports/settings-app-snippets.nss'`, added once
// Turning it off removes the import and deletes the copy (the snapshot taken first keeps it).
// On start, a copy whose hash is still the recorded one (the user never edited it) is
// replaced when the bundled snippet changed; an edited copy is left alone.

#include "nss/NssDocument.h"
#include "nss/OverridesWriter.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snippets {

inline constexpr std::string_view kListPath = "imports/settings-app-snippets.nss"; // relative to the config folder
inline constexpr std::string_view kCopyDir = "imports/snippets";
inline constexpr std::string_view kListHeader = nss::OverridesWriter::header;

// RubicBG's collection: the "by RubicBG" link of his snippets.
inline constexpr std::string_view kRubicUrl = "https://github.com/RubicBG/Nilesoft-Shell-Snippets";

enum class Risk
{
    None,
    Admin,   // runs something as administrator
    Network, // uses the internet
};

struct Text
{
    std::string title;
    std::string description;
};

struct Snippet
{
    std::string id;          // [a-z0-9-], also the file name
    std::string title;       // English
    std::string description; // one line, English
    std::string category;    // files, folders, archives, terminal, security, ...
    std::string author;
    std::string authorUrl;   // the "by ..." link, "" for none
    std::string license;     // SPDX id: MIT, CC0-1.0
    std::string source;      // where it comes from (a URL)
    std::vector<std::string> needs; // catalog "requires"; see checkRequirement()
    Risk risk = Risk::None;
    std::string minVersion;  // the oldest Shell version it works with, "" for any
    std::string sha256;      // hashText() of the file
    std::map<std::string, Text> translations; // "zh-CN" -> title and description
    std::string text;        // the .nss, normalize()d
    std::string origin;      // the Source's id

    // The title and description in `lang` ("zh-CN"), else English.
    const std::string &titleIn(std::string_view lang) const;
    const std::string &descriptionIn(std::string_view lang) const;
};

struct Catalog
{
    std::vector<Snippet> snippets;
    std::vector<std::string> errors; // entries left out, and why

    const Snippet *find(std::string_view id) const;
};

// The file text the app compares and hashes: no UTF-8 BOM, LF line endings.
std::string normalize(std::string_view bytes);
// SHA-256 (lower-case hex) of normalize(bytes).
std::string hashText(std::string_view bytes);
// normalize()d text with CRLF line endings, as the copies are written.
std::string toCrlf(std::string_view text);

bool validId(std::string_view id);

// Parses a catalog.json. `file(id)` gives the bytes of <id>.nss (nullopt: missing). An
// entry without a valid id, title or file, or whose file doesn't match its sha256, is
// left out and named in `errors`. Unknown fields are ignored.
// The catalog's own "author", "author_url" and "license" are the default of every entry.
// An entry with a "path" (validRepoPath()) in the catalog's "repository" at "commit" has
// https://github.com/<repository>/blob/<commit>/<path> as its source, unless it names one.
Catalog parseCatalog(std::string_view json, const std::function<std::optional<std::string>(const std::string &id)> &file,
                     std::string_view origin);

// --- sources ---------------------------------------------------------------------------

// Where snippets come from. load() may be slow (it runs on the worker thread).
class Source
{
public:
    virtual ~Source() = default;
    virtual std::string id() const = 0;
    virtual Catalog load() const = 0;
};

// The snippets embedded in the exe (src/bin/snippets).
class BundledSource : public Source
{
public:
    std::string id() const override { return "bundled"; }
    Catalog load() const override;
};

// RubicBG's snippets (src/bin/snippets/rubicbg), embedded in the exe.
class RubicSource : public Source
{
public:
    std::string id() const override { return "rubicbg"; }
    Catalog load() const override;
};

// Every source's catalog, in order (bundled first). An id already taken is left out.
Catalog loadAll();

// A path inside a repository: letters, digits, '.', '_', '-' and '/' between names,
// ending in .nss; no "..", no leading '/'.
bool validRepoPath(std::string_view path);

// --- requirements ----------------------------------------------------------------------

// How requirements are checked, replaceable in tests.
struct Probe
{
    std::function<bool(const std::wstring &path)> fileExists;
    std::function<std::wstring(const std::wstring &text)> expand; // %VARIABLES%
    std::function<bool(const std::wstring &exe)> onPath;          // PATH or App Paths
};
Probe systemProbe();

struct Requirement
{
    std::string name; // "7-Zip"
    bool met = false;
};

// A requirement is one of:
//   - an app id this file knows: "7-zip", "windows-terminal", "git", "vscode", "everything";
//   - a path to an exe ('\' or '/' in it; %VARIABLES% are expanded);
//   - an exe name looked up on PATH and in App Paths ("code" or "code.exe").
Requirement checkRequirement(std::string_view requirement, const Probe &probe);

// "2.0.6" against the running core version ("2.0.6-beta.2" counts as 2.0.6). An empty or
// unreadable minimum is met.
bool versionMet(std::string_view minVersion, std::string_view running);

// --- the files in the config folder ----------------------------------------------------

struct Entry
{
    std::string id;
    std::string sha256; // of the copy as the app wrote it ("" if unknown)
};

// The `import 'snippets/<id>.nss'` lines of settings-app-snippets.nss, in order, with the
// hash from the `// snippet <id> sha256=...` line before each.
std::vector<Entry> parseList(std::string_view text);
std::string generateList(const std::vector<Entry> &entries); // CRLF
// The list with `entries` as the snippets that are on, made from the file as it is
// (`existing`, nullopt: generateList()): the import of a snippet that went off goes,
// with its "// snippet" line; a new one is added at the end; a changed hash is
// updated in place. Anything else the user wrote (comments, other imports or
// statements) stays where it is. CRLF.
std::string updateList(const std::optional<std::string> &existing, const std::vector<Entry> &entries);

// "imports/snippets/<id>.nss"
std::string copyPath(std::string_view id);

// What the config folder holds, read by the caller.
struct ConfigFiles
{
    std::optional<std::string> list;                          // settings-app-snippets.nss
    std::map<std::string, std::optional<std::string>> copies; // id -> imports/snippets/<id>.nss
};

enum class CopyState
{
    Off,       // not in the list
    Current,   // on, and the copy is the bundled snippet
    Outdated,  // on, unedited, and the bundled snippet changed: refresh() replaces it
    Edited,    // on, and the user changed the copy: kept as it is
    Missing,   // on, but the copy is gone: refresh() writes it again
};
CopyState copyState(const Snippet &snippet, const ConfigFiles &files);

struct Write
{
    std::string path; // relative to the config folder, '/' separated
    std::string text; // UTF-8, CRLF
};

struct Plan
{
    std::vector<Write> writes;
    std::vector<std::string> removes; // relative paths to delete once the writes went through
    std::vector<std::string> changes; // "turn on copy-file-hash", for the snapshot reason
    bool needsImport = false;         // shell.nss must import settings-app-snippets.nss
    std::string error;
    bool empty() const { return writes.empty() && removes.empty(); }
};

// A plain message when the user edited settings-app-snippets.nss by hand so that it
// doesn't parse (then nothing is written to it); "" otherwise.
std::string listProblem(const ConfigFiles &files);

// Turn one snippet on or off. Plan::error is listProblem() when there is one.
Plan planSet(const Catalog &catalog, const ConfigFiles &files, std::string_view id, bool on);
// Bring the copies of the snippets that are on up to date: Outdated and Missing ones are
// written again; Edited ones and snippets no longer in the catalog are left alone.
Plan planRefresh(const Catalog &catalog, const ConfigFiles &files);

// Adds `import 'imports/settings-app-snippets.nss'` to the end of the root, or turns an
// existing import of it back on (the Context Menus page can comment it out). Returns true
// when the root changed.
bool ensureImported(nss::NssDocument &root);

// The category's label in English (nsp::mark()ed; translate with nsp::tr), or nullptr
// for a category the app doesn't know (the page then shows it as written).
const char *categoryLabel(std::string_view category);
// The known categories, in the order the page shows them.
const std::vector<std::string> &knownCategories();

} // namespace snippets
