#pragma once

// App updates without administrator rights for an all-users (MSI) install.
//
// The installed NilesoftShellPro.exe (in Program Files, which only an administrator can
// change) is what the Start menu shortcut and the autostart start. An update that only
// changes the app (and the shipped menu files) is installed for the signed-in user into
//   %LOCALAPPDATA%\Programs\Nilesoft Shell Pro\app\<version>\
// with an install.json (Record) naming the install folder it belongs to and the SHA-256
// of every file. When the installed exe starts, it looks there (decide()) and hands off
// to the newest per-user copy that is newer than itself and whose NilesoftShellPro.exe
// still has the recorded SHA-256, with the same command line (handOff()). Anything older
// than the installed version (a newer MSI was installed since), damaged, or that failed to
// start is removed and the installed copy runs instead. An elevated start never hands off:
// the per-user folder is writable without administrator rights.
//
// A per-user copy knows it is one from its install.json: baseDir() is then the install
// folder (shell.exe, the MSI's files), and launcherPath() the installed exe, which is
// what shortcuts, autostart entries and relaunches must start, never the per-user copy
// (it is deleted when a newer one or a newer MSI arrives).
//
// No Slint here (tests/test_app_channel.cpp).

#include "Updater.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace appchannel {

inline constexpr const wchar_t *kRecordName = L"install.json";
// Written by the installed exe when a per-user copy exited before its window was up.
inline constexpr const wchar_t *kFailedName = L"failed-to-start.txt";
// The named event a per-user copy sets once it is running (handOff() waits for it).
inline constexpr const wchar_t *kHandoffEnv = L"NSP_HANDOFF_EVENT";
// Set (to anything) to never hand off: for tests and troubleshooting.
inline constexpr const wchar_t *kNoHandoffEnv = L"NSP_NO_HANDOFF";
// Set (to a short name) to run apart from the copy of the app that is open: its own
// single-instance mutex and instance window class. For tests only.
inline constexpr const wchar_t *kInstanceEnv = L"NSP_INSTANCE_ID";

// "" normally; "." + NSP_INSTANCE_ID when that is set. Appended to the single-instance
// mutex name and the instance window class.
std::wstring instanceSuffix();

// %LOCALAPPDATA% (the environment variable first, so tests can point it elsewhere).
std::filesystem::path localAppData();

// %LOCALAPPDATA%\Programs\Nilesoft Shell Pro\app
std::filesystem::path appRoot();

// install.json
struct Record
{
    updater::Version version;
    std::filesystem::path base;                  // the install folder this copy belongs to
    std::map<std::string, std::string> files;    // '/'-separated path -> sha256
    std::string exeHash() const;                 // files["NilesoftShellPro.exe"], or ""
};

std::string recordJson(const Record &r);
std::optional<Record> parseRecord(const std::string &json);
std::optional<Record> readRecord(const std::filesystem::path &dir);
bool writeRecord(const std::filesystem::path &dir, const Record &r);

// One <appRoot>\<name> folder.
struct Candidate
{
    std::filesystem::path dir;
    std::optional<Record> record; // nullopt: no readable install.json
    bool failed = false;          // kFailedName is there
};

// The version folders under `root` (".partial" folders are left out).
std::vector<Candidate> scan(const std::filesystem::path &root);

enum class Verdict
{
    Use,          // the newest good copy: run it
    Older,        // not newer than the installed version (or than a newer good copy): remove
    Damaged,      // no install.json, a name that isn't its version, or the exe's hash differs: remove
    Failed,       // exited before its window was up last time: remove
    OtherInstall, // belongs to another install folder: leave alone
};

struct Decision
{
    std::optional<std::filesystem::path> exe; // the per-user NilesoftShellPro.exe to run
    updater::Version version;                 // its version
    std::vector<std::pair<std::filesystem::path, Verdict>> verdicts; // every candidate
    std::vector<std::filesystem::path> remove() const;               // Older, Damaged, Failed
};

// Which per-user copy the installed exe (version `running`, in `baseDir`) hands off to.
// `hashOf` returns a file's SHA-256 ("" when unreadable); only the chosen copy's exe is
// hashed. `elevated`: nothing is used or removed.
Decision decide(const updater::Version &running, const std::filesystem::path &baseDir, bool elevated,
                const std::vector<Candidate> &candidates,
                const std::function<std::string(const std::filesystem::path &)> &hashOf);

// An app update for `offered` that is installed for this user already and only waits for a
// restart: the folder of the per-user copy a restart would run (`pending` is decide() for
// this process, without elevation), if it is that version or newer. Nothing to download.
std::optional<std::filesystem::path> installedFor(const updater::Version &offered, const Decision &pending);

// The per-user copy a restart of this app would hand off to (decide() for the version
// `running` and the install folder `baseDir`; nothing is removed). Hashes that copy's exe.
Decision pendingCopy(const updater::Version &running, const std::filesystem::path &baseDir);

// --- one copy of the app at a time ---------------------------------------------------------
//
// The running copy's hidden instance window (TrayIcon.h) has its version in its title, so a
// newer copy that starts (after an update, the installed exe hands off to it) can tell the
// older one to quit instead of showing it again. 2.2.0 to 2.2.1 have no version there.
std::wstring instanceTitle(const updater::Version &v);
// The version in an instance window's title, nullopt when it has none.
std::optional<updater::Version> instanceVersion(const std::wstring &title);

enum class Existing
{
    Activate, // show the running copy (as before) and end this start
    Replace,  // ask the running copy to quit, then carry on as the only one
};
// A start of `self` found the app running: `running` is its version (nullopt when its
// instance window doesn't say), `instanceWindow` whether it has that window at all (a
// version from before the tray has none, and no quit message).
Existing onExisting(const updater::Version &self, const std::optional<updater::Version> &running, bool instanceWindow);

// The arguments of a command line without the program name (CommandLineToArgvW rules for
// the first argument), unchanged, so they can be passed on as they are.
std::wstring argumentsOf(const std::wstring &commandLine);

// This exe.
std::filesystem::path selfPath();
// install.json next to this exe: this is a per-user copy.
const std::optional<Record> &selfRecord();
bool isPerUserCopy();
// The install folder: shell.exe and the MSI's files are there. This exe's folder unless
// this is a per-user copy.
std::filesystem::path baseDir();
// baseDir()\NilesoftShellPro.exe: what shortcuts, autostart and relaunches start.
std::filesystem::path launcherPath();

// The ProductVersion of an exe (NilesoftShellPro.exe has the full release version there,
// "2.1.1-beta.1"), nullopt when it has none.
std::optional<updater::Version> productVersion(const std::filesystem::path &exe);

// The release the installed (base) app came with: the version of the core installed with it.
std::optional<updater::Version> installedRelease();

bool isElevated();

// In main(), before the single-instance check: hands off to a newer per-user copy.
// -1: carry on in this process; otherwise the exit code to return.
int handOff(const updater::Version &running);

// A per-user copy started by handOff() is up (its window exists, or it brought the
// running window forward): the installed exe that started it can exit. Safe to call more
// than once and in any process.
void signalStarted();

// Removes per-user copies older than `running` (except this exe's folder), copies of
// versions not above the installed release, and .partial folders. Background thread.
void cleanup(const updater::Version &running);

} // namespace appchannel
