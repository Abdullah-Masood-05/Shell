#include "ContextMenusController.h"

#include "EditorSession.h"
#include "MenuModel.h"
#include "ProgramIcon.h"
#include "ShellCli.h"
#include "Tr.h"
#include "WindowsTitles.h"

#include "nss/ConfigGraph.h"
#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <memory>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }
std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }

bool isUnder(const fs::path &child, const fs::path &parent)
{
    auto c = nss::pathKey(child), p = nss::pathKey(parent);
    if (p.empty())
        return false;
    if (p.back() != '\\')
        p += '\\';
    return c.rfind(p, 0) == 0;
}

bool fileExists(const fs::path &p)
{
    auto a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// A full path that isn't there (a bare name like "calc.exe" Windows finds by itself).
bool missingProgram(const std::string &program)
{
    if (program.size() < 3 || program[1] != ':' || (program[2] != '\\' && program[2] != '/'))
        return program.empty();
    return GetFileAttributesW(nss::widen(program).c_str()) == INVALID_FILE_ATTRIBUTES;
}

std::string pathKey(std::string_view p)
{
    auto k = nss::toLower(nss::trim(p));
    std::replace(k.begin(), k.end(), '\\', '/');
    while (k.rfind("./", 0) == 0)
        k.erase(0, 2);
    return k;
}

constexpr int kEditorPage = 6; // Pages.editor in ui/types.slint

} // namespace

ContextMenusController::ContextMenusController(slint::ComponentHandle<AppWindow> ui)
    : ui_(std::move(ui)), shellExe_(ShellCli::locate()), paths_(taskbarapps::realPaths())
{
    taskbar_ = taskbarOriginal_ = taskbarapps::defaults(paths_);
    // Development switches (see main.cpp), for snapshots: --taskbar-select <n> opens
    // entry n of the Taskbar menu list, --taskbar-picker the installed apps list.
    int argc = 0;
    if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--taskbar-select") == 0 && i + 1 < argc)
                devSelect_ = _wtoi(argv[++i]);
            else if (_wcsicmp(argv[i], L"--taskbar-picker") == 0)
                devPicker_ = true;
        }
        LocalFree(argv);
    }
    bindCallbacks();
    statusPoll_.start(slint::TimerMode::Repeated, 500ms, [this] { pollStatus(); });
    // The rows are built from the documents in memory, so staged changes stay.
    nsp::onLanguageChanged([this] {
        if (root_.empty() && !statusPath_.empty())
            ui_->global<MenusState>().set_load_error(ss(nsp::trf("The config file doesn't exist: {}", statusPath_)));
        if (installedLoaded_) {
            // Windows' own apps are named in the app's language.
            installedLoaded_ = false;
            installed_.clear();
            if (ui_->global<MenusState>().get_picker_shown())
                openPicker();
        }
        push();
    });
}

ContextMenusController::~ContextMenusController() = default;

void ContextMenusController::bindCallbacks()
{
    const auto &st = ui_->global<MenusState>();
    st.on_activated([this] {
        pollStatus();
        // Pick up edits made elsewhere (Config Editor, Menu Builder) when nothing is staged.
        if (!root_.empty() && dirtyCount() == 0 && !saving_)
            load();
    });
    st.on_toggle_module([this](int i, bool on) { toggleModule(i, on); });
    st.on_toggle_rule([this](int i, bool on) { toggleRule(i, on); });
    st.on_open_in_editor([this](slint::SharedString path, int line) { openInEditor(str(path), line); });
    st.on_save([this] { save(); });
    st.on_discard([this] { discard(); });
    st.on_create_user_config([this] { ui_->global<AppState>().invoke_action("config-init"); });

    st.on_add_module([this] { addModule(); });
    st.on_ask_remove_module([this](int i) { askRemoveModule(i); });
    st.on_remove_module([this] { removeModule(); });
    st.on_cancel_remove_module([this] {
        removeIndex_ = -1;
        ui_->global<MenusState>().set_remove_index(-1);
    });

    st.on_taskbar_select([this](int i) { taskbarSelect(i); });
    st.on_taskbar_toggle([this](int i, bool on) { taskbarToggle(i, on); });
    st.on_taskbar_move([this](int d) { taskbarMove(d); });
    st.on_taskbar_remove([this] { taskbarRemove(); });
    st.on_taskbar_set_text([this](slint::SharedString f, slint::SharedString v) { taskbarSetText(str(f), str(v)); });
    st.on_taskbar_set_admin([this](bool on) { taskbarSetAdmin(on); });
    st.on_taskbar_browse([this] { taskbarBrowse(false); });
    st.on_taskbar_browse_program([this] { taskbarBrowse(true); });
    st.on_taskbar_add_separator([this] { taskbarAdd(taskbarapps::makeSeparator()); });
    st.on_taskbar_open_picker([this] { openPicker(); });
    st.on_taskbar_filter([this](slint::SharedString t) {
        filter_ = str(t);
        pushInstalled();
    });
    st.on_taskbar_pick([this](int i) { pick(i); });
    st.on_taskbar_reset([this] {
        if (!taskbarEditable())
            return;
        // The four apps as shipped; the file's own header and comments at its end stay.
        taskbar_.entries = taskbarapps::defaults(paths_).entries;
        taskbarSelected_ = -1;
        taskbarEdited(true);
    });
}

