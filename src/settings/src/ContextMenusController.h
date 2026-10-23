#pragma once

// The Context Menus page (ui/pages/context-menus.slint, MenusState):
//  - modules: the root shell.nss import lines, on/off; the user's own .nss
//    files can be added (copied into <config>\imports\ when they're elsewhere)
//    and removed (optionally deleting the file);
//  - the taskbar menu's Apps submenu: imports/settings-app-taskbar.nss
//    (TaskbarApps.h), with apps picked from a file or from the installed ones;
//  - the modify(...)/remove(...) rules of every file in the import tree, on/off
//    by commenting them out.
// Changes are staged in memory; Save writes them through nss::ConfigStore
// with the `shell.exe -config:check` validator and a snapshot.

#include "app.h"

#include "ConfigRules.h"
#include "TaskbarApps.h"
#include "Worker.h"

#include "nss/NssDocument.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class ContextMenusController
{
public:
    explicit ContextMenusController(slint::ComponentHandle<AppWindow> ui);
    ~ContextMenusController();

private:
    struct File
    {
        std::filesystem::path path;
        std::string label;        // relative to the config folder, or the file name when outside it
        bool outside = false;     // not under the config folder
        nss::NssDocument original;
        nss::NssDocument current;
        bool writable = false;
    };

    struct RuleRef
    {
        size_t file = 0;
        rules::Rule rule;
    };

    // A file Add copies into <config>\imports\ on save.
    struct NewFile
    {
        std::filesystem::path path;
        std::string rel;          // "imports/mine.nss"
        std::string text;
        nss::TextFormat format;
    };

    void bindCallbacks();
    void pollStatus();
    void load();
    void push();
    void toggleModule(int index, bool on);
    void toggleRule(int index, bool on);
    void save();
    void discard();
    void openInEditor(const std::string &path, int line);
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});
    int dirtyCount() const;
    const rules::Names &names();
    std::string pickFile(const std::wstring &title, const std::wstring &filterName, const wchar_t *filterSpec,
                         const std::wstring &folder = {});

    // Modules
    void addModule();
    void askRemoveModule(int index);
    void removeModule();
    std::filesystem::path modulePath(const rules::Module &m) const;

    // Taskbar menu
    void pushTaskbar();
    void taskbarEdited(bool reloadForm);
    void taskbarSelect(int index);
    void taskbarToggle(int index, bool on);
    void taskbarMove(int delta);
    void taskbarRemove();
    void taskbarSetText(const std::string &field, const std::string &value);
    void taskbarSetAdmin(bool on);
    void taskbarBrowse(bool replaceProgram);
    void taskbarAdd(taskbarapps::Entry e);
    void openPicker();
    void pushInstalled();
    void pick(int index);
    bool taskbarDirty() const;
    bool taskbarEditable() const;
    std::string iconSource(const taskbarapps::Entry &e) const;
    std::optional<slint::Image> icon(const std::string &source);
    void requestIcons();

    slint::ComponentHandle<AppWindow> ui_;
    std::filesystem::path shellExe_;
    std::filesystem::path root_;
    std::string statusPath_;
    bool shared_ = false;
    std::vector<File> files_;           // [0] is the root
    std::vector<rules::Module> modules_;
    std::vector<RuleRef> rules_;
    rules::Names names_;                // how rule summaries name menus and items
    std::string namesKey_;              // the language and folders names_ was built for
    std::vector<std::string> changes_;  // for the snapshot reason
    bool saving_ = false;
    uint64_t gen_ = 0;

    // Modules
    std::vector<NewFile> newFiles_;
    std::vector<std::filesystem::path> deleteAfterSave_; // the user's files to delete once saved
    int removeIndex_ = -1;

    // Taskbar menu
    taskbarapps::Paths paths_;
    taskbarapps::List taskbar_;
    taskbarapps::List taskbarOriginal_;
    nss::TextFormat taskbarFormat_;
    bool taskbarHooked_ = true;         // imports/taskbar.nss reads the list
    std::filesystem::path taskbarNss_;  // the imported taskbar.nss
    int taskbarSelected_ = -1;
    int taskbarSerial_ = 0;
    std::vector<taskbarapps::Installed> installed_;
    std::vector<size_t> shown_;         // installed_ indexes the picker lists
    std::string filter_;
    bool installedLoading_ = false;
    bool installedLoaded_ = false;
    std::map<std::string, std::optional<slint::Image>> icons_; // by lower-case source; nullopt: none
    std::set<std::string> iconsPending_;
    int devSelect_ = -1;                // --taskbar-select
    bool devPicker_ = false;            // --taskbar-picker

    slint::Timer statusPoll_;
    slint::Timer toastTimer_;
    slint::Timer jumpTimer_;
    Worker worker_; // last: joined first
};
