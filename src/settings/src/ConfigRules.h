#pragma once

// The Context Menus page's model, free of UI code:
//  - modules: the root shell.nss `import` lines (on or off), described;
//  - rules: modify(...) / remove(...) declarations in any file, on or off,
//    with a readable summary of what each one does.
// Turning things off comments them out ("// "), turning them back on removes
// exactly that, so off-then-on gives back the original text.

#include "nss/NssDocument.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rules {

struct Module
{
    size_t importIndex = 0;   // into NssDocument::imports()
    size_t line = 0;
    std::string path;         // the import path as evaluated ("imports/goto.nss"), or the expression
    std::string name;         // file stem ("goto")
    std::string title;        // "Go to"
    std::string description;
    bool enabled = true;
    bool locked = false;      // can't be toggled here (see lockReason)
    std::string lockReason;
    bool own = false;         // not a file Shell ships or the app writes: the user's own, can be removed
};

// Config imports at the top level of the root (not `import lang`, not
// imports/settings-app.nss).
std::vector<Module> listModules(const nss::NssDocument &root);
bool setModuleEnabled(nss::NssDocument &root, size_t importIndex, bool enabled);

// Whether the root imports `path` ("imports/mine.nss"; either slash, any case), on or off.
bool hasModule(const nss::NssDocument &root, std::string_view path);
// Adds `import '<path>'` (relative to the root's folder) on its own line: before the
// app's own imports at the end (imports/settings-app*.nss, so settings-app.nss stays
// last), else at the end. False when it's already imported.
bool addModule(nss::NssDocument &root, std::string_view path);
// Removes the import line (on or off). False when it shares its line with something else.
bool removeModule(nss::NssDocument &root, size_t importIndex);

enum class RuleKind
{
    Modify,
    Remove,
};

struct Rule
{
    RuleKind kind = RuleKind::Modify;
    bool enabled = true;
    size_t line = 0;          // 1-based first line
    size_t lineCount = 1;
    std::string text;         // the declaration as written (enabled form)
    std::string summary;      // one sentence: "Moves “Copy as path” into “File manage”"
    std::string where;        // the raw where expression, when there is one
    bool complex = false;     // the summary can't say exactly what it does
    bool toggleable = true;   // can be turned on/off with a line comment
    std::string note;         // why it can't be toggled
};

// How summaries name menus and Windows items. Left empty, they use readable English
// made from the names in the config (id.copy_as_path -> "Copy as path").
struct Names
{
    std::map<std::string, std::string> loc;        // loc.<key> -> title in the UI language
    std::map<std::string, std::string> locEnglish; // loc.<key> -> title in en.nss
    // id.<name> / title.<name> (lower case) -> the item's title; nullopt: not known,
    // "": no such item on this Windows (left out of the summary).
    std::function<std::optional<std::string>(std::string_view)> windows;
};

// The name = value lines of an `import lang` file.
std::map<std::string, std::string> readLangFile(const std::filesystem::path &path);

// loc.* titles from the first folder that has the file: en.nss, then <language>.nss
// (or its base language) on top. `language` is a code like "zh-CN"; "" is English.
Names langNames(const std::vector<std::filesystem::path> &langDirs, std::string_view language);

std::vector<Rule> listRules(const nss::NssDocument &doc, const Names &names = {});
// Finds the rule again by line and text (the document may have been edited).
bool setRuleEnabled(nss::NssDocument &doc, const Rule &rule, bool enabled);

// id.copy_as_path -> "Copy as path"; title.more_options -> "More options".
std::string humanize(std::string_view name);

} // namespace rules
