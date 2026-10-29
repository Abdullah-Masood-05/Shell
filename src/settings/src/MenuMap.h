#pragma once

// The Menu map page's model, free of UI code (tests/test_menumap.cpp):
//  - the tree `shell.exe -config:tree` prints: Shell's own items with the file
//    and line of each, the Windows menu built off-screen, and the modify/remove
//    rules with the Windows items each one reaches;
//  - the rules the page writes to hide, rename or move a Windows item
//    (imports/settings-app-map.nss, see MapRules.h);
//  - one item's declaration in its .nss file, found by line and replaced
//    exactly, for "Edit code", hiding and renaming Shell's own items.

#include "nss/NssDocument.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace menumap {

enum class Origin
{
    Shell,  // the user's config
    System, // Windows or another program
};

struct Node
{
    Origin origin = Origin::System;
    std::string kind;          // menu | item | separator
    std::string title;         // as shown: accelerators removed ("Open with")
    std::string rawTitle;      // as written or as the menu has it ("Open wit&h")

    // Shell's items
    std::string state;         // text | empty | dynamic | none (no title)
    std::string titleSource;   // loc.<name> / title.<name>, or ""
    std::filesystem::path file;
    int line = 0;
    int column = 0;
    std::string vis;           // a constant vis=: hidden | disabled | static | label | enabled; dynamic; ""
    std::vector<std::string> conditions; // where, find, mode, vis, type
    bool dynamic = false;      // the title or the item list depends on the selection
    std::string menu;          // its menu= as the menu reads it ("System", "A/B", "/" the main menu)
    bool menuDynamic = false;  // menu= depends on the selection
    bool relocated = false;    // menu= put it in another menu here
    std::string movedFrom;     // the menu it is written in, as a path of titles ("" the main menu)
    std::string pos;           // its pos=: top, bottom, middle, N, before:<title>, after:<title>,
                               // indexof, dynamic, auto; "" without one

    // Windows' items
    std::string name;          // normalized title, what find= and this.name see
    std::string path;          // the submenu it is in, as in= names it ("send to"); "" at the top
    std::string id;            // id.<name> when Shell knows the item, else ""
    std::string verb;
    std::string source;        // windows | program | unknown
    std::string program;       // "7-Zip"
    std::string module;        // the DLL or EXE behind it
    std::string clsid;
    std::string handler;       // the handler's registered name
    bool disabled = false;
    bool ownerDrawn = false;
    std::vector<int> rules;    // indexes into Tree::rules that reach it
    std::vector<int> maybe;    // rules that may, depending on the selection
    bool hidden = false;       // a rule in `rules` hides it
    bool maybeHidden = false;
    std::string moved;         // the menu a rule moves it to
    std::string renamed;       // the title a rule gives it
    bool unlisted = false;     // not in the menu read here: a title typed on the page, or
                               // one a rule made here names (addUnlisted)

    std::vector<Node> children;
};

struct Rule
{
    int index = 0;
    std::filesystem::path file;
    int line = 0;
    int column = 0;
    bool applies = false;      // its type= lets it reach this context
    bool clsid = false;
    std::string find;
    bool findDynamic = false;
    std::string in;
    std::string where;         // "", item (only this.* and constants), selection
    std::string vis;
    std::string menu;
    bool menuDynamic = false;
    std::string title;
    bool titleDynamic = false;
};

struct Tree
{
    bool ok = false;
    std::vector<std::string> errors;   // "file:line: message"
    std::string context;
    std::string sample;
    std::vector<Node> shell;
    std::vector<Node> system;
    std::vector<Rule> rules;
    std::string systemError;           // why `system` is empty ("taskbar": not built there)
    // Programs' menu handlers that loaded but added nothing for this sample: some
    // add their items only inside File Explorer (Windows' own are left out).
    std::vector<std::string> quietPrograms;
};

// The output of `shell.exe -config:tree`. nullopt (and `error`) when it isn't JSON
// of that shape; a config that doesn't load parses, with ok false and its errors.
std::optional<Tree> parseTree(std::string_view json, std::string *error = nullptr);

// "Open wit&h" -> "Open with" ("&&" -> "&").
std::string stripAccelerators(std::string_view title);

// --- Rules for Windows' items (one line each) ---

// The properties that pick out `n` in a rule: where=this.id==id.<id> when Shell
// knows the item, else find="\"<name>\"" (the exact title); plus in="<path>" for
// an item in a submenu. "" when the item can't be named this way (no title, or
// one find= can't hold, such as a title with " or |).
std::string matchProps(const Node &n);
std::string hideRule(const Node &n);                                   // remove(...)
std::string renameRule(const Node &n, std::string_view title);         // modify(... title="...")
std::string moveRule(const Node &n, std::string_view menuPath);        // modify(... menu="...")
// "/" (also " / "): the main menu as menu= names it. shell.dll keeps an item that
// menu='/' put in the main menu there: the rules after it don't move it.
bool isRootPath(std::string_view menuPath);

// --- Items the map can't list ---
//
// Some programs add their items only inside File Explorer (or only for some
// files), so the menu read off-screen doesn't have them. Rules can still name such
// an item by its title.

// The name find= and this.name see for a title typed by hand: '&' and the dots at
// the end left out, spaces around trimmed ("Scan with Microsoft Defender...").
std::string itemName(std::string_view title);

