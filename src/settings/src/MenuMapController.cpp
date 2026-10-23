#include "MenuMapController.h"

#include "EditorSession.h"
#include "MapRules.h"
#include "Tr.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using menumap::Node;
using menumap::Origin;

namespace {

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }
std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }

constexpr int kEditorPage = 6; // Pages.editor in ui/types.slint
constexpr int kMenusPage = 4;  // Pages.menus
constexpr int kMapPage = 10;   // Pages.menumap

// Contexts, in the order of the page's switcher.
constexpr const char *kContexts[] = { "file", "folder", "back", "desktop", "drive", "taskbar" };
constexpr const char *kContextLabels[] = { nsp::mark("File"),    nsp::mark("Folder"), nsp::mark("Background"),
                                           nsp::mark("Desktop"), nsp::mark("Drive"),  nsp::mark("Taskbar") };
// Sample files for the File context.
constexpr const wchar_t *kSampleExt[] = { L".txt", L".png", L".exe", L".zip", L".pdf", L".mp3" };
constexpr const char *kSampleLabels[] = { nsp::mark("Text file (.txt)"), nsp::mark("Image (.png)"),
                                          nsp::mark("Program (.exe)"),   nsp::mark("Archive (.zip)"),
                                          nsp::mark("PDF (.pdf)"),       nsp::mark("Music (.mp3)") };

// The node view's geometry, in logical pixels (ui/pages/menu-map.slint draws the
// panels this wide).
constexpr float kPanelWidth = 248;
constexpr float kColumnGap = 64;
constexpr float kHeader = 34;
constexpr float kRow = 28;
constexpr float kPanelPad = 8;
constexpr float kPanelGap = 20;
constexpr float kMargin = 16;

bool isUnder(const fs::path &child, const fs::path &parent)
{
    auto c = nss::pathKey(child), p = nss::pathKey(parent);
    if (p.empty())
        return false;
    if (p.back() != '\\')
        p += '\\';
    return c.rfind(p, 0) == 0;
}

std::string lower(std::string s)
{
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// %TEMP%\NilesoftShellPro\menu-map: the sample file and folder whose menus are read.
fs::path sampleDir()
{
    wchar_t buf[MAX_PATH + 1]{};
    DWORD n = GetTempPathW(MAX_PATH, buf);
    fs::path dir = n ? fs::path(std::wstring(buf, n)) : fs::temp_directory_path();
    return dir / L"NilesoftShellPro" / L"menu-map";
}

// The path a context's menu is read for: an empty sample file of the chosen type,
// a sample folder, or the system drive. Empty for the desktop and the taskbar.
fs::path prepareSample(int context, int sample)
{
    std::error_code ec;
    auto dir = sampleDir();
    const std::string ctx = kContexts[context];
    if (ctx == "file") {
        fs::create_directories(dir, ec);
        auto file = dir / (std::wstring(L"Sample") + kSampleExt[sample]);
        if (!fs::exists(file, ec)) {
            std::ofstream out(file, std::ios::binary);
            if (std::wstring(kSampleExt[sample]) == L".zip") {
                // An empty archive (end of central directory only), so it opens as one.
                const char eocd[22] = { 'P', 'K', 5, 6 };
                out.write(eocd, sizeof eocd);
            }
        }
        return file;
    }
    if (ctx == "folder" || ctx == "back") {
        auto folder = dir / L"Sample folder";
        fs::create_directories(folder, ec);
        return folder;
    }
    if (ctx == "drive") {
        wchar_t drive[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"SystemDrive", drive, MAX_PATH))
            wcscpy_s(drive, L"C:");
        return fs::path(std::wstring(drive) + L"\\");
    }
    return {};
}

std::vector<std::string> readLines(const fs::path &file)
{
    std::vector<std::string> lines;
    auto doc = nss::NssDocument::load(file);
    if (!doc)
        return lines;
    std::string_view text = doc->text();
    size_t at = 0;
    while (at <= text.size()) {
        auto end = text.find('\n', at);
        auto line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.emplace_back(line);
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return lines;
}

std::optional<nss::NssDocument> loadDecls(const fs::path &file, std::string *error)
{
    auto doc = nss::NssDocument::load(file, {}, error);
    if (doc && !doc->diagnostics().empty()) {
        // A file imported inside menu{}: its declarations parse as a menu body.
        nss::ParseContext menu;
        menu.mode = nss::ParseMode::Menu;
        if (auto alt = nss::NssDocument::load(file, menu, nullptr); alt && alt->diagnostics().empty())
            return alt;
    }
    return doc;
}

std::string fmt(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", double(v));
    return buf;
}

} // namespace

MenuMapController::MenuMapController(slint::ComponentHandle<AppWindow> ui, SaveService &save)
    : ui_(std::move(ui)), save_(save), shellExe_(ShellCli::locate())
{
    bindCallbacks();
    nsp::onLanguageChanged([this] { push(); });
    // Pending changes (this page's, or a Discard in the footer): the switches follow.
    save_.onChanged([this] { push(); });
    // A save went through or the config was replaced: read the map again.
    save_.onConfig([this] {
        stale_ = true;
        if (ui_->global<AppState>().get_page() == kMapPage)
            activated();
    });
}

MenuMapController::~MenuMapController() = default;

void MenuMapController::setDevOptions(DevOptions options)
{
    dev_ = std::move(options);
    devPending_ = true;
    context_ = std::clamp(dev_.context, 0, int(std::size(kContexts)) - 1);
    view_ = dev_.view == 1 ? 1 : 0;
    stale_ = true;
}

