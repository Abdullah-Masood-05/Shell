#include "EditorController.h"

#include "Highlight.h"
#include "ShellCli.h"
#include "Tr.h"

#include "nss/Text.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }
std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }

bool fontInstalled(const wchar_t *face)
{
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &lf, [](const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool *>(p) = true;
            return 0;
        },
        LPARAM(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

bool isUnder(const fs::path &child, const fs::path &parent)
{
    auto c = nss::pathKey(child), p = nss::pathKey(parent);
    if (p.empty())
        return false;
    if (p.back() != '\\')
        p += '\\';
    return c.rfind(p, 0) == 0;
}

} // namespace

EditorController::EditorController(slint::ComponentHandle<AppWindow> ui)
    : ui_(std::move(ui)), shellExe_(ShellCli::locate())
{
    const char *mono = fontInstalled(L"Cascadia Mono") ? "Cascadia Mono" : "Consolas";
    ui_->global<EditorState>().set_mono_font(mono);
    ui_->global<BackupsState>().set_mono_font(mono);
    ui_->global<EditorState>().set_check_state("idle");
    // Development switches (see main.cpp): --editor-edit, --editor-open <file> [--editor-line <n>].
    int argc = 0;
    if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        fs::path open;
        int line = 0;
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--editor-edit") == 0)
                startEditing_ = true;
            else if (_wcsicmp(argv[i], L"--editor-open") == 0 && i + 1 < argc)
                open = argv[++i];
            else if (_wcsicmp(argv[i], L"--editor-line") == 0 && i + 1 < argc)
                line = _wtoi(argv[++i]);
            else if (_wcsicmp(argv[i], L"--editor-debug-overlay") == 0)
                ui_->global<EditorState>().set_debug_overlay(true);
            else if (_wcsicmp(argv[i], L"--editor-select") == 0 && i + 1 < argc) {
                // <first>-<last>: select those lines (screenshots)
                std::wstring v = argv[++i];
                auto dash = v.find(L'-');
                devSelectFirst_ = _wtoi(v.substr(0, dash).c_str());
                devSelectLast_ = dash == std::wstring::npos ? devSelectFirst_ : _wtoi(v.substr(dash + 1).c_str());
            } else if (_wcsicmp(argv[i], L"--editor-find") == 0 && i + 1 < argc)
                devFind_ = nss::narrow(argv[++i]);
        }
        if (!open.empty())
            pendingJump_ = { open, line };
        LocalFree(argv);
    }
    ui_->global<EditorState>().set_edit_lines(rows_.model());
    // Shared by every CodeEditor (this page and the Menu map's "Edit code").
    ui_->global<CodeText>().on_selection([](slint::SharedString text, int anchor, int cursor) {
        return codeui::selectionRows(std::string_view(text), anchor, cursor);
    });
    bindCallbacks();
    // The active config comes from AppState.status (shell.exe -status, run by
    // AppController); follow it, so "Create personal config" switches over.
    statusPoll_.start(slint::TimerMode::Repeated, 500ms, [this] { pollStatus(); });
    nsp::onLanguageChanged([this] {
        const auto &st = ui_->global<EditorState>();
        if (checkText_)
            st.set_check_text(ss(checkText_()));
        if (loadError_)
            st.set_load_error(ss(loadError_()));
        if (externalText_)
            st.set_external_text(ss(externalText_()));
        if (current_ >= 0 && current_ < int(entries_.size())) {
            std::string reason;
            readOnly(entries_[size_t(current_)], &reason);
            st.set_read_only_reason(ss(reason));
        }
    });
}

EditorController::~EditorController() { watcher_.stop(); }

