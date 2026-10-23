#pragma once

// The app in the background: starting with Windows (the per-user Run entry) and the
// update checks it makes while it sits in the system tray. UI-free, so the tests can
// run it against a fake registry (startup::Store).
//
// Settings, under HKCU\Software\Nilesoft\Shell\Settings:
//   KeepInTray              DWORD, default 1: closing the window leaves the app in the tray
//   StartWithWindows        DWORD, default 1: the Run entry (only while KeepInTray is on)
//   StartupDefaultsApplied  DWORD, 1 once the app has set up the Run entry the first time,
//                           so a Run entry the user removed later is never put back
//   TrayNotifiedVersion     REG_SZ, the last version a tray notification was shown for
//
// The Run entry: HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value
// NilesoftShellPro = "<path>\NilesoftShellPro.exe" --background. Per user, no admin.

#include <windows.h>

#include <chrono>
#include <ctime>
#include <optional>
#include <string>

namespace startup {

inline constexpr const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Where Task Manager and Settings > Apps > Startup keep their on/off switch for a Run entry.
inline constexpr const wchar_t *kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
inline constexpr const wchar_t *kRunValue = L"NilesoftShellPro";
inline constexpr const wchar_t *kBackgroundArg = L"--background";

inline constexpr const wchar_t *kKeepInTray = L"KeepInTray";
inline constexpr const wchar_t *kStartWithWindows = L"StartWithWindows";
inline constexpr const wchar_t *kDefaultsApplied = L"StartupDefaultsApplied";

// The registry, as far as this file needs it. RegistryStore is the real one.
class Store
{
public:
    virtual ~Store() = default;
    // HKCU\Software\Nilesoft\Shell\Settings
    virtual std::optional<DWORD> pref(const wchar_t *name) = 0;
    virtual bool setPref(const wchar_t *name, DWORD value) = 0;
    // The Run entry
    virtual std::optional<std::wstring> runValue() = 0;
    virtual bool setRunValue(const std::wstring &command) = 0;
    virtual bool deleteRunValue() = 0;
    // The entry is switched off in Task Manager / Settings > Apps > Startup.
    virtual bool disabledInWindows() = 0;
    // Switch it back on there (drops our StartupApproved value).
    virtual bool clearDisabledInWindows() = 0;
    virtual bool fileExists(const std::wstring &path) = 0;
};

class RegistryStore final : public Store
{
public:
    std::optional<DWORD> pref(const wchar_t *name) override;
    bool setPref(const wchar_t *name, DWORD value) override;
    std::optional<std::wstring> runValue() override;
    bool setRunValue(const std::wstring &command) override;
    bool deleteRunValue() override;
    bool disabledInWindows() override;
    bool clearDisabledInWindows() override;
    bool fileExists(const std::wstring &path) override;
};

struct Prefs
{
    bool keepInTray = true;
    bool startWithWindows = true; // the setting itself
    bool disabledInWindows = false;
    // What the Preferences switch shows: on only if Windows will really start the app.
    bool startsWithWindows() const { return keepInTray && startWithWindows && !disabledInWindows; }
};

Prefs load(Store &store);

// "C:\...\NilesoftShellPro.exe" --background
std::wstring runCommand(const std::wstring &exe);
// The program of a command line: the quoted first part, or everything up to " --" / " -"
// for an unquoted one. Empty for an empty command.
std::wstring programOf(const std::wstring &command);
// The Run entry names a NilesoftShellPro.exe (this app, any copy).
bool isOurs(const std::wstring &command);

enum class Sync { Unchanged, Created, Repaired, Removed };

// Once per start (never for --snapshot):
//  - the first start of a version with this feature: create the Run entry if the
//    settings want it (both default on), and remember that it was done;
//  - the settings say no (the user turned it off, or the tray): no Run entry of ours;
//  - our Run entry names a NilesoftShellPro.exe that is gone (the folder moved): this one.
// A Run entry the user deleted after the first time stays deleted.
Sync onStartup(Store &store, const std::wstring &exe);

// The Preferences switches. Each writes the setting and adds or removes the Run entry.
void setKeepInTray(Store &store, const std::wstring &exe, bool on);
void setStartWithWindows(Store &store, const std::wstring &exe, bool on);

} // namespace startup

namespace background {

// The first check after the app starts in the background (Windows is busy signing in),
// then whenever the last one is this old. The schedule is looked at every kTick, by the
// wall clock, so a PC that slept through a check catches up when it wakes.
inline constexpr std::chrono::seconds kFirstCheck{ 90 };
inline constexpr std::chrono::seconds kInterval{ 8 * 60 * 60 };
inline constexpr std::chrono::seconds kTick{ 30 * 60 };

// No check yet, a clock that went backwards, or the last check is at least `interval` old.
bool checkDue(std::time_t now, std::time_t lastCheck, std::chrono::seconds interval = kInterval);

// A tray notification for `version`: not for the one already announced or skipped.
bool shouldNotify(const std::string &version, const std::string &lastNotified, const std::string &skipped);

} // namespace background