void ContextMenusController::toast(const std::string &kind, const std::string &title, const std::string &text)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(text));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

void ContextMenusController::pollStatus()
{
    auto status = ui_->global<AppState>().get_status();
    if (!status.loaded)
        return;
    auto path = str(status.config_path);
    if (path == statusPath_)
        return;
    statusPath_ = path;
    const auto &st = ui_->global<MenusState>();
    if (!status.config_exists || path.empty()) {
        root_.clear();
        files_.clear();
        st.set_load_error(ss(nsp::trf("The config file doesn't exist: {}", path)));
        push();
        return;
    }
    root_ = fs::path(nss::widen(path)).lexically_normal();
    shared_ = str(status.config_source) == "install" || editor::isProtectedPath(root_);
    st.set_load_error("");
    load();
}

void ContextMenusController::load()
{
    if (root_.empty())
        return;
    auto gen = ++gen_;
    auto root = root_;
    auto shared = shared_;
    auto appDir = shellExe_.empty() ? fs::path() : shellExe_.parent_path();
    worker_.post([this, gen, root, shared, appDir] {
        auto files = std::make_shared<std::vector<File>>();
        std::string error;
        nss::GraphOptions options;
        if (!appDir.empty())
            options.appDir = appDir;
        options.followLang = false;
        auto graph = nss::ConfigGraph::load(root, options);
        const auto rootDir = root.parent_path();
        fs::path taskbarNss;
        bool hooked = true;
        for (size_t i = 0; i < graph.files().size(); ++i) {
            const auto &gf = graph.files()[i];
            if (!gf.doc) {
                if (i == 0)
                    error = gf.error.empty() ? std::string(nsp::tr("The config couldn't be read.")) : gf.error;
                continue;
            }
            // The taskbar menu's file: whether it reads the Apps list.
            if (taskbarNss.empty() && nss::toLower(gf.path.filename().string()) == "taskbar.nss") {
                taskbarNss = gf.path;
                hooked = gf.doc->text().find(taskbarapps::kFileName) != std::string::npos;
            }
            if (gf.context.mode != nss::ParseMode::Config && gf.context.mode != nss::ParseMode::Menu)
                continue;
            if (i > 0 && rules::listRules(*gf.doc).empty())
                continue;
            // The same file imported twice is listed once.
            if (std::any_of(files->begin(), files->end(),
                            [&](const File &f) { return nss::pathKey(f.path) == nss::pathKey(gf.path); }))
                continue;
            File f;
            f.path = gf.path;
            f.outside = !isUnder(gf.path, rootDir);
            f.label = !f.outside ? nss::narrow(gf.path.lexically_relative(rootDir).generic_wstring())
                                 : nss::narrow(gf.path.filename().wstring());
            f.original = *gf.doc;
            f.current = *gf.doc;
            f.writable = !shared && isUnder(gf.path, rootDir) && !editor::isProtectedPath(gf.path);
            files->push_back(std::move(f));
        }

        // The Apps list: its file, else Shell's four apps.
        auto taskbarFile = rootDir / L"imports" / nss::widen(taskbarapps::kFileName);
        auto taskbar = std::make_shared<taskbarapps::List>();
        auto format = std::make_shared<nss::TextFormat>(nss::defaultFormat());
        std::string bytes;
        if (fileExists(taskbarFile) && nss::readFile(taskbarFile, bytes)) {
            auto decoded = nss::decode(bytes);
            *taskbar = taskbarapps::parse(decoded.text, paths_);
            *format = decoded.format;
            if (!decoded.ok) {
                taskbar->error = decoded.error;
                taskbar->entries.clear();
            }
        } else {
            *taskbar = taskbarapps::defaults(paths_);
        }

        slint::invoke_from_event_loop([this, gen, files, error, taskbar, format, taskbarNss, hooked] {
            if (gen != gen_)
                return;
            files_ = std::move(*files);
            newFiles_.clear();
            deleteAfterSave_.clear();
            taskbar_ = taskbarOriginal_ = *taskbar;
            taskbarFormat_ = *format;
            taskbarNss_ = taskbarNss;
            taskbarHooked_ = hooked;
            if (devSelect_ >= 0 && devSelect_ < int(taskbar_.entries.size()))
                taskbarSelected_ = devSelect_;
            devSelect_ = -1;
            if (taskbarSelected_ >= int(taskbar_.entries.size()))
                taskbarSelected_ = -1;
            ++taskbarSerial_;
            const auto &st = ui_->global<MenusState>();
            st.set_loaded(true);
            st.set_shared_config(shared_);
            st.set_load_error(ss(error));
            push();
            if (devPicker_) {
                devPicker_ = false;
                openPicker();
            }
        });
    });
}

bool ContextMenusController::taskbarDirty() const
{
    return taskbar_.error.empty() && taskbarapps::dirty(taskbar_, taskbarOriginal_, paths_);
}

int ContextMenusController::dirtyCount() const
{
    int n = 0;
    for (const auto &f : files_)
        n += f.current.text() != f.original.text();
    n += int(newFiles_.size());
    n += taskbarDirty() ? 1 : 0;
    return n;
}