void MenuMapController::bindCallbacks()
{
    const auto &st = ui_->global<MenuMapState>();
    st.on_activated([this] { activated(); });
    st.on_set_context([this](int i) {
        if (i < 0 || i >= int(std::size(kContexts)) || i == context_)
            return;
        context_ = i;
        load();
    });
    st.on_set_sample([this](int i) {
        if (i < 0 || i >= int(std::size(kSampleExt)) || i == sample_)
            return;
        sample_ = i;
        load();
    });
    st.on_set_view([this](int v) {
        view_ = v == 1 ? 1 : 0;
        ui_->global<MenuMapState>().set_view(view_);
        push();
    });
    st.on_refresh([this] { load(); });
    st.on_select([this](int key) { select(key); });
    st.on_toggle_expand([this](int key) { toggleExpand(key); });
    st.on_expand_all([this](bool on) { expandAll(on); });
    st.on_toggle([this](int key, bool on) { toggle(key, on); });
    st.on_rename([this](slint::SharedString t) { rename(str(t)); });
    st.on_reset_title([this] { resetTitle(); });
    st.on_move_to([this](int i) { moveTo(i); });
    st.on_reset_menu([this] { resetMenu(); });
    st.on_edit_code([this] { editCode(); });
    st.on_edit_edited([this](slint::SharedString t, int cursor, int beforeAnchor, int beforeCursor) {
        auto r = editDoc_.input(str(t), size_t(std::max(0, cursor)), codeui::selection(beforeAnchor, beforeCursor));
        editChanged(r);
        return codeui::toSlint(r);
    });
    st.on_edit_command([this](slint::SharedString name, int anchor, int cursor) {
        auto r = codeui::command(editDoc_, str(name), anchor, cursor, false);
        editChanged(r);
        return codeui::toSlint(r);
    });
    st.on_edit_find([this](slint::SharedString q, int anchor, int cursor, bool backwards, bool incremental) {
        return codeui::find(editDoc_, str(q), anchor, cursor, backwards, incremental);
    });
    st.on_edit_selected_text([this](int anchor, int cursor) { return ss(codeui::findSeed(editDoc_, anchor, cursor)); });
    st.on_edit_save([this] { editSave(); });
    st.on_edit_cancel([this] { editCancel(); });
    st.on_open_in_editor([this] {
        if (selected_ >= 0 && entries_[size_t(selected_)].node)
            openInEditor(nss::narrow(entries_[size_t(selected_)].node->file.wstring()),
                         entries_[size_t(selected_)].node->line);
    });
    st.on_open_rule([this] {
        auto d = ui_->global<MenuMapState>().get_detail();
        if (!d.rule_path.empty())
            openInEditor(str(d.rule_path), d.rule_line);
    });
    st.on_open_context_menus([this] { ui_->global<AppState>().set_page(kMenusPage); });
    st.on_create_user_config([this] { ui_->global<AppState>().invoke_action("config-init"); });
}

void MenuMapController::toast(const std::string &kind, const std::string &title, const std::string &text)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(text));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

void MenuMapController::activated()
{
    auto status = ui_->global<AppState>().get_status();
    if (!status.loaded)
        return; // AppController fills it in shortly; the page asks again on refresh
    fs::path root = status.config_exists ? fs::path(nss::widen(str(status.config_path))).lexically_normal() : fs::path();
    bool shared = str(status.config_source) == "install" || (!root.empty() && editor::isProtectedPath(root));
    if (root != root_ || shared != shared_) {
        root_ = root;
        shared_ = shared;
        stale_ = true;
    }
    ui_->global<MenuMapState>().set_shared_config(shared_);
    if (stale_ || !haveTree_)
        load();
    else
        push();
}

void MenuMapController::load()
{
    const auto &st = ui_->global<MenuMapState>();
    stale_ = false;
    if (root_.empty()) {
        st.set_error(ss(nsp::tr("The config file doesn't exist. Create a personal config from Home first.")));
        st.set_loaded(true);
        return;
    }
    if (shellExe_.empty()) {
        st.set_error(ss(nsp::tr("shell.exe was not found next to NilesoftShellPro.exe or the registered shell.dll.")));
        st.set_loaded(true);
        return;
    }
    auto gen = ++gen_;
    st.set_loading(true);
    worker_.post([this, gen, root = root_, exe = shellExe_, context = context_, sample = sample_] {
        auto samplePath = prepareSample(context, sample);
        std::vector<std::wstring> args{ L"-config:tree", root.wstring(),
                                        L"-context:" + nss::widen(kContexts[context]) };
        if (!samplePath.empty())
            args.push_back(L"-sample:" + samplePath.wstring());
        auto r = ShellCli(exe).run(args, 30000);
        std::string error;
        std::optional<menumap::Tree> tree;
        if (!r.started)
            error = nss::narrow(r.error);
        else if (r.timedOut)
            error = nsp::tr("shell.exe took too long to read the menu and was stopped.");
        else {
            std::string why;
            tree = menumap::parseTree(r.output, &why);
            if (!tree)
                error = nsp::trf("shell.exe gave no menu ({}). It may be older than this app.", why);
        }
        auto lines = readLines(root.parent_path() / fs::path(nss::widen(maprules::relativePath)));
        slint::invoke_from_event_loop([this, gen, tree = std::move(tree), error, lines = std::move(lines)]() mutable {
            if (gen != gen_)
                return;
            const auto &st = ui_->global<MenuMapState>();
            st.set_loading(false);
            st.set_loaded(true);
            st.set_error(ss(error));
            if (!tree) {
                haveTree_ = false;
                tree_ = {};
                entries_.clear();
                selected_ = -1;
                push();
                return;
            }
            tree_ = std::move(*tree);
            haveTree_ = true;
            mapLines_ = std::move(lines);
            buildEntries();
            if (devPending_) {
                devPending_ = false;
                if (dev_.expandAll) {
                    collapsed_.clear();
                    for (const auto &e : entries_)
                        if (e.node && e.node->kind == "menu" && !e.kids.empty())
                            expanded_.insert(e.key);
                }
                for (size_t i = 0; i < entries_.size() && !dev_.select.empty(); ++i)
                    if (entries_[i].node && entries_[i].node->title == dev_.select) {
                        selected_ = int(i);
                        selectedKey_ = entries_[i].key;
                        // Its submenus open, so the node view shows where it is.
                        for (int p = entries_[i].parent; p >= 0; p = entries_[size_t(p)].parent)
                            expanded_.insert(entries_[size_t(p)].key);
                        break;
                    }
            }
            push();
        });
    });
}

