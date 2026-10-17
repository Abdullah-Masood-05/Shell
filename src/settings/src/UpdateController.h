#pragma once

// Connects UpdateState (ui/update-state.slint) to Updater.h: the startup and manual checks,
// the download with progress, and handing over to the helper that installs the update.
//
// Settings, under HKCU\Software\Nilesoft\Shell\Settings:
//   UpdateAutoCheck     DWORD, default 1: check when the app starts, at most once a day
//   UpdateAutoInstall   DWORD, default 0: download and verify in the background, then offer a restart
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
#include <optional>
#include <string>
#include <vector>

struct UpdateOptions
{
    bool checkNow = false;       // --check-updates-now: check at startup, whatever the settings
    bool persist = true;         // false for --snapshot: nothing is written to the registry
    bool allowAutoCheck = true;  // false for --snapshot unless --check-updates-now
    std::wstring updated;        // --updated <msi|portable>: the helper just installed this build
    std::wstring updateFailed;   // --update-failed <message>
    bool updateCancelled = false; // --update-cancelled
};

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

private:
    void bindCallbacks();
    void check(bool manual);
    void applyCheck(updater::FeedResult feed, updater::InstallType type, bool manual);
    void reselect();
    void download(bool thenInstall, bool quiet);
    void install();
    void handOver();
    void skip();
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
    std::filesystem::path installDir_;
    std::filesystem::path ready_;   // the verified MSI, or the extracted portable folder
    std::string readyVersion_;

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
//   --apply-msi <msi> <installdir> <pid>          elevated: run msiexec, relaunch unelevated
// Returns the exit code, or -1 when the command line is not one of these.
int runUpdateHelper();
