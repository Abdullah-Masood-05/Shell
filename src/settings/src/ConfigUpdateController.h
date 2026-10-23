#pragma once

// After an update (ui/config-update-state.slint, ConfigUpdateState): once the first
// `shell.exe -status` is in, and only when this app version hasn't done it yet (see
// ConfigUpdate.h), runs `shell.exe -config:upgrade` for a personal config. shell.exe
// snapshots the config into Backups before it writes anything. Home then says how many
// files were brought up to date and lists the ones the user changed, which were kept:
// each can be compared with the new version or replaced by it (`-config:upgrade -take:`,
// again after a snapshot). Nothing runs as administrator, and the MSI never does this:
// the app does it the next time it starts. A per-user copy of the app (AppChannel.h)
// does the same itself, with its own newer shell.nss and imports\ as the shipped files
// (config_update::runLocal), also for a personal config made later.

#include "app.h"

#include "ConfigUpdate.h"
#include "ShellCli.h"
#include "Worker.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class ConfigUpdateController
{
public:
    // `enabled` false: never runs (screenshots, a --status-json run). `forced`: run even if
    // this version already did (the update helper just installed a build).
    ConfigUpdateController(slint::ComponentHandle<AppWindow> ui, bool enabled, bool forced);
    ~ConfigUpdateController();

    // Files of the personal config were replaced. UI thread.
    void onFilesChanged(std::function<void()> fn) { filesChanged_.push_back(std::move(fn)); }

private:
    void bindCallbacks();
    void pollStatus();
    void run(const std::string &root);
    // `shell.exe -config:upgrade [-take:]`, or the same in-process from `shipped`
    // (config_update::runLocal). Worker thread.
    static std::optional<config_update::Report> upgrade(const ShellCli &cli, const std::wstring &shipped,
                                                        const std::string &root, const std::vector<std::string> &take,
                                                        CliResult &r);
    void show(const config_update::Report &report);
    void pushText();
    void compare(const std::string &path);
    void take(const std::string &path);
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});
    void notifyFilesChanged();

    slint::ComponentHandle<AppWindow> ui_;
    ShellCli cli_;
    std::wstring shipped_; // config_update::localShippedDir(): upgrade in-process from there
    bool enabled_ = true;
    bool forced_ = false;
    bool checked_ = false;
    bool running_ = false;
    std::string upgradedRoot_; // the personal root file already brought up to date
    std::string configDir_, installDir_;
    size_t changed_ = 0;
    std::vector<std::string> kept_;
    std::string comparePath_;
    uint64_t compareGen_ = 0;
    std::vector<std::function<void()>> filesChanged_;
    slint::Timer statusPoll_;
    slint::Timer toastTimer_;
    Worker worker_; // last: joined first
};
