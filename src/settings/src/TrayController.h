#pragma once

// The app in the system tray: Preferences > Startup and tray (TrayState in
// ui/tray-state.slint), the tray icon and its menu (TrayIcon.h), and the update checks
// and notifications while the window is closed (Background.h).
//
// Updates go through UpdateController: this class asks it for a check and reads what is on
// offer from its summary(). The checking, the channel, "Skip this version" and the
// automatic download and install stay there; an update that needs administrator approval
// (summary().needsElevation) is only ever announced from the tray, never installed.

#include "app.h"

#include "Background.h"
#include "TrayIcon.h"
#include "UpdateController.h"

#include <functional>
#include <memory>
#include <string>

class TrayController
{
public:
    struct Hooks
    {
        std::function<void(int page)> show; // show the window (bring it forward), on this page (-1: as it is)
        std::function<void()> quit;         // end the app
    };

    // `persist` false (--snapshot): the switches show the settings and nothing is written.
    TrayController(slint::ComponentHandle<AppWindow> ui, UpdateController &updates, Hooks hooks, bool persist);
    ~TrayController();

    // Once, not for --snapshot: the Run entry (Background.h), the instance window that a
    // second start finds, and the icon if the setting wants it. False if the window
    // couldn't be created.
    bool start(bool background);

    // The window was closed: true if the app stays in the tray.
    bool keepRunning() const { return prefs_.keepInTray && icon_ && icon_->visible(); }
    // The window is hidden: give its memory back to Windows.
    void windowHidden();

private:
    void bindCallbacks();
    void pushPrefs();
    void tick();
    void check(bool manual);
    void watchCheck(bool manual);
    void afterCheck(bool manual);
    void command(TrayIcon::Command c);
    std::wstring label(TrayIcon::Command c) const;
    // What the Run entry starts: the installed exe (AppChannel launcherPath()), also in a
    // per-user copy of the app; for a portable copy its own exe.
    std::wstring exePath() const;

    slint::ComponentHandle<AppWindow> ui_;
    UpdateController &updates_;
    Hooks hooks_;
    bool persist_;
    startup::RegistryStore store_;
    startup::Prefs prefs_;
    std::unique_ptr<TrayIcon> icon_;
    bool background_ = false;
    int watchTicks_ = 0;
    bool manualPending_ = false;
    slint::Timer firstCheck_;
    slint::Timer schedule_;
    slint::Timer watch_;
    slint::Timer trim_;
};