void EditorController::bindCallbacks()
{
    const auto &st = ui_->global<EditorState>();
    st.on_activated([this] {
        // A new page: put the edit view back where the user left it.
        rememberView();
        if (!replayJump_ && !shownKey_.empty())
            viewTimer_.start(slint::TimerMode::SingleShot, 120ms, [this] {
                if (auto *d = currentDoc(); d && d->view.saved) {
                    const auto &es = ui_->global<EditorState>();
                    auto sel = d->toDisplay(d->view.selection);
                    es.set_caret_anchor(int(sel.anchor));
                    es.set_caret_offset(int(sel.cursor));
                    es.set_caret_scroll(true);
                    es.set_caret_scroll_x(d->view.x);
                    es.set_caret_scroll_y(d->view.y);
                    es.set_caret_serial(es.get_caret_serial() + 1);
                }
            });
        pollStatus();
        if (startEditing_) {
            startEditing_ = false;
            ui_->global<EditorState>().set_editing(true);
        }
        // Home's diagnostics and "Edit in Config Editor" links jump before the page
        // exists, so it never saw the change: scroll there once it is laid out.
        if (replayJump_) {
            replayJump_ = false;
            replayTimer_.start(slint::TimerMode::SingleShot, 120ms, [this] {
                const auto &es = ui_->global<EditorState>();
                es.set_jump_serial(es.get_jump_serial() + 1);
            });
        }
    });
    st.on_select_file([this](int i) { selectFile(i); });
    st.on_set_editing([this](bool editing) {
        ui_->global<EditorState>().set_editing(editing);
        if (!editing)
            rehighlight();
    });
    st.on_edited([this](slint::SharedString text, int cursor, int beforeAnchor, int beforeCursor) {
        return onEdited(str(text), cursor, beforeAnchor, beforeCursor);
    });
    st.on_command([this](slint::SharedString name, int anchor, int cursor) { return onCommand(str(name), anchor, cursor); });
    st.on_find([this](slint::SharedString query, int anchor, int cursor, bool backwards, bool incremental) {
        auto *d = currentDoc();
        return d ? codeui::find(*d, str(query), anchor, cursor, backwards, incremental) : FindResult{};
    });
    st.on_selected_text([this](int anchor, int cursor) {
        auto *d = currentDoc();
        return ss(d ? codeui::findSeed(*d, anchor, cursor) : std::string{});
    });
    st.on_save([this] { save(false); });
    st.on_save_anyway([this] {
        ui_->global<EditorState>().set_confirm_force(false);
        save(true);
    });
    st.on_cancel_force([this] { ui_->global<EditorState>().set_confirm_force(false); });
    st.on_discard([this] { discard(); });
    st.on_open_external([this] { openExternal(); });
    st.on_reload_external([this] {
        std::vector<std::string> keys(externalPending_.begin(), externalPending_.end());
        externalPending_.clear();
        ui_->global<EditorState>().set_external_change(false);
        reloadBuffers(keys);
    });
    st.on_keep_mine([this] {
        externalPending_.clear();
        ui_->global<EditorState>().set_external_change(false);
    });
    st.on_jump([this](int i) {
        if (i >= 0 && i < int(problems_.size()))
            jumpTo(problems_[size_t(i)].file, problems_[size_t(i)].line);
    });
    st.on_open_at([this](slint::SharedString file, int line) {
        fs::path p(nss::widen(str(file)));
        if (root_.empty())
            pendingJump_ = { p, line };
        else
            jumpTo(p, line);
    });
    st.on_create_user_config([this] { ui_->global<AppState>().invoke_action("config-init"); });
}

void EditorController::toast(const std::string &kind, const std::string &title, const std::string &text)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(text));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

void EditorController::setCheckText(TextFn make)
{
    checkText_ = std::move(make);
    ui_->global<EditorState>().set_check_text(ss(checkText_ ? checkText_() : std::string{}));
}

void EditorController::setLoadError(TextFn make)
{
    loadError_ = std::move(make);
    ui_->global<EditorState>().set_load_error(ss(loadError_ ? loadError_() : std::string{}));
}

void EditorController::setExternalText(TextFn make)
{
    externalText_ = std::move(make);
    ui_->global<EditorState>().set_external_text(ss(externalText_ ? externalText_() : std::string{}));
}

// ---------------------------------------------------------------------------
// Loading

void EditorController::pollStatus()
{
    const auto &app = ui_->global<AppState>();
    auto status = app.get_status();
    if (!status.loaded)
        return;
    auto path = str(status.config_path);
    if (path != statusPath_ && status.config_exists && !path.empty()) {
        statusPath_ = path;
        loadRoot(fs::path(nss::widen(path)), str(status.config_source) == "install");
    } else if (path != statusPath_ && !status.config_exists) {
        statusPath_ = path;
        setLoadError([path] { return nsp::trf("The config file doesn't exist: {}", path); });
    }

    // shell.log errors (AppState.diagnostics) join the problems list.
    auto diags = app.get_diagnostics();
    std::string sig;
    if (diags)
        for (size_t i = 0; i < diags->row_count(); ++i)
            if (auto d = diags->row_data(i))
                sig += str(d->time) + str(d->message) + "\n";
    if (sig != logSignature_) {
        logSignature_ = sig;
        pushProblems();
    }
}