// loc.* titles from the language files next to the config (or the install's), in the
// app's language, and Windows' own titles for id.* and title.*.
const rules::Names &ContextMenusController::names()
{
    std::vector<fs::path> dirs;
    if (!root_.empty())
        dirs.push_back(root_.parent_path() / L"imports" / L"lang");
    if (!shellExe_.empty())
        dirs.push_back(shellExe_.parent_path() / L"imports" / L"lang");
    auto key = nsp::currentLanguage();
    for (const auto &d : dirs)
        key += "|" + nss::narrow(d.wstring());
    if (key != namesKey_ || !names_.windows) {
        names_ = rules::langNames(dirs, nsp::currentLanguage());
        names_.windows = [](std::string_view name) { return wintitles::displayTitle(name); };
        namesKey_ = key;
    }
    return names_;
}

void ContextMenusController::push()
{
    const auto &st = ui_->global<MenusState>();

    // Modules: the root's imports.
    modules_.clear();
    std::vector<ModuleRow> moduleRows;
    if (!files_.empty()) {
        const auto &root = files_[0];
        modules_ = rules::listModules(root.current);
        auto before = rules::listModules(root.original);
        for (const auto &m : modules_) {
            auto same = std::find_if(before.begin(), before.end(),
                                     [&](const rules::Module &b) { return pathKey(b.path) == pathKey(m.path); });
            bool changed = same == before.end() || same->enabled != m.enabled;
            ModuleRow r;
            r.title = ss(nsp::tr(m.title.c_str()));
            r.file = ss(m.path);
            r.description = ss(m.description);
            r.enabled = m.enabled;
            r.changed = changed;
            r.locked = m.locked || !root.writable;
            r.lock_reason = ss(!root.writable ? std::string(nsp::tr("Read-only")) : m.lockReason);
            r.path = ss(nss::narrow(root.path.wstring()));
            r.line = int(m.line);
            r.own = m.own && root.writable;
            moduleRows.push_back(r);
        }
    }
    st.set_modules(std::make_shared<slint::VectorModel<ModuleRow>>(moduleRows));

    // Rules, file by file.
    rules_.clear();
    std::vector<RuleRow> ruleRows;
    size_t fileCount = 0;
    for (size_t fi = 0; fi < files_.size(); ++fi) {
        const auto &f = files_[fi];
        auto list = rules::listRules(f.current, names());
        if (list.empty())
            continue;
        ++fileCount;
        auto before = rules::listRules(f.original);
        bool first = true;
        for (auto &r : list) {
            bool changed = std::any_of(before.begin(), before.end(),
                                       [&](const rules::Rule &b) { return b.line == r.line && b.enabled != r.enabled; });
            bool toggleable = r.toggleable && !r.complex && f.writable;
            std::string note = !f.writable ? std::string(nsp::tr("Read-only"))
                             : !r.toggleable ? r.note
                             : r.complex     ? std::string(nsp::tr("Complex: edit it in the Config Editor"))
                                             : std::string{};
            std::string label = f.outside ? nsp::trf("{} (outside your config)", f.label) : f.label;
            ruleRows.push_back(RuleRow{ ss(first ? label : std::string{}), ss(r.summary),
                                        ss(r.where), r.enabled, changed, toggleable, r.complex, ss(note),
                                        ss(nss::narrow(f.path.wstring())), int(r.line) });
            rules_.push_back(RuleRef{ fi, std::move(r) });
            first = false;
        }
    }
    st.set_rules(std::make_shared<slint::VectorModel<RuleRow>>(ruleRows));
    auto n = rules_.size();
    st.set_rules_caption(ss(n == 0 ? std::string(nsp::tr("No modify or remove rules in your config."))
                                   : nsp::trn("{n} rule in {0}. Turning one off comments it out; turning it back on restores it.",
                                              "{n} rules in {0}. Turning one off comments it out; turning it back on restores it.",
                                              (long long)n, nsp::trn("{n} file", "{n} files", (long long)fileCount))));
    pushTaskbar();
    st.set_dirty_count(dirtyCount());
}

void ContextMenusController::toggleModule(int index, bool on)
{
    if (saving_ || index < 0 || index >= int(modules_.size()) || files_.empty() || !files_[0].writable)
        return;
    const auto &m = modules_[size_t(index)];
    if (m.locked)
        return;
    if (!rules::setModuleEnabled(files_[0].current, m.importIndex, on))
        toast("error", nsp::trf("Couldn't change {}", nsp::tr(m.title.c_str())), nsp::tr("Edit the import line in the Config Editor instead."));
    push();
}

void ContextMenusController::toggleRule(int index, bool on)
{
    if (saving_ || index < 0 || index >= int(rules_.size()))
        return;
    auto &ref = rules_[size_t(index)];
    auto &f = files_[ref.file];
    if (!f.writable)
        return;
    if (!rules::setRuleEnabled(f.current, ref.rule, on))
        toast("error", nsp::tr("Couldn't change this rule"), nsp::tr("Edit it in the Config Editor instead."));
    push();
}

