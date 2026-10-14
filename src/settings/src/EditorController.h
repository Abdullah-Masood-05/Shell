#pragma once

// The Config Editor page (ui/pages/editor.slint, EditorState): the import tree,
// a highlighted read view, a highlighted edit view (a TextInput with invisible
// glyphs over the highlighted rows; tabs shown as blank glyphs, see
// Highlight.h), a live `-config:check` of the edited tree, saving through
// nss::ConfigStore, and external-change handling.
// Heavy work runs on its own Worker; results come back on the UI thread.

#include "app.h"

#include "EditorSession.h"
#include "Highlight.h"
#include "FileWatcher.h"
#include "Worker.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class EditorController
{
public:
    explicit EditorController(slint::ComponentHandle<AppWindow> ui);
    ~EditorController();

    // Development: open the edit view on start (--editor-edit).
    void setStartInEditMode(bool edit) { startEditing_ = edit; }

private:
    struct Problem
    {
        std::string source; // check | log
        bool error = true;
        std::filesystem::path file;
        int line = 0;
        int column = 0;
        std::string message;
    };

    void bindCallbacks();
    void pollStatus();
    void loadRoot(const std::filesystem::path &root, bool shared);
    void reloadTree();
    void applyTree(std::vector<editor::FileEntry> entries);
    void selectFile(int index);
    void selectPath(const std::filesystem::path &path);
    void showCurrent();
    void rehighlight();
    void rehighlightEdit();
    std::map<int, int> issueMarks(const editor::Buffer &b) const;
    void setEditText(size_t cursor);
    void fixClipboard();
    void pushFiles();
    void pushProblems();
    void updateDirty();
    int onEdited(std::string display);
    void scheduleCheck(int delayMs);
    void runCheck();
    void save(bool force);
    void discard();
    void openExternal();
    void onExternalChange(const std::vector<std::filesystem::path> &paths);
    void reloadBuffers(const std::vector<std::string> &keys);
    void jumpTo(const std::filesystem::path &file, int line);
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});

    editor::Buffer *currentBuffer();
    bool readOnly(const editor::FileEntry &e, std::string *reason = nullptr) const;
    std::string labelOf(const std::filesystem::path &p) const;
    std::vector<editor::SaveRequest> dirtyRequests(bool writableOnly) const;

    slint::ComponentHandle<AppWindow> ui_;
    std::filesystem::path shellExe_;
    std::filesystem::path root_;
    std::string statusPath_;      // AppState.status.config-path last seen
    bool shared_ = false;
    bool startEditing_ = false;
    bool replayJump_ = false; // a jump landed while the page was hidden: show it once it is up
    std::vector<editor::FileEntry> entries_;
    std::map<std::string, editor::Buffer> buffers_; // pathKey -> buffer
    int current_ = -1;
    std::vector<editor::Issue> checkIssues_;
    bool checkErrors_ = false;
    uint64_t checkGen_ = 0;
    uint64_t loadGen_ = 0;
    std::vector<Problem> problems_;
    std::string logSignature_;
    std::set<std::string> externalPending_;
    std::optional<std::pair<std::filesystem::path, int>> pendingJump_;
    bool saving_ = false;
    bool lastDirty_ = false;

    // Edit view: the current buffer as shown (tabs as glyphs) and its rows.
    hl::TabGlyphs glyphs_;
    std::string display_;
    std::vector<hl::Line> editLines_;
    std::vector<int> editMarks_;
    std::shared_ptr<slint::VectorModel<CodeLine>> editModel_;
    slint::Timer highlightTimer_;

    slint::Timer statusPoll_;
    slint::Timer checkTimer_;
    slint::Timer toastTimer_;
    slint::Timer replayTimer_;
    FileWatcher watcher_;
    Worker worker_; // last: joined first
};
