#pragma once

// The Config Editor page (ui/pages/editor.slint, EditorState): the import tree,
// a highlighted read view, a highlighted edit view (a TextInput with invisible
// glyphs over the highlighted rows; tabs shown as blank glyphs, see
// Highlight.h; each file's text, caret and undo history in a code::Document,
// see CodeDocument.h), a live `-config:check` of the edited tree, saving
// through nss::ConfigStore, and external-change handling.
// Heavy work runs on its own Worker; results come back on the UI thread.

#include "app.h"

#include "CodeEditorBridge.h"
#include "EditorSession.h"
#include "Highlight.h"
#include "FileWatcher.h"
#include "Worker.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "WindowPoll.h"

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
    void showDoc();
    void rememberView();
    void afterEdit(const code::Result &r);
    EditResult onEdited(const std::string &display, int cursor, int beforeAnchor, int beforeCursor);
    EditResult onCommand(const std::string &name, int anchor, int cursor);
    void applyDevSelection();
    void pushFiles();
    void pushProblems();
    void updateDirty();
    void scheduleCheck(int delayMs);
    void runCheck();
    void save(bool force);
    void discard();
    void openExternal();
    void onExternalChange(const std::vector<std::filesystem::path> &paths);
    void reloadBuffers(const std::vector<std::string> &keys);
    void jumpTo(const std::filesystem::path &file, int line);
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});
    // Text that stays on screen is kept as a function, so a language change can push it again.
    using TextFn = std::function<std::string()>;
    void setCheckText(TextFn make);
    void setLoadError(TextFn make);
    void setExternalText(TextFn make);

    editor::Buffer *currentBuffer();
    code::Document *currentDoc(); // the current buffer's, created on first use
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
    TextFn checkText_, loadError_, externalText_;

    // Edit view: its rows, and the buffer whose text it shows (pathKey).
    codeui::Rows rows_;
    std::string highlighted_; // the text the rows show (big files patch them while typing)
    std::string shownKey_;
    slint::Timer highlightTimer_;
    slint::Timer viewTimer_;
    // --editor-select <line>-<line> and --editor-find <text> (screenshots).
    int devSelectFirst_ = 0, devSelectLast_ = 0;
    std::string devFind_;

    WindowPoll statusPoll_;
    slint::Timer checkTimer_;
    slint::Timer toastTimer_;
    slint::Timer replayTimer_;
    FileWatcher watcher_;
    Worker worker_; // last: joined first
};
