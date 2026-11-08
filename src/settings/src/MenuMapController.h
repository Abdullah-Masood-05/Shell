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
//  - Shell's own items: the switch (vis=vis.remove), Rename, Icon (image=), Move to
//    (menu=) and the place in its menu (pos=) are saved right away the same way.
//    For an item in a file that came with Shell (imports/terminal.nss, goto.nss...)
//    they are override(...) lines in imports/settings-app-map.nss (ShellOverrides.h),
//    so that file stays as shipped and updates replace it. In the user's own files
//    they change the item's own declaration (or move it up or down in the file).
//    Edit code always changes the file itself.

#include "app.h"

#include "MenuMap.h"
#include "SaveService.h"
#include "ShellOverrides.h"
#include "ShellCli.h"
#include "Worker.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "CodeEditorBridge.h"
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
        bool openMove = false;     // the selected item's Move to list opens (--map-open-move)
        bool openPlace = false;    // its Place in this menu list opens (--map-open-place)
        int sample = -1;           // the File context's sample, by index (--map-sample)
    };
    void setDevOptions(DevOptions options);

private:
    using Entry = menumap::ListEntry;

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
        int entry = -1;       // its entry in entries_
    };

    // "Move to" and the place in its menu for one of Shell's items.
    struct Choice
    {
        std::string label;
        std::string value;    // the value of menu= or pos=; "" takes it out
    };

    void bindCallbacks();
    void activated();
    void load();
    void buildEntries();
    // The list for an older shell.exe, without the order: your items, then Windows'.
    // With one: menumap::buildMenuList.
    void buildLegacy();
    int addGroup(int group, const char *key);
    std::string uniqueShellKey(const menumap::Node &n) const;
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
    // Icon: image= on your item's line, or a modify(... image=) rule for a Windows item.
    // `typed` as the field has it (menumap::imageValue()); "" or "null" takes it away.
    void setIcon(const std::string &typed);
    void resetIcon();
    // A file dialog for an icon: the file as the Icon field takes it ("C:pp.exe,0").
    std::string browseIcon();
    // The rules for the selected Windows item's icon that a new one replaces: pending
    // ones taken out, the saved ones in settings-app-map.nss staged for removal.
    void dropIconRules(const menumap::Node &n, const std::string &label);
    void moveTo(int index);
    void resetMenu();
    // A Windows or program item the map can't list, named by its title.
    void addUnlisted(const std::string &title);
    void place(int index);
    void resetPosition();
    void reorder(int key, bool up);
    // The items of the menu `e` is in, as planReorder() takes them.
    std::vector<menumap::Sibling> siblingsOf(const Entry &e, size_t *index) const;
    // A Windows item's menu= or title= ("menu", "title") as its rules will give it
    // once the changes waiting here are saved; `saved` is what they give it now.
    std::string ruleValue(const menumap::Node &n, const char *prop, const std::string &saved) const;
    menumap::Reorder reorderOf(const Entry &e, bool up) const;
    bool canMoveOwn(const menumap::Node &n) const;
    // One of Windows' or a program's items in the main menu that a rule (pos=) can place.
    bool canPlaceWindows(const Entry &e) const;
    // Gives it this pos= (a rule in settings-app-map.nss, staged like Move to's); "" takes the
    // place a rule made here gave it away.
    void setWindowsPos(const menumap::Node &n, const std::string &value, const std::string &label);
    // The note for a file that came with Shell: changing it makes it "edited by you"
    // for config updates.
    std::string shippedNote(const std::filesystem::path &file) const;
    static std::string shellKey(const std::filesystem::path &file, int line, int column);
    void editCode();
    void editChanged(const code::Result &r);
    void editSave();
    void editCancel();
    void openInEditor(const std::string &path, int line);
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});

    ItemState stateOf(const Entry &e) const;
    // A group's rows show (Not in this menu starts closed).
    bool groupOpen(const Entry &e) const;
    // Hidden itself or inside a hidden menu, as it will be once the changes here are saved.
    bool inHiddenMenu(int entry) const;
    std::string statusOf(const Entry &e, std::string *kind) const;
    std::string originLabel(const Entry &e) const;
    std::string fileLabel(const std::filesystem::path &file) const;
    // The text of an item's pos= (the override line's for one in a shipped file), "" without one.
    std::string positionOf(const menumap::Node &n) const;
    bool writable(const std::filesystem::path &file) const;
    // The rule lines of settings-app-map.nss among `indexes` (rules of the tree).
    std::vector<std::string> mapLinesOf(const std::vector<int> &indexes, const char *what) const;
    // Changes one of Shell's items in its file and saves it (checked, snapshot first).
    void saveShellEdit(const menumap::Node &n, const std::string &reason,
                       const std::function<bool(nss::NssDocument &, std::string &)> &edit);
    // One of Shell's items in a file that came with Shell: what names it in an
    // override(...) line (nullopt for one in the user's own files, or one that has no
    // title= to name it by), and whether its own line has the vis=vis.remove that
    // older versions of this page added. Read once per load.
    struct OwnInfo
    {
        std::optional<overrides::Key> key;
        bool legacyHidden = false;
    };
    const OwnInfo &ownInfo(const menumap::Node &n) const;
    // The value the override lines give the item's `name` property, if any.
    std::optional<std::string> overrideProp(const overrides::Key &key, std::string_view name) const;
    // Changes the item's override line and saves settings-app-map.nss right away
    // (checked, snapshot first), with the root's import of it when that is missing.
    void saveOverride(const menumap::Node &n, const std::string &reason, const overrides::Key &key,
                      std::string_view name, const std::optional<std::string> &value,
                      std::string_view name2 = {}, const std::optional<std::string> &value2 = std::nullopt);
    // Saves `writes` (checked by shell.exe, snapshot first) and reloads the pages.
    void commitWrites(std::vector<nss::FileWrite> writes, const std::string &reason, const std::string &select);
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
    std::vector<overrides::Override> overrides_; // its override(...) lines
    mutable std::map<std::string, OwnInfo> own_; // by shellKey()
    mutable std::map<std::string, std::optional<nss::NssDocument>> ownDocs_; // by pathKey(), for own_
    std::vector<Entry> entries_;
    // Where each item went in the last list (by its key: 0 in the menu, 1 switched off,
    // 2 not in this menu), and where the next one keeps the items that were there: a
    // switch saved on this page reloads the list, and the row stays until Refresh.
    std::map<std::string, int> disp_, pin_;
    bool keepPins_ = false;
    std::set<std::string> expanded_;
    std::set<std::string> collapsed_;     // groups the user folded
    int selected_ = -1;
    std::string selectedKey_;
    std::vector<MoveTarget> targets_;
    std::map<int, std::vector<std::string>> typed_; // titles typed for unlisted items, by context
    std::vector<MoveTarget> sysTargets_;  // for the selected Windows item: the main menu, then targets_
    std::vector<Choice> ownTargets_;      // for the selected Shell item
    std::vector<Choice> positions_;       // the same
    std::string pendingSelect_;           // the key a moved declaration gets, once saved
    bool reveal_ = false;                 // open the menus the selected item is in, on the next read
    int context_ = 0;
    int sample_ = 0;
    int view_ = 0;
    uint64_t gen_ = 0;
    DevOptions dev_;
    bool devPending_ = false;

    // Edit code
    std::filesystem::path editFile_;
    size_t editLine_ = 0, editColumn_ = 0;
    std::string editOriginal_;            // the block as read (the file's line ends)
    code::Document editDoc_;              // the block as edited ('\n' line ends), with undo
    codeui::Rows editRows_;

    slint::Timer toastTimer_;
    slint::Timer jumpTimer_;
    Worker worker_; // last: joined first
};
