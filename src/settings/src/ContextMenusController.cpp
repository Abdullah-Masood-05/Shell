#include "ContextMenusController.h"

#include "EditorSession.h"
#include "ShellCli.h"

#include "nss/ConfigGraph.h"
#include "nss/ConfigStore.h"
#include "nss/Text.h"

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

constexpr int kEditorPage = 6; // Pages.editor in ui/types.slint

} // namespace

ContextMenusController::ContextMenusController(slint::ComponentHandle<AppWindow> ui)
    : ui_(std::move(ui)), shellExe_(ShellCli::locate())
{
    bindCallbacks();
    statusPoll_.start(slint::TimerMode::Repeated, 500ms, [this] { pollStatus(); });
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
        st.set_load_error(ss("The config file doesn't exist: " + path));
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
        for (size_t i = 0; i < graph.files().size(); ++i) {
            const auto &gf = graph.files()[i];
            if (!gf.doc) {
                if (i == 0)
                    error = gf.error.empty() ? "The config couldn't be read." : gf.error;
                continue;
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
            f.label = isUnder(gf.path, rootDir) ? nss::narrow(gf.path.lexically_relative(rootDir).generic_wstring())
                                                : nss::narrow(gf.path.filename().wstring()) + " (outside your config)";
            f.original = *gf.doc;
            f.current = *gf.doc;
            f.writable = !shared && isUnder(gf.path, rootDir) && !editor::isProtectedPath(gf.path);
            files->push_back(std::move(f));
        }
        slint::invoke_from_event_loop([this, gen, files, error] {
            if (gen != gen_)
                return;
            files_ = std::move(*files);
            const auto &st = ui_->global<MenusState>();
            st.set_loaded(true);
            st.set_shared_config(shared_);
            st.set_load_error(ss(error));
            push();
        });
    });
}

int ContextMenusController::dirtyCount() const
{
    int n = 0;
    for (const auto &f : files_)
        n += f.current.text() != f.original.text();
    return n;
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
            bool changed = std::any_of(before.begin(), before.end(), [&](const rules::Module &b) {
                return b.importIndex == m.importIndex && b.enabled != m.enabled;
            });
            bool locked = m.locked || !root.writable;
            std::string reason = !root.writable ? std::string("Read-only") : m.lockReason;
            moduleRows.push_back(ModuleRow{ ss(m.title), ss(m.path), ss(m.description), m.enabled, changed, locked,
                                            ss(reason), ss(nss::narrow(root.path.wstring())), int(m.line) });
        }
    }
    st.set_modules(std::make_shared<slint::VectorModel<ModuleRow>>(moduleRows));

    // Rules, file by file.
    rules_.clear();
    std::vector<RuleRow> ruleRows;
    size_t fileCount = 0;
    for (size_t fi = 0; fi < files_.size(); ++fi) {
        const auto &f = files_[fi];
        auto list = rules::listRules(f.current);
        if (list.empty())
            continue;
        ++fileCount;
        auto before = rules::listRules(f.original);
        bool first = true;
        for (auto &r : list) {
            bool changed = std::any_of(before.begin(), before.end(),
                                       [&](const rules::Rule &b) { return b.line == r.line && b.enabled != r.enabled; });
            bool toggleable = r.toggleable && !r.complex && f.writable;
            std::string note = !f.writable ? std::string("Read-only")
                             : !r.toggleable ? r.note
                             : r.complex     ? std::string("Complex: edit it in the Config Editor")
                                             : std::string{};
            ruleRows.push_back(RuleRow{ ss(first ? f.label : std::string{}), ss(r.action), ss(r.target), ss(r.scope),
                                        ss(r.where), r.enabled, changed, toggleable, r.complex, ss(note),
                                        ss(nss::narrow(f.path.wstring())), int(r.line) });
            rules_.push_back(RuleRef{ fi, std::move(r) });
            first = false;
        }
    }
    st.set_rules(std::make_shared<slint::VectorModel<RuleRow>>(ruleRows));
    auto n = rules_.size();
    st.set_rules_caption(ss(n == 0 ? std::string("No modify or remove rules in your config.")
                                   : std::to_string(n) + (n == 1 ? " rule" : " rules") + " in " +
                                         std::to_string(fileCount) + (fileCount == 1 ? " file" : " files") +
                                         ". Turning one off comments it out; turning it back on restores it."));
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
        toast("error", "Couldn't change " + m.title, "Edit the import line in the Config Editor instead.");
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
        toast("error", "Couldn't change this rule", "Edit it in the Config Editor instead.");
    push();
}

void ContextMenusController::discard()
{
    for (auto &f : files_)
        f.current = f.original;
    push();
}

void ContextMenusController::save()
{
    if (saving_ || files_.empty() || dirtyCount() == 0)
        return;
    if (shellExe_.empty()) {
        toast("error", "Can't check the config", "shell.exe was not found, so nothing was saved.");
        return;
    }

    // What changed, for the snapshot reason.
    std::vector<std::string> changes;
    auto before = rules::listModules(files_[0].original);
    for (const auto &m : rules::listModules(files_[0].current))
        for (const auto &b : before)
            if (b.importIndex == m.importIndex && b.enabled != m.enabled)
                changes.push_back(std::string(m.enabled ? "turn on " : "turn off ") + m.title);
    for (const auto &f : files_) {
        auto old = rules::listRules(f.original);
        for (const auto &r : rules::listRules(f.current))
            for (const auto &b : old)
                if (b.line == r.line && b.enabled != r.enabled)
                    changes.push_back(std::string(r.enabled ? "turn on " : "turn off ") + "rule " +
                                      nss::narrow(f.path.filename().wstring()) + ":" + std::to_string(r.line));
    }
    std::string reason = "Context Menus: ";
    for (size_t i = 0; i < changes.size(); ++i) {
        if (reason.size() > 140) {
            reason += ", and " + std::to_string(changes.size() - i) + " more";
            break;
        }
        reason += (i ? ", " : "") + changes[i];
    }

    std::vector<nss::FileWrite> writes;
    for (const auto &f : files_) {
        if (f.current.text() == f.original.text())
            continue;
        if (!f.writable || editor::isProtectedPath(f.path)) {
            toast("error", "Not saved", nss::narrow(f.path.wstring()) + " is read-only here.");
            return;
        }
        writes.push_back(nss::FileWrite{ f.path, f.current.text(), f.current.format() });
    }

    saving_ = true;
    ui_->global<MenusState>().set_saving(true);
    worker_.post([this, writes = std::move(writes), reason, root = root_, exe = shellExe_]() mutable {
        nss::ConfigStore store(root);
        nss::SaveOptions options;
        options.reason = reason;
        options.validator = nss::makeShellCheckValidator(exe);
        auto result = store.save(std::move(writes), options);
        slint::invoke_from_event_loop([this, result = std::move(result), root] {
            saving_ = false;
            ui_->global<MenusState>().set_saving(false);
            if (result.ok) {
                toast("ok", "Saved", result.snapshotId.empty() ? std::string("No changes were needed.")
                                                                : "Snapshot " + result.snapshotId + " was taken first.");
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
                toast("error", "Not saved: the config would fail to load", detail);
                return;
            }
            toast("error", "Couldn't save", detail);
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