void EditorController::loadRoot(const fs::path &root, bool shared)
{
    root_ = root.lexically_normal();
    shared_ = shared || editor::isProtectedPath(root_);
    buffers_.clear();
    entries_.clear();
    checkIssues_.clear();
    externalPending_.clear();
    current_ = -1;
    const auto &st = ui_->global<EditorState>();
    st.set_shared_config(shared_);
    st.set_external_change(false);
    setLoadError(nullptr);
    st.set_root_label(ss(nss::narrow(root_.parent_path().wstring())));
    updateDirty();

    // Watch the config folder for changes made by other programs.
    watcher_.stop();
    if (!shared_) {
        auto backupsDir = root_.parent_path() / L"backups";
        watcher_.start(
            root_.parent_path(),
            [this](std::vector<fs::path> changed) {
                slint::invoke_from_event_loop([this, changed = std::move(changed)] { onExternalChange(changed); });
            },
            [backupsDir](const fs::path &p) {
                return nss::iequals(nss::narrow(p.extension().wstring()), ".nss") && !isUnder(p, backupsDir);
            });
    }
    reloadTree();
}

void EditorController::reloadTree()
{
    auto gen = ++loadGen_;
    auto root = root_;
    auto appDir = shellExe_.empty() ? fs::path() : shellExe_.parent_path();
    worker_.post([this, gen, root, appDir] {
        auto entries = editor::loadTree(root, appDir);
        slint::invoke_from_event_loop([this, gen, entries = std::move(entries)]() mutable {
            if (gen != loadGen_)
                return;
            applyTree(std::move(entries));
        });
    });
}

void EditorController::applyTree(std::vector<editor::FileEntry> entries)
{
    fs::path keep = current_ >= 0 && current_ < int(entries_.size()) ? entries_[size_t(current_)].path : root_;
    entries_ = std::move(entries);
    const auto &st = ui_->global<EditorState>();
    st.set_loaded(true);
    if (entries_.empty() || entries_[0].missing) {
        if (entries_.empty())
            setLoadError([] { return std::string(nsp::tr("Nothing to show.")); });
        else
            setLoadError([note = entries_[0].note] { return note; });
    }
    current_ = -1;
    selectPath(keep);
    if (current_ < 0 && !entries_.empty())
        selectFile(0);
    if (pendingJump_) {
        auto [file, line] = *pendingJump_;
        pendingJump_.reset();
        jumpTo(file, line);
    }
    applyDevSelection();
    scheduleCheck(0);
}

bool EditorController::readOnly(const editor::FileEntry &e, std::string *reason) const
{
    auto set = [&](const char *r) {
        if (reason)
            *reason = r;
        return true;
    };
    if (shared_)
        return set(nsp::tr("Shared config: read-only"));
    if (e.managed)
        return set(nsp::tr("Managed by the app"));
    if (e.missing || e.unresolved)
        return set(nsp::tr("Not found"));
    if (!isUnder(e.path, root_.parent_path()))
        return set(nsp::tr("Outside your config folder"));
    if (editor::isProtectedPath(e.path))
        return set(nsp::tr("Read-only"));
    return false;
}

std::string EditorController::labelOf(const fs::path &p) const
{
    if (isUnder(p, root_.parent_path()))
        return nss::narrow(p.lexically_relative(root_.parent_path()).generic_wstring());
    return nss::narrow(p.filename().wstring());
}

editor::Buffer *EditorController::currentBuffer()
{
    if (current_ < 0 || current_ >= int(entries_.size()))
        return nullptr;
    auto it = buffers_.find(nss::pathKey(entries_[size_t(current_)].path));
    return it == buffers_.end() ? nullptr : &it->second;
}

code::Document *EditorController::currentDoc()
{
    auto *b = currentBuffer();
    if (!b)
        return nullptr;
    if (!b->doc)
        b->doc = std::make_shared<code::Document>(b->text);
    return b->doc.get();
}

void EditorController::selectPath(const fs::path &path)
{
    auto key = nss::pathKey(path);
    for (size_t i = 0; i < entries_.size(); ++i)
        if (!entries_[i].unresolved && nss::pathKey(entries_[i].path) == key) {
            selectFile(int(i));
            return;
        }
}