void ContextMenusController::discard()
{
    for (auto &f : files_)
        f.current = f.original;
    newFiles_.clear();
    deleteAfterSave_.clear();
    taskbar_ = taskbarOriginal_;
    if (taskbarSelected_ >= int(taskbar_.entries.size()))
        taskbarSelected_ = -1;
    ++taskbarSerial_;
    push();
}

void ContextMenusController::save()
{
    if (saving_ || files_.empty() || dirtyCount() == 0)
        return;
    if (shellExe_.empty()) {
        toast("error", nsp::tr("Can't check the config"), nsp::tr("shell.exe was not found, so nothing was saved."));
        return;
    }

    // What changed, for the snapshot reason.
    std::vector<std::string> changes;
    auto before = rules::listModules(files_[0].original);
    auto after = rules::listModules(files_[0].current);
    for (const auto &m : after) {
        auto b = std::find_if(before.begin(), before.end(), [&](const rules::Module &x) { return pathKey(x.path) == pathKey(m.path); });
        if (b == before.end())
            changes.push_back("add " + m.path);
        else if (b->enabled != m.enabled)
            changes.push_back(std::string(m.enabled ? "turn on " : "turn off ") + m.title);
    }
    for (const auto &b : before)
        if (std::none_of(after.begin(), after.end(), [&](const rules::Module &m) { return pathKey(m.path) == pathKey(b.path); }))
            changes.push_back("remove " + b.path);
    for (size_t fi = 0; fi < files_.size(); ++fi) {
        const auto &f = files_[fi];
        auto old = rules::listRules(f.original);
        for (const auto &r : rules::listRules(f.current))
            for (const auto &b : old)
                if (b.line == r.line && b.enabled != r.enabled)
                    changes.push_back(std::string(r.enabled ? "turn on " : "turn off ") + "rule " +
                                      nss::narrow(f.path.filename().wstring()) + ":" + std::to_string(r.line));
    }
    const bool taskbarChanged = taskbarDirty();
    if (taskbarChanged)
        changes.push_back("taskbar apps");
    std::string reason = "Context Menus: ";
    for (size_t i = 0; i < changes.size(); ++i) {
        if (reason.size() > 140) {
            reason += ", and " + std::to_string(changes.size() - i) + " more";
            break;
        }
        reason += (i ? ", " : "") + changes[i];
    }

    std::vector<nss::FileWrite> writes;
    for (const auto &nf : newFiles_)
        writes.push_back(nss::FileWrite{ nf.path, nf.text, nf.format });
    if (taskbarChanged) {
        auto path = root_.parent_path() / L"imports" / nss::widen(taskbarapps::kFileName);
        if (!taskbarEditable()) {
            toast("error", nsp::tr("Not saved"), nsp::trf("{} is read-only here.", nss::narrow(path.wstring())));
            return;
        }
        writes.push_back(nss::FileWrite{ path, taskbarapps::generate(taskbar_, paths_), taskbarFormat_ });
    }
    for (const auto &f : files_) {
        if (f.current.text() == f.original.text())
            continue;
        if (!f.writable || editor::isProtectedPath(f.path)) {
            toast("error", nsp::tr("Not saved"), nsp::trf("{} is read-only here.", nss::narrow(f.path.wstring())));
            return;
        }
        writes.push_back(nss::FileWrite{ f.path, f.current.text(), f.current.format() });
    }

    saving_ = true;
    ui_->global<MenusState>().set_saving(true);
    auto deletes = deleteAfterSave_;
    worker_.post([this, writes = std::move(writes), reason, root = root_, exe = shellExe_, deletes]() mutable {
        nss::ConfigStore store(root);
        nss::SaveOptions options;
        options.reason = reason;
        options.validator = nss::makeShellCheckValidator(exe);
        auto result = store.save(std::move(writes), options);
        // Files of modules that were removed, when asked to: the snapshot just
        // taken has them.
        std::vector<std::string> notDeleted;
        if (result.ok)
            for (const auto &p : deletes) {
                std::error_code ec;
                fs::remove(p, ec);
                if (ec)
                    notDeleted.push_back(nss::narrow(p.filename().wstring()));
            }
        slint::invoke_from_event_loop([this, result = std::move(result), root, notDeleted] {
            saving_ = false;
            ui_->global<MenusState>().set_saving(false);
            if (result.ok) {
                if (!notDeleted.empty())
                    toast("warning", nsp::tr("Saved, but a file couldn't be deleted"), notDeleted.front());
                else
                    toast("ok", nsp::tr("Saved"), result.snapshotId.empty() ? std::string(nsp::tr("No changes were needed."))
                                                                             : nsp::trf("Snapshot {} was taken first.", result.snapshotId));
                load();
                return;
            }
            std::string detail = result.error;
            if (!result.check.ok && !result.check.message.empty()) {
                auto outcome = editor::parseCheck(result.check.message, {}, root.parent_path());
                for (const auto &i : outcome.issues)
                    if (i.error) {
                        detail = i.message;
                        if (!i.file.empty())
                            detail += " (" + nss::narrow(i.file.filename().wstring()) + ":" + std::to_string(i.line) + ")";
                        break;
                    }
                toast("error", nsp::tr("Not saved: the config would fail to load"), detail);
                return;
            }
            toast("error", nsp::tr("Couldn't save"), detail);
        });
    });
}