// Adds to tree.system, at the end of the main menu, one unlisted item for each
// name in `typed` and for each rule of settings-app-map.nss that names an item by
// its exact title (find="\"Title\"", no in= or where=) and reaches nothing listed,
// so its rules can be seen and taken back. Names already listed in the main menu
// are skipped. Each one gets the rules that name it that way (hidden, moved,
// renamed as for a listed item).
void addUnlisted(Tree &tree, const std::vector<std::string> &typed);

// True for imports/settings-app-map.nss, the file the rules above go to.
bool isMapFile(const std::filesystem::path &file);

// --- One declaration in its file ---

struct Block
{
    nss::Span span;     // what replaceBlock() replaces
    size_t line = 0;    // 1-based line of the keyword
    std::string text;   // the declaration as written, from the start of its line when
                        // only indentation comes before it (so its indentation stays)
};

// The menu/item/separator/modify/remove declared at `line` (and `column`, 1-based
// in characters, when several start on that line).
std::optional<Block> findBlock(const nss::NssDocument &doc, size_t line, size_t column = 0);

// Replaces exactly block.span with `text`, which must be one declaration that
// parses on its own; the rest of the file is left byte for byte. False (and the
// document unchanged) otherwise, with the reason in `error`.
bool replaceBlock(nss::NssDocument &doc, const Block &block, std::string_view text, std::string *error = nullptr);

// Hides one of Shell's items by adding vis=vis.remove to its declaration, or shows
// it again by removing exactly that. An item whose own vis= is something else
// can't be switched here (false, with `error`).
bool setShellItemHidden(nss::NssDocument &doc, size_t line, size_t column, bool hidden, std::string *error = nullptr);
// True when the declaration has vis=vis.remove (what setShellItemHidden adds).
bool shellItemHidden(const nss::NssDocument &doc, size_t line, size_t column);

// Gives one of Shell's items a new title: its title= becomes "<title>" (added when
// it has none).
bool setShellItemTitle(nss::NssDocument &doc, size_t line, size_t column, std::string_view title,
                       std::string *error = nullptr);

// "where=sel.count>1  find='.txt'": the properties that decide when the item shows.
std::string conditionText(const nss::NssDocument &doc, size_t line, size_t column);

// --- Moving and ordering Shell's items ---
//
// An item goes into another menu with menu= on its own declaration, and to another
// place in its menu with pos=: the declaration stays where it is written, so taking
// the property out again puts the item back. (Both work the same as in a file
// written by hand; see docs/configuration/properties.)

// The text of a property of the declaration (menu= also under its other names
// parent=, sub= and move=; pos= also as position=), or nullopt without one.
std::optional<std::string> shellItemProp(const nss::NssDocument &doc, size_t line, size_t column, std::string_view name);

// Sets the property to `value` (an expression, as written): the value of the one
// it has is replaced, else ` name=value` goes after its last property. An empty
// `value` takes the property out, with the space before it.
bool setShellItemProp(nss::NssDocument &doc, size_t line, size_t column, std::string_view name, std::string_view value,
                      std::string *error = nullptr);

// One menu on the way to a target: its title as written and where that comes from.
struct MenuStep
{
    std::string rawTitle;      // "File &manage"
    std::string titleSource;   // "loc.file_manage", or "" for a plain title
};

// The value of menu= for a menu path: loc.tools_system, "Tools", or
// loc.develop + "/" + "Tools" for a submenu; "/" (the main menu) for an empty path.
std::string menuValue(const std::vector<MenuStep> &path);

// The value of pos= that puts an item right before (after == false) or right after
// the item titled `title`: indexof(quote(loc.x), 1), or indexof("\"Title\"", 0) for
// a plain title. "" when the title can't be matched that way.
std::string posValue(const MenuStep &sibling, bool after);

// One item of a menu as the map lists it, for planning a move up or down.
struct Sibling
{
    bool own = false;          // one of Shell's items with a file and line
    bool separator = false;
    bool titled = false;       // a plain title or loc.*: pos=indexof() can name it
    MenuStep step;
    std::filesystem::path file;
    int line = 0;
    int column = 0;
    bool fixed = false;        // has menu= or pos=, or was moved here: its text order
                               // doesn't decide where it shows
};

// How to move siblings[index] one place up or down among its siblings.
struct Reorder
{
    bool possible = false;
    // First choice: move the declaration's text right before (or after) that of
    // siblings[anchor] in the same file, when both are written there one after
    // the other's block, have no menu= or pos=, and nothing else is on their lines.
    bool text = false;
    size_t anchor = 0;
    bool after = false;
    // Else: the pos= value that puts it there.
    std::string pos;
};
Reorder planReorder(const std::vector<Sibling> &siblings, size_t index, bool up);

// Moves the declaration at (line, column) right before (or after) the one at
// (anchorLine, anchorColumn): both must be declarations of the same menu (or of
// the file's top level) and alone on their lines. Comment lines right above a
// declaration go with it. Only those lines move; every byte stays as it is.
// `newLine`: the line the declaration starts on afterwards.
bool moveDeclaration(nss::NssDocument &doc, size_t line, size_t column, size_t anchorLine, size_t anchorColumn,
                     bool after, std::string *error = nullptr, size_t *newLine = nullptr);

} // namespace menumap
