#pragma once

// The "Open with" page (ui/pages/openwith.slint, OpenWithState): editors and
// terminals for the right-click menu, kept in the app-owned
// imports/settings-app-openwith.nss. Presets are detected read-only on load;
// custom apps can be added. Save writes the file (and, once, the import line
// in the root shell.nss, and develop.nss when its Editors submenu is hidden or
// shown) through nss::ConfigStore with the `shell.exe -config:check`
// validator and a snapshot ("Open with: ...").

#include "app.h"

#include "OpenWithModel.h"
#include "Worker.h"

#include "nss/NssDocument.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

class OpenWithController
{
public:
    explicit OpenWithController(slint::ComponentHandle<AppWindow> ui);
    ~OpenWithController();

private:
    struct IconPixels
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
    };

    void bindCallbacks();
    void pollStatus();
    void load();
    void edited(bool reloadForm);
    void pushRows();
    void pushForm();
    void pushOptions();
    void pushPreview();
    void pushCode();
    void pushTest();
    void select(int index);
    void setText(const std::string &field, const std::string &value);
    void setChoice(const std::string &field, int value);
    void setFlag(const std::string &field, bool value);
    void move(int delta);
    std::string pickFile(const wchar_t *title, const wchar_t *filterName, const wchar_t *filterSpec,
                         const std::string &start);
    void requestIcons();
    std::string iconSource(const openwith::App &app) const;
    void save();
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});
    std::filesystem::path openWithFile() const;
    bool readOnly() const { return shared_ || root_.empty() || !loaded_; }
    bool dirty() const;

    slint::ComponentHandle<AppWindow> ui_;
    std::filesystem::path shellExe_;
    std::filesystem::path root_;
    std::string statusPath_;
    bool shared_ = false;
    bool loaded_ = false;
    openwith::Env env_;
    openwith::Model model_;
    openwith::Model saved_;
    bool fileExists_ = false;
    nss::TextFormat format_ = nss::defaultFormat();

    // develop.nss
    std::filesystem::path developPath_;
    bool developFound_ = false;
    bool developWritable_ = false;
    bool developHidden_ = false;      // staged
    bool developSavedHidden_ = false; // on disk

    int selected_ = -1;
    int devSelect_ = -1; // --openwith-select
    bool testShown_ = false;
    int formSerial_ = 0;
    int optionsSerial_ = 0;
    int previewKind_ = 0;
    bool saving_ = false;
    uint64_t gen_ = 0;

    std::map<std::string, std::optional<slint::Image>> icons_; // by lower-case path; nullopt: none
    std::map<std::string, bool> iconsPending_;

    slint::Timer statusPoll_;
    slint::Timer toastTimer_;
    Worker worker_; // last: joined first
};
