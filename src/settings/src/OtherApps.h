#pragma once

// Other apps' own right-click items (Git's "Open Git GUI here", VLC, 7-Zip...),
// read from the registry for the Open with page's "Other apps' menu items"
// list. Nothing here writes to the registry: hiding one is a remove() rule
// in the page's .nss file (openwith::Hidden).
//
//  - scan() reads the verbs under <type>\shell and the handlers under
//    <type>\shellex\ContextMenuHandlers, for files, folders, folder
//    background and drives, in HKCR and HKCU\Software\Classes;
//  - collect() turns that into one row per menu title, leaving out Windows'
//    own items and Shell's own handler. It is pure, so tests feed it fixtures.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace otherapps {

// Where an item shows, as bits.
enum Where : unsigned
{
    Files = 1,
    Folders = 2,
    Background = 4,
    Drives = 8,
};

// The registry types scanned and what each one is for.
struct Type
{
    const char *key; // "Directory\\Background"
    unsigned where;
};
const std::vector<Type> &types();

// One key under <type>\shell (UTF-8; values as stored, not resolved).
struct Verb
{
    std::string type;      // a types() key
    bool user = false;     // HKCU\Software\Classes (else HKCR)
    std::string key;       // "git_gui"
    std::string muiVerb;   // MUIVerb: "&PowerShell 7", "@C:\\x\\app.dll,-100"
    std::string text;      // the default value: "Open Git &GUI here"
    std::string command;   // command\(default)
    std::string icon;      // Icon
    bool submenu = false;  // SubCommands or ExtendedSubCommandsKey
    bool extended = false; // Extended: only with Shift held
    bool hiddenByApp = false; // ProgrammaticAccessOnly or LegacyDisable: never in the menu
};

// One key under <type>\shellex\ContextMenuHandlers.
struct Handler
{
    std::string type;
    bool user = false;
    std::string name;      // "7-Zip", or the CLSID itself
    std::string clsid;     // "{23170F69-...}"
    std::string className; // CLSID\{...}\(default), may be ""
    std::string server;    // InprocServer32\(default), environment variables expanded
};

struct Scan
{
    std::vector<Verb> verbs;
    std::vector<Handler> handlers;
};

// "@C:\\x\\app.dll,-100" -> its text, "" when it can't be read.
using Resolver = std::function<std::string(const std::string &)>;

struct Item
{
    std::string kind;    // "verb" or "handler" (openwith::Hidden::kind)
    std::string name;    // the verb's key or the handler's name (openwith::Hidden::name)
    std::string title;   // the title Shell matches: '&' marks gone, trailing dots dropped. A
                         // handler's is a guess (its name): the app sets the real one.
    bool titleKnown = true; // false for handlers
    std::string program; // the program or DLL behind it, "" when unknown
    unsigned where = 0;  // Where bits
    bool shiftOnly = false;
    std::vector<std::string> keys; // registry keys it comes from: "HKCR\\Directory\\shell\\git_gui"
};

// One row per verb title (the same title under several types or both hives is
// one item) and per handler name, sorted by title. Left out: Windows' own
// items (system32 programs and strings, Windows' verb names), Shell's own
// handler, and verbs Windows never shows.
// systemRoot: "C:\\Windows" (lower or upper case).
std::vector<Item> collect(const Scan &scan, const Resolver &resolve, std::string_view systemRoot);

// A menu title as Shell compares it: "Open Git &GUI here" -> "Open Git GUI here",
// "Save && exit" -> "Save & exit", "Add to archive..." -> "Add to archive".
std::string menuTitle(std::string_view raw);
// Whether a program path or "@module,-id" string belongs to Windows itself.
bool isWindowsFile(std::string_view path, std::string_view systemRoot);
// The program a command line runs: "\"C:\\x\\a.exe\" \"%1\"" -> "C:\\x\\a.exe".
std::string commandProgram(std::string_view command);
// "Files, folders, background", for the page.
std::string describeWhere(unsigned where);

// Reads the registry (read-only). Slow-ish: call it off the UI thread.
Scan scan();
// SHLoadIndirectString.
std::string resolveIndirect(const std::string &text);
// %SystemRoot%
std::string systemRoot();

} // namespace otherapps
