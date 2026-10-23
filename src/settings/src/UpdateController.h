#pragma once

// Connects UpdateState (ui/update-state.slint) to Updater.h: the startup and manual checks,
// the download with progress, and handing over to the helper that installs the update.
//
// Settings, under HKCU\Software\Nilesoft\Shell\Settings:
//   UpdateAutoCheck     DWORD, default 1: check when the app starts, at most once a day
//   UpdateAutoInstall   DWORD, default 0: download and verify in the background (an app-only
//                       update is also installed for the user), then offer a restart; never
//                       for an update that needs administrator approval
//   UpdateChannel       DWORD 0 stable, 1 beta; absent: beta only for a pre-release build
//   UpdateLastCheck     DWORD, Unix time of the last successful check
//   UpdateSkipVersion   REG_SZ, the version "Skip this version" was used for

#include "app.h"

#include "Updater.h"
#include "Worker.h"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct UpdateOptions
{
    bool checkNow = false;       // --check-updates-now: check at startup, whatever the settings
    bool installNow = false;     // --install-update-now (tests): then download and install an update
                                 // that needs no administrator rights, as the automatic install does
    bool persist = true;         // false for --snapshot: nothing is written to the registry
    bool allowAutoCheck = true;  // false for --snapshot unless --check-updates-now
    std::wstring updated;        // --updated <msi|portable|app:<version>>: the helper just installed this build
    std::wstring updateFailed;   // --update-failed <message>
    bool updateCancelled = false; // --update-cancelled
};

// What is on offer, for callers outside the Updates UI (the tray, notifications).
struct UpdateSummary
{
    // idle | checking | up-to-date | error | available | downloading | ready | installing
    std::string state;
    std::string version;         // the update on offer, "" for none
    bool appOnly = false;        // installed for this user only, without administrator rights
    bool portable = false;       // a portable copy, replaced in place without administrator rights
    bool needsElevation = false; // only the installer can install it: administrator approval, never automatic
    bool readyForRestart = false; // downloaded and verified (app only: installed, used from the next start)
};

// The default update path never asks for administrator rights:
//   - portable copies are replaced in place (the folder is the user's);
//   - an all-users (MSI) install gets the app package installed for this user only
//     (AppChannel.h), when the release has one and its Shell core is the installed one.
// Only a release that changes the Shell core (or one without an app package) needs the MSI
// and so administrator approval: shown as an explicit choice, never started automatically,
// and automatic installs skip it and only notify.
class UpdateController
{
public:
    // A message for the UI, built again in the new language when the language changes.
    using Text = std::function<std::string()>;

    // `quit` saves the window placement and ends the event loop (the helper waits for it).
    UpdateController(slint::ComponentHandle<AppWindow> ui, UpdateOptions options, std::function<void()> quit);
    ~UpdateController();

    void attachWindow(HWND hwnd) { hwnd_ = hwnd; }
    void start();

    // --- for the tray and other callers; UI thread ---------------------------------------
    UpdateSummary summary() const;
    // Called after every change of summary().
    void onChanged(std::function<void()> fn) { changed_.push_back(std::move(fn)); }
    // A check like the daily one: failures stay quiet. `manual`: like "Check now".
    void checkForUpdates(bool manual = false) { check(manual); }
    // Downloads and verifies the update on offer; an app-only update is also installed
    // for this user (used from the next start). Never asks for administrator rights: false
    // when there is nothing to do or the update needs them (needsElevation).
    bool downloadInBackground();
    // The Install button: closes the app and installs (a core update asks for administrator
    // approval here, so call this only for a click of the user's).
    void installNow() { install(); }

private:
    void bindCallbacks();
    void check(bool manual);
    void applyCheck(updater::FeedResult feed, updater::InstallType type, bool manual);
    void reselect();
    void loadManifest(const updater::Release &release);
    void maybeAutoInstall();
    void download(bool thenInstall, bool quiet);
    void install();
    void handOver();
    void handOverApp();
    void skip();
    void changed();
    void setState(const std::string &state, Text message = {}, const std::string &kind = {});
    void showRelease();
    void showLastChecked();
    void notice(const std::string &kind, Text title, Text text, const std::string &action);
    void retranslate();
    updater::Channel channel() const { return channel_; }

    slint::ComponentHandle<AppWindow> ui_;
    UpdateOptions options_;
    std::function<void()> quit_;
    HWND hwnd_ = nullptr;

    updater::Version current_;
    updater::Channel channel_ = updater::Channel::Stable;
    bool autoCheck_ = true;
    bool autoInstall_ = false;
    std::string skipped_;
    std::time_t lastCheck_ = 0;

    bool working_ = false;
    std::vector<updater::Release> releases_;
    bool localFeed_ = false;
    std::optional<updater::Release> found_;
    updater::InstallType installType_ = updater::InstallType::Portable;
    std::filesystem::path installDir_;               // the install folder (AppChannel baseDir())
    std::optional<updater::Version> installedRelease_; // the release of the installed core
    std::map<std::string, updater::ManifestResult> manifests_; // by version
    std::string manifestLoading_;                   // the version whose update.json is on its way
    updater::UpdatePlan plan_;
    bool autoPending_ = false;                      // a check asked for an automatic install
    std::filesystem::path ready_;   // the verified MSI, the extracted portable folder, or the installed per-user app
    std::string readyVersion_;
    std::vector<std::function<void()>> changed_;

    Text message_;     // UpdateState.message
    Text noticeTitle_; // UpdateState.notice-title
    Text noticeText_;  // UpdateState.notice-text

    std::atomic<bool> cancel_{ false };
    slint::Timer startTimer_;
    slint::Timer dailyTimer_;
    Worker worker_; // last: joined first
};

// NilesoftShellPro.exe started as the update helper (a copy in %TEMP%):
//   --apply-update <staging> <installdir> <pid>   portable: copy the files over, relaunch
//   --apply-app <appdir> <installdir> <pid>       app only: the per-user copy is installed
//                                                  already; relaunch the installed exe, which
//                                                  hands off to it (AppChannel.h)
//   --apply-msi <msi> <installdir> <pid>          elevated: run msiexec, relaunch unelevated
// Returns the exit code, or -1 when the command line is not one of these.
int runUpdateHelper();
