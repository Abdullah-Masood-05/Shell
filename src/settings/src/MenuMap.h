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
#include <map>
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
    int index = 0;             // its place in the tree's shell or system list (top level)
    int seq = 0;               // its place among all of the config's items, in the order they are
                               // declared across files (0: an older shell.exe doesn't tell)
    std::string shows;         // in this menu for the sample: yes | no | maybe (by where=, find=,
                               // a vis= that depends on the selection, and its menu's);
                               // "" from an older shell.exe that doesn't tell
    bool showsSample = false;  // that depends on which file or folder the sample is
    std::string showsWhy;      // what decides it: where | find | vis | menu (its menu isn't here) |
                               // empty (nothing in the menu shows)
    std::string titleSample;   // a title that depends on the selection, as it comes out for the
                               // sample ("" when shell.exe can't work it out, or an older one)

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
    bool hasPos = false;       // it gives a pos=
};

// One item of the main menu in the order it shows: Tree::shell[index] or
// Tree::system[index].
struct OrderRef
{
    bool shell = false;
    int index = 0;
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
    // The main menu as it shows, your items and Windows' together (shell.exe works it
    // out as the menu does: pos=, the separator nearest the middle...). Your items the
    // menu leaves out aren't in it. hasOrder false: an older shell.exe without it.
    bool hasOrder = false;
    std::vector<OrderRef> order;
};

// The output of `shell.exe -config:tree`. nullopt (and `error`) when it isn't JSON
// of that shape; a config that doesn't load parses, with ok false and its errors.
std::optional<Tree> parseTree(std::string_view json, std::string *error = nullptr);

// One of Shell's items that isn't in this menu for the sample: its where=, find= or
// vis=, or those of the menu it is in, leave it out (Node::shows "no").
bool notInMenu(const Node &n);
// One of Shell's items that shows only some of the time: shows "maybe" (a condition
// on keys, the time, the window...). With an older shell.exe (no "shows"): any
// where=, find= or vis= that isn't a constant, as before.
bool showsSometimes(const Node &n);

// The main menu as it shows: tree.order resolved to its nodes, without your items
// that aren't in it, then whatever the order doesn't name (your items, then Windows',
// such as those addUnlisted adds). nullopt without an order (an older shell.exe): the
// map then lists your items and Windows' apart, as before.
struct MenuRef
{
    const Node *node = nullptr;
    size_t index = 0;   // in tree.shell or tree.system
};
std::optional<std::vector<MenuRef>> mergedMenu(const Tree &tree);

// "Open wit&h" -> "Open with" ("&&" -> "&").
std::string stripAccelerators(std::string_view title);

// Where an item of the tab's menu goes in the Menu map's list (a shell.exe that gives
// the order; without one the list stays as it was):
//  - Menu: in this menu, where it is (shows yes or maybe);
//  - Hidden: switched off (vis=vis.remove, or a rule that removes it);
//  - Out: not in this menu: its where=, find= or vis= leaves it out, it names a menu that
//    isn't here, or it is a title typed on the page that this menu doesn't have;
//  - Moved: one of Windows' items that a rule sends to another menu, which then lists it.
enum class Placing
{
    Menu,
    Hidden,
    Out,
    Moved,
};
Placing placingOf(const Node &n);

// A menu path as menu= and rules compare it: lower case, accelerators and the '/' around
// it left out ("/Pin/Unpin/" and "pin/unpin" are the same).
std::string menuPathKey(std::string_view path);

// The separators of a menu's items that the real menu keeps: none first or last, never two
// in a row. `separator[i]` says whether item i is one; the result is the indexes kept.
std::vector<size_t> collapseSeparators(const std::vector<bool> &separator);

// The titles of a menu's first items that have one (a title that depends on the selection
// counts when shell.exe worked it out), at most `max`. `more` says there were others.
std::vector<std::string> childTitles(const Node &menu, size_t max, bool *more = nullptr);

// --- Rules for Windows' items (one line each) ---

// The properties that pick out `n` in a rule: where=this.id==id.<id> when Shell
// knows the item, else find="\"<name>\"" (the exact title); plus in="<path>" for
// an item in a submenu. "" when the item can't be named this way (no title, or
// one find= can't hold, such as a title with " or |).
std::string matchProps(const Node &n);
std::string hideRule(const Node &n);                                   // remove(...)
std::string renameRule(const Node &n, std::string_view title);         // modify(... title="...")
std::string moveRule(const Node &n, std::string_view menuPath);        // modify(... menu="...")
// modify(... pos=<value>): `value` as posValue() or quoteString("top") gives it.
std::string posRule(const Node &n, std::string_view value);
// True for a map-file line that is only modify(<props> pos=<value>): one the app wrote for the
// item with these matchProps, not a hand-written rule that sets more.
bool isOwnPosLine(const std::string &props, std::string_view line);
// The value a rule made by renameRule or moveRule gives: prop "title" or "menu" of
// `rule` when it is modify(<matchProps(n)> <prop>="..."), else nullopt.
std::optional<std::string> ruleProp(const Node &n, std::string_view rule, std::string_view prop);
// modify(... image=<value>): `value` as imageValue() gives it; "null" takes the icon away.
std::string imageRule(const Node &n, std::string_view value);
// The image= of a rule written on one line, such as imageRule() makes: nullopt without one.
std::optional<std::string> ruleImage(std::string_view rule);
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

