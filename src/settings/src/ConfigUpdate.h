#pragma once

// After an update, the app brings the personal config's menu files up to date with
// `shell.exe -config:upgrade` (src/shared/ConfigUpgrade.h). This is the part without UI:
// when to run it, and reading what it printed. ConfigUpdateController.h shows the result.

#include "nss/ConfigStore.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace config_update {

// HKCU\Software\Nilesoft\Shell\Settings, REG_SZ: the app version the personal config was
// last brought up to. Absent before the first run of a version that has this.
inline constexpr const wchar_t *versionValue = L"ConfigUpgradeVersion";

// Run when this version hasn't yet (a new install, an MSI or portable update, the in-app
// updater), or when the update helper just installed a build (`forced`).
bool shouldRun(std::string_view lastVersion, std::string_view currentVersion, bool forced);

// Only a personal config (in %APPDATA%, or the HKCU "config" file) is upgraded; the
// shared one in the install folder is the shipped files already.
bool isPersonal(std::string_view configSource, bool configExists);

// What `shell.exe -config:upgrade [-take:<file>]` printed.
struct Report
{
    bool ok = false;
    std::string configDir;  // empty: no personal config
    std::string installDir;
    std::string backup;     // snapshot id, empty when nothing was written
    std::vector<std::string> updated, added, modifiedKept, unchanged, failed;
    std::string error;

    size_t changed() const { return updated.size() + added.size(); }
};

std::optional<Report> parseReport(std::string_view json);

// The same upgrade without shell.exe, for a per-user copy of the app (AppChannel.h): its
// folder has this version's shell.nss and imports\, newer than the install folder's, and
// the app knows every earlier shipped version of them (src/shared/ShippedConfigHashes.inc
// as of this build). `configRoot` is the active personal root file (`shell.exe -status`
// configPath). The report is the one `shell.exe -config:upgrade [-take:]` prints.
Report runLocal(const std::wstring &shippedDir, const std::wstring &configRoot, const std::vector<std::string> &take = {});

// Files the upgrade kept because the user changed them (Report::modifiedKept), brought
// to the new version with the user's Menu map changes kept (CarryOver.h): the new
// version of each file, and override lines in imports/settings-app-map.nss for the
// rest. Without `partial`, only files whose every change can be kept that way; with it
// (the app's "Use the new version"), each of `files`, keeping what can be kept. One
// save for all of them: checked with `validator` when given, a snapshot of the config
// first (reasonCarry / reasonTake), nothing written when that fails.
struct Carried
{
    bool ok = true;                  // false: the save failed (`error`), nothing was written
    std::string error;
    std::string snapshot;            // the snapshot taken first
    std::vector<std::string> files;  // brought to the new version (as named in `files`)
    size_t kept = 0;                 // the user's changes kept (properties and import lines)
};
inline constexpr const char *reasonCarry = "Before keeping your Menu map changes in the new version";
Carried carryOver(const std::filesystem::path &configRoot, const std::filesystem::path &installDir,
                  const std::vector<std::string> &files, bool partial, const nss::Validator &validator = {});

// The folder of shipped files the app upgrades from itself (runLocal), or "" when
// `shell.exe -config:upgrade` does it: a per-user copy of the app with shell.nss next to it.
std::wstring localShippedDir();

} // namespace config_update
