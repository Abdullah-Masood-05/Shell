#pragma once

// The Menu Builder page (ui/pages/menu-builder.slint, BuilderState): a tree of
// menus, items and separators kept in the app-owned
// imports/settings-app-menus.nss, a properties form, a preview, and the
// generated .nss. Save writes the file (and, once, the import line in the
// root shell.nss) through nss::ConfigStore with the `shell.exe -config:check`
// validator and a snapshot. The user may also edit the file by hand: what the
// builder doesn't edit is kept (MenuWriter.h); a file that doesn't parse is
// left alone, with a notice.

#include "app.h"

#include "MenuModel.h"
#include "Worker.h"

#include <cstdint>
#include <optional>
#include <filesystem>
#include <functional>
#include <string>

class MenuBuilderController
{
public:
    explicit MenuBuilderController(slint::ComponentHandle<AppWindow> ui);
    ~MenuBuilderController();

private:
    void bindCallbacks();
    void pollStatus();
    void load();
    void select(const menus::Path &path, bool reloadForm = true);
    void edited(bool structure);  // after any change to tree_
    void pushRows();
    void pushNode();
    void pushPreview();
    void pushCode();
    void structural(std::optional<menus::Path> (*op)(menus::Tree &, const menus::Path &));
    void setText(const std::string &field, const std::string &value);
    void setFlag(const std::string &field, bool value);
    void setChoice(const std::string &field, int value);
    void save();
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});
    std::filesystem::path menusFile() const;
    bool readOnly() const { return shared_ || root_.empty(); }

    slint::ComponentHandle<AppWindow> ui_;
    std::filesystem::path shellExe_;
    std::filesystem::path root_;
    std::string statusPath_;
    std::function<void()> loadText_; // sets notice and load_error in the current language
    bool shared_ = false;
    menus::Tree tree_;
    menus::Tree saved_;
    std::string tail_;               // the file's own lines after the last entry (menus::ParseResult::tail)
    std::string savedTail_;
    std::string savedText_;          // the file as generated from saved_
    nss::TextFormat format_ = nss::defaultFormat();
    menus::Path selected_;
    bool saving_ = false;
    uint64_t gen_ = 0;
    int serial_ = 0;

    slint::Timer statusPoll_;
    slint::Timer toastTimer_;
    Worker worker_; // last: joined first
};
