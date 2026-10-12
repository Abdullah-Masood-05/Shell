#pragma once

// The Context Menus page (ui/pages/context-menus.slint, MenusState): the root
// shell.nss import lines as on/off modules, and the modify(...)/remove(...)
// rules of every file in the import tree, on/off by commenting them out.
// Changes are staged in memory; Save writes them through nss::ConfigStore
// with the `shell.exe -config:check` validator and a snapshot.

#include "app.h"

#include "ConfigRules.h"
#include "Worker.h"

#include "nss/NssDocument.h"

#include <cstdint>
#include <filesystem>
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
        std::string label;        // relative to the config folder
        nss::NssDocument original;
        nss::NssDocument current;
        bool writable = false;
    };

    struct RuleRef
    {
        size_t file = 0;
        rules::Rule rule;
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

    slint::ComponentHandle<AppWindow> ui_;
    std::filesystem::path shellExe_;
    std::filesystem::path root_;
    std::string statusPath_;
    bool shared_ = false;
    std::vector<File> files_;           // [0] is the root
    std::vector<rules::Module> modules_;
    std::vector<RuleRef> rules_;
    std::vector<std::string> changes_;  // for the snapshot reason
    bool saving_ = false;
    uint64_t gen_ = 0;

    slint::Timer statusPoll_;
    slint::Timer toastTimer_;
    slint::Timer jumpTimer_;
    Worker worker_; // last: joined first
};