void MenuMapController::buildEntries()
{
    entries_.clear();
    targets_.clear();
    selected_ = -1;
    std::function<void(const std::vector<Node> &, int, int, const std::string &, const std::string &)> add =
        [&](const std::vector<Node> &list, int parent, int depth, const std::string &prefix, const std::string &menuPath) {
            for (size_t i = 0; i < list.size(); ++i) {
                const auto &n = list[i];
                Entry e;
                e.node = &n;
                e.group = entries_[size_t(parent)].group;
                e.depth = depth;
                e.parent = parent;
                std::string name = n.origin == Origin::System ? lower(n.name) : n.title + "@" + std::to_string(n.line);
                e.key = prefix + "/" + std::to_string(i) + ":" + name;
                if (n.kind == "menu") {
                    if (n.origin == Origin::System)
                        e.menuPath = n.path.empty() ? lower(n.name) : n.path + "/" + lower(n.name);
                    else if (n.state == "text")
                        e.menuPath = menuPath.empty() ? n.rawTitle : menuPath + "/" + n.rawTitle;
                }
                int idx = int(entries_.size());
                entries_.push_back(e);
                entries_[size_t(parent)].kids.push_back(idx);
                if (!e.menuPath.empty() && !e.menuPath.empty() && (n.origin == Origin::Shell || !n.name.empty())) {
                    std::string label = n.origin == Origin::Shell ? menumap::stripAccelerators(e.menuPath)
                                                                  : nsp::trf("{} (Windows)", n.title);
                    targets_.push_back(MoveTarget{ label, e.menuPath });
                }
                add(n.children, idx, depth + 1, e.key, e.menuPath);
            }
        };
    for (int group = 0; group < 2; ++group) {
        Entry g;
        g.group = group;
        g.key = group == 0 ? "s" : "w";
        int idx = int(entries_.size());
        entries_.push_back(g);
        add(group == 0 ? tree_.shell : tree_.system, idx, 1, g.key, {});
    }
    // Keep the selection across a reload when the item is still there.
    for (size_t i = 0; i < entries_.size(); ++i)
        if (!selectedKey_.empty() && entries_[i].key == selectedKey_)
            selected_ = int(i);
}

std::string MenuMapController::fileLabel(const fs::path &file) const
{
    if (file.empty())
        return {};
    auto dir = root_.parent_path();
    if (isUnder(file, dir))
        return nss::narrow(file.lexically_relative(dir).generic_wstring());
    return nss::narrow(file.filename().wstring());
}

bool MenuMapController::writable(const fs::path &file) const
{
    return !shared_ && !file.empty() && isUnder(file, root_.parent_path()) && !editor::isProtectedPath(file);
}

std::vector<std::string> MenuMapController::mapLinesOf(const std::vector<int> &indexes, const char *what) const
{
    std::vector<std::string> out;
    const std::string w = what;
    for (int i : indexes) {
        if (i < 0 || i >= int(tree_.rules.size()))
            continue;
        const auto &r = tree_.rules[size_t(i)];
        if (!menumap::isMapFile(r.file) || r.line < 1 || r.line > int(mapLines_.size()))
            continue;
        bool match = w == "vis" ? r.vis == "hidden" : w == "title" ? !r.title.empty() : w == "menu" ? !r.menu.empty() : true;
        if (match)
            out.push_back(std::string(nss::trim(mapLines_[size_t(r.line - 1)])));
    }
    return out;
}

MenuMapController::ItemState MenuMapController::stateOf(const Entry &e) const
{
    ItemState s;
    if (!e.node || e.node->kind == "separator")
        return s;
    const auto &n = *e.node;
    const auto &pending = save_.pending();
    if (n.origin == Origin::Shell) {
        s.shown = n.vis != "hidden";
        s.canToggle = writable(n.file) && n.line > 0 && (n.vis.empty() || n.vis == "hidden" || n.vis == "enabled");
        if (shared_)
            s.note = nsp::tr("The shared config is read-only here.");
        else if (!writable(n.file))
            s.note = nsp::tr("This file is read-only here.");
        else if (!s.canToggle)
            s.note = nsp::tr("It has its own vis= setting; change it with Edit code.");
        return s;
    }

    // Windows' items: rules.
    std::vector<int> hiding;
    for (int i : n.rules)
        if (i >= 0 && i < int(tree_.rules.size()) && tree_.rules[size_t(i)].vis == "hidden")
            hiding.push_back(i);
    s.mapLines = mapLinesOf(hiding, "vis");
    bool hiddenElsewhere = s.mapLines.size() < hiding.size();
    if (n.hidden) {
        bool allPending = !s.mapLines.empty();
        for (const auto &line : s.mapLines) {
            const auto *c = pending.findRule(line);
            allPending &= c && !c->add;
        }
        s.shown = !hiddenElsewhere && allPending;
        s.changed = allPending;
        s.canToggle = !hiddenElsewhere && !s.mapLines.empty() && !shared_;
        if (hiddenElsewhere)
            s.note = nsp::tr("A rule in your config hides it. Turn that rule off on the Context Menus page.");
    } else {
        auto rule = menumap::hideRule(n);
        const auto *c = rule.empty() ? nullptr : pending.findRule(rule);
        s.shown = !(c && c->add);
        s.changed = c != nullptr;
        s.canToggle = !rule.empty() && !shared_;
        if (rule.empty())
            s.note = nsp::tr("Its title changes with the selection, so a rule can't name it. Use the Config Editor.");
    }
    if (shared_)
        s.note = nsp::tr("The shared config is read-only here.");
    // Renames and moves waiting to be saved.
    auto props = menumap::matchProps(n);
    if (!props.empty())
        for (const auto &[text, change] : pending.rules())
            if (text.find(props) != std::string::npos)
                s.changed = true;
    return s;
}