void EditorController::selectFile(int index)
{
    if (index < 0 || index >= int(entries_.size()) || entries_[size_t(index)].unresolved)
        return;
    const auto &e = entries_[size_t(index)];
    auto key = nss::pathKey(e.path);
    if (!buffers_.count(key) && !e.missing) {
        std::string error;
        if (auto b = editor::loadBuffer(e.path, &error))
            buffers_.emplace(key, std::move(*b));
        else
            toast("error", nsp::trf("Couldn't open {}", e.name), error);
    }
    current_ = index;
    showCurrent();
}

void EditorController::showCurrent()
{
    const auto &st = ui_->global<EditorState>();
    rememberView(); // of the file shown until now
    st.set_current(current_);
    st.set_highlight_line(0);
    if (current_ < 0) {
        st.set_current_label("");
        st.set_current_path("");
        showDoc();
        st.set_lines(std::make_shared<slint::VectorModel<CodeLine>>());
        rehighlightEdit();
        return;
    }
    const auto &e = entries_[size_t(current_)];
    std::string reason;
    bool ro = readOnly(e, &reason);
    st.set_current_label(ss(e.label));
    st.set_current_path(ss(nss::narrow(e.path.wstring())));
    st.set_read_only(ro);
    st.set_read_only_reason(ss(reason));
    showDoc();
    updateDirty();
    rehighlight();
    rehighlightEdit();
}

// The edit view's TextInput gets the current file's text, with the caret and
// scroll position the file had when the user left it (else at the top).
void EditorController::showDoc()
{
    const auto &st = ui_->global<EditorState>();
    auto *d = currentDoc();
    code::Selection sel;
    float x = 0, y = 0;
    if (d && d->view.saved) {
        sel = d->toDisplay(d->view.selection);
        x = d->view.x;
        y = d->view.y;
    }
    st.set_text(ss(d ? d->display() : std::string{}));
    st.set_caret_anchor(int(sel.anchor));
    st.set_caret_offset(int(sel.cursor));
    st.set_caret_scroll(true);
    st.set_caret_scroll_x(x);
    st.set_caret_scroll_y(y);
    st.set_caret_serial(st.get_caret_serial() + 1);
    shownKey_ = d && current_ >= 0 ? nss::pathKey(entries_[size_t(current_)].path) : std::string{};
}

// Notes where the edit view was in the file it shows (EditorState.view-*,
// kept up to date by the page), before it shows another one.
void EditorController::rememberView()
{
    if (shownKey_.empty())
        return;
    auto it = buffers_.find(shownKey_);
    if (it == buffers_.end() || !it->second.doc)
        return;
    auto &d = *it->second.doc;
    const auto &st = ui_->global<EditorState>();
    d.view.saved = true;
    d.view.selection = d.toText(codeui::selection(st.get_view_anchor(), st.get_view_cursor()));
    d.view.x = st.get_view_x();
    d.view.y = st.get_view_y();
    d.closeGroup();
}

std::map<int, int> EditorController::issueMarks(const editor::Buffer &b) const
{
    // Lines with problems in this file get a marker in the gutter.
    std::map<int, int> marks;
    auto key = nss::pathKey(b.path);
    for (const auto &p : problems_)
        if (p.line > 0 && nss::pathKey(p.file) == key)
            marks[p.line] = std::max(marks[p.line], p.error ? 2 : 1);
    return marks;
}

// The edit view's rows: the display text, highlighted. Only the rows that
// changed are replaced, so typing doesn't rebuild the visible rows.
void EditorController::rehighlightEdit()
{
    highlightTimer_.stop();
    auto *b = currentBuffer();
    auto *d = currentDoc();
    if (!b || !d) {
        rows_.clear();
        highlighted_.clear();
        return;
    }
    highlighted_ = d->text();
    auto lines = hl::highlight(d->text(), d->tabWidth(), &d->glyphs());
    std::vector<int> marks(lines.size(), 0);
    for (auto [line, mark] : issueMarks(*b))
        if (line >= 1 && line <= int(marks.size()))
            marks[size_t(line - 1)] = mark;
    rows_.update(std::move(lines), std::move(marks));
}

