#pragma once

// The language of the right-click menu, set from Preferences > Language.
//
// shell.nss loads imports/lang/en.nss and then the file that matches Windows' language
// (sys.lang). A later `import lang` replaces the strings of an earlier one key by key
// (Parser.cpp, Scope::set), so the app keeps imports/settings-app-lang.nss, imported
// at the end of shell.nss, with the file of the language the user picked:
//
//   import lang 'lang/en.nss'      <- English first: a string the language lacks is English
//   import lang 'lang/es-ES.nss'   <- then the language (the path is relative to imports/)
//
// Auto removes the file and its import, and the menu follows Windows as before.
// shell.dll watches every imported file, so the next right-click picks the change up.
// Free of UI code.

#include "nss/Text.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace menulang {

inline constexpr std::string_view kRelativePath = "imports/settings-app-lang.nss";
inline constexpr std::string_view kFileName = "settings-app-lang.nss";

// The names (without .nss) of the menu's language files in `langDir`, sorted.
std::vector<std::string> menuFiles(const std::filesystem::path &langDir);

// The menu language file for the app's Language setting ("" = Auto, "en", or a bundled
// UI language such as "es" or "zh-TW"): the exact name first, then the base language
// ("es" -> "es-ES"). "" when the setting is Auto or no file fits; the menu then
// follows Windows.
std::string pick(std::string_view setting, const std::vector<std::string> &files);

// The text of the file for a menu language file ("es-ES"): English first, then the
// language (English alone for "en").
std::string generate(std::string_view file, nss::Eol eol = nss::Eol::CRLF);

// The language file an existing settings-app-lang.nss ends with ("es-ES"), "" for none.
std::string read(std::string_view text);

struct Result
{
    bool ok = true;
    bool changed = false;   // a file was written or removed
    std::string error;
    std::string snapshotId; // the backup taken first (Backups page)
};

// Makes the menu follow the Language setting: writes or removes the file and its import
// in the root shell.nss. Does nothing (changed = false) when they already say the same.
// A snapshot is taken first, with a reason such as "Language: menu in Spanish".
// Only for a config the app may write (the personal one); the caller checks that.
Result apply(const std::filesystem::path &root, std::string_view setting);

// "es-ES" -> "Spanish": the language's English name, for snapshot reasons.
std::string englishName(std::string_view file);

} // namespace menulang