std::string MenuMapController::statusOf(const Entry &e, std::string *kind) const
{
    kind->clear();
    if (!e.node)
        return {};
    const auto &n = *e.node;
    if (n.origin == Origin::Shell) {
        if (n.vis == "hidden") {
            *kind = "hidden";
            return nsp::tr("Hidden");
        }
        if (n.vis == "disabled") {
            *kind = "maybe";
            return nsp::tr("Disabled");
        }
        bool sometimes = n.vis == "dynamic";
        for (const auto &c : n.conditions)
            sometimes |= c == "where" || c == "find";
        if (sometimes) {
            *kind = "maybe";
            return nsp::tr("Shows only sometimes");
        }
        return {};
    }
    if (n.hidden) {
        *kind = "hidden";
        return nsp::tr("Hidden");
    }
    if (!n.moved.empty()) {
        *kind = "moved";
        return nsp::trf("Moved to {}", menumap::stripAccelerators(n.moved));
    }
    if (!n.renamed.empty()) {
        *kind = "renamed";
        return nsp::trf("Renamed to {}", n.renamed);
    }
    if (n.maybeHidden) {
        *kind = "maybe";
        return nsp::tr("May be hidden");
    }
    if (!n.maybe.empty()) {
        *kind = "maybe";
        return nsp::tr("A rule may change it");
    }
    return {};
}

std::string MenuMapController::originLabel(const Entry &e) const
{
    if (!e.node)
        return {};
    const auto &n = *e.node;
    if (n.origin == Origin::Shell)
        return n.line > 0 ? nsp::trf("{}:{}", fileLabel(n.file), n.line) : fileLabel(n.file);
    if (n.kind == "separator")
        return {};
    if (n.source == "windows")
        return nsp::tr("Windows");
    if (n.source == "program")
        return n.program.empty() ? std::string(nsp::tr("A program")) : n.program;
    return nsp::tr("Unknown");
}

void MenuMapController::push()
{
    const auto &st = ui_->global<MenuMapState>();
    std::vector<slint::SharedString> contexts, samples;
    for (auto label : kContextLabels)
        contexts.push_back(ss(nsp::tr(label)));
    for (auto label : kSampleLabels)
        samples.push_back(ss(nsp::tr(label)));
    st.set_contexts(std::make_shared<slint::VectorModel<slint::SharedString>>(contexts));
    st.set_samples(std::make_shared<slint::VectorModel<slint::SharedString>>(samples));
    st.set_context(context_);
    st.set_sample(sample_);
    st.set_view(view_);

    std::vector<slint::SharedString> errors;
    for (const auto &e : tree_.errors)
        errors.push_back(ss(e));
    st.set_config_errors(std::make_shared<slint::VectorModel<slint::SharedString>>(errors));

    std::string notice;
    if (haveTree_ && tree_.ok) {
        if (tree_.systemError == "taskbar")
            notice = nsp::tr("Windows builds the taskbar's menu itself, so only Shell's items are listed here.");
        else if (!tree_.systemError.empty())
            notice = nsp::trf("Windows' items couldn't be read: {}", tree_.systemError);
    }
    st.set_notice(ss(notice));

    size_t shellCount = 0, systemCount = 0;
    for (const auto &e : entries_)
        if (e.node && e.node->kind != "separator")
            (e.group == 0 ? shellCount : systemCount)++;
    st.set_summary(ss(haveTree_ ? nsp::trf("{} · {}", nsp::trn("{n} of your items", "{n} of your items", (long long)shellCount),
                                           nsp::trn("{n} from Windows and programs", "{n} from Windows and programs",
                                                    (long long)systemCount))
                                : std::string{}));

    std::vector<slint::SharedString> targets;
    for (const auto &t : targets_)
        targets.push_back(ss(t.label));
    st.set_move_targets(std::make_shared<slint::VectorModel<slint::SharedString>>(targets));

    pushRows();
    pushPanels();
    pushDetail();
}

namespace {

MapRow makeRow(int key, int depth, std::string kind, std::string title)
{
    MapRow r{};
    r.key = key;
    r.depth = depth;
    r.kind = ss(kind);
    r.title = ss(title);
    r.shown = true;
    return r;
}

} // namespace

