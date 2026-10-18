#pragma once

// The Snippets page (ui/pages/snippets.slint, SnippetsState): the catalog (Snippets.h) with
// a search box and a category filter, one switch per snippet. A switch saves right away:
// the copy and imports/settings-app-snippets.nss are written through nss::ConfigStore,
// checked with `shell.exe -config:check` on a temporary copy first and snapshotted, so a
// snippet Shell would reject changes nothing. Once the config is known, copies the user
// never edited are brought up to the bundled version (unless `allowWrites` is false: the
// --snapshot tour writes nothing).

#include "app.h"

#include "Snippets.h"
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

private:
    struct Loaded
    {
        snippets::ConfigFiles files;
        std::map<std::string, std::vector<snippets::Requirement>> requirements; // by id
    };

    void bindCallbacks();
    void pollStatus();
    void load(bool refresh);
    void push();
    void toggle(const std::string &id, bool on);
    void apply(snippets::Plan plan, const std::string &reason, bool quiet);
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});
    std::shared_ptr<slint::VectorModel<CodeLine>> code(const snippets::Snippet &s, int &columns);

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
    std::string query_;
    int category_ = 0;                  // 0: all, else categories_[category_ - 1]
    std::vector<std::string> categories_;
    bool busy_ = false;
    std::string busyId_;
    uint64_t gen_ = 0;

    slint::Timer statusPoll_;
    slint::Timer toastTimer_;
    Worker worker_; // last: joined first
};