// The declaration node at `line` (and `column`, 1-based in characters, when several
// start on that line), or null.
const nss::Node *findDeclaration(const nss::NssDocument &doc, size_t line, size_t column = 0);

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

// --- Icons ---
//
// The Menu map's Icon field holds what goes in image=: a file (C:\Icons\a.ico, or a
// program's icon C:\app.exe,0) typed as it is, or an expression such as
// image.mdl(\uE8C8) or icon.copy.

// The value of image= for what was typed: a file path in quotes (image.svgf() for an
// .svg), anything else as it is. "" for nothing.
std::string imageValue(std::string_view typed);
// What the field shows for an image= value: the file of a plain quoted path, else the
// value as written. imageValue(imageText(v)) means the same as v.
std::string imageText(std::string_view value);

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
    bool rulable = false;      // one of Windows' items a rule can name: it can be placed with pos=
    bool laterInOrder = false; // has pos=N or pos=indexof() and is declared after the item being
                               // placed: not in the menu yet when that one is placed, so pos=indexof()
                               // can't name it (titled false); `sameAnchor` may still do
    bool fixed = false;        // has menu= or pos=, or was moved here: its text order
                               // doesn't decide where it shows
    bool shown = true;         // in the menu: not hidden, and not moved to another
                               // menu by menu=. Shell leaves a hidden item out, so
                               // pos=indexof() can't find it and a move past it
                               // changes nothing that can be seen.
};

// The places "Place in this menu" offers for siblings[index], between At the top and
// At the bottom: right before and right after each sibling that shows and that
// pos=indexof() can name. Not itself, separators, or hidden items: Shell leaves a
// hidden item out of the menu, so a place next to it isn't one that can be seen.
// Items that show only sometimes (where=, find=, vis=dynamic) or disabled stay:
// they are in the menu at least some of the time.
struct Place
{
    std::string title;   // without & accelerators
    bool after = false;
    std::string pos;     // the value of pos=
    size_t sibling = 0;  // its index in the siblings
};
std::vector<Place> placesNextTo(const std::vector<Sibling> &siblings, size_t index);

// The Sibling for `n`, in the menu of `self` (the item being placed; may be null).
// `shown`: not hidden, counting a hide or show waiting to be saved. For Windows'
// items, `moved` and `renamed` are the menu= and title= its rules give it, counting
// changes waiting to be saved ("" for none). What decides it, as Shell builds the menu:
// - A Windows item moved to another menu is taken out of this one before your items
//   are placed, as a hidden one is. Kept in the main menu (menu='/') it stays.
// - A Windows item renamed is found by its new title.
// - One of your items with its own pos=N or pos=indexof() is placed in the order the
//   items are declared, so one declared after `self` (or in another file) isn't in
//   the menu yet when `self` is placed: pos=indexof() can't name it (titled false).
Sibling siblingOf(const Node &n, const Node *self, bool shown, std::string_view moved, std::string_view renamed);

// How to move siblings[index] one place up or down among its siblings. Siblings
// that aren't shown are stepped over: the move goes past the next one that shows.
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

// --- The list the Menu map shows ---
//
// One entry per row: the groups ("In this menu", "Hidden", "Not in this menu") and the
// items in them, with their parent and kids.
struct ListEntry
{
    const Node *node = nullptr;          // null: a group
    int group = 0;                       // 0 your items, 1 Windows and programs in this menu,
                                         // 2 not in this menu, 4 switched off; for a group's
                                         // own entry also 3, the menu as it shows
    int depth = 0;
    int parent = -1;
    std::vector<int> kids;
    std::string key;                     // stable across reloads, for expansion and selection
    std::string menuPath;                // for "Move to": the path rules name this menu by
    std::vector<MenuStep> steps;         // Shell's menus: the path menu= names it by
    std::string from;                    // switched off or left out: the menu it was in ("" the main menu);
                                         // for one moved in: the menu it came from
    bool movedIn = false;                // one of Windows' items a rule moved into the menu it is listed in
    bool dropped = false;                // a separator the real menu collapses: not listed
};

// The key of one of Shell's items: its declaration, the same wherever menu= and pos= put it.
std::string shellKey(const std::filesystem::path &file, int line, int column);

struct MenuList
{
    std::vector<ListEntry> entries;      // the three groups first (3, 4, 2), then the items
    // Where each item went, by key: 0 in the menu, 1 switched off, 2 not in this menu.
    std::map<std::string, int> placed;
};

// The list for a shell.exe that gives the order (nullopt without one: the map then lists
// your items and Windows' apart, as before). Only what the real menu has is in "In this
// menu", at the level it shows at, in the menu's order, separators as the menu collapses
// them. Your items that are switched off, and Windows' items a rule removes, are in the
// closed "Hidden" group. What its where=, find= or vis= leaves out, what names a menu
// that isn't here, and titles typed on the page that this menu doesn't have are in "Not in
// this menu". Windows' items a rule moves into a menu are listed in that menu, after its
// own items (no such menu in this tab: in "Not in this menu").
// `pinned`: items to keep in the group they were in (key -> 0, 1 or 2), so a row doesn't
// jump when a switch is flipped.
// Whether "Move to" offers the menu of entries[target] for entries[selected]: a menu of the
// menu as it shows (not one that is switched off or isn't in this menu, nor one inside
// those), and never the item itself or a menu inside it.
bool canMoveInto(const std::vector<ListEntry> &entries, int target, int selected);

std::optional<MenuList> buildMenuList(const Tree &tree, const std::map<std::string, int> &pinned = {});

} // namespace menumap
