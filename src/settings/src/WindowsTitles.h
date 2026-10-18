#pragma once

// The titles of Windows' own menu items, by the names a config uses for them
// (id.copy_as_path, title.more_options), for the Context Menus page's rules.
//
// shell.dll reads each one from a string or menu resource of a Windows binary
// (src/dll/src/Initializer.cpp, Initializer::load_mui); the table here repeats
// the entries a rule is likely to name, with the same module and resource ids,
// so the app shows the title the menu shows. Not every entry of shell.dll's
// table is here: the rest fall back to readable English.

#include <optional>
#include <string>
#include <string_view>

namespace wintitles {

// The English title of a known name ("copy_as_path" -> "Copy as path"), or nullptr.
const char *englishTitle(std::string_view name);

// The title as Windows shows it (in the Windows display language), accelerators
// removed. nullopt: not a known name, or the resource couldn't be read. "": known,
// but Windows 7 only, so no such item on this system.
std::optional<std::string> systemTitle(std::string_view name);

// The title for text in the app's current language: systemTitle() when Windows shows
// its menus in that language, otherwise the app's translation of the English title
// (so a Chinese sentence names "复制为路径" on an English Windows too).
std::optional<std::string> displayTitle(std::string_view name);

// "复制为路径(&A)..." -> "复制为路径"; "Copy &as path" -> "Copy as path"; "A && B" -> "A & B".
std::string stripAccelerators(std::string_view title);

} // namespace wintitles
