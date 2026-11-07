#pragma once

// The Menu map's changes to Shell's own items in the files that came with Shell
// (imports/terminal.nss, goto.nss, ...): override(...) lines in the app-owned
// imports/settings-app-map.nss (MapRules.h), not edits of those files. So they stay
// as shipped and updates replace them (src/shared/ConfigUpgrade.h), and the changes
// still apply to the new version.
//
//   override(file='imports/terminal.nss' item=title.terminal vis=vis.remove)
//   override(file='imports/goto.nss' item=loc.personalization index=2 title='Personal')
//
// shell.dll reads such a line as if the properties were written on the item's own
// line (src/dll/src/Parser/Parser.h, OverrideRef): the item is the index-th menu or
// item (1-based, in the order they are read: a menu before the items in it) of `file`
// whose title= is written as `item`. Titles are compared as sourceKey() gives them.
//
// One line per item; the Menu map keeps it that way. Lines the user turned off
// ("// override(...)") or wrote by hand some other way stay as they are.

#include "MapRules.h"

#include "nss/ConfigStore.h"
#include "nss/NssDocument.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace overrides {

// The text of an expression as written, for matching (Parser::source_key): comments
// left out, each run of blanks and line breaks outside quotes made one space, trimmed.
std::string sourceKey(std::string_view text);

struct Key
{
    std::string file; // relative to the root's folder, '/' separated: "imports/terminal.nss"
    std::string item; // sourceKey() of its title= as written: "title.terminal", "'Copy path'"
    int index = 1;

    // The file in any case, either slash.
    bool operator==(const Key &other) const;
};

struct Override
{
    Key key;
    // The properties it sets, by canonical() name, with their values as written.
    std::vector<std::pair<std::string, std::string>> props;

    const std::string *find(std::string_view name) const;
};

// vis/visibility -> "vis", title/text -> "title", image/icon -> "image", image.sel ->
// "image.sel", menu/parent/sub/move -> "menu", pos/position -> "pos", sep/separator ->
// "sep", "tip", "keys"; "" for the others (override() doesn't take them).
std::string canonical(std::string_view name);

// One line of settings-app-map.nss: the override it is, or nullopt (another rule, a
// comment, a line turned off, or one without file= and item=).
std::optional<Override> parse(std::string_view line);
// The line for `o`: override(file='...' item=... [index=N] name=value ...).
std::string format(const Override &o);

// The menu and item declarations of `doc` in the order shell.dll reads them.
std::vector<const nss::Node *> declarations(const nss::NssDocument &doc);
// sourceKey() of the declaration's title= (or text=), "" without one.
std::string titleText(const nss::NssDocument &doc, const nss::Node &decl);
// The key that names `decl` in `doc`, the file at `rel`. nullopt for a declaration
// that can't be named (no title=, or a separator).
std::optional<Key> keyOf(const nss::NssDocument &doc, std::string_view rel, const nss::Node &decl);
// The declaration `key` names in `doc` (its file), or null.
const nss::Node *find(const nss::NssDocument &doc, const Key &key);

// The value the lines give property `name` of `key`'s item (the last one), if any.
std::optional<std::string> get(const maprules::Rules &lines, const Key &key, std::string_view name);
// Whether a line changes `key`'s item.
bool has(const maprules::Rules &lines, const Key &key);
// Sets property `name` of `key`'s item to `value`, or takes it out (nullopt): the
// first line for the item gets it and the other ones lose it; a line left with no
// property goes; without a line, one is added at the end. True when the lines changed.
bool set(maprules::Rules &lines, const Key &key, std::string_view name, const std::optional<std::string> &value);

// `rel` (relative to the root's folder) is a file that came with Shell and that
// updates replace (src/shared/ShippedConfigHashes.inc), not one of the app's own.
bool isShipped(std::string_view rel);

// What saving a change of settings-app-map.nss writes: the file with `edit` applied
// to its lines, and the root when it doesn't import the file yet (MapRules.h). Not
// ok (with `error`) when the file can't be read or the edit fails; no writes when
// nothing changes.
struct MapWrite
{
    bool ok = false;
    std::string error;
    std::vector<nss::FileWrite> writes;
};
MapWrite planMapWrite(const std::filesystem::path &root,
                      const std::function<bool(maprules::Rules &lines, std::string &error)> &edit);

} // namespace overrides