void ContextMenusController::openInEditor(const std::string &path, int line)
{
    ui_->global<AppState>().set_page(kEditorPage);
    // Let the editor page come up first, so its jump handler sees the request.
    jumpTimer_.start(slint::TimerMode::SingleShot, 150ms, [this, path, line] {
        ui_->global<EditorState>().invoke_open_at(ss(path), line);
    });
}

std::string ContextMenusController::pickFile(const std::wstring &title, const std::wstring &filterName, const wchar_t *filterSpec,
                                             const std::wstring &folder)
{
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return {};
    DWORD flags = 0;
    dlg->GetOptions(&flags);
    // FOS_NODEREFERENCELINKS: a shortcut is picked as the shortcut, not where it points.
    dlg->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NODEREFERENCELINKS);
    dlg->SetTitle(title.c_str());
    const std::wstring allFiles = nss::widen(nsp::tr("All files"));
    COMDLG_FILTERSPEC filters[] = { { filterName.c_str(), filterSpec }, { allFiles.c_str(), L"*.*" } };
    dlg->SetFileTypes(UINT(std::size(filters)), filters);
    IShellItem *item = nullptr;
    if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dlg->SetFolder(item);
        item->Release();
    }
    HWND owner = ui_->window().win32_hwnd();
    std::string picked;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem *result = nullptr;
        if (SUCCEEDED(dlg->GetResult(&result))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                picked = nss::narrow(path);
                CoTaskMemFree(path);
            }
            result->Release();
        }
    }
    dlg->Release();
    return picked;
}

// --- Modules ---------------------------------------------------------------------

fs::path ContextMenusController::modulePath(const rules::Module &m) const
{
    auto p = fs::path(nss::widen(m.path));
    if (p.is_relative())
        p = root_.parent_path() / p;
    return p.lexically_normal();
}

void ContextMenusController::addModule()
{
    if (saving_ || files_.empty() || !files_[0].writable)
        return;
    const auto rootDir = root_.parent_path();
    auto picked = pickFile(nss::widen(nsp::tr("Choose a .nss file to add")), nss::widen(nsp::tr("Shell config files")), L"*.nss",
                           (rootDir / L"imports").wstring());
    if (picked.empty())
        return;
    fs::path src = fs::path(nss::widen(picked)).lexically_normal();
    if (nss::pathKey(src) == nss::pathKey(root_)) {
        toast("info", nsp::tr("That's your shell.nss"), nsp::tr("Pick another .nss file to add to it."));
        return;
    }
    std::string loadError;
    auto doc = nss::NssDocument::load(src, {}, &loadError);
    if (!doc) {
        toast("error", nsp::tr("Couldn't read the file"), loadError);
        return;
    }

    std::string rel;
    if (isUnder(src, rootDir) && !editor::isProtectedPath(src)) {
        rel = nss::narrow(src.lexically_relative(rootDir).generic_wstring());
    } else {
        // Copied into <config>\imports\, under a free name.
        auto stem = src.stem().wstring();
        fs::path target;
        for (int k = 1; k < 100; ++k) {
            auto name = k == 1 ? stem + L".nss" : stem + L"-" + std::to_wstring(k) + L".nss";
            auto candidate = rootDir / L"imports" / name;
            bool taken = fileExists(candidate) || std::any_of(newFiles_.begin(), newFiles_.end(), [&](const NewFile &f) {
                             return nss::pathKey(f.path) == nss::pathKey(candidate);
                         });
            if (!taken) {
                target = candidate;
                break;
            }
            // The same file again: use the copy that's there.
            std::string a, b;
            if (fileExists(candidate) && nss::readFile(candidate, a) && nss::readFile(src, b) && a == b) {
                target = candidate;
                break;
            }
        }
        if (target.empty())
            return;
        rel = nss::narrow(target.lexically_relative(rootDir).generic_wstring());
        if (!fileExists(target))
            newFiles_.push_back(NewFile{ target, rel, doc->text(), doc->format() });
    }

    if (!rules::addModule(files_[0].current, rel)) {
        toast("info", nsp::tr("Already in your config"), nsp::trf("shell.nss already imports {}.", rel));
        return;
    }
    toast("info", nsp::trf("{} added", rel), nsp::tr("Save to check it and use it. Nothing is written until then."));
    push();
}

void ContextMenusController::askRemoveModule(int index)
{
    if (saving_ || index < 0 || index >= int(modules_.size()) || files_.empty() || !files_[0].writable)
        return;
    const auto &m = modules_[size_t(index)];
    if (!m.own)
        return;
    auto path = modulePath(m);
    // One added in this session: nothing to ask, nothing on disk yet.
    auto added = std::find_if(newFiles_.begin(), newFiles_.end(),
                              [&](const NewFile &f) { return nss::pathKey(f.path) == nss::pathKey(path); });
    if (added != newFiles_.end()) {
        newFiles_.erase(added);
        rules::removeModule(files_[0].current, m.importIndex);
        push();
        return;
    }
    removeIndex_ = index;
    bool deletable = fileExists(path) && isUnder(path, root_.parent_path()) && !editor::isProtectedPath(path);
    const auto &st = ui_->global<MenusState>();
    st.set_remove_title(ss(nsp::tr(m.title.c_str())));
    st.set_remove_file(ss(deletable ? m.path : std::string{}));
    st.set_remove_delete(false);
    st.set_remove_index(index);
}

