#pragma once

// Connects AppWindow (ui/app.slint) to shell.exe, the registry and shell.log.
// Every shell.exe call runs on the Worker; results come back on the UI thread.

#include "app.h"

#include "LogReader.h"
#include "ShellCli.h"
#include "Status.h"
#include "Worker.h"

#include <optional>
#include <string>
#include <vector>

struct AppOptions
{
    bool noOnboarding = false; // --no-onboarding (development)
    int startPage = 0;         // --page <name>  (development)
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
    std::optional<uint32_t> lastAccent_;
    slint::Timer toastTimer_;
    slint::Timer themePoll_;
    Worker worker_; // last: joined first, before cli_ goes away
};
