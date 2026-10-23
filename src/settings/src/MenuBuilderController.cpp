#include "MenuBuilderController.h"

#include "EditorSession.h"
#include "Highlight.h"
#include "MenuWriter.h"
#include "ShellCli.h"
#include "Tr.h"

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

constexpr unsigned kGlyphFirst = 0xE000;
constexpr unsigned kGlyphLast = 0xE29A; // the Nilesoft.Shell font (res/nilesoft-shell.ttf)
constexpr int kContextMenusPage = 4;    // Pages.menus in ui/types.slint

std::string utf8(unsigned cp)
{
    std::string s;
    s += char(0xE0 | (cp >> 12));
    s += char(0x80 | ((cp >> 6) & 0x3F));
    s += char(0x80 | (cp & 0x3F));
    return s;
}

// "" -> the glyph; anything else -> "".
std::string glyphOf(std::string_view image)
{
    auto t = nss::trim(image);
    if (t.size() != 6 || t[0] != '\\' || (t[1] != 'u' && t[1] != 'U'))
        return {};
    unsigned cp = 0;
    for (size_t i = 2; i < 6; ++i) {
        char c = t[i];
        cp <<= 4;
        if (c >= '0' && c <= '9')
            cp |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f')
            cp |= unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            cp |= unsigned(c - 'A' + 10);
        else
            return {};
    }
    return cp >= kGlyphFirst && cp <= kGlyphLast ? utf8(cp) : std::string{};
}

std::string hex4(unsigned v)
{
    static const char *digits = "0123456789ABCDEF";
    std::string s;
    for (int shift = 12; shift >= 0; shift -= 4)
        s += digits[(v >> shift) & 0xF];
    return s;
}

const char *kindName(menus::Kind k)
{
    return k == menus::Kind::Menu ? "menu" : k == menus::Kind::Item ? "item" : "separator";
}

// "&Open" -> "Open"; "&&" -> "&" (menu accelerators).
std::string displayTitle(const menus::Node &n)
{
    if (n.title.empty())
        return n.extra.empty() ? nsp::tr("(no title)") : nsp::tr("(title from an expression)");
    std::string out;
    for (size_t i = 0; i < n.title.size(); ++i) {
        if (n.title[i] == '&') {
            if (i + 1 < n.title.size() && n.title[i + 1] == '&')
                out += '&', ++i;
            continue;
        }
        out += n.title[i];
    }
    return out;
}

constexpr const char *kModes[] = { "", "single", "multiple", "multi_unique", "multi_single", "none" };
constexpr const char *kWindows[] = { "", "hidden", "minimized", "maximized" };
constexpr const char *kSeps[] = { "", "before", "after", "both" };

template <size_t N>
int indexOf(const char *const (&list)[N], const std::string &v)
{
    for (size_t i = 0; i < N; ++i)
        if (v == list[i])
            return int(i);
    return 0;
}

size_t countKind(const menus::Tree &t, menus::Kind k)
{
    size_t n = 0;
    for (const auto &x : t)
        n += (x.kind == k) + countKind(x.children, k);
    return n;
}

} // namespace

MenuBuilderController::MenuBuilderController(slint::ComponentHandle<AppWindow> ui)
    : ui_(std::move(ui)), shellExe_(ShellCli::locate())
{
    std::vector<slint::SharedString> glyphs;
    for (unsigned cp = kGlyphFirst; cp <= kGlyphLast; ++cp)
        glyphs.push_back(ss(utf8(cp)));
    ui_->global<BuilderState>().set_glyphs(std::make_shared<slint::VectorModel<slint::SharedString>>(glyphs));
    bindCallbacks();
    statusPoll_.start(slint::TimerMode::Repeated, 500ms, [this] { pollStatus(); });
    edited(true);
    nsp::onLanguageChanged([this] {
        pushRows();
        pushPreview();
        if (loadText_)
            loadText_();
    });
}

MenuBuilderController::~MenuBuilderController() = default;