void MenuMapController::pushRows()
{
    std::vector<MapRow> rows;
    auto rowFor = [&](int idx, int depth) {
        const auto &e = entries_[size_t(idx)];
        if (!e.node) {
            auto r = makeRow(idx, 0, "group", e.group == 0 ? nsp::tr("Your Shell items") : nsp::tr("Windows and other programs"));
            r.expandable = !e.kids.empty();
            r.expanded = !collapsed_.count(e.key);
            r.origin_label = ss(nsp::trn("{n} item", "{n} items", (long long)e.kids.size()));
            r.selected = false;
            return r;
        }
        const auto &n = *e.node;
        std::string title = n.title;
        if (n.kind != "separator" && title.empty())
            title = n.ownerDrawn ? nsp::tr("(drawn by its program)")
                  : n.state == "dynamic" ? nsp::tr("(set when the menu opens)")
                                         : nsp::tr("(no title)");
        auto r = makeRow(idx, depth, n.kind, title);
        r.origin = ss(n.origin == Origin::Shell ? std::string("shell") : n.source);
        r.origin_label = ss(originLabel(e));
        std::string kind;
        r.status = ss(statusOf(e, &kind));
        r.status_kind = ss(kind);
        r.expandable = n.kind == "menu" && !n.children.empty();
        r.expanded = expanded_.count(e.key) > 0;
        auto s = stateOf(e);
        r.shown = s.shown;
        r.can_toggle = s.canToggle && n.kind != "separator";
        r.changed = s.changed;
        r.dim = !s.shown;
        r.selected = idx == selected_;
        return r;
    };
    std::function<void(int, int)> add = [&](int idx, int depth) {
        rows.push_back(rowFor(idx, depth));
        const auto &e = entries_[size_t(idx)];
        bool open = !e.node ? !collapsed_.count(e.key) : expanded_.count(e.key) > 0;
        if (open)
            for (int k : e.kids)
                add(k, depth + 1);
    };
    for (size_t i = 0; i < entries_.size(); ++i)
        if (!entries_[i].node)
            add(int(i), 0);
    ui_->global<MenuMapState>().set_rows(std::make_shared<slint::VectorModel<MapRow>>(rows));
}

void MenuMapController::pushPanels()
{
    const auto &st = ui_->global<MenuMapState>();
    std::vector<MapPanel> panels;
    std::vector<float> nextY;
    std::string links;
    float maxX = 0, maxY = 0;

    std::function<float(int, size_t, float)> place = [&](int idx, size_t col, float wantY) -> float {
        if (nextY.size() <= col)
            nextY.resize(col + 1, kMargin);
        const auto &e = entries_[size_t(idx)];
        float x = kMargin + float(col) * (kPanelWidth + kColumnGap);
        float y = std::max(nextY[col], wantY);
        float h = kHeader + float(e.kids.size()) * kRow + kPanelPad;
        nextY[col] = y + h + kPanelGap;
        maxX = std::max(maxX, x + kPanelWidth);
        maxY = std::max(maxY, y + h);

        MapPanel p{};
        p.key = idx;
        p.title = ss(!e.node ? std::string(e.group == 0 ? nsp::tr("Your Shell items") : nsp::tr("Windows and other programs"))
                             : e.node->title);
        p.origin = ss(!e.node ? std::string() : e.node->origin == Origin::Shell ? std::string("shell") : e.node->source);
        p.x = x;
        p.y = y;
        p.height = h;
        auto rows = std::make_shared<slint::VectorModel<MapRow>>();
        for (size_t j = 0; j < e.kids.size(); ++j) {
            int k = e.kids[j];
            const auto &ke = entries_[size_t(k)];
            const auto &n = *ke.node;
            std::string title = n.title;
            if (n.kind != "separator" && title.empty())
                title = n.state == "dynamic" ? nsp::tr("(set when the menu opens)") : nsp::tr("(no title)");
            auto r = makeRow(k, 0, n.kind, title);
            r.origin = ss(n.origin == Origin::Shell ? std::string("shell") : n.source);
            r.origin_label = ss(originLabel(ke));
            std::string kind;
            r.status = ss(statusOf(ke, &kind));
            r.status_kind = ss(kind);
            r.expandable = n.kind == "menu" && !n.children.empty();
            r.expanded = expanded_.count(ke.key) > 0;
            auto s = stateOf(ke);
            r.shown = s.shown;
            r.can_toggle = s.canToggle && n.kind != "separator";
            r.changed = s.changed;
            r.dim = !s.shown;
            r.selected = k == selected_;
            rows->push_back(r);
        }
        p.rows = rows;
        size_t at = panels.size();
        panels.push_back(p);
        (void)at;

        // Open submenus go in the next column, next to the item that opens them.
        for (size_t j = 0; j < e.kids.size(); ++j) {
            int k = e.kids[j];
            const auto &ke = entries_[size_t(k)];
            if (!ke.node || ke.node->kind != "menu" || ke.kids.empty() || !expanded_.count(ke.key))
                continue;
            float rowY = y + kHeader + float(j) * kRow + kRow / 2;
            float cy = place(k, col + 1, rowY - kHeader / 2);
            float x1 = x + kPanelWidth, x2 = x + kPanelWidth + kColumnGap, y2 = cy + kHeader / 2;
            links += "M " + fmt(x1) + " " + fmt(rowY) + " C " + fmt(x1 + kColumnGap / 2) + " " + fmt(rowY) + " " +
                     fmt(x2 - kColumnGap / 2) + " " + fmt(y2) + " " + fmt(x2) + " " + fmt(y2) + " ";
        }
        return y;
    };
    for (size_t i = 0; i < entries_.size(); ++i)
        if (!entries_[i].node)
            place(int(i), 0, 0);

    st.set_panels(std::make_shared<slint::VectorModel<MapPanel>>(panels));
    st.set_links(ss(links.empty() ? std::string("M 0 0") : links));
    st.set_canvas_width(maxX + kMargin);
    st.set_canvas_height(maxY + kMargin);
}

