#pragma once

// The Context Menus page's model, free of UI code:
//  - modules: the root shell.nss `import` lines (on or off), described;
//  - rules: modify(...) / remove(...) declarations in any file, on or off,
//    with a readable summary of what each one does.
// Turning things off comments them out ("// "), turning them back on removes
// exactly that, so off-then-on gives back the original text.

#include "nss/NssDocument.h"

#include <filesystem>
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
};

// Config imports at the top level of the root (not `import lang`, not
// imports/settings-app.nss).
std::vector<Module> listModules(const nss::NssDocument &root);
bool setModuleEnabled(nss::NssDocument &root, size_t importIndex, bool enabled);

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
    std::string action;       // "Hide", "Move to “File manage”", "Rename to …"
    std::string target;       // what it applies to
    std::string scope;        // "multiple selection · recycle bin", ...
    std::string where;        // the raw where expression, when there is one
    bool complex = false;     // the summary can't say exactly what it does
    bool toggleable = true;   // can be turned on/off with a line comment
    std::string note;         // why it can't be toggled
};

std::vector<Rule> listRules(const nss::NssDocument &doc);
// Finds the rule again by line and text (the document may have been edited).
bool setRuleEnabled(nss::NssDocument &doc, const Rule &rule, bool enabled);

// id.copy_as_path -> "Copy as path"; title.more_options -> "More options".
std::string humanize(std::string_view name);

} // namespace rules
