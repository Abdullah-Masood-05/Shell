#pragma once

// After an update: a file that came with Shell which the user changed is kept as it is
// (src/shared/ConfigUpgrade.h, "modified_kept"). Before 2.3.0-beta.6 the Menu map made
// such changes itself: vis=vis.remove on an item's line, a new title=, image=, menu= or
// pos=. Those can live in imports/settings-app-map.nss instead (ShellOverrides.h), so
// the file can be the new version and the changes still apply.
//
// carry() compares the user's copy with the version it started from: this version's
// file, or one shipped earlier (ShippedBases.inc). When everything that differs is
// such a property of a menu or item (and, in shell.nss, import lines the user added or
// turned off), the result is "complete": the new version's text (with those import
// lines) and the override lines that keep the rest. Anything else (a new item, a
// changed command, a comment, an edited settings block) makes it incomplete: the app
// then leaves the file and offers Compare / Use the new version, where the same
// result, computed best-effort (`partial`), keeps what it can.

#include "MapRules.h"
#include "ShellOverrides.h"

#include "nss/Text.h"

#include <string>
#include <string_view>
#include <vector>

namespace carryover {

// Earlier shipped texts of `rel` ('/' separated), from ShippedBases.inc.
std::vector<std::string_view> shippedBases(std::string_view rel);

struct Result
{
    bool complete = false;       // every change found a place: nothing of the user's is lost
    std::string why;             // when not: the first change that didn't (English, for logs and tests)
    std::string text;            // the file to write: the new version, UTF-8
    nss::TextFormat format;      // the new version's encoding and line ends
    std::vector<overrides::Override> overrides; // for settings-app-map.nss
    size_t carried = 0;          // properties and import lines kept
    size_t failed = 0;           // changes that couldn't be kept
};

// `user` and `shipped` are file bytes; `bases` the earlier shipped texts the user's
// copy may have started from (the shipped file itself is tried first). With
// `partial`, the closest base is used and the menus and items are paired as a line
// diff pairs lines: what pairs carries over, the rest is the new version's.
Result carry(std::string_view rel, std::string_view user, std::string_view shipped, const std::vector<std::string_view> &bases,
             bool partial = false);

// Puts the properties of `list` on their items' lines in `lines`: these win over a
// value already there.
void addOverrides(maprules::Rules &lines, const std::vector<overrides::Override> &list);

} // namespace carryover