void ContextMenusController::removeModule()
{
    const auto &st = ui_->global<MenusState>();
    int index = removeIndex_;
    removeIndex_ = -1;
    st.set_remove_index(-1);
    if (saving_ || index < 0 || index >= int(modules_.size()) || files_.empty())
        return;
    const auto &m = modules_[size_t(index)];
    bool alsoDelete = st.get_remove_delete() && !str(st.get_remove_file()).empty();
    auto path = modulePath(m);
    if (!rules::removeModule(files_[0].current, m.importIndex)) {
        toast("error", nsp::trf("Couldn't remove {}", nsp::tr(m.title.c_str())), nsp::tr("Edit the import line in the Config Editor instead."));
        return;
    }
    if (alsoDelete)
        deleteAfterSave_.push_back(path);
    push();
}

// --- Taskbar menu --------------------------------------------------------------------

bool ContextMenusController::taskbarEditable() const
{
    return !root_.empty() && !shared_ && !files_.empty() && files_[0].writable && taskbar_.error.empty();
}

std::string ContextMenusController::iconSource(const taskbarapps::Entry &e) const
{
    // A shortcut's icon location or target, written as image="C:\x\app.exe".
    if (!e.image.empty() && (e.image.front() == '"' || e.image.front() == '\'')) {
        bool quoted = false;
        auto v = menus::unquote(e.image, &quoted);
        if (quoted)
            return paths_.expand ? paths_.expand(v) : v;
    }
    return e.program;
}

std::optional<slint::Image> ContextMenusController::icon(const std::string &source)
{
    if (source.empty())
        return std::nullopt;
    auto it = icons_.find(nss::toLower(source));
    return it == icons_.end() ? std::nullopt : it->second;
}

void ContextMenusController::requestIcons()
{
    std::vector<std::string> wanted;
    auto want = [&](const std::string &src) {
        if (src.empty())
            return;
        auto key = nss::toLower(src);
        if (icons_.count(key) || iconsPending_.count(key))
            return;
        iconsPending_.insert(key);
        wanted.push_back(src);
    };
    for (const auto &e : taskbar_.entries)
        if (e.kind == taskbarapps::Kind::App)
            want(iconSource(e));
    if (ui_->global<MenusState>().get_picker_shown())
        for (auto i : shown_)
            want(installed_[i].icon);
    if (wanted.empty())
        return;
    worker_.post([this, wanted] {
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto got = std::make_shared<std::vector<std::pair<std::string, std::optional<IconPixels>>>>();
        for (const auto &src : wanted)
            got->emplace_back(src, programIcon(src, 32));
        if (SUCCEEDED(com))
            CoUninitialize();
        slint::invoke_from_event_loop([this, got] {
            for (auto &[src, px] : *got) {
                auto key = nss::toLower(src);
                iconsPending_.erase(key);
                if (!px) {
                    icons_[key] = std::nullopt;
                    continue;
                }
                slint::SharedPixelBuffer<slint::Rgba8Pixel> buf{ px->width, px->height };
                auto *dst = buf.begin();
                for (size_t i = 0; i < size_t(px->width) * px->height; ++i)
                    dst[i] = { px->rgba[i * 4], px->rgba[i * 4 + 1], px->rgba[i * 4 + 2], px->rgba[i * 4 + 3] };
                icons_[key] = slint::Image(buf);
            }
            pushTaskbar();
            pushInstalled();
        });
    });
}

