#pragma once

// After an update, the app brings the personal config's menu files up to date with
// `shell.exe -config:upgrade` (src/shared/ConfigUpgrade.h). This is the part without UI:
// when to run it, and reading what it printed. ConfigUpdateController.h shows the result.

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

// The folder of shipped files the app upgrades from itself (runLocal), or "" when
// `shell.exe -config:upgrade` does it: a per-user copy of the app with shell.nss next to it.
std::wstring localShippedDir();

} // namespace config_update
