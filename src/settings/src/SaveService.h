#pragma once

// SaveService: the pending changes of every page and the save transaction,
// exposed to Slint through the SaveState global (ui/save-state.slint).
//
// save() runs on its Worker:
//   1. `shell.exe -status` for the active root shell.nss. If that is the shared
//      config in the install folder, the UI offers a per-user copy
//      (`shell.exe -config:init`) first; nothing is ever written there.
//   2. planSave(): imports/settings-app.nss regenerated with the pending values,
//      and `import 'imports/settings-app.nss'` made the root's last statement.
//   3. nss::ConfigStore::save(): `shell.exe -config:check` on a temp copy, a
//      snapshot ("Appearance: ..."), .bak files, atomic writes.
//   4. Verification on the real tree: -config:check again, then shell.log is
//      watched for ~2.5 s for new parser errors; a failure rolls the files back.

#include "app.h"

#include "PendingChanges.h"
#include "ShellCli.h"
#include "Worker.h"

#include "nss/ConfigGraph.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Where saves go, from `shell.exe -status`.
struct ConfigTarget
{
    bool ok = false;
    std::string error;
    std::filesystem::path root;        // the active shell.nss
    std::string source;                // registry | appdata | install
    std::filesystem::path installDir;  // app.dir
    std::filesystem::path logPath;     // shell.log next to the active shell.dll
};

class SaveService
{
public:
    SaveService(slint::ComponentHandle<AppWindow> ui, std::filesystem::path shellExe);
    ~SaveService();

    // Ctrl+S anywhere in the window: the Config Editor saves its own files, every
    // other page saves the pending changes.
    void attachWindow(HWND hwnd);

    const PendingChanges &pending() const { return pending_; }
    // Validated with ThemeSchema; returns the errors (and changes nothing) on failure.
    std::vector<std::string> set(std::string_view key, std::string_view literal, std::string_view page);
    void reset(std::string_view key, std::string_view page);
    void forget(std::string_view key);
    void discard();

    // Pending set changed (edits, discard, a save). UI thread.
    void onChanged(std::function<void()> fn) { changed_.push_back(std::move(fn)); }
    // The files changed on disk (a save went through). UI thread.
    void onSaved(std::function<void()> fn) { saved_.push_back(std::move(fn)); }

    // Runs `-status` on the worker, then `done` on the UI thread.
    void loadTarget(std::function<void(ConfigTarget)> done);
    const ConfigTarget &target() const { return target_; }

    // The config as the pages show it: `-status`, then the ConfigGraph of the active
    // root, both on the worker. Every listener runs on the UI thread once it is in
    // (also after each save that wrote files). `graph()` is null until then or when
    // the target couldn't be read (see target().error).
    void reloadConfig();
    void onConfig(std::function<void()> fn) { config_.push_back(std::move(fn)); }
    const nss::ConfigGraph *graph() const { return graph_.get(); }
    bool configLoading() const { return configLoading_; }

    // Where a key's value comes from: the last assignment in the user's files, and
    // the one in this app's imports/settings-app.nss.
    struct Origins
    {
        const nss::ValueOrigin *file = nullptr;
        const nss::ValueOrigin *app = nullptr;
    };
    Origins origins(const std::string &key) const;
    // The value text in effect once the pending changes are saved: the pending value,
    // else this app's saved value, else the files'. nullopt: built-in default.
    std::optional<std::string> current(const std::string &key) const;
    // "theme.nss, line 7"
    static std::string where(const nss::ValueOrigin &origin);
    // The caption under a setting: "From theme.nss, line 7", "Changed here · not saved yet", ...
    std::string originText(const std::string &key) const;

    void save();
    bool saving() const { return saving_; }

    const std::filesystem::path &shellExe() const { return cli_.exe(); }
    Worker &worker() { return worker_; }

private:
    struct Outcome
    {
        bool ok = false;
        bool needsUserConfig = false;
        bool nothingToDo = false;
        bool rolledBack = false;
        bool rejected = false;  // shell.exe -config:check said no
        std::string message;
        ConfigTarget target;
        std::map<std::string, PendingChanges::Change> saved;
    };

    ConfigTarget queryTarget() const;                            // worker
    Outcome runSave(const PendingChanges &snapshot) const;       // worker
    void finish(Outcome outcome);                                // UI
    void createUserConfig();
    void publish();
    void notifyChanged();
    void setResult(const std::string &kind, const std::string &text);
    void saveFromKeyboard();

    static LRESULT CALLBACK keyProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    slint::ComponentHandle<AppWindow> ui_;
    ShellCli cli_;
    PendingChanges pending_;
    ConfigTarget target_;
    bool saving_ = false;
    HWND hwnd_ = nullptr;
    std::vector<std::function<void()>> changed_;
    std::vector<std::function<void()>> saved_;
    std::vector<std::function<void()>> config_;
    std::shared_ptr<const nss::ConfigGraph> graph_;
    std::filesystem::path overridesPath_;
    bool configLoading_ = false;
    bool configAgain_ = false;
    slint::Timer resultTimer_;
    Worker worker_; // last: joined first
};
