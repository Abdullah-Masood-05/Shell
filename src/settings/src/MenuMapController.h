#pragma once

// The Menu map page (ui/pages/menu-map.slint, MenuMapState): the real context menu
// for a file, folder, folder background, desktop, drive or the taskbar, from
// `shell.exe -config:tree` (MenuMap.h). Shell's own items come with their file and
// line; Windows' and other programs' items with the program that added them and
// the rules that reach them.
//
// Changes:
//  - Windows' items: hide, rename and move are rules in imports/settings-app-map.nss,
//    staged in SaveService like every other page's changes (the footer saves them,
//    checked by shell.exe, snapshot first), so they show on the Context Menus page.
//  - Shell's own items: the switch (vis=vis.remove), Rename and Edit code change the
//    item's own declaration in its file, saved right away the same way.

#include "app.h"

#include "MenuMap.h"
#include "SaveService.h"
#include "ShellCli.h"
#include "Worker.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Highlight.h"

class MenuMapController
{
public:
    MenuMapController(slint::ComponentHandle<AppWindow> ui, SaveService &save);
    ~MenuMapController();

    // Development (src/main.cpp: --map-context, --map-view, --map-expand, --map-select):
    // how the page starts, for screenshots. `select` is an item's title.
    struct DevOptions
    {
        int context = 0;
        int view = 0;
        bool expandAll = false;
        std::string select;
    };
    void setDevOptions(DevOptions options);

private:
    struct Entry
    {
        const menumap::Node *node = nullptr; // null: a group (Shell's items / Windows' items)
        int group = 0;                       // 0 your items, 1 Windows and programs
        int depth = 0;
        int parent = -1;
        std::vector<int> kids;
        std::string key;                     // stable across reloads, for expansion and selection
        std::string menuPath;                // for "Move to": the path rules name this menu by
    };

    struct ItemState
    {
        bool shown = true;
        bool canToggle = false;
        bool changed = false;     // a change not saved yet
        std::string note;         // why it can't be switched here
        std::vector<std::string> mapLines;   // rules of settings-app-map.nss that hide it
    };

    struct MoveTarget
    {
        std::string label;
        std::string path;
    };

    void bindCallbacks();
    void activated();
    void load();
    void buildEntries();
    void push();
    void pushRows();
    void pushPanels();
    void pushDetail();
    void select(int key);
    void toggleExpand(int key);
    void expandAll(bool on);
    void toggle(int key, bool on);
    void rename(const std::string &title);
    void resetTitle();
    void moveTo(int index);
    void resetMenu();
    void editCode();
    int editEdited(const std::string &display);
    void editSave();
    void editCancel();
    void openInEditor(const std::string &path, int line);
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});

    ItemState stateOf(const Entry &e) const;
    std::string statusOf(const Entry &e, std::string *kind) const;
    std::string originLabel(const Entry &e) const;
    std::string fileLabel(const std::filesystem::path &file) const;
    bool writable(const std::filesystem::path &file) const;
    // The rule lines of settings-app-map.nss among `indexes` (rules of the tree).
    std::vector<std::string> mapLinesOf(const std::vector<int> &indexes, const char *what) const;
    // Changes one of Shell's items in its file and saves it (checked, snapshot first).
    void saveShellEdit(const menumap::Node &n, const std::string &reason,
                       const std::function<bool(nss::NssDocument &, std::string &)> &edit);
    void stageRule(const std::string &text, bool add, const std::string &label);

    slint::ComponentHandle<AppWindow> ui_;
    SaveService &save_;
    std::filesystem::path shellExe_;
    std::filesystem::path root_;          // the active shell.nss
    bool shared_ = false;
    bool stale_ = true;                   // reload on the next activation

    menumap::Tree tree_;
    bool haveTree_ = false;
    std::vector<std::string> mapLines_;   // settings-app-map.nss as read with the tree, by line - 1
    std::vector<Entry> entries_;
    std::set<std::string> expanded_;
    std::set<std::string> collapsed_;     // groups the user folded
    int selected_ = -1;
    std::string selectedKey_;
    std::vector<MoveTarget> targets_;
    int context_ = 0;
    int sample_ = 0;
    int view_ = 0;
    uint64_t gen_ = 0;
    DevOptions dev_;
    bool devPending_ = false;

    // Edit code
    std::filesystem::path editFile_;
    size_t editLine_ = 0, editColumn_ = 0;
    std::string editOriginal_;            // the block as read
    std::string editText_;                // the block as edited
    std::string editDisplay_;
    hl::TabGlyphs glyphs_;

    slint::Timer toastTimer_;
    slint::Timer jumpTimer_;
    Worker worker_; // last: joined first
};
