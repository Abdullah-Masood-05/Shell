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

} // namespace menumap
