#pragma once

// Connects AppWindow (ui/app.slint) to shell.exe, the registry and shell.log.
// Every shell.exe call runs on the Worker; results come back on the UI thread.

#include "app.h"

#include "LogReader.h"
#include "ShellCli.h"
#include "Status.h"
#include "Worker.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct AppOptions
{
    bool noOnboarding = false; // --no-onboarding (development)
    int startPage = 0;         // --page <name>  (development)
    int onboardingStep = -1;   // --onboarding <1|2|3>: show the dialog at that step (development)
    std::wstring statusFile;   // --status-json <file>: Home reads it instead of -status (development)
    std::string language;      // --lang <auto|en|zh-CN|...>: this run only, not saved (development)
    std::string shownLanguage; // --ui-lang <en|zh-CN|...>: from the update helper, see AppController.cpp
    std::wstring testPo;       // --po <file>: the translator preview with that .po file
};

class AppController
{
public:
    AppController(slint::ComponentHandle<AppWindow> ui, AppOptions options);
    ~AppController();

    // Call once the event loop runs and the native window exists.
    void attachWindow(HWND hwnd);
    void start();
    void saveWindowPlacement();

    // A `-config:init` from Home or the first-run dialog went through: the active
    // root shell.nss changed. UI thread.
    void onConfigChanged(std::function<void()> fn) { configChanged_.push_back(std::move(fn)); }

private:
    struct Snapshot
    {
        std::optional<ShellStatus> status;
        std::string error;
        std::vector<LogEntry> log;
        std::string logPath;
    };

    void bindCallbacks();
    void refresh(bool quiet);
    void runAction(const std::string &id);
    void applyTheme();
    void setTheme(int mode);
    // The Language setting: "" (Windows), "en" or a bundled code. Switches Slint's and the
    // C++ catalogs; nsp::onLanguageChanged callbacks push their text again.
    void applyLanguage(const std::string &setting, const std::string &shown = {});
    void setLanguage(int index);
    void pushLanguageOptions();
    // The translator preview (Preferences > Language > Test a translation file, --po):
    // shows a .po file for this session, nothing saved. Stop goes back to the setting.
    void pickTestTranslation();
    bool loadTestTranslation(const std::wstring &file, bool reload);
    void stopTestTranslation();
    void setBusy(bool busy, const std::string &label = {});
    void showToast(const std::string &kind, const std::string &title, const std::string &text = {});
    void applySnapshot(Snapshot snap, bool quiet);
    void openPath(const std::string &path);

    Snapshot collect() const; // worker thread

    slint::ComponentHandle<AppWindow> ui_;
    AppOptions options_;
    ShellCli cli_;
    std::optional<ShellStatus> status_;
    HWND hwnd_ = nullptr;
    bool firstStatus_ = true;
    bool busy_ = false;
    bool lastDark_ = true;
    std::string languageSetting_; // "" follows Windows
    std::wstring testFile_;       // the translator preview's .po file, while it shows
    std::vector<std::function<void()>> configChanged_;
    slint::Timer toastTimer_;
    slint::Timer themePoll_;
    Worker worker_; // last: joined first, before cli_ goes away
};
