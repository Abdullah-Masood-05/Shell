#pragma once

// The "Open with" page's model, free of UI code:
//  - presets for common editors and terminals, and where each one is usually
//    installed (per-user, per-machine, PATH), found read-only with detect();
//  - the apps the user picked (presets and custom ones) and how the menu
//    looks (top-level "Open in <Name>" items at the top of the menu, or an
//    "Open in" submenu there; and where it shows);
//  - generate(): the app-owned imports/settings-app-openwith.nss, and parse()
//    to read it back (parse(generate(m)) == m for any model the page makes);
//  - argument templates ({path}, {paths}, {dir}, {name}) and the command line
//    an item would run, for the page's Test button;
//  - the Develop menu's "Editors" submenu in develop.nss, turned off and on
//    by commenting it out, like the Context Menus page does with rules.

#include "nss/NssDocument.h"
#include "nss/Text.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openwith {

// Where detection looks. current() reads this process's environment; tests
// point every folder at a temporary directory.
struct Env
{
    std::wstring localAppData;    // %LOCALAPPDATA%    user.localappdata
    std::wstring programFiles;    // %ProgramFiles%    sys.prog
    std::wstring programFilesX86; // %ProgramFiles(x86)%  sys.prog32
    std::wstring windir;          // %WINDIR%          sys.dir
    std::wstring userProfile;     // %USERPROFILE%     user.home
    std::vector<std::wstring> path; // the PATH folders, in order

    static Env current();
};

// Which selections an app's item shows for, within the menu's locations.
enum class Scope
{
    All,        // everything the menu shows on
    Files,      // files only
    Folders,    // folders, folder background and the desktop
    Extensions, // files with one of `extensions`
};

struct App
{
    std::string id;         // a preset id, or "custom"
    std::string name;       // "Zed"
    std::string exe;        // absolute path; "" when not found
    std::string args;       // template, e.g. "{path}"
    std::string icon;       // "" = the program's own icon; else a file path or '@...' text
    Scope scope = Scope::All;
    std::string extensions; // ".py .js" for Scope::Extensions
    bool admin = false;
    bool enabled = false;

    bool operator==(const App &) const = default;
};

struct Options
{
    bool submenu = false;          // true: the items in an "Open in" submenu; false: top-level "Open in <Name>" items
    std::string title = "Open in"; // the submenu's title
    bool files = true;
    bool folders = true;
    bool background = true;        // folder background
    bool desktop = false;

    bool operator==(const Options &) const = default;
};

struct Model
{
    Options options;
    std::vector<App> apps;

    bool operator==(const Model &) const = default;
};

// --- Presets and detection --------------------------------------------------

struct Preset
{
    std::string id;
    std::string name;
    std::string args;
    std::string icon;                 // usually "" (the program's icon)
    Scope scope = Scope::All;
    bool suggested = true;            // turned on the first time when found
    std::vector<std::string> candidates; // "%LOCALAPPDATA%\\Programs\\Zed\\Zed.exe"; '*' matches one folder level
    struct Probe
    {
        std::string name;             // looked up on PATH: "code.cmd"
        std::string relative;         // from the folder it was found in: "..\\Code.exe"; "" = the file itself
    };
    std::vector<Probe> probes;
};

const std::vector<Preset> &presets();
const Preset *findPreset(std::string_view id);

// The installed program for a preset, or "" (UTF-8, absolute). Read-only.
std::string detect(const Preset &preset, const Env &env);
// Every preset as an App: found ones carry their path; `suggested` and found
// ones are enabled. A Toolbox script (.cmd) gets the IDE it starts as its icon.
std::vector<App> detectAll(const Env &env);

// A PATH lookup like where.exe: the first folder with `name` (PATHEXT is tried
// when `name` has no extension). "" when not found.
std::wstring findOnPath(std::wstring_view name, const std::vector<std::wstring> &path);
// A regular file (also an app execution alias such as WindowsApps\wt.exe).
bool fileExists(const std::wstring &path);

// The model the page starts from: the saved apps in their order, then the
// presets the file doesn't mention (off, with their detected path; found ones
// first). With no saved file, found presets are on and listed first.
Model merge(const std::optional<Model> &saved, const std::vector<App> &detected);

// --- Paths in .nss ----------------------------------------------------------

// "C:\Users\me\AppData\Local\Programs\Zed\Zed.exe" -> "@user.localappdata\Programs\Zed\Zed.exe"
std::string toNssPath(std::string_view absolute, const Env &env);
// The reverse; other text comes back unchanged.
std::string fromNssPath(std::string_view nssPath, const Env &env);

// --- Argument templates -------------------------------------------------------

// {path} -> @sel.path.quote, {paths} -> @sel(true), {dir} -> "@sel.dir", {name} -> @sel.name
std::string argsToNss(std::string_view tmpl);
std::string nssToArgs(std::string_view nss);
bool usesAllSelected(std::string_view tmpl); // {paths}: the item takes several items

// What an item runs for a sample selection: "\"C:\\...\\Zed.exe\" \"C:\\Users\\me\\notes.txt\"".
struct Sample
{
    std::vector<std::string> paths; // selected items (one, or several for {paths})
    std::string dir;                // the folder the selection is in, or the folder itself
};
std::string expandArgs(std::string_view tmpl, const Sample &sample);
std::string commandLine(const App &app, const Sample &sample);
// Problems that keep an app out of the menu, or "" (the page shows it).
std::string validate(const App &app);

// --- The generated file ------------------------------------------------------

constexpr std::string_view kHeader = "// generated by Nilesoft Shell Pro, do not edit by hand";
constexpr std::string_view kRelativePath = "imports/settings-app-openwith.nss";

// The `type` for the menu's locations ("file|dir|back"), "" when none.
std::string locationType(const Options &options);
// Where the generated items (or the submenu) go: the top of the menu, with the
// "Open", "Open in new window" group (Shell's `pos=top`, in file order).
constexpr std::string_view kPosition = "top";

// The item title as shown: "Zed" in the submenu, "Open in Zed" at the top level.
std::string itemTitle(const Options &options, const App &app);
// Whether an app's item shows for a selection kind ("file", "dir", "back", "desktop").
bool showsFor(const Options &options, const App &app, std::string_view kind, std::string_view extension = {});

std::string generate(const Model &model, const Env &env, nss::Eol eol = nss::Eol::CRLF);

struct ParseResult
{
    Model model;
    std::vector<std::string> warnings; // lines that weren't understood (replaced on save)
    // Written by an older version of the page (no `pos=top` on the options
    // line): an "Open in" submenu in the middle of the menu, or "Open with
    // <Name>" items. It reads back in today's default layout (top-level items
    // at the top) and is rewritten on the next save.
    bool legacy = false;
};
ParseResult parse(std::string_view text, const Env &env);

// --- develop.nss: the Develop > Editors submenu --------------------------------

struct EditorsMenu
{
    bool found = false;
    bool enabled = false;   // shown (not commented out)
    size_t firstLine = 0;   // 1-based
    size_t lastLine = 0;
};
EditorsMenu findEditorsMenu(const nss::NssDocument &doc);
// Comments the submenu out ("// " on each line) or back in; off-then-on gives
// back the original text. False when it isn't there or can't be toggled.
bool setEditorsMenuEnabled(nss::NssDocument &doc, bool enabled);

} // namespace openwith