void MenuMapController::pushDetail()
{
    const auto &st = ui_->global<MenuMapState>();
    MapDetail d{};
    d.key = -1;
    if (selected_ < 0 || selected_ >= int(entries_.size()) || !entries_[size_t(selected_)].node) {
        st.set_detail(d);
        return;
    }
    const auto &e = entries_[size_t(selected_)];
    const auto &n = *e.node;
    auto s = stateOf(e);
    d.key = selected_;
    d.title = ss(n.title.empty() ? std::string(nsp::tr("(no title)")) : n.title);
    d.kind_label = ss(n.kind == "menu" ? nsp::tr("Submenu") : n.kind == "separator" ? nsp::tr("Separator") : nsp::tr("Item"));
    d.origin = ss(n.origin == Origin::Shell ? std::string("shell") : n.source);
    d.shown = s.shown;
    d.can_toggle = s.canToggle && n.kind != "separator";

    std::string status, ruleLabel, rulePath;
    int ruleLine = 0;
    auto noteRule = [&](const menumap::Rule &r) {
        if (rulePath.empty()) {
            rulePath = nss::narrow(r.file.wstring());
            ruleLine = r.line;
            ruleLabel = nsp::trf("{}, line {}", fileLabel(r.file), r.line);
        }
    };

    if (n.origin == Origin::Shell) {
        d.origin_text = ss(nsp::tr("From your config"));
        d.file_label = ss(nsp::trf("{}, line {}", fileLabel(n.file), n.line));
        d.file_path = ss(nss::narrow(n.file.wstring()));
        d.line = n.line;
        if (!n.titleSource.empty())
            d.facts = ss(nsp::trf("Title from {}", n.titleSource));
        if (auto doc = loadDecls(n.file, nullptr))
            d.conditions = ss(menumap::conditionText(*doc, size_t(n.line), size_t(n.column)));
        if (n.vis == "hidden")
            status = nsp::tr("Hidden by vis= in its own line.");
        d.can_rename = writable(n.file) && n.kind != "separator";
        d.rename_value = ss(n.title);
        d.move_note = ss(nsp::tr("To move one of your items, use Edit code or the Menu Builder."));
        d.can_edit_code = writable(n.file) && n.line > 0;
        d.can_open = n.line > 0;
        d.toggle_note = ss(s.canToggle ? std::string(nsp::tr("Turning it off adds vis=vis.remove to its line. Saved right away, with a snapshot first."))
                                       : s.note);
    } else {
        if (n.source == "windows")
            d.origin_text = ss(nsp::tr("Added by Windows"));
        else if (n.source == "program")
            d.origin_text = ss(nsp::trf("Added by {}", n.program.empty() ? std::string(nsp::tr("a program")) : n.program));
        else if (n.kind != "separator")
            d.origin_text = ss(nsp::tr("Added by Windows or an installed app; Shell couldn't tell which."));
        std::string detail = n.module;
        if (!n.handler.empty())
            detail += (detail.empty() ? "" : "\n") + n.handler;
        d.origin_detail = ss(detail);
        std::string facts;
        if (!n.id.empty())
            facts = "id." + n.id;
        if (!n.verb.empty())
            facts += (facts.empty() ? "" : "  ·  ") + nsp::trf("verb {}", n.verb);
        if (!n.path.empty())
            facts += (facts.empty() ? "" : "  ·  ") + nsp::trf("in {}", n.path);
        d.facts = ss(facts);

        for (int i : n.rules) {
            if (i < 0 || i >= int(tree_.rules.size()))
                continue;
            const auto &r = tree_.rules[size_t(i)];
            auto where = menumap::isMapFile(r.file) ? std::string(nsp::tr("a rule made here"))
                                                     : nsp::trf("{}, line {}", fileLabel(r.file), r.line);
            std::string what;
            if (r.vis == "hidden")
                what = nsp::trf("Hidden by {}.", where);
            else if (!r.menu.empty())
                what = nsp::trf("Moved to {} by {}.", menumap::stripAccelerators(r.menu), where);
            else if (!r.title.empty())
                what = nsp::trf("Renamed to {} by {}.", r.title, where);
            else
                what = nsp::trf("Changed by {}.", where);
            status += (status.empty() ? "" : "\n") + what;
            if (!menumap::isMapFile(r.file))
                noteRule(r);
        }
        for (int i : n.maybe) {
            if (i < 0 || i >= int(tree_.rules.size()))
                continue;
            const auto &r = tree_.rules[size_t(i)];
            status += (status.empty() ? "" : "\n") +
                      nsp::trf("{}, line {} may change it, depending on what is selected.", fileLabel(r.file), r.line);
            noteRule(r);
        }
        bool named = !menumap::matchProps(n).empty() && !shared_;
        d.can_rename = named && n.kind != "separator";
        d.rename_value = ss(n.renamed.empty() ? n.title : n.renamed);
        d.can_reset_title = !mapLinesOf(n.rules, "title").empty() && !shared_;
        d.can_move = named && n.kind != "separator" && !targets_.empty();
        d.can_reset_menu = !mapLinesOf(n.rules, "menu").empty() && !shared_;
        if (!named && n.kind != "separator")
            d.move_note = ss(shared_ ? std::string(nsp::tr("The shared config is read-only here."))
                                     : std::string(nsp::tr("Its title changes with the selection, so a rule can't name it. Use the Config Editor.")));
        if (s.canToggle)
            d.toggle_note = ss(s.shown ? std::string(nsp::tr("Turning it off adds a rule that removes it from the menu. Save to apply."))
                                       : std::string(nsp::tr("Hidden by a rule made here. Turning it on removes that rule.")));
        else
            d.toggle_note = ss(s.note);
    }
    d.status = ss(status);
    d.rule_label = ss(ruleLabel);
    d.rule_path = ss(rulePath);
    d.rule_line = ruleLine;
    if (s.changed)
        d.pending = ss(n.hidden || !s.shown ? (s.shown ? std::string(nsp::tr("Shown again once you save."))
                                                       : std::string(nsp::tr("Hidden once you save.")))
                                            : std::string(nsp::tr("Changes once you save.")));
    st.set_detail(d);
}

void MenuMapController::select(int key)
{
    if (key < 0 || key >= int(entries_.size()) || !entries_[size_t(key)].node)
        return;
    selected_ = key;
    selectedKey_ = entries_[size_t(key)].key;
    const auto &st = ui_->global<MenuMapState>();
    st.set_detail_serial(st.get_detail_serial() + 1);
    push();
}