void ContextMenusController::pushTaskbar()
{
    const auto &st = ui_->global<MenusState>();
    const auto &loc = names().loc;
    std::vector<TaskbarRow> rows;
    for (const auto &e : taskbar_.entries) {
        TaskbarRow r;
        r.kind = ss(e.kind == taskbarapps::Kind::App ? "app" : e.kind == taskbarapps::Kind::Separator ? "sep" : "other");
        r.title = ss(e.kind == taskbarapps::Kind::Other ? std::string(nsp::tr("Written by hand")) : taskbarapps::displayTitle(e, loc));
        r.enabled = e.enabled;
        r.changed = e.edited();
        if (e.kind == taskbarapps::Kind::App) {
            r.detail = ss(e.args.empty() ? e.program : e.program + "  " + e.args);
            if (auto img = icon(iconSource(e))) {
                r.icon = *img;
                r.has_icon = true;
            }
            if (e.program.empty())
                r.problem = ss(nsp::tr("Choose a program for it"));
            else if (missingProgram(e.program))
                r.problem = ss(nsp::trf("Not found: {}", e.program));
        } else if (e.kind == taskbarapps::Kind::Other) {
            r.detail = ss(e.summary);
        }
        rows.push_back(r);
    }
    st.set_taskbar_rows(std::make_shared<slint::VectorModel<TaskbarRow>>(rows));

    TaskbarForm form;
    if (taskbarSelected_ >= 0 && taskbarSelected_ < int(taskbar_.entries.size())) {
        const auto &e = taskbar_.entries[size_t(taskbarSelected_)];
        form.kind = ss(e.kind == taskbarapps::Kind::App ? "app" : e.kind == taskbarapps::Kind::Separator ? "sep" : "other");
        form.title = ss(taskbarapps::displayTitle(e, loc));
        form.program = ss(e.program);
        form.args = ss(e.args);
        form.admin = e.admin;
        form.builtin = !taskbarapps::builtinOf(e).empty() && !e.titleExpr.empty();
    }
    st.set_taskbar_form(form);
    st.set_taskbar_selected(taskbarSelected_);
    st.set_taskbar_serial(taskbarSerial_);
    st.set_taskbar_can_up(taskbarSelected_ > 0);
    st.set_taskbar_can_down(taskbarSelected_ >= 0 && taskbarSelected_ + 1 < int(taskbar_.entries.size()));
    st.set_taskbar_custom(taskbarOriginal_.exists);
    {
        auto shipped = taskbar_;
        shipped.entries = taskbarapps::defaults(paths_).entries;
        st.set_taskbar_is_default(taskbarapps::generate(shipped, paths_) == taskbarapps::generate(taskbar_, paths_));
    }
    st.set_taskbar_locked(!taskbarEditable() || saving_);
    const auto file = root_.empty() ? fs::path() : root_.parent_path() / L"imports" / nss::widen(taskbarapps::kFileName);
    st.set_taskbar_file(ss(nss::narrow(file.wstring())));
    st.set_taskbar_nss(ss(nss::narrow(taskbarNss_.wstring())));
    st.set_taskbar_error(ss(taskbar_.error.empty() ? std::string{}
                                                   : nsp::trf("Line {}: {}", taskbar_.errorLine, taskbar_.error)));

    // Whether the taskbar's menu shows the list at all.
    std::string notice;
    if (!files_.empty()) {
        auto mod = std::find_if(modules_.begin(), modules_.end(), [](const rules::Module &m) { return m.name == "taskbar"; });
        if (mod == modules_.end() || !mod->enabled)
            notice = nsp::tr("The Taskbar module is off, so the taskbar's menu doesn't show these apps. Turn it on under Modules.");
        else if (!taskbarHooked_)
            notice = nsp::tr("Your imports\\taskbar.nss doesn't read this list: it was changed by hand, or it comes from an older version. Take the new taskbar.nss on the Home page (your copy goes into a backup), or open it in the Config Editor.");
    }
    st.set_taskbar_notice(ss(notice));
    st.set_dirty_count(dirtyCount());
    requestIcons();
}

void ContextMenusController::taskbarEdited(bool reloadForm)
{
    if (reloadForm)
        ++taskbarSerial_;
    pushTaskbar();
}

void ContextMenusController::taskbarSelect(int index)
{
    taskbarSelected_ = index == taskbarSelected_ ? -1 : index;
    taskbarEdited(true);
}

void ContextMenusController::taskbarToggle(int index, bool on)
{
    if (!taskbarEditable() || index < 0 || index >= int(taskbar_.entries.size()))
        return;
    auto &e = taskbar_.entries[size_t(index)];
    if (e.kind == taskbarapps::Kind::Other)
        return;
    e.enabled = on;
    taskbarEdited(false);
}

void ContextMenusController::taskbarMove(int delta)
{
    int to = taskbarSelected_ + delta;
    if (!taskbarEditable() || taskbarSelected_ < 0 || to < 0 || to >= int(taskbar_.entries.size()))
        return;
    std::swap(taskbar_.entries[size_t(taskbarSelected_)], taskbar_.entries[size_t(to)]);
    taskbarSelected_ = to;
    taskbarEdited(false);
}

void ContextMenusController::taskbarRemove()
{
    if (!taskbarEditable() || taskbarSelected_ < 0 || taskbarSelected_ >= int(taskbar_.entries.size()))
        return;
    taskbar_.entries.erase(taskbar_.entries.begin() + taskbarSelected_);
    if (taskbarSelected_ >= int(taskbar_.entries.size()))
        taskbarSelected_ = int(taskbar_.entries.size()) - 1;
    taskbarEdited(true);
}

void ContextMenusController::taskbarSetText(const std::string &field, const std::string &value)
{
    if (!taskbarEditable() || taskbarSelected_ < 0 || taskbarSelected_ >= int(taskbar_.entries.size()))
        return;
    auto &e = taskbar_.entries[size_t(taskbarSelected_)];
    if (e.kind != taskbarapps::Kind::App)
        return;
    if (field == "title") {
        // Typed back to what it was: the title as written (loc.paint) again.
        auto read = e;
        read.title = read.read.title;
        read.titleExpr = read.read.titleExpr;
        if (!read.text.empty() && value == taskbarapps::displayTitle(read, names().loc)) {
            e.title = e.read.title;
            e.titleExpr = e.read.titleExpr;
        } else {
            e.title = value;
            e.titleExpr.clear();
        }
    } else if (field == "program") {
        e.program = std::string(nss::trim(value));
    } else if (field == "args") {
        e.args = value;
    } else {
        return;
    }
    taskbarEdited(false);
}