void MenuBuilderController::bindCallbacks()
{
    const auto &st = ui_->global<BuilderState>();
    st.on_activated([this] {
        pollStatus();
        if (!root_.empty() && tree_ == saved_ && !saving_)
            load(); // pick up changes made elsewhere
    });
    st.on_select([this](int index) {
        auto rows = menus::flatten(tree_);
        if (index >= 0 && index < int(rows.size()))
            select(rows[size_t(index)].path);
    });
    st.on_add([this](slint::SharedString kind) {
        if (readOnly())
            return;
        auto k = str(kind);
        auto node = menus::makeNode(k == "menu" ? menus::Kind::Menu : k == "separator" ? menus::Kind::Separator
                                                                                        : menus::Kind::Item);
        if (auto p = menus::add(tree_, selected_, std::move(node))) {
            selected_ = *p;
            edited(true);
        }
    });
    st.on_remove([this] {
        // The last entry at the root: the file's own lines above it stay, at the end.
        if (!readOnly() && selected_.size() == 1 && selected_[0] + 1 == tree_.size())
            tail_ = menus::joinKept(tree_.back().before, tail_);
        structural(&menus::remove);
    });
    st.on_duplicate([this] { structural(&menus::duplicate); });
    st.on_move_up([this] { structural(&menus::moveUp); });
    st.on_move_down([this] { structural(&menus::moveDown); });
    st.on_indent([this] { structural(&menus::indent); });
    st.on_outdent([this] { structural(&menus::outdent); });
    st.on_set_text([this](slint::SharedString f, slint::SharedString v) { setText(str(f), str(v)); });
    st.on_set_flag([this](slint::SharedString f, bool v) { setFlag(str(f), v); });
    st.on_set_choice([this](slint::SharedString f, int v) { setChoice(str(f), v); });
    st.on_pick_glyph([this](int index) {
        auto *n = menus::nodeAt(tree_, selected_);
        if (!n || readOnly())
            return;
        n->image = index < 0 ? std::string{} : "\\u" + hex4(kGlyphFirst + unsigned(index));
        ++serial_;
        edited(false);
    });
    st.on_glyph_code([](int index) {
        return ss(index < 0 ? std::string{} : "\\u" + hex4(kGlyphFirst + unsigned(index)));
    });
    st.on_save([this] { save(); });
    st.on_discard([this] {
        tree_ = saved_;
        tail_ = savedTail_;
        if (!menus::nodeAt(tree_, selected_))
            selected_.clear();
        edited(true);
    });
    st.on_create_user_config([this] { ui_->global<AppState>().invoke_action("config-init"); });
    st.on_open_context_menus([this] { ui_->global<AppState>().set_page(kContextMenusPage); });
}

void MenuBuilderController::toast(const std::string &kind, const std::string &title, const std::string &text)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(text));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

fs::path MenuBuilderController::menusFile() const
{
    return root_.parent_path() / fs::path(nss::widen(menus::MenuWriter::relativePath)).make_preferred();
}

void MenuBuilderController::pollStatus()
{
    auto status = ui_->global<AppState>().get_status();
    if (!status.loaded)
        return;
    auto path = str(status.config_path);
    if (path == statusPath_)
        return;
    statusPath_ = path;
    const auto &st = ui_->global<BuilderState>();
    if (!status.config_exists || path.empty()) {
        root_.clear();
        loadText_ = [this, path] {
            ui_->global<BuilderState>().set_load_error(ss(nsp::trf("The config file doesn't exist: {}", path)));
        };
        loadText_();
        return;
    }
    root_ = fs::path(nss::widen(path)).lexically_normal();
    shared_ = str(status.config_source) == "install" || editor::isProtectedPath(root_);
    st.set_shared_config(shared_);
    st.set_load_error("");
    loadText_ = nullptr;
    load();
}

