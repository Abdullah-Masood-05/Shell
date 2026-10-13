#pragma once

// settings.exclude{} as the Rules & Filters page edits it. No Slint here, so it
// is unit-tested (tests/test_rules.cpp).
//
// The core (ContextMenu::is_excluded) hides the menu when
//   exclude.where   evaluates to true,
//   exclude.process is, or lists, the host process name (the file name without
//                   ".exe", Path::Title of the process path),
//   exclude.window  is, or lists, the class name of the root owner window.
// process and window take 'name' or ['a', 'b']; anything else (a function call,
// a variable) is an expression, which the page shows read-only.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rules {

// The condition behind the "Only in File Explorer" switch.
inline constexpr std::string_view kExplorerOnly = "!process.is_explorer";

// The names a process/window value lists; nullopt when it isn't a plain string or
// an array of plain strings.
std::optional<std::vector<std::string>> parseNames(std::string_view literal);

// 'a' or ['a', 'b'] (double-quoted strings); "" for an empty list.
std::string formatNames(const std::vector<std::string> &names);

// "C:\Windows\notepad.exe" -> "notepad"; trims, drops a path and a trailing .exe.
std::string processName(std::string_view input);
// A window class name, trimmed.
std::string windowClass(std::string_view input);

// Adds `name` unless it is empty or already listed (case-insensitive). Returns
// whether the list changed.
bool addName(std::vector<std::string> &names, const std::string &name);

enum class Where
{
    Off,          // unset, false or 0: no condition
    ExplorerOnly, // !process.is_explorer
    Custom,       // any other value
};
Where classifyWhere(std::string_view text);

} // namespace rules