void ContextMenusController::taskbarSetAdmin(bool on)
{
    if (!taskbarEditable() || taskbarSelected_ < 0 || taskbarSelected_ >= int(taskbar_.entries.size()))
        return;
    auto &e = taskbar_.entries[size_t(taskbarSelected_)];
    if (e.kind != taskbarapps::Kind::App)
        return;
    e.admin = on;
    taskbarEdited(false);
}

void ContextMenusController::taskbarAdd(taskbarapps::Entry e)
{
    if (!taskbarEditable())
        return;
    // Under the selected entry, else at the end.
    size_t at = taskbarSelected_ >= 0 && taskbarSelected_ < int(taskbar_.entries.size()) ? size_t(taskbarSelected_) + 1
                                                                                         : taskbar_.entries.size();
    taskbar_.entries.insert(taskbar_.entries.begin() + ptrdiff_t(at), std::move(e));
    taskbarSelected_ = int(at);
    taskbarEdited(true);
}

void ContextMenusController::taskbarBrowse(bool replaceProgram)
{
    if (!taskbarEditable())
        return;
    std::wstring folder;
    if (replaceProgram && taskbarSelected_ >= 0 && taskbarSelected_ < int(taskbar_.entries.size())) {
        auto p = fs::path(nss::widen(taskbar_.entries[size_t(taskbarSelected_)].program)).parent_path();
        if (!p.empty() && fs::is_directory(p))
            folder = p.wstring();
    }
    if (folder.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &p)) && p)
            folder = p;
        CoTaskMemFree(p);
    }
    auto picked = pickFile(nss::widen(nsp::tr("Choose a program or shortcut")), nss::widen(nsp::tr("Programs and shortcuts")),
                           L"*.exe;*.lnk;*.bat;*.cmd;*.msc", folder);
    if (picked.empty())
        return;
    auto app = taskbarapps::fromFile(picked);
    if (!replaceProgram) {
        taskbarAdd(taskbarapps::makeApp(app.name, app.program, app.args, app.image));
        return;
    }
    if (taskbarSelected_ < 0 || taskbarSelected_ >= int(taskbar_.entries.size()))
        return;
    auto &e = taskbar_.entries[size_t(taskbarSelected_)];
    if (e.kind != taskbarapps::Kind::App)
        return;
    e.program = app.program;
    if (e.text.empty())
        e.image = app.image;
    if (e.title.empty() && e.titleExpr.empty())
        e.title = app.name;
    taskbarEdited(true);
}

void ContextMenusController::openPicker()
{
    const auto &st = ui_->global<MenusState>();
    st.set_picker_shown(true);
    if (installedLoaded_ || installedLoading_) {
        pushInstalled();
        return;
    }
    installedLoading_ = true;
    st.set_installed_loading(true);
    worker_.post([this] {
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto list = std::make_shared<std::vector<taskbarapps::Installed>>(taskbarapps::windowsApps());
        auto more = taskbarapps::installedApps();
        list->insert(list->end(), more.begin(), more.end());
        if (SUCCEEDED(com))
            CoUninitialize();
        slint::invoke_from_event_loop([this, list] {
            installed_ = std::move(*list);
            installedLoading_ = false;
            installedLoaded_ = true;
            ui_->global<MenusState>().set_installed_loading(false);
            pushInstalled();
        });
    });
}

void ContextMenusController::pushInstalled()
{
    const auto &st = ui_->global<MenusState>();
    auto needle = nss::toLower(nss::trim(filter_));
    shown_.clear();
    std::vector<InstalledRow> rows;
    std::string lastGroup;
    for (size_t i = 0; i < installed_.size(); ++i) {
        const auto &a = installed_[i];
        if (!needle.empty() && nss::toLower(a.name).find(needle) == std::string::npos &&
            nss::toLower(a.detail).find(needle) == std::string::npos)
            continue;
        InstalledRow r;
        r.name = ss(a.name);
        r.detail = ss(a.detail);
        if (a.group != lastGroup)
            r.section = ss(a.group == "windows" ? std::string(nsp::tr("Windows")) : std::string(nsp::tr("Installed apps")));
        lastGroup = a.group;
        if (auto img = icon(a.icon)) {
            r.icon = *img;
            r.has_icon = true;
        }
        r.added = std::any_of(taskbar_.entries.begin(), taskbar_.entries.end(), [&](const taskbarapps::Entry &e) {
            if (e.kind != taskbarapps::Kind::App)
                return false;
            if (!a.builtin.empty())
                return taskbarapps::builtinOf(e) == a.builtin;
            return nss::toLower(e.program) == nss::toLower(a.program);
        });
        rows.push_back(r);
        shown_.push_back(i);
    }
    st.set_installed(std::make_shared<slint::VectorModel<InstalledRow>>(rows));
    st.set_picker_filter(ss(filter_));
    if (st.get_picker_shown())
        requestIcons();
}

void ContextMenusController::pick(int index)
{
    if (index < 0 || index >= int(shown_.size()))
        return;
    const auto &a = installed_[shown_[size_t(index)]];
    ui_->global<MenusState>().set_picker_shown(false);
    if (!a.builtin.empty()) {
        if (auto e = taskbarapps::builtinEntry(a.builtin, paths_)) {
            taskbarAdd(std::move(*e));
            return;
        }
    }
    taskbarAdd(taskbarapps::makeApp(a.name, a.program, a.args, a.image));
}
