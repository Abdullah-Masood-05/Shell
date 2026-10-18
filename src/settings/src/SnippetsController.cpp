#include "SnippetsController.h"

#include "EditorSession.h"
#include "Highlight.h"
#include "ShellCli.h"
#include "Tr.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }
std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }

// Case-insensitive for ASCII; Chinese text matches as written.
bool contains(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    return nss::toLower(haystack).find(nss::toLower(needle)) != std::string::npos;
}

std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t'))
        ++b;
    return s.substr(b);
}

} // namespace

SnippetsController::SnippetsController(slint::ComponentHandle<AppWindow> ui, bool allowWrites)
    : ui_(std::move(ui)), allowWrites_(allowWrites), shellExe_(ShellCli::locate())
{
    bindCallbacks();
    statusPoll_.start(slint::TimerMode::Repeated, 500ms, [this] { pollStatus(); });
    nsp::onLanguageChanged([this] {
        if (root_.empty() && !statusPath_.empty())
            ui_->global<SnippetsState>().set_load_error(ss(nsp::trf("The config file doesn't exist: {}", statusPath_)));
        push();
    });
}

SnippetsController::~SnippetsController() = default;

void SnippetsController::showCode(const std::string &id)
{
    expanded_.insert(id);
    push();
}

void SnippetsController::bindCallbacks()
{
    const auto &st = ui_->global<SnippetsState>();
    st.on_activated([this] {
        pollStatus();
        // Pick up changes made elsewhere (the Config Editor, a restored snapshot), and
        // programs installed since (a requirement met now).
        if (!root_.empty() && !busy_)
            load(false);
    });
    st.on_search([this](slint::SharedString text) {
        query_ = trimmed(str(text));
        push();
    });
    st.on_set_category([this](int index) {
        category_ = std::clamp(index, 0, int(categories_.size()));
        push();
    });
    st.on_toggle([this](slint::SharedString id, bool on) { toggle(str(id), on); });
    st.on_toggle_code([this](slint::SharedString id) {
        auto key = str(id);
        if (!expanded_.erase(key))
            expanded_.insert(key);
        push();
    });
    st.on_create_user_config([this] { ui_->global<AppState>().invoke_action("config-init"); });
}

void SnippetsController::toast(const std::string &kind, const std::string &title, const std::string &text)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(text));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

void SnippetsController::pollStatus()
{
    auto status = ui_->global<AppState>().get_status();
    if (!status.loaded)
        return;
    auto path = str(status.config_path);
    if (path == statusPath_ && (catalog_ || root_.empty()))
        return;
    statusPath_ = path;
    const auto &st = ui_->global<SnippetsState>();
    if (!status.config_exists || path.empty()) {
        root_.clear();
        haveFiles_ = false;
        st.set_load_error(ss(nsp::trf("The config file doesn't exist: {}", path)));
        load(false); // the catalog still lists them
        return;
    }
    root_ = fs::path(nss::widen(path)).lexically_normal();
    shared_ = str(status.config_source) == "install" || editor::isProtectedPath(root_);
    st.set_load_error("");
    load(true);
}

void SnippetsController::load(bool refresh)
{
    auto gen = ++gen_;
    auto root = root_;
    auto catalog = catalog_;
    worker_.post([this, gen, root, catalog, refresh]() mutable {
        if (!catalog)
            catalog = std::make_shared<const snippets::Catalog>(snippets::BundledSource{}.load());
        auto out = std::make_shared<Loaded>();
        if (!root.empty()) {
            const auto dir = root.parent_path();
            std::string bytes;
            if (nss::readFile(dir / nss::widen(snippets::kListPath), bytes))
                out->files.list = bytes;
            for (const auto &s : catalog->snippets) {
                std::string copy;
                if (nss::readFile(dir / nss::widen(snippets::copyPath(s.id)), copy))
                    out->files.copies[s.id] = copy;
                else
                    out->files.copies[s.id] = std::nullopt;
            }
        }
        auto probe = snippets::systemProbe();
        for (const auto &s : catalog->snippets)
            for (const auto &r : s.needs)
                out->requirements[s.id].push_back(snippets::checkRequirement(r, probe));
        slint::invoke_from_event_loop([this, gen, catalog, out, root, refresh] {
            if (gen != gen_)
                return;
            catalog_ = catalog;
            loaded_ = std::move(*out);
            haveFiles_ = !root.empty();
            // Categories: the known ones in their order, then any other the catalog has.
            categories_.clear();
            for (const auto &c : snippets::knownCategories())
                if (std::any_of(catalog_->snippets.begin(), catalog_->snippets.end(),
                                [&](const snippets::Snippet &s) { return s.category == c; }))
                    categories_.push_back(c);
            for (const auto &s : catalog_->snippets)
                if (std::find(categories_.begin(), categories_.end(), s.category) == categories_.end() && !s.category.empty())
                    categories_.push_back(s.category);
            category_ = std::min(category_, int(categories_.size()));
            const auto &st = ui_->global<SnippetsState>();
            st.set_loaded(true);
            st.set_shared_config(shared_);
            push();

            // Copies the user never edited follow the bundled version (once per config).
            const auto key = nss::pathKey(root);
            if (refresh && allowWrites_ && haveFiles_ && !shared_ && !busy_ && !shellExe_.empty() && !refreshed_.count(key)) {
                refreshed_.insert(key);
                auto plan = snippets::planRefresh(*catalog_, loaded_.files);
                if (!plan.empty()) {
                    std::string reason = "Snippets: ";
                    for (size_t i = 0; i < plan.changes.size(); ++i)
                        reason += (i ? ", " : "") + plan.changes[i];
                    if (plan.changes.empty())
                        reason += "record the installed versions";
                    apply(std::move(plan), reason, true);
                }
            }
        });
    });
}