void MenuMapController::toggleExpand(int key)
{
    if (key < 0 || key >= int(entries_.size()))
        return;
    const auto &e = entries_[size_t(key)];
    if (!e.node) {
        if (!collapsed_.erase(e.key))
            collapsed_.insert(e.key);
    } else if (!expanded_.erase(e.key))
        expanded_.insert(e.key);
    push();
}

void MenuMapController::expandAll(bool on)
{
    if (!on) {
        expanded_.clear();
    } else {
        collapsed_.clear();
        for (const auto &e : entries_)
            if (e.node && e.node->kind == "menu" && !e.kids.empty())
                expanded_.insert(e.key);
    }
    push();
}

void MenuMapController::stageRule(const std::string &text, bool add, const std::string &label)
{
    save_.edit([&](PendingChanges &p) {
        if (add)
            p.addRule(text, label);
        else
            p.removeRule(text, label);
    });
}

void MenuMapController::toggle(int key, bool on)
{
    if (key < 0 || key >= int(entries_.size()) || !entries_[size_t(key)].node || save_.saving())
        return;
    const auto &e = entries_[size_t(key)];
    const auto &n = *e.node;
    auto s = stateOf(e);
    if (!s.canToggle || on == s.shown)
        return;

    if (n.origin == Origin::Shell) {
        saveShellEdit(n, (on ? "Menu map: show " : "Menu map: hide ") + n.title,
                      [&n, on](nss::NssDocument &doc, std::string &error) {
                          return menumap::setShellItemHidden(doc, size_t(n.line), size_t(n.column), !on, &error);
                      });
        return;
    }
    const std::string label = (on ? "show " : "hide ") + n.title;
    if (n.hidden) {
        // Hidden by rules made here: remove them (or put them back).
        for (const auto &line : s.mapLines)
            stageRule(line, !on, label);
    } else {
        stageRule(menumap::hideRule(n), !on, label);
    }
}

void MenuMapController::rename(const std::string &title)
{
    if (selected_ < 0 || !entries_[size_t(selected_)].node || save_.saving())
        return;
    const auto &n = *entries_[size_t(selected_)].node;
    auto t = std::string(nss::trim(title));
    if (t.empty())
        return;
    if (n.origin == Origin::Shell) {
        if (t == n.title)
            return;
        saveShellEdit(n, "Menu map: rename " + n.title + " to " + t, [&n, t](nss::NssDocument &doc, std::string &error) {
            return menumap::setShellItemTitle(doc, size_t(n.line), size_t(n.column), t, &error);
        });
        return;
    }
    const std::string label = "rename " + n.title;
    // Pending renames of this item give way to the new one.
    auto props = menumap::matchProps(n);
    std::vector<std::string> drop;
    for (const auto &[text, change] : save_.pending().rules())
        if (change.add && text.rfind("modify(" + props + " title=", 0) == 0)
            drop.push_back(text);
    for (const auto &text : drop)
        stageRule(text, false, label);
    for (const auto &line : mapLinesOf(n.rules, "title"))
        stageRule(line, false, label);
    if (t != n.title)
        stageRule(menumap::renameRule(n, t), true, label + " to " + t);
}

void MenuMapController::resetTitle()
{
    if (selected_ < 0 || !entries_[size_t(selected_)].node)
        return;
    const auto &n = *entries_[size_t(selected_)].node;
    for (const auto &line : mapLinesOf(n.rules, "title"))
        stageRule(line, false, "original title for " + n.title);
}

void MenuMapController::moveTo(int index)
{
    if (selected_ < 0 || !entries_[size_t(selected_)].node || index < 0 || index >= int(targets_.size()) || save_.saving())
        return;
    const auto &n = *entries_[size_t(selected_)].node;
    if (n.origin == Origin::Shell)
        return;
    const auto &target = targets_[size_t(index)];
    const std::string label = "move " + n.title + " to " + menumap::stripAccelerators(target.path);
    auto props = menumap::matchProps(n);
    std::vector<std::string> drop;
    for (const auto &[text, change] : save_.pending().rules())
        if (change.add && text.rfind("modify(" + props + " menu=", 0) == 0)
            drop.push_back(text);
    for (const auto &text : drop)
        stageRule(text, false, label);
    for (const auto &line : mapLinesOf(n.rules, "menu"))
        stageRule(line, false, label);
    stageRule(menumap::moveRule(n, target.path), true, label);
}

void MenuMapController::resetMenu()
{
    if (selected_ < 0 || !entries_[size_t(selected_)].node)
        return;
    const auto &n = *entries_[size_t(selected_)].node;
    for (const auto &line : mapLinesOf(n.rules, "menu"))
        stageRule(line, false, "put " + n.title + " back");
}

void MenuMapController::saveShellEdit(const Node &n, const std::string &reason,
                                      const std::function<bool(nss::NssDocument &, std::string &)> &edit)
{
    if (!writable(n.file) || shellExe_.empty())
        return;
    auto doc = loadDecls(n.file, nullptr);
    std::string error;
    if (!doc) {
        toast("error", nsp::tr("Not saved"), nsp::trf("{} couldn't be read.", fileLabel(n.file)));
        return;
    }
    auto before = doc->text();
    if (!edit(*doc, error)) {
        toast("error", nsp::tr("Not changed"), error);
        return;
    }
    if (doc->text() == before)
        return;
    ui_->global<MenuMapState>().set_busy(true);
    worker_.post([this, write = nss::FileWrite{ n.file, doc->text(), doc->format() }, reason, root = root_, exe = shellExe_] {
        nss::ConfigStore store(root);
        nss::SaveOptions options;
        options.reason = reason;
        options.validator = nss::makeShellCheckValidator(exe);
        auto result = store.save({ write }, options);
        slint::invoke_from_event_loop([this, result = std::move(result), root] {
            ui_->global<MenuMapState>().set_busy(false);
            if (result.ok) {
                toast("ok", nsp::tr("Saved"), result.snapshotId.empty() ? std::string(nsp::tr("No changes were needed."))
                                                                         : nsp::trf("Snapshot {} was taken first.", result.snapshotId));
                save_.reloadConfig(); // the pages that read the config, this one too
                return;
            }
            std::string detail = result.error;
            if (!result.check.ok && !result.check.message.empty()) {
                auto outcome = editor::parseCheck(result.check.message, {}, root.parent_path());
                for (const auto &i : outcome.issues)
                    if (i.error) {
                        detail = i.message;
                        break;
                    }
                toast("error", nsp::tr("Not saved: the config would fail to load"), detail);
                return;
            }
            toast("error", nsp::tr("Couldn't save"), detail);
        });
    });
}