void MenuBuilderController::load()
{
    auto gen = ++gen_;
    auto root = root_;
    auto file = menusFile();
    worker_.post([this, gen, root, file] {
        struct Loaded
        {
            menus::Tree tree;
            std::string tail;
            nss::TextFormat format = nss::defaultFormat();
            std::optional<nss::NssDocument> broken; // edited by hand so that it doesn't parse
            bool readFailed = false;
            std::string error;        // why the file couldn't be read
            bool importOff = false;
        };
        auto out = std::make_shared<Loaded>();
        std::error_code ec;
        if (fs::exists(file, ec)) {
            std::string error;
            if (auto doc = nss::NssDocument::load(file, {}, &error)) {
                auto parsed = menus::parse(*doc);
                out->tree = std::move(parsed.tree);
                out->tail = std::move(parsed.tail);
                out->format = doc->format();
                if (!doc->diagnostics().empty())
                    out->broken = std::move(*doc);
            } else {
                out->readFailed = true;
                out->error = error;
            }
        }
        if (auto rootDoc = nss::NssDocument::load(root)) {
            int idx = menus::MenuWriter::findImport(*rootDoc);
            out->importOff = idx >= 0 && !rootDoc->imports()[size_t(idx)]->enabled;
        }
        slint::invoke_from_event_loop([this, gen, out, file, root] {
            if (gen != gen_)
                return;
            const auto &st = ui_->global<BuilderState>();
            tree_ = out->tree;
            saved_ = out->tree;
            tail_ = savedTail_ = out->tail;
            format_ = out->format;
            if (!menus::nodeAt(tree_, selected_))
                selected_ = tree_.empty() ? menus::Path{} : menus::Path{ 0 };
            st.set_loaded(true);
            loadText_ = [this, out, file] {
                const auto &bs = ui_->global<BuilderState>();
                std::string notice;
                if (out->broken)
                    notice = menus::handEditProblem(*out->broken, nss::narrow(file.filename().wstring()));
                bs.set_notice(ss(notice));
                bs.set_load_error(ss(!out->readFailed
                                         ? std::string{}
                                         : nsp::trf("Couldn't read {}: {}", nss::narrow(file.wstring()), out->error)));
            };
            loadText_();
            st.set_import_off(out->importOff);
            st.set_file_label(ss(nss::narrow(file.lexically_relative(root.parent_path()).generic_wstring())));
            edited(true);
        });
    });
}

void MenuBuilderController::select(const menus::Path &path, bool reloadForm)
{
    selected_ = path;
    if (reloadForm)
        ++serial_;
    pushRows();
    pushNode();
    pushPreview();
}

void MenuBuilderController::structural(std::optional<menus::Path> (*op)(menus::Tree &, const menus::Path &))
{
    if (readOnly() || selected_.empty())
        return;
    if (auto p = op(tree_, selected_)) {
        selected_ = *p;
        edited(true);
    }
}

void MenuBuilderController::edited(bool structure)
{
    if (structure)
        ++serial_;
    pushRows();
    pushNode();
    pushPreview();
    pushCode();
    const auto &st = ui_->global<BuilderState>();
    st.set_dirty(tree_ != saved_ || tail_ != savedTail_);
}

void MenuBuilderController::pushRows()
{
    const auto &st = ui_->global<BuilderState>();
    auto rows = menus::flatten(tree_);
    std::vector<BuilderRow> out;
    int selectedRow = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &n = *rows[i].node;
        if (rows[i].path == selected_)
            selectedRow = int(i);
        out.push_back(BuilderRow{ rows[i].depth, kindName(n.kind),
                                  ss(n.kind == menus::Kind::Separator ? std::string(nsp::tr("Separator")) : displayTitle(n)),
                                  ss(glyphOf(n.image)), ss(menus::summary(n)) });
    }
    st.set_rows(std::make_shared<slint::VectorModel<BuilderRow>>(out));
    st.set_selected(selectedRow);

    bool has = selectedRow >= 0 && !readOnly();
    const menus::Tree *level = &tree_;
    if (selected_.size() > 1)
        if (auto parent = menus::nodeAt(tree_, menus::Path(selected_.begin(), selected_.end() - 1)))
            level = &parent->children;
    size_t i = selected_.empty() ? 0 : selected_.back();
    st.set_can_up(has && i > 0);
    st.set_can_down(has && i + 1 < level->size());
    st.set_can_indent(has && i > 0 && i - 1 < level->size() && (*level)[i - 1].kind == menus::Kind::Menu);
    st.set_can_outdent(has && selected_.size() > 1);
}