std::shared_ptr<slint::VectorModel<CodeLine>> SnippetsController::code(const snippets::Snippet &s, int &columns)
{
    auto rows = std::make_shared<slint::VectorModel<CodeLine>>();
    size_t maxColumns = 0;
    int number = 0;
    for (const auto &line : hl::highlight(s.text)) {
        ++number;
        auto spans = std::make_shared<slint::VectorModel<CodeSpan>>();
        size_t n = 0;
        for (const auto &sp : line.spans) {
            spans->push_back(CodeSpan{ ss(sp.text), sp.kind });
            n += sp.text.size();
        }
        maxColumns = std::max(maxColumns, n);
        rows->push_back(CodeLine{ number, spans, 0 });
    }
    columns = int(std::min<size_t>(maxColumns, 2000));
    return rows;
}

void SnippetsController::push()
{
    const auto &st = ui_->global<SnippetsState>();
    const auto lang = nsp::currentLanguage();

    std::vector<slint::SharedString> cats{ ss(nsp::tr("All")) };
    for (const auto &c : categories_) {
        const char *label = snippets::categoryLabel(c);
        cats.push_back(ss(label ? std::string(nsp::tr(label)) : c));
    }
    st.set_categories(std::make_shared<slint::VectorModel<slint::SharedString>>(cats));
    st.set_category(category_);

    std::vector<SnippetRow> rows;
    int on = 0, total = 0;
    if (catalog_) {
        const std::string wanted = category_ > 0 && category_ <= int(categories_.size()) ? categories_[size_t(category_ - 1)] : "";
        for (const auto &s : catalog_->snippets) {
            ++total;
            auto state = haveFiles_ ? snippets::copyState(s, loaded_.files) : snippets::CopyState::Off;
            const bool enabled = state != snippets::CopyState::Off;
            on += enabled;
            if (!wanted.empty() && s.category != wanted)
                continue;
            const auto &title = s.titleIn(lang);
            const auto &description = s.descriptionIn(lang);
            if (!query_.empty() && !contains(title, query_) && !contains(description, query_) && !contains(s.title, query_) &&
                !contains(s.description, query_) && !contains(s.id, query_))
                continue;

            // What it needs: the missing apps, or a newer Shell.
            std::vector<std::string> missing;
            if (auto r = loaded_.requirements.find(s.id); r != loaded_.requirements.end())
                for (const auto &req : r->second)
                    if (!req.met)
                        missing.push_back(req.name);
            std::string needs;
            if (!snippets::versionMet(s.minVersion, SETTINGS_VERSION))
                needs = nsp::trf("Needs Shell {} or later", s.minVersion);
            else if (!missing.empty()) {
                std::string names;
                for (size_t i = 0; i < missing.size(); ++i)
                    names += (i == 0 ? "" : i + 1 == missing.size() ? nsp::tr(" and ") : ", ") + missing[i];
                needs = nsp::trf("Needs {}", names);
            }

            std::string note;
            switch (state) {
            case snippets::CopyState::Edited:
                note = nsp::tr("You changed this copy, so newer versions aren't applied to it.");
                break;
            case snippets::CopyState::Missing:
                note = nsp::trf("The copy in {} is missing. Turn the snippet off and on again.", "imports\\snippets");
                break;
            case snippets::CopyState::Outdated:
                note = nsp::tr("A newer version is applied the next time the app starts.");
                break;
            default:
                break;
            }

            const bool expanded = expanded_.count(s.id) > 0;
            int columns = 0;
            auto lines = expanded ? code(s, columns) : std::make_shared<slint::VectorModel<CodeLine>>();
            const char *label = snippets::categoryLabel(s.category);
            rows.push_back(SnippetRow{
                ss(s.id), ss(title), ss(description), ss(s.author),
                ss(label ? std::string(nsp::tr(label)) : s.category), enabled,
                // A snippet that is on can always be turned off.
                needs.empty() || enabled, ss(needs),
                ss(s.risk == snippets::Risk::Admin ? "admin" : s.risk == snippets::Risk::Network ? "network" : ""),
                ss(note), expanded, lines, columns });
        }
    }
    st.set_rows(std::make_shared<slint::VectorModel<SnippetRow>>(rows));
    st.set_summary(ss(catalog_ ? nsp::trn("{n} of {0} on", "{n} of {0} on", on, total) : std::string()));
    st.set_empty_text(ss(!catalog_ ? std::string(nsp::tr("Loading…"))
                         : catalog_->snippets.empty() ? std::string(nsp::tr("No snippets are available."))
                                                      : std::string(nsp::tr("No snippet matches the search."))));
    st.set_busy(busy_);
    st.set_busy_id(ss(busyId_));
}