void MenuMapController::editCode()
{
    if (selected_ < 0 || !entries_[size_t(selected_)].node)
        return;
    const auto &n = *entries_[size_t(selected_)].node;
    if (n.origin != Origin::Shell || !writable(n.file) || n.line <= 0)
        return;
    std::string error;
    auto doc = loadDecls(n.file, &error);
    auto block = doc ? menumap::findBlock(*doc, size_t(n.line), size_t(n.column)) : std::nullopt;
    if (!block) {
        toast("error", nsp::tr("Couldn't open the code"), doc ? nsp::trf("{} has no declaration on line {}.", fileLabel(n.file), n.line)
                                                               : error);
        return;
    }
    editFile_ = n.file;
    editLine_ = size_t(n.line);
    editColumn_ = size_t(n.column);
    editOriginal_ = block->text;
    editDoc_.reset(code::normalizeNewlines(block->text));
    const auto &st = ui_->global<MenuMapState>();
    st.set_edit_title(ss(n.title.empty() ? std::string(nsp::tr("(no title)")) : n.title));
    st.set_edit_file_label(ss(nsp::trf("{}, line {}", fileLabel(n.file), n.line)));
    st.set_edit_text(ss(editDoc_.display()));
    editRows_.update(hl::highlight(editDoc_.text(), editDoc_.tabWidth(), &editDoc_.glyphs()));
    st.set_edit_lines(editRows_.model());
    st.set_edit_error("");
    st.set_edit_dirty(false);
    st.set_editing(true);
}

// An edit or a command in the code box (see CodeDocument.h).
void MenuMapController::editChanged(const code::Result &r)
{
    const auto &st = ui_->global<MenuMapState>();
    if (r.display)
        st.set_edit_text(ss(editDoc_.display()));
    if (!r.edited)
        return;
    editRows_.update(hl::highlight(editDoc_.text(), editDoc_.tabWidth(), &editDoc_.glyphs()));
    st.set_edit_dirty(editDoc_.text() != code::normalizeNewlines(editOriginal_));
    st.set_edit_error("");
}

void MenuMapController::editCancel()
{
    ui_->global<MenuMapState>().set_editing(false);
    editFile_.clear();
}

void MenuMapController::editSave()
{
    const auto &st = ui_->global<MenuMapState>();
    if (editFile_.empty() || st.get_busy())
        return;
    // Back to the file's own line ends.
    const bool crlf = editOriginal_.find("\r\n") != std::string::npos;
    const std::string editText = editor::fromEditorText(editDoc_.text(), crlf ? nss::Eol::CRLF : nss::Eol::LF);
    if (editText == editOriginal_) {
        editCancel();
        return;
    }
    std::string error;
    auto doc = loadDecls(editFile_, &error);
    auto block = doc ? menumap::findBlock(*doc, editLine_, editColumn_) : std::nullopt;
    if (!block || block->text != editOriginal_) {
        st.set_edit_error(ss(nsp::tr("The file changed since this code was opened. Cancel, then open it again.")));
        return;
    }
    if (!menumap::replaceBlock(*doc, *block, editText, &error)) {
        st.set_edit_error(ss(nsp::trf("Not saved: {}", error)));
        return;
    }
    st.set_busy(true);
    auto title = str(st.get_edit_title());
    worker_.post([this, write = nss::FileWrite{ editFile_, doc->text(), doc->format() }, title, root = root_, exe = shellExe_] {
        nss::ConfigStore store(root);
        nss::SaveOptions options;
        options.reason = "Menu map: edit the code of " + title;
        options.validator = nss::makeShellCheckValidator(exe);
        auto result = store.save({ write }, options);
        slint::invoke_from_event_loop([this, result = std::move(result), root] {
            const auto &st = ui_->global<MenuMapState>();
            st.set_busy(false);
            if (result.ok) {
                editCancel();
                toast("ok", nsp::tr("Saved"), result.snapshotId.empty() ? std::string(nsp::tr("No changes were needed."))
                                                                         : nsp::trf("Snapshot {} was taken first.", result.snapshotId));
                save_.reloadConfig();
                return;
            }
            std::string detail = result.error;
            if (!result.check.ok && !result.check.message.empty()) {
                auto outcome = editor::parseCheck(result.check.message, {}, root.parent_path());
                for (const auto &i : outcome.issues)
                    if (i.error) {
                        detail = i.message;
                        if (i.line > 0)
                            detail = nsp::trf("line {}: {}", i.line, i.message);
                        break;
                    }
                st.set_edit_error(ss(nsp::trf("Not saved: the config would fail to load. {}", detail)));
                return;
            }
            st.set_edit_error(ss(nsp::trf("Couldn't save: {}", detail)));
        });
    });
}

void MenuMapController::openInEditor(const std::string &path, int line)
{
    if (path.empty())
        return;
    ui_->global<AppState>().set_page(kEditorPage);
    jumpTimer_.start(slint::TimerMode::SingleShot, 150ms, [this, path, line] {
        ui_->global<EditorState>().invoke_open_at(ss(path), line);
    });
}