void EditorController::rehighlight()
{
    const auto &st = ui_->global<EditorState>();
    auto *b = currentBuffer();
    auto rows = std::make_shared<slint::VectorModel<CodeLine>>();
    if (!b) {
        st.set_lines(rows);
        return;
    }
    auto marks = issueMarks(*b);

    auto lines = hl::highlight(b->text);
    size_t maxColumns = 0;
    int number = 0;
    for (const auto &line : lines) {
        ++number;
        auto spans = std::make_shared<slint::VectorModel<CodeSpan>>();
        size_t columns = 0;
        for (const auto &s : line.spans) {
            spans->push_back(CodeSpan{ ss(s.text), s.kind });
            columns += s.text.size();
        }
        maxColumns = std::max(maxColumns, columns);
        auto m = marks.find(number);
        rows->push_back(CodeLine{ number, spans, m == marks.end() ? 0 : m->second });
    }
    st.set_max_columns(int(std::min<size_t>(maxColumns, 2000)));
    st.set_lines(rows);
}

void EditorController::pushFiles()
{
    std::map<std::string, int> errors;
    for (const auto &p : problems_)
        if (p.error && !p.file.empty())
            errors[nss::pathKey(p.file)]++;
    std::vector<EditorFile> rows;
    rows.reserve(entries_.size());
    for (const auto &e : entries_) {
        auto key = nss::pathKey(e.path);
        auto b = buffers_.find(key);
        auto err = errors.find(key);
        rows.push_back(EditorFile{
            ss(nss::narrow(e.path.wstring())), ss(e.label), ss(e.name), e.depth, e.managed, e.missing,
            e.unresolved, !e.unresolved && !isUnder(e.path, root_.parent_path()),
            b != buffers_.end() && b->second.dirty(), e.unresolved || err == errors.end() ? 0 : err->second,
            ss(e.note) });
    }
    ui_->global<EditorState>().set_files(std::make_shared<slint::VectorModel<EditorFile>>(rows));
}

void EditorController::updateDirty()
{
    int count = 0;
    for (const auto &[k, b] : buffers_)
        count += b.dirty();
    auto *cur = currentBuffer();
    const auto &st = ui_->global<EditorState>();
    st.set_dirty(cur && cur->dirty());
    st.set_dirty_count(count);
    pushFiles();
}

void EditorController::pushProblems()
{
    problems_.clear();
    for (const auto &i : checkIssues_)
        problems_.push_back(Problem{ "check", i.error, i.file, i.line, i.column, i.message });

    // shell.log: parser errors from the last time Explorer loaded the config.
    auto diags = ui_->global<AppState>().get_diagnostics();
    if (diags && !root_.empty()) {
        for (size_t i = 0; i < diags->row_count() && problems_.size() < 200; ++i) {
            auto d = diags->row_data(i);
            if (!d || str(d->level) != "error" || d->file.empty())
                continue;
            fs::path f(nss::widen(str(d->file)));
            if (!f.is_absolute())
                f = root_.parent_path() / f;
            problems_.push_back(Problem{ "log", true, f.lexically_normal(), d->line, d->column,
                                         str(d->message) + " (" + str(d->time) + ")" });
        }
    }

    std::vector<EditorProblem> rows;
    for (const auto &p : problems_)
        rows.push_back(EditorProblem{ ss(p.source), p.error, ss(nss::narrow(p.file.wstring())),
                                      ss(p.file.empty() ? std::string{} : labelOf(p.file)), p.line, p.column,
                                      ss(p.message) });
    ui_->global<EditorState>().set_problems(std::make_shared<slint::VectorModel<EditorProblem>>(rows));
    pushFiles();
    if (!ui_->global<EditorState>().get_editing())
        rehighlight();
    rehighlightEdit();
}

// ---------------------------------------------------------------------------
// Editing and the live check

// The TextInput changed its text (typing, Backspace, Delete, an IME). The
// text is written back only when what it shows must change.
EditResult EditorController::onEdited(const std::string &display, int cursor, int beforeAnchor, int beforeCursor)
{
    auto *d = currentDoc();
    if (!d)
        return {};
    auto r = d->input(display, size_t(std::max(0, cursor)), codeui::selection(beforeAnchor, beforeCursor));
    afterEdit(r);
    return codeui::toSlint(r);
}

// Undo, redo, clipboard, Tab, Shift+Tab, Enter (see codeui::command).
EditResult EditorController::onCommand(const std::string &name, int anchor, int cursor)
{
    auto *d = currentDoc();
    if (!d || current_ < 0)
        return {};
    auto r = codeui::command(*d, name, anchor, cursor, readOnly(entries_[size_t(current_)]));
    afterEdit(r);
    return codeui::toSlint(r);
}

