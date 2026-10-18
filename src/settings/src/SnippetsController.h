#pragma once

// The Snippets page (ui/pages/snippets.slint, SnippetsState): the catalog (Snippets.h) with
// a search box and a category filter, one switch per snippet. A remote snippet (RubicBG's)
// is downloaded and checked against its hash when it is turned on or its code is shown;
// a failed download changes nothing. A switch saves right away:
// the copy and imports/settings-app-snippets.nss are written through nss::ConfigStore,
// checked with `shell.exe -config:check` on a temporary copy first and snapshotted, so a
// snippet Shell would reject changes nothing. Once the config is known, copies the user
// never edited are brought up to the bundled version (unless `allowWrites` is false: the
// --snapshot tour writes nothing).
//
// The preview (SnippetPreview.h): clicking a row selects it (the first one is selected at
// first); after a short pause its menu is read with `shell.exe -config:tree` on a scratch
// root for every context, on a worker of its own, and kept for the session. A remote
// snippet is downloaded (and checked) first, as for "Show code". The menu is drawn with
// the Appearance page's theme (setTheme()), so Light/Dark there and here is the same switch.

#include "app.h"

#include "SnippetPreview.h"
#include "Snippets.h"
#include "ThemeModel.h"
#include "Worker.h"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class SnippetsController
{
public:
    SnippetsController(slint::ComponentHandle<AppWindow> ui, bool allowWrites);
    ~SnippetsController();

    // Expands a snippet's code (the --snippets-code development switch).
    void showCode(const std::string &id);
    // Selects a snippet for the preview, and the context it shows ("file", "back", ...;
    // "" for the first): the --snippets-select and --snippets-context switches.
    void selectForPreview(const std::string &id, const std::string &context = {});
    // The theme the preview is drawn with (AppearanceController::onPreview).
    void setTheme(const theme::Resolved &resolved);

private:
    // One snippet's preview, as the worker made it.
    struct Preview
    {
        struct Items
        {
            snippetpreview::Context context;
            std::vector<snippetpreview::Item> items;
        };
        struct Pixels
        {
            std::vector<uint8_t> rgba;
            uint32_t width = 0, height = 0;
        };
        std::string error;                    // shell.exe couldn't read the menu: why
        std::vector<Items> contexts;          // the ones where it adds items, in order
        snippetpreview::Sample sample;
        snippetpreview::ImageDefs defs;
        std::map<std::string, Pixels> files;  // file icons, by Icon::path + "," + index
    };

    struct Loaded
    {
        snippets::ConfigFiles files;
        std::map<std::string, std::vector<snippets::Requirement>> requirements; // by id
        std::map<std::string, std::string> texts; // remote snippets downloaded for the refresh
    };

    void bindCallbacks();
    void pollStatus();
    void load(bool refresh);
    void push();
    void toggle(const std::string &id, bool on);
    void setNow(const std::string &id, bool on);
    void fetchCode(const std::string &id);
    // catalog_ with these remote snippets' text filled in (a new catalog: workers may hold the old one).
    void addTexts(const std::map<std::string, std::string> &texts);
    void apply(snippets::Plan plan, const std::string &reason, bool quiet);
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});
    std::shared_ptr<slint::VectorModel<CodeLine>> code(const snippets::Snippet &s, int &columns);
    void select(const std::string &id);
    void startPreview();
    void pushPreview();
    static std::shared_ptr<const Preview> makePreview(const std::string &id, const std::string &text,
                                                      const std::filesystem::path &exe, const std::filesystem::path &root);
    slint::Image iconImage(const snippetpreview::Icon &icon, const Preview &p);

    slint::ComponentHandle<AppWindow> ui_;
    bool allowWrites_ = true;
    std::filesystem::path shellExe_;
    std::filesystem::path root_;
    std::string statusPath_;
    bool shared_ = false;
    std::shared_ptr<const snippets::Catalog> catalog_;
    Loaded loaded_;
    bool haveFiles_ = false;
    std::set<std::string> refreshed_;   // roots whose copies were brought up to date
    std::set<std::string> expanded_;    // ids showing their code
    std::set<std::string> fetching_;    // remote ids whose code is downloading
    std::map<std::string, std::string> codeErrors_; // remote ids whose code couldn't be downloaded
    std::string query_;
    int category_ = 0;                  // 0: all, else categories_[category_ - 1]
    std::vector<std::string> categories_;
    bool busy_ = false;
    std::string busyId_;
    uint64_t gen_ = 0;

    std::string selected_;              // the snippet the preview shows
    std::string wantedContext_;         // --snippets-context
    int previewContext_ = 0;            // index into the selected preview's contexts
    std::map<std::string, std::shared_ptr<const Preview>> previews_; // by id, for the session
    std::set<std::string> previewing_;  // ids whose preview is being made
    theme::Resolved theme_;
    std::map<std::string, slint::Image> svgImages_; // by the filled-in SVG

    slint::Timer statusPoll_;
    slint::Timer toastTimer_;
    slint::Timer previewDelay_;
    Worker previewWorker_;
    Worker worker_; // last: joined first
};
