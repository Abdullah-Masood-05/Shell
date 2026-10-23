#pragma once

// The Apps submenu of the taskbar's menu, as the Context Menus page's "Taskbar
// menu" card edits it. Free of UI code.
//
// src/bin/imports/taskbar.nss imports imports/settings-app-taskbar.nss inside its
// Apps menu. While that file exists, its items are shown there instead of the
// four shipped ones (Paint, Edge, Calculator, Registry Editor). Without it the
// page starts from those four, so the first save writes them out as they are.
//
// The file is the user's as much as the app's:
//  - parse() reads it without losing anything: each app is an item(...)
//    declaration; a switched-off app is the same declaration commented out
//    ("// item(...)"). Comments, blank lines and whatever the page doesn't edit
//    (a submenu, a variable, an item whose command is an expression) are kept
//    and listed as written by hand;
//  - generate() writes the list back. An entry the user didn't touch keeps its
//    text byte for byte; in an edited one only the changed values are spliced
//    in, so properties the page doesn't know (where=, keys=, sep=...) stay;
//  - a file that doesn't parse is not written over (List::error).

#include "nss/Text.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace taskbarapps {

inline constexpr std::string_view kRelativePath = "imports/settings-app-taskbar.nss";
inline constexpr std::string_view kFileName = "settings-app-taskbar.nss";
// The first lines of a new file.
inline constexpr std::string_view kHeader[] = {
    "// Nilesoft Shell Pro keeps this file: the Apps submenu of the taskbar's menu",
    "// (Context Menus page, Taskbar menu). You can edit it; the app keeps your changes.",
};

enum class Kind
{
    App,       // item(... cmd='...')
    Separator, // sep / separator
    Other,     // anything else: kept as written, can be moved or removed
};

struct Entry
{
    Kind kind = Kind::App;
    bool enabled = true;    // false: commented out (apps and separators)

    // App values as the page shows and edits them.
    std::string title;      // plain text; "" when titleExpr is set
    std::string titleExpr;  // the title as an expression (loc.paint), when it isn't plain text
    std::string program;    // what cmd= runs: a full path ("@sys.prog32\..." expanded), a shortcut, a command
    std::string args;
    bool admin = false;
    std::string image;      // new entries only: "" = `image` (the program's own icon), else a raw value

    // Kept text.
    std::vector<std::string> lead; // comment and blank lines right above it
    std::vector<std::string> text; // the declaration's lines as read; empty for a new entry
    std::string summary;           // Other: a short description ("menu(title='Dev')")
    size_t line = 0;               // 1-based first line of the declaration in the file, 0 = new

    // As read: generate() rewrites an entry only when one of these changed.
    struct Read
    {
        std::string title, titleExpr, program, args;
        bool admin = false;
        bool enabled = true;
        bool operator==(const Read &) const = default;
    } read;

    Read values() const { return { title, titleExpr, program, args, admin, enabled }; }
    bool edited() const { return text.empty() || values() != read; }
};

struct List
{
    bool exists = false;            // read from a file (else: the shipped defaults)
    std::vector<std::string> head;  // lines before the first entry (the header, blank lines)
    std::vector<Entry> entries;
    std::vector<std::string> tail;  // lines after the last entry
    nss::Eol eol = nss::Eol::CRLF;
    std::string error;              // the file doesn't read: shown, and never written over
    size_t errorLine = 0;
};

// Paths in cmd=: "@sys.prog32\x" <-> "C:\Program Files (x86)\x" (openwith::Env).
struct Paths
{
    std::function<std::string(std::string_view)> expand;   // as written -> full path
    std::function<std::string(std::string_view)> contract; // full path -> as written
};
Paths realPaths();

// The four apps taskbar.nss ships, as a list (not read from a file).
List defaults(const Paths &paths = {});
// `text` is the file's text (UTF-8).
List parse(std::string_view text, const Paths &paths = {});
std::string generate(const List &list, const Paths &paths = {});
// Whether saving would change anything: generate(list) differs from the file
// (or, without a file, from the defaults).
bool dirty(const List &list, const List &original, const Paths &paths = {});

// A new app entry.
Entry makeApp(std::string title, std::string program, std::string args = {}, std::string image = {});
Entry makeSeparator();

// The title to show: the plain title, or what titleExpr gives (loc.paint ->
// `loc` entry, else readable English).
std::string displayTitle(const Entry &e, const std::map<std::string, std::string> &loc = {});

// Which of the shipped apps an entry is ("paint", "edge", "calculator",
// "regedit"), "" for others.
std::string builtinOf(const Entry &e);

// --- Finding apps (read-only) ------------------------------------------------

struct Installed
{
    std::string name;     // "Notepad++"
    std::string program;  // what cmd= gets: a full path to the .exe or the .lnk, or a command
    std::string args;
    std::string image;    // "" = the program's own icon, else a raw image value ('C:\x\app.exe')
    std::string icon;     // the file the page draws the icon from
    std::string group;    // "windows" (Windows' own apps) or "installed"
    std::string detail;   // the path to show
    std::string builtin;  // one of taskbar.nss's four apps (builtinOf()): added as that file has it
};

// The shipped entry for a builtinOf() id, as defaults() has it.
std::optional<Entry> builtinEntry(std::string_view id, const Paths &paths = {});

// Windows' own apps worth a click: the four shipped ones, Notepad, File Explorer,
// Command Prompt, PowerShell, Control Panel... those found on this PC.
std::vector<Installed> windowsApps();
// The Start menu's shortcuts to programs (both the user's and everyone's), plus
// the editors and terminals the Open with page finds, by name; no uninstallers,
// help files or web links. Slow-ish: call it off the UI thread (COM initialized).
std::vector<Installed> installedApps();
// A picked file as an app: a shortcut gives its own name and icon; a program its
// description ("Visual Studio Code") or file name.
Installed fromFile(const std::string &path);

// Where a shortcut points, or "" (COM initialized).
std::string shortcutTarget(const std::string &lnk, std::string *iconLocation = nullptr);

// Whether a name from the Start menu is something other than an app
// ("Uninstall X", "X Help", "Readme", "X Website").
bool isNoise(std::string_view name, std::string_view target);

} // namespace taskbarapps