void SnippetsController::toggle(const std::string &id, bool on)
{
    if (busy_ || !catalog_ || !haveFiles_ || shared_ || root_.empty())
        return;
    const auto *s = catalog_->find(id);
    if (!s)
        return;
    if (shellExe_.empty()) {
        toast("error", nsp::tr("Can't check the config"), nsp::tr("shell.exe was not found, so nothing was saved."));
        return;
    }
    if (editor::isProtectedPath(root_)) {
        toast("error", nsp::tr("Not saved"), nsp::tr("Program Files is never written to."));
        return;
    }
    auto plan = snippets::planSet(*catalog_, loaded_.files, id, on);
    if (!plan.error.empty()) {
        toast("error", nsp::tr("Couldn't change this snippet"), plan.error);
        return;
    }
    if (plan.empty())
        return;
    busyId_ = id;
    apply(std::move(plan), "Snippets: " + std::string(on ? "turn on " : "turn off ") + s->title, false);
}

void SnippetsController::apply(snippets::Plan plan, const std::string &reason, bool quiet)
{
    busy_ = true;
    push();
    const bool turningOn = !plan.changes.empty() && plan.changes.front().rfind("turn on ", 0) == 0;
    const bool turningOff = !plan.changes.empty() && plan.changes.front().rfind("turn off ", 0) == 0;
    worker_.post([this, plan = std::move(plan), reason, quiet, turningOn, turningOff, root = root_, exe = shellExe_] {
        nss::SaveResult result;
        const auto dir = root.parent_path();
        std::vector<nss::FileWrite> writes;
        for (const auto &w : plan.writes)
            writes.push_back(nss::FileWrite{ (dir / nss::widen(w.path)).lexically_normal(), w.text, std::nullopt });
        if (plan.needsImport) {
            std::string error;
            auto rootDoc = nss::NssDocument::load(root, {}, &error);
            if (!rootDoc)
                result.error = nsp::trf("Couldn't read {}: {}", nss::narrow(root.wstring()), error);
            else if (snippets::ensureImported(*rootDoc))
                writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
        }
        if (result.error.empty() && !writes.empty()) {
            nss::ConfigStore store(root);
            nss::SaveOptions options;
            options.reason = reason;
            options.validator = nss::makeShellCheckValidator(exe);
            options.verify = options.validator; // on the real files: a failure rolls them back
            result = store.save(std::move(writes), options);
        } else if (result.error.empty()) {
            result.ok = true;
        }
        // The copies of snippets turned off go once the list no longer imports them (the
        // snapshot taken before has them).
        if (result.ok)
            for (const auto &r : plan.removes) {
                std::error_code ec;
                fs::remove(dir / nss::widen(r), ec);
            }
        slint::invoke_from_event_loop([this, result = std::move(result), quiet, turningOn, turningOff, root] {
            busy_ = false;
            const std::string id = busyId_;
            busyId_.clear();
            if (result.ok) {
                if (!quiet) {
                    const auto *s = catalog_ ? catalog_->find(id) : nullptr;
                    const std::string name = s ? s->titleIn(nsp::currentLanguage()) : id;
                    if (turningOn)
                        toast("ok", nsp::trf("Turned on {}", name),
                              nsp::tr("It's in the menu the next time you right-click."));
                    else if (turningOff)
                        toast("ok", nsp::trf("Turned off {}", name),
                              result.snapshotId.empty() ? std::string()
                                                        : nsp::trf("Snapshot {} has it if you want it back.", result.snapshotId));
                }
                load(false);
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
                toast("error", result.rolledBack ? nsp::tr("Shell rejected the change, so it was undone")
                                                 : nsp::tr("Not saved: the config would fail to load"),
                      detail);
            } else {
                toast("error", nsp::tr("Couldn't save"), detail);
            }
            load(false);
        });
    });
}
