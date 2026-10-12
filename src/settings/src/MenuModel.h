#pragma once

// The Menu Builder's model, free of UI code: a tree of menu / item / separator
// entries, the edits the page offers (add, delete, duplicate, move, indent,
// outdent), the "where" presets, and reading an existing
// imports/settings-app-menus.nss back into a tree. MenuWriter.h turns a tree
// into .nss text.

#include "nss/NssDocument.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace menus {

enum class Kind
{
    Menu,
    Item,
    Separator,
};

// Where an entry shows; a friendly front for the type / find / where properties.
enum class Location
{
    Everywhere, // no type, find or where
    Files,      // type='file'
    Folders,    // type='dir'
    Background, // type='back'
    Desktop,    // type='desktop'
    Drives,     // type='drive'
    Extensions, // type='file' find='.txt|.md'
    Custom,     // type / find / where as written
};

struct Prop
{
    std::string name;  // lower-case
    std::string value; // raw .nss text; empty for a flag such as `admin`

    bool operator==(const Prop &) const = default;
};

struct Node
{
    Kind kind = Kind::Item;

    // Values as the user sees them; MenuWriter quotes them.
    std::string title;   // a plain string
    std::string image;   // raw: "" none, "" (a Nilesoft.Shell glyph), icon.copy, #ff0000, ...
    std::string type;    // e.g. "file|dir"
    std::string find;    // e.g. ".txt|.md"
    std::string where;   // raw expression
    std::string mode;    // "" (single, the default) | single | multiple | multi_unique | multi_single | none
    std::string cmd;     // interpolated string ('...'), so @sel.path works
    std::string args;    // interpolated string
    bool admin = false;
    std::string window;  // "" | hidden | minimized | maximized
    std::string keys;    // shortcut text shown on the right
    std::string tip;
    std::string sep;     // "" | before | after | both

    std::vector<Prop> extra; // anything else, kept verbatim and written last
    std::vector<Node> children; // menus only

    bool operator==(const Node &) const = default;
};

using Tree = std::vector<Node>;
using Path = std::vector<size_t>; // child indexes from the root

// --- Locations ------------------------------------------------------------

Location locationOf(const Node &n);
// Sets type/find/where for a preset; Custom keeps them as they are.
void setLocation(Node &n, Location loc, std::string_view extensions = {});
// ".txt .md, py" -> ".txt|.md|.py"; "" when nothing usable.
std::string extensionsToFind(std::string_view text);
// ".txt|.md" -> ".txt .md"
std::string findToExtensions(std::string_view find);
const char *locationLabel(Location loc);

// --- Tree navigation --------------------------------------------------------

struct Row
{
    Path path;
    int depth = 0;
    const Node *node = nullptr;
};

// Depth-first, every node (menus are always expanded in the builder).
std::vector<Row> flatten(const Tree &tree);
Node *nodeAt(Tree &tree, const Path &path);
const Node *nodeAt(const Tree &tree, const Path &path);
// Index of `path` in flatten(tree), or -1.
int rowOf(const Tree &tree, const Path &path);

// --- Edits ------------------------------------------------------------------
// Each returns the path of the entry to select afterwards, or nullopt when the
// edit does not apply (and then the tree is unchanged).

// Adds after `at` as a sibling; into `at` (last child) when it is a menu;
// at the end of the root when `at` is empty.
std::optional<Path> add(Tree &tree, const Path &at, Node node);
std::optional<Path> remove(Tree &tree, const Path &at);  // selects the next sibling, else the previous, else the parent
std::optional<Path> duplicate(Tree &tree, const Path &at);
std::optional<Path> moveUp(Tree &tree, const Path &at);
std::optional<Path> moveDown(Tree &tree, const Path &at);
std::optional<Path> indent(Tree &tree, const Path &at);  // into the previous sibling menu, as its last child
std::optional<Path> outdent(Tree &tree, const Path &at); // after its parent menu

Node makeNode(Kind kind);

// --- Reading ------------------------------------------------------------------

struct ParseResult
{
    Tree tree;
    std::vector<std::string> warnings; // statements that were not menu entries (dropped)
};

ParseResult parse(const nss::NssDocument &doc);
ParseResult parse(std::string_view text);

// Unquotes a string value: '...', `...` as written; "..." with escapes decoded.
// Anything else (an expression) comes back unchanged with `wasString` false.
std::string unquote(std::string_view raw, bool *wasString = nullptr);

// A short description of an entry for the tree ("Files · multiple").
std::string summary(const Node &n);

} // namespace menus