void EditorController::afterEdit(const code::Result &r)
{
    auto *b = currentBuffer();
    auto *d = currentDoc();
    if (!b || !d)
        return;
    const auto &st = ui_->global<EditorState>();
    if (r.display)
        st.set_text(ss(d->display()));
    if (!r.edited)
        return;
    bool wasDirty = b->dirty();
    b->text = d->text();
    if (wasDirty != b->dirty())
        updateDirty();
    else
        st.set_dirty(b->dirty());
    st.set_highlight_line(0);
    // The glyphs on screen are the highlighted rows, so they follow every key.
    // Small files are highlighted whole; in big ones only the changed lines,
    // and the whole file once typing pauses.
    if (b->text.size() < 48 * 1024 || r.display) {
        rehighlightEdit();
    } else {
        rows_.patch(hl::highlightPatch(highlighted_, d->text(), d->tabWidth(), &d->glyphs()));
        highlighted_ = d->text();
        highlightTimer_.start(slint::TimerMode::SingleShot, 300ms, [this] { rehighlightEdit(); });
    }
    scheduleCheck(800);
}

void EditorController::scheduleCheck(int delayMs)
{
    checkTimer_.start(slint::TimerMode::SingleShot, std::chrono::milliseconds(delayMs), [this] { runCheck(); });
}

std::vector<editor::SaveRequest> EditorController::dirtyRequests(bool writableOnly) const
{
    std::vector<editor::SaveRequest> out;
    for (const auto &[key, b] : buffers_) {
        if (!b.dirty())
            continue;
        if (writableOnly) {
            auto it = std::find_if(entries_.begin(), entries_.end(),
                                   [&](const editor::FileEntry &e) { return nss::pathKey(e.path) == key; });
            if (it == entries_.end() || readOnly(*it))
                continue;
        }
        out.push_back(editor::SaveRequest{ b.path, b.text, b.format });
    }
    return out;
}

void EditorController::runCheck()
{
    const auto &st = ui_->global<EditorState>();
    if (root_.empty())
        return;
    if (shellExe_.empty()) {
        st.set_check_state("failed");
        setCheckText([] { return std::string(nsp::tr("shell.exe was not found, so the config can't be checked")); });
        return;
    }
    auto gen = ++checkGen_;
    auto overlays = dirtyRequests(false);
    st.set_check_state("checking");
    setCheckText([edited = !overlays.empty()] {
        return std::string(edited ? nsp::tr("Checking your edits…") : nsp::tr("Checking…"));
    });
    worker_.post([this, gen, overlays, root = root_, exe = shellExe_] {
        auto outcome = editor::checkTree(exe, root, overlays);
        slint::invoke_from_event_loop([this, gen, outcome = std::move(outcome), edited = !overlays.empty()] {
            if (gen != checkGen_)
                return;
            const auto &st = ui_->global<EditorState>();
            checkIssues_ = outcome.issues;
            int errors = int(std::count_if(checkIssues_.begin(), checkIssues_.end(),
                                           [](const editor::Issue &i) { return i.error; }));
            checkErrors_ = outcome.ran && errors > 0;
            st.set_error_count(errors);
            if (!outcome.ran) {
                st.set_check_state("failed");
                setCheckText([failure = outcome.failure.substr(0, 200)] {
                    return nsp::trf("The check couldn't run: {}", failure);
                });
            } else if (errors > 0) {
                st.set_check_state("errors");
                setCheckText([errors, edited] {
                    return edited ? nsp::trn("{n} error in your edits", "{n} errors in your edits", errors)
                                  : nsp::trn("{n} error", "{n} errors", errors);
                });
            } else {
                st.set_check_state("ok");
                setCheckText([edited] {
                    return std::string(edited ? nsp::tr("Your edits pass the check") : nsp::tr("The config passes the check"));
                });
            }
            pushProblems();
        });
    });
}

// ---------------------------------------------------------------------------
// Save, discard, external editor