void MenuBuilderController::pushNode()
{
    const auto &st = ui_->global<BuilderState>();
    BuilderNode bn{};
    if (const auto *n = menus::nodeAt(tree_, selected_)) {
        bn.kind = kindName(n->kind);
        bn.title = ss(n->title);
        bn.image = ss(n->image);
        bn.glyph = ss(glyphOf(n->image));
        auto loc = menus::locationOf(*n);
        bn.location = int(loc);
        bn.extensions = ss(loc == menus::Location::Extensions ? menus::findToExtensions(n->find) : std::string{});
        bn.type = ss(n->type);
        bn.find = ss(n->find);
        bn.where = ss(n->where);
        bn.mode = indexOf(kModes, n->mode);
        bn.cmd = ss(n->cmd);
        bn.args = ss(n->args);
        bn.admin = n->admin;
        bn.window = indexOf(kWindows, n->window);
        bn.keys = ss(n->keys);
        bn.tip = ss(n->tip);
        bn.sep = indexOf(kSeps, n->sep);
        std::string extra;
        for (const auto &p : n->extra)
            extra += (extra.empty() ? "" : "\n") + p.name + (p.value.empty() ? "" : "=" + p.value);
        bn.extra = ss(extra);
    }
    st.set_node(bn);
    st.set_node_serial(serial_);
}

void MenuBuilderController::pushPreview()
{
    const auto &st = ui_->global<BuilderState>();
    std::vector<PreviewRow> root, flyout;
    int openAt = -1;
    size_t top = selected_.empty() ? size_t(-1) : selected_[0];
    for (size_t i = 0; i < tree_.size(); ++i) {
        const auto &n = tree_[i];
        bool open = i == top && n.kind == menus::Kind::Menu;
        if (open)
            openAt = int(i);
        root.push_back(PreviewRow{ kindName(n.kind), ss(displayTitle(n)), ss(glyphOf(n.image)), ss(n.keys),
                                   selected_.size() == 1 && i == top, open });
    }
    if (openAt >= 0)
        for (size_t j = 0; j < tree_[size_t(openAt)].children.size(); ++j) {
            const auto &c = tree_[size_t(openAt)].children[j];
            flyout.push_back(PreviewRow{ kindName(c.kind), ss(displayTitle(c)), ss(glyphOf(c.image)), ss(c.keys),
                                         selected_.size() >= 2 && selected_[1] == j, false });
        }
    st.set_preview(std::make_shared<slint::VectorModel<PreviewRow>>(root));
    st.set_flyout(std::make_shared<slint::VectorModel<PreviewRow>>(flyout));
    st.set_flyout_at(openAt);
    // Where the open menu's row sits in the preview (PreviewItem heights, 4 px padding).
    int offset = 4;
    for (int i = 0; i < openAt; ++i)
        offset += tree_[size_t(i)].kind == menus::Kind::Separator ? 9 : 32;
    st.set_flyout_offset(offset);
}

void MenuBuilderController::pushCode()
{
    const auto &st = ui_->global<BuilderState>();
    auto text = menus::MenuWriter::generate(tree_, nss::Eol::LF, tail_);
    auto rows = std::make_shared<slint::VectorModel<CodeLine>>();
    size_t maxColumns = 0;
    int number = 0;
    for (const auto &line : hl::highlight(text)) {
        ++number;
        auto spans = std::make_shared<slint::VectorModel<CodeSpan>>();
        size_t columns = 0;
        for (const auto &s : line.spans) {
            spans->push_back(CodeSpan{ ss(s.text), s.kind });
            columns += s.text.size();
        }
        maxColumns = std::max(maxColumns, columns);
        rows->push_back(CodeLine{ number, spans, 0 });
    }
    st.set_code(rows);
    st.set_code_columns(int(std::min<size_t>(maxColumns, 2000)));
}

void MenuBuilderController::setText(const std::string &field, const std::string &value)
{
    auto *n = menus::nodeAt(tree_, selected_);
    if (!n || readOnly())
        return;
    if (field == "title")
        n->title = value;
    else if (field == "image")
        n->image = std::string(nss::trim(value));
    else if (field == "extensions")
        menus::setLocation(*n, menus::Location::Extensions, value);
    else if (field == "type")
        n->type = std::string(nss::trim(value));
    else if (field == "find")
        n->find = std::string(nss::trim(value));
    else if (field == "where")
        n->where = std::string(nss::trim(value));
    else if (field == "cmd")
        n->cmd = value;
    else if (field == "args")
        n->args = value;
    else if (field == "keys")
        n->keys = value;
    else if (field == "tip")
        n->tip = value;
    else
        return;
    edited(false);
}