void EditorController::save(bool force)
{
    if (saving_ || root_.empty())
        return;
    const auto &st = ui_->global<EditorState>();
    auto requests = dirtyRequests(true);
    if (requests.empty()) {
        if (!dirtyRequests(false).empty())
            toast("info", nsp::tr("Nothing to save"), nsp::tr("These files are read-only here."));
        return;
    }
    if (shellExe_.empty() && !force) {
        toast("error", nsp::tr("Can't check the config"), nsp::tr("shell.exe was not found."));
        return;
    }
    // Errors from the live check: ask before writing a config that turns the menu off.
    if (!force && checkErrors_ && st.get_check_state() == "errors") {
        const editor::Issue *first = nullptr;
        for (const auto &i : checkIssues_)
            if (i.error) {
                first = &i;
                break;
            }
        std::string where = first && !first->file.empty()
                                ? " (" + labelOf(first->file) + ":" + std::to_string(first->line) + ")"
                                : std::string{};
        st.set_confirm_text(ss(nsp::trn("The check found {n} error: {0}.\n\nShell turns the whole context menu off while the config has an error, until it's fixed and saved again. A snapshot is taken first either way.",
                                        "The check found {n} errors: {0}.\n\nShell turns the whole context menu off while the config has an error, until it's fixed and saved again. A snapshot is taken first either way.",
                                        st.get_error_count(),
                                        first ? first->message + where : std::string(nsp::tr("see Problems")))));
        st.set_confirm_force(true);
        return;
    }

    saving_ = true;
    st.set_saving(true);
    // Note the exact bytes about to be written, so the watcher ignores our own save.
    for (const auto &r : requests)
        watcher_.noteOwnWrite(r.path, nss::encode(editor::fromEditorText(r.text, r.format.eol), r.format));

    worker_.post([this, requests, force, root = root_, exe = shellExe_] {
        auto result = editor::save(exe, root, requests, force);
        slint::invoke_from_event_loop([this, requests, result = std::move(result), force] {
            saving_ = false;
            const auto &st = ui_->global<EditorState>();
            st.set_saving(false);
            if (result.ok) {
                for (const auto &r : requests) {
                    auto it = buffers_.find(nss::pathKey(r.path));
                    if (it != buffers_.end()) {
                        it->second.original = r.text;
                        it->second.diskHash = FileWatcher::hashFile(r.path);
                    }
                }
                updateDirty();
                std::string title = requests.size() == 1
                                        ? nsp::trf("Saved {}", labelOf(requests[0].path))
                                        : nsp::trn("Saved {n} file", "Saved {n} files", (long long)requests.size());
                std::string body = result.snapshotId.empty() ? std::string(nsp::tr("No changes were needed."))
                                                             : nsp::trf("Snapshot {} was taken first.", result.snapshotId);
                if (force && !result.snapshotId.empty())
                    body += std::string(" ") + nsp::tr("The config has errors; Shell's menu stays off until they're fixed.");
                toast(force ? "warning" : "ok", title, body);
                reloadTree(); // imports may have changed
                return;
            }
            if (!result.check.ok && !result.check.message.empty()) {
                auto outcome = editor::parseCheck(result.check.message, {}, root_.parent_path());
                if (outcome.ran && !outcome.ok) {
                    checkIssues_ = outcome.issues;
                    checkErrors_ = true;
                    int errors = int(std::count_if(checkIssues_.begin(), checkIssues_.end(),
                                                   [](const editor::Issue &i) { return i.error; }));
                    st.set_error_count(errors);
                    st.set_check_state("errors");
                    setCheckText([errors] { return nsp::trn("{n} error in your edits", "{n} errors in your edits", errors); });
                    pushProblems();
                    save(false); // shows the "save anyway" dialog
                    return;
                }
            }
            toast("error", nsp::tr("Couldn't save"), result.error);
        });
    });
}

void EditorController::discard()
{
    // Through the document where there is one, so Ctrl+Z brings the edits back.
    for (auto &[k, b] : buffers_) {
        if (b.doc && b.doc->text() != b.original) {
            b.doc->replace({ 0, b.doc->display().size() }, b.original);
            b.doc->closeGroup();
        }
        b.text = b.original;
    }
    showCurrent();
    scheduleCheck(0);
}

void EditorController::openExternal()
{
    if (current_ < 0)
        return;
    auto path = entries_[size_t(current_)].path.wstring();
    worker_.post([path] {
        auto rc = INT_PTR(ShellExecuteW(nullptr, L"edit", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (rc <= 32) {
            auto quoted = ShellCli::quote(path);
            ShellExecuteW(nullptr, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
        }
    });
}

void EditorController::onExternalChange(const std::vector<fs::path> &paths)
{
    if (root_.empty())
        return;
    bool all = std::any_of(paths.begin(), paths.end(),
                           [&](const fs::path &p) { return nss::pathKey(p) == nss::pathKey(root_.parent_path()); });
    std::vector<std::string> reload;
    std::vector<std::string> conflicts;
    for (auto &[key, b] : buffers_) {
        bool hit = all || std::any_of(paths.begin(), paths.end(),
                                      [&](const fs::path &p) { return nss::pathKey(p) == key; });
        if (!hit || FileWatcher::hashFile(b.path) == b.diskHash)
            continue;
        if (b.dirty()) {
            externalPending_.insert(key);
            conflicts.push_back(labelOf(b.path));
        } else {
            reload.push_back(key);
        }
    }
    if (!reload.empty())
        reloadBuffers(reload);
    if (!conflicts.empty()) {
        std::string names;
        for (const auto &n : conflicts)
            names += (names.empty() ? "" : ", ") + n;
        setExternalText([names] {
            return nsp::trf("{} changed on disk while you have unsaved edits. Reload to see the new version (your edits are dropped), or close this to keep editing yours.",
                            names);
        });
        ui_->global<EditorState>().set_external_change(true);
    }
    reloadTree();
}

void EditorController::reloadBuffers(const std::vector<std::string> &keys)
{
    bool currentChanged = false;
    auto *cur = currentBuffer();
    for (const auto &key : keys) {
        auto it = buffers_.find(key);
        if (it == buffers_.end())
            continue;
        if (cur == &it->second)
            currentChanged = true;
        std::string error;
        if (auto b = editor::loadBuffer(it->second.path, &error)) {
            auto view = it->second.doc ? it->second.doc->view : code::Document::View{};
            it->second = std::move(*b);
            if (view.saved) {
                it->second.doc = std::make_shared<code::Document>(it->second.text);
                it->second.doc->view = view;
            }
        } else
            buffers_.erase(it); // gone: the tree reload shows it as missing
    }
    if (currentChanged)
        showCurrent();
    else
        updateDirty();
    scheduleCheck(300);
}

void EditorController::jumpTo(const fs::path &file, int line)
{
    if (!file.empty()) {
        auto before = current_;
        selectPath(file);
        if (current_ < 0 || nss::pathKey(entries_[size_t(current_)].path) != nss::pathKey(file)) {
            if (before != current_)
                showCurrent();
            toast("info", nsp::tr("That file isn't in the import tree"), nss::narrow(file.wstring()));
            return;
        }
    }
    auto *d = currentDoc();
    if (!d || line <= 0)
        return;
    auto [begin, end] = editor::lineOffsets(d->display(), line); // offsets into the edit view's text
    const auto &st = ui_->global<EditorState>();
    st.set_caret_scroll(false); // the jump scrolls to the line
    st.set_highlight_line(line);
    st.set_jump_line(line);
    st.set_jump_start(int(begin));
    st.set_jump_end(int(end));
    // Opening the file just bumped caret-serial too; whichever the page sees last selects the line.
    st.set_caret_anchor(int(end));
    st.set_caret_offset(int(begin));
    st.set_jump_serial(st.get_jump_serial() + 1);
    replayJump_ = ui_->global<AppState>().get_page() != ui_->global<Pages>().get_editor();
}

// --editor-select and --editor-find (screenshots of selected text).
void EditorController::applyDevSelection()
{
    auto *d = currentDoc();
    if (!d)
        return;
    const auto &st = ui_->global<EditorState>();
    if (devSelectFirst_ > 0) {
        auto first = editor::lineOffsets(d->display(), devSelectFirst_).first;
        auto last = editor::lineOffsets(d->display(), std::max(devSelectFirst_, devSelectLast_)).second;
        devSelectFirst_ = 0;
        viewTimer_.start(slint::TimerMode::SingleShot, 300ms, [this, first, last] {
            const auto &es = ui_->global<EditorState>();
            es.set_caret_anchor(int(first));
            es.set_caret_offset(int(last));
            es.set_caret_scroll(false);
            es.set_caret_serial(es.get_caret_serial() + 1);
        });
    }
    if (!devFind_.empty()) {
        st.set_find_query(ss(devFind_));
        devFind_.clear();
        replayTimer_.start(slint::TimerMode::SingleShot, 400ms, [this] {
            const auto &es = ui_->global<EditorState>();
            es.set_find_serial(es.get_find_serial() + 1);
        });
    }
}