void MenuBuilderController::setFlag(const std::string &field, bool value)
{
    auto *n = menus::nodeAt(tree_, selected_);
    if (!n || readOnly())
        return;
    if (field == "admin")
        n->admin = value;
    edited(false);
}

void MenuBuilderController::setChoice(const std::string &field, int value)
{
    auto *n = menus::nodeAt(tree_, selected_);
    if (!n || readOnly() || value < 0)
        return;
    if (field == "location") {
        auto loc = menus::Location(std::min(value, int(menus::Location::Custom)));
        auto ext = menus::findToExtensions(n->find);
        menus::setLocation(*n, loc, loc == menus::Location::Extensions && ext.empty() ? ".txt" : ext);
        ++serial_; // the form shows other fields now
    } else if (field == "mode" && value < int(std::size(kModes)))
        n->mode = kModes[value];
    else if (field == "window" && value < int(std::size(kWindows)))
        n->window = kWindows[value];
    else if (field == "sep" && value < int(std::size(kSeps)))
        n->sep = kSeps[value];
    else
        return;
    edited(false);
}

void MenuBuilderController::save()
{
    if (saving_ || readOnly())
        return;
    if (shellExe_.empty()) {
        toast("error", nsp::tr("Can't check the config"), nsp::tr("shell.exe was not found, so nothing was saved."));
        return;
    }
    auto file = menusFile();
    if (editor::isProtectedPath(file) || editor::isProtectedPath(root_)) {
        toast("error", nsp::tr("Not saved"), nsp::tr("Program Files is never written to."));
        return;
    }
    auto tree = tree_;
    auto tail = tail_;
    auto menusCount = countKind(tree, menus::Kind::Menu), itemCount = countKind(tree, menus::Kind::Item);
    std::string reason = "Menu Builder: " + std::to_string(menusCount) + (menusCount == 1 ? " menu, " : " menus, ") +
                         std::to_string(itemCount) + (itemCount == 1 ? " item" : " items");
    saving_ = true;
    ui_->global<BuilderState>().set_saving(true);
    worker_.post([this, tree, tail, reason, file, root = root_, exe = shellExe_, format = format_] {
        nss::SaveResult result;
        std::string error;
        // A hand edit that doesn't parse can't be read into the builder: the file stays as it is.
        std::error_code ec;
        if (fs::exists(file, ec))
            if (auto now = nss::NssDocument::load(file, {}, &error))
                result.error = menus::handEditProblem(*now, nss::narrow(file.filename().wstring()));
        auto rootDoc = nss::NssDocument::load(root, {}, &error);
        if (!result.error.empty()) {
            // reported below
        } else if (!rootDoc) {
            result.error = nsp::trf("Couldn't read {}: {}", nss::narrow(root.wstring()), error);
        } else {
            std::vector<nss::FileWrite> writes;
            writes.push_back(nss::FileWrite{ file, menus::MenuWriter::generate(tree, format.eol, tail), format });
            if (menus::MenuWriter::ensureImported(*rootDoc))
                writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
            nss::ConfigStore store(root);
            nss::SaveOptions options;
            options.reason = reason;
            options.validator = nss::makeShellCheckValidator(exe);
            result = store.save(std::move(writes), options);
        }
        slint::invoke_from_event_loop([this, tree, tail, result = std::move(result), root] {
            saving_ = false;
            ui_->global<BuilderState>().set_saving(false);
            if (result.ok) {
                saved_ = tree;
                savedTail_ = tail;
                edited(false);
                toast("ok", nsp::tr("Saved your menus"),
                      result.snapshotId.empty()
                          ? std::string(nsp::tr("No changes were needed."))
                          : nsp::trf("Snapshot {} was taken first. Right-click to see them.", result.snapshotId));
                return;
            }
            std::string detail = result.error;
            if (!result.check.ok && !result.check.message.empty()) {
                auto outcome = editor::parseCheck(result.check.message, {}, root.parent_path());
                for (const auto &i : outcome.issues)
                    if (i.error) {
                        detail = i.line > 0 ? nsp::trf("{} (line {})", i.message, i.line) : i.message;
                        break;
                    }
                toast("error", nsp::tr("Not saved: Shell would reject these menus"), detail);
                return;
            }
            toast("error", nsp::tr("Couldn't save"), detail);
        });
    });
}
