#include "OpenWithController.h"

#include "ConfigRules.h"
#include "EditorSession.h"
#include "FileIcon.h"
#include "Highlight.h"
#include "MenuWriter.h"
#include "ShellCli.h"
#include "Tr.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <windows.h>
#include <objbase.h>
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

constexpr int kContextMenusPage = 4; // Pages.menus in ui/types.slint
constexpr const char *kKinds[] = { "file", "dir", "back" }; // the preview's "A file / A folder / Background"

bool exists(const std::string &utf8) { return openwith::fileExists(nss::widen(utf8)); }

std::string unquotePath(std::string_view s)
{
    auto t = nss::trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        t = nss::trim(t.substr(1, t.size() - 2));
    return std::string(t);
}

// "C:\x\app.dll,3" -> ("C:\x\app.dll", 3)
std::pair<std::wstring, int> splitIconIndex(const std::wstring &path)
{
    auto comma = path.rfind(L',');
    if (comma == std::wstring::npos || comma + 1 >= path.size())
        return { path, 0 };
    auto tail = path.substr(comma + 1);
    if (!std::all_of(tail.begin(), tail.end(), [](wchar_t c) { return iswdigit(c) || c == L'-'; }))
        return { path, 0 };
    return { path.substr(0, comma), _wtoi(tail.c_str()) };
}

// The program's (or icon file's) icon at 32 px, as straight RGBA.
std::optional<std::vector<uint8_t>> extractIcon(const std::string &utf8, uint32_t &w, uint32_t &h)
{
    auto [path, index] = splitIconIndex(nss::widen(utf8));
    return fileicon::extract(path, index, 32, w, h);
}

// Whether an app's item can show anywhere the menu shows.
bool reachable(const openwith::Options &o, const openwith::App &a)
{
    for (const char *k : { "file", "dir", "back", "desktop" })
        if (openwith::showsFor(o, a, k))
            return true;
    // Files with extensions: the menu has to show on files.
    return a.scope == openwith::Scope::Extensions && o.files;
}

std::string lowerKey(const std::string &s) { return nss::toLower(s); }

// "A, B and C", for the page.
std::string joinNames(const std::vector<std::string> &names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i == 0)
            out = names[i];
        else if (i + 1 == names.size())
            out = nsp::trf("{} and {}", out, names[i]);
        else
            out = nsp::trf("{}, {}", out, names[i]);
    }
    return out;
}

} // namespace

OpenWithController::OpenWithController(slint::ComponentHandle<AppWindow> ui)
    : ui_(std::move(ui)), shellExe_(ShellCli::locate()), env_(openwith::Env::current())
{
    ui_->global<OpenWithState>().set_preset_count(int(openwith::presets().size()));
    // Development switch (see main.cpp): --openwith-select <n> opens row n with its
    // Test result, for snapshots.
    int argc = 0;
    if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i)
            if (_wcsicmp(argv[i], L"--openwith-select") == 0)
                devSelect_ = _wtoi(argv[++i]);
        LocalFree(argv);
    }
    bindCallbacks();
    statusPoll_.start(slint::TimerMode::Repeated, 500ms, [this] { pollStatus(); });
    nsp::onLanguageChanged([this] {
        if (loadText_)
            loadText_();
        if (!loaded_)
            return;
        pushRows();
        pushOthers();
        pushOptions();
        pushPreview();
        pushTest();
    });
}

OpenWithController::~OpenWithController() = default;

void OpenWithController::bindCallbacks()
{
    const auto &st = ui_->global<OpenWithState>();
    st.on_activated([this] {
        pollStatus();
        if (!root_.empty() && !dirty() && !saving_)
            load(); // pick up changes made elsewhere, and newly installed apps
    });
    st.on_select([this](int index) { select(index); });
    st.on_toggle([this](int index, bool on) {
        if (readOnly() || index < 0 || index >= int(model_.apps.size()))
            return;
        openwith::setEnabled(model_.apps[size_t(index)], on);
        edited(false);
    });
    st.on_locate([this](int index) {
        if (readOnly() || index < 0 || index >= int(model_.apps.size()))
            return;
        select(index);
        ui_->global<OpenWithState>().invoke_browse_program();
    });
    st.on_move_up([this] { move(-1); });
    st.on_move_down([this] { move(1); });
    st.on_add_custom([this] {
        if (readOnly())
            return;
        openwith::App a;
        a.id = "custom";
        a.name = nsp::trc("Default name of a new app in Open with", "New app");
        a.args = "{path}";
        a.enabled = true;
        model_.apps.push_back(a);
        select(int(model_.apps.size()) - 1);
        edited(true);
    });
    st.on_remove([this] {
        if (readOnly() || selected_ < 0 || selected_ >= int(model_.apps.size()) ||
            model_.apps[size_t(selected_)].id != "custom")
            return;
        model_.apps.erase(model_.apps.begin() + selected_);
        selected_ = -1;
        testShown_ = false;
        edited(true);
    });
    st.on_browse_program([this] {
        if (readOnly() || selected_ < 0 || selected_ >= int(model_.apps.size()))
            return;
        auto &a = model_.apps[size_t(selected_)];
        auto picked = pickFile(nss::widen(nsp::tr("Choose the program")).c_str(), nss::widen(nsp::tr("Programs")).c_str(),
                               L"*.exe;*.cmd;*.bat;*.com", a.exe);
        if (picked.empty())
            return;
        if (a.id == "nvim") {
            // Neovim runs in a Windows Terminal tab.
            std::string wt;
            if (const auto *p = openwith::findPreset("wt"))
                wt = openwith::detect(*p, env_);
            if (!wt.empty() && nss::toLower(fs::path(nss::widen(picked)).filename().string()) == "nvim.exe") {
                a.icon = picked;
                a.args = "new-tab -d {dir} \"" + picked + "\" {path}";
                picked = wt;
            }
        }
        a.exe = picked;
        openwith::setEnabled(a, true);
        edited(true);
    });
    st.on_browse_icon([this] {
        if (readOnly() || selected_ < 0 || selected_ >= int(model_.apps.size()))
            return;
        auto &a = model_.apps[size_t(selected_)];
        auto picked = pickFile(nss::widen(nsp::tr("Choose an icon")).c_str(), nss::widen(nsp::tr("Icons and programs")).c_str(),
                               L"*.ico;*.exe;*.dll", a.icon.empty() ? a.exe : a.icon);
        if (picked.empty())
            return;
        a.icon = picked;
        edited(true);
    });
    st.on_set_text([this](slint::SharedString f, slint::SharedString v) { setText(str(f), str(v)); });
    st.on_set_choice([this](slint::SharedString f, int v) { setChoice(str(f), v); });
    st.on_set_flag([this](slint::SharedString f, bool v) { setFlag(str(f), v); });
    st.on_set_preview_kind([this](int kind) {
        previewKind_ = std::clamp(kind, 0, 2);
        ui_->global<OpenWithState>().set_preview_kind(previewKind_);
        pushPreview();
    });
    st.on_test([this] {
        testShown_ = true;
        pushTest();
    });
    st.on_save([this] { save(); });
    st.on_discard([this] {
        model_ = saved_;
        developHidden_ = developSavedHidden_;
        if (selected_ >= int(model_.apps.size()))
            selected_ = -1;
        ++optionsSerial_;
        edited(true);
    });
    st.on_create_user_config([this] { ui_->global<AppState>().invoke_action("config-init"); });
    st.on_open_context_menus([this] { ui_->global<AppState>().set_page(kContextMenusPage); });
    st.on_other_toggle([this](int index, bool shown) { toggleOther(index, shown); });
    st.on_hide_leftovers([this] { hideLeftovers(); });
    st.on_other_title([this](int index, slint::SharedString t) { setOtherTitle(index, str(t)); });
}

std::vector<openwith::OtherRef> OpenWithController::otherRows() const
{
    return openwith::otherRows(others_, owners_, model_.hidden);
}

void OpenWithController::toggleOther(int index, bool shown)
{
    auto rows = otherRows();
    if (readOnly() || index < 0 || index >= int(rows.size()))
        return;
    const auto r = rows[size_t(index)];
    if (shown && r.hidden >= 0) {
        model_.hidden.erase(model_.hidden.begin() + r.hidden);
    } else if (!shown && r.hidden < 0 && r.item >= 0) {
        const auto &item = others_[size_t(r.item)];
        // A shell extension's title is the app's own: its name is the best guess, and the field is there to correct it.
        model_.hidden.push_back(openwith::Hidden{ item.kind, item.name, item.title });
    } else {
        return;
    }
    edited(false);
}

void OpenWithController::hideLeftovers()
{
    if (readOnly())
        return;
    bool any = false;
    for (const auto &r : otherRows()) {
        if (r.item < 0 || r.hidden >= 0)
            continue;
        const auto &item = others_[size_t(r.item)];
        if (item.missing.empty())
            continue;
        model_.hidden.push_back(openwith::Hidden{ item.kind, item.name, item.title });
        any = true;
    }
    if (any)
        edited(false);
}

void OpenWithController::setOtherTitle(int index, const std::string &title)
{
    auto rows = otherRows();
    if (readOnly() || index < 0 || index >= int(rows.size()) || rows[size_t(index)].hidden < 0)
        return;
    auto &h = model_.hidden[size_t(rows[size_t(index)].hidden)];
    if (h.kind != "handler")
        return; // a verb's title comes from the registry
    h.title = std::string(nss::trim(title));
    edited(false);
}

void OpenWithController::relink()
{
    if (othersLoaded_)
        owners_ = openwith::linkNatives(model_, others_, env_);
    else
        owners_.clear();
}

std::string OpenWithController::nativesText(size_t app) const
{
    std::string out;
    if (app >= model_.apps.size())
        return out;
    for (const auto &h : model_.apps[app].natives) {
        bool submenu = false;
        for (size_t i = 0; i < others_.size() && i < owners_.size(); ++i)
            if (owners_[i] == int(app) && others_[i].kind == h.kind && others_[i].name == h.name)
                submenu = others_[i].submenu;
        std::string title = h.title.empty() ? h.name : h.title;
        out += (out.empty() ? "" : ", ") + title + (submenu ? " \u203a" : "");
    }
    return out;
}

void OpenWithController::toast(const std::string &kind, const std::string &title, const std::string &text)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(text));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

fs::path OpenWithController::openWithFile() const
{
    return root_.parent_path() / fs::path(nss::widen(openwith::kRelativePath)).make_preferred();
}

bool OpenWithController::dirty() const
{
    return !(model_ == saved_) || developHidden_ != developSavedHidden_ || fileLegacy_;
}

void OpenWithController::pollStatus()
{
    auto status = ui_->global<AppState>().get_status();
    if (!status.loaded)
        return;
    auto path = str(status.config_path);
    if (path == statusPath_)
        return;
    statusPath_ = path;
    const auto &st = ui_->global<OpenWithState>();
    if (!status.config_exists || path.empty()) {
        root_.clear();
        loadText_ = [this, path] {
            ui_->global<OpenWithState>().set_load_error(ss(nsp::trf("The config file doesn't exist: {}", path)));
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

void OpenWithController::load()
{
    auto gen = ++gen_;
    auto root = root_;
    auto file = openWithFile();
    auto shared = shared_;
    worker_.post([this, gen, root, file, shared] {
        struct Loaded
        {
            openwith::Env env;
            openwith::Model model;
            openwith::Model saved;
            bool fileExists = false;
            bool legacy = false;     // an older page's layout
            bool customised = false; // lines the page didn't write
            nss::TextFormat format = nss::defaultFormat();
            std::optional<nss::NssDocument> broken; // edited by hand so that it doesn't parse
            bool readFailed = false;
            std::string error;   // why the file couldn't be read
            bool importOff = false;
            fs::path developPath;
            bool developFound = false;
            bool developHidden = false;
            bool developWritable = false;
            std::vector<otherapps::Item> others;
            bool ownChanged = false; // see Migrate::Own
        };
        auto out = std::make_shared<Loaded>();
        out->env = openwith::Env::current();
        out->others = otherapps::collect(otherapps::scan(), otherapps::resolveIndirect, otherapps::systemRoot(),
                                        otherapps::realFiles());
        auto detected = openwith::detectAll(out->env);
        std::optional<openwith::Model> saved;
        std::error_code ec;
        if (fs::exists(file, ec)) {
            std::string error;
            if (auto doc = nss::NssDocument::load(file, {}, &error)) {
                auto parsed = openwith::parse(doc->text(), out->env);
                saved = parsed.model;
                out->fileExists = true;
                out->format = doc->format();
                out->legacy = parsed.legacy;
                out->customised = !parsed.warnings.empty();
                if (!doc->diagnostics().empty()) {
                    out->customised = true; // no rewrite in today's layout either
                    out->broken = std::move(*doc);
                }
            } else {
                out->readFailed = true;
                out->error = error;
            }
        }
        out->model = openwith::merge(saved, detected);
        auto inFile = out->model; // the apps' own entries as the file has them
        openwith::linkNatives(out->model, out->others, out->env);
        out->saved = out->model;
        if (!saved)
            for (auto &a : out->saved.apps)
                a.enabled = false; // nothing is in the menu until the first save
        if (saved) {
            // An app switched off whose own entries aren't the file's (installed
            // since, or a file from before they were linked): the file is
            // rewritten, so they're hidden. For an app that's on, nothing in
            // the menu changes: it waits for the next save.
            for (size_t i = 0; i < inFile.apps.size(); ++i) {
                const auto &was = inFile.apps[i];
                if (was.exe.empty() || was.natives == out->model.apps[i].natives)
                    continue;
                if (openwith::hidesNatives(was)) {
                    out->saved.apps[i].natives = was.natives;
                    out->ownChanged = true;
                }
            }
        }

        if (auto rootDoc = nss::NssDocument::load(root)) {
            int idx = menus::MenuWriter::findImport(*rootDoc, openwith::kRelativePath);
            out->importOff = idx >= 0 && !rootDoc->imports()[size_t(idx)]->enabled;
            for (const auto &m : rules::listModules(*rootDoc)) {
                if (m.name != "develop" || !m.enabled)
                    continue;
                auto p = (root.parent_path() / fs::path(nss::widen(m.path))).lexically_normal();
                if (auto dev = nss::NssDocument::load(p)) {
                    auto e = openwith::findEditorsMenu(*dev);
                    if (e.found) {
                        out->developPath = p;
                        out->developFound = true;
                        out->developHidden = !e.enabled;
                        auto key = nss::pathKey(p), dir = nss::pathKey(root.parent_path());
                        out->developWritable = !shared && !editor::isProtectedPath(p) && key.rfind(dir, 0) == 0;
                    }
                }
                break;
            }
        }
        slint::invoke_from_event_loop([this, gen, out, file, root] {
            if (gen != gen_)
                return;
            const auto &st = ui_->global<OpenWithState>();
            env_ = out->env;
            others_ = out->others;
            othersLoaded_ = true;
            model_ = out->model;
            saved_ = out->saved;
            fileExists_ = out->fileExists;
            fileLegacy_ = out->legacy;
            format_ = out->format;
            developPath_ = out->developPath;
            developFound_ = out->developFound;
            developHidden_ = developSavedHidden_ = out->developHidden;
            developWritable_ = out->developWritable;
            loaded_ = true;
            if (selected_ >= int(model_.apps.size()))
                selected_ = -1;
            if (devSelect_ >= 0 && devSelect_ < int(model_.apps.size())) {
                selected_ = devSelect_;
                testShown_ = true;
            }
            devSelect_ = -1;
            int found = 0;
            for (const auto &a : model_.apps)
                found += a.id != "custom" && exists(a.exe);
            st.set_found_count(found);
            st.set_loaded(true);
            // The older-layout note shows unless the file is rewritten right away (below).
            const bool legacyNote = out->legacy && (out->customised || readOnly());
            loadText_ = [this, out, file, legacyNote] {
                const auto &ows = ui_->global<OpenWithState>();
                std::string notice;
                if (out->broken)
                    notice = menus::handEditProblem(*out->broken, nss::narrow(file.filename().wstring()));
                if (legacyNote)
                    notice = nsp::trf("This file uses an older layout; Save puts the items under WinRAR or \"Open in new window\". {}",
                                      notice);
                ows.set_notice(ss(notice));
                ows.set_load_error(ss(!out->readFailed
                                          ? std::string{}
                                          : nsp::trf("Couldn't read {}: {}", nss::narrow(file.wstring()), out->error)));
            };
            loadText_();
            st.set_import_off(out->importOff);
            st.set_file_label(ss(nss::narrow(file.lexically_relative(root.parent_path()).generic_wstring())));
            ++optionsSerial_;
            edited(true);
            requestIcons();
            // A file from an older page (an "Open in" submenu in the middle of
            // the menu) is rewritten in today's layout right away, unless it has
            // lines of the user's own: then it waits for Save, with the notice.
            if ((out->legacy || out->ownChanged) && !out->customised && !readOnly())
                save(out->legacy ? Migrate::Layout : Migrate::Own);
        });
    });
}

std::string OpenWithController::iconSource(const openwith::App &app) const
{
    if (!app.icon.empty() && app.icon.find('@') == std::string::npos &&
        openwith::fileExists(splitIconIndex(nss::widen(app.icon)).first))
        return app.icon;
    return app.exe;
}

void OpenWithController::requestIcons()
{
    std::vector<std::string> wanted;
    auto want = [&](const std::string &src) {
        if (src.empty())
            return;
        auto key = lowerKey(src);
        if (icons_.count(key) || iconsPending_.count(key))
            return;
        iconsPending_[key] = true;
        wanted.push_back(src);
    };
    for (const auto &a : model_.apps)
        want(iconSource(a));
    for (const auto &o : others_)
        if (o.kind == "verb") // a shell extension's DLL rarely has the menu's icon
            want(o.program);
    if (wanted.empty())
        return;
    worker_.post([this, wanted] {
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto got = std::make_shared<std::vector<std::pair<std::string, std::optional<IconPixels>>>>();
        for (const auto &src : wanted) {
            IconPixels px;
            auto rgba = extractIcon(src, px.width, px.height);
            if (rgba) {
                px.rgba = std::move(*rgba);
                got->emplace_back(src, std::move(px));
            } else {
                got->emplace_back(src, std::nullopt);
            }
        }
        if (SUCCEEDED(com))
            CoUninitialize();
        slint::invoke_from_event_loop([this, got] {
            for (auto &[src, px] : *got) {
                auto key = lowerKey(src);
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
            pushRows();
            pushPreview();
            pushOthers();
        });
    });
}

void OpenWithController::edited(bool reloadForm)
{
    relink(); // a new program path can mean other entries
    if (reloadForm)
        ++formSerial_;
    pushRows();
    pushForm();
    pushOptions();
    pushPreview();
    pushCode();
    pushTest();
    pushOthers();
    const auto &st = ui_->global<OpenWithState>();
    st.set_dirty(dirty());
    st.set_first_run(!fileExists_);
    st.set_develop_found(developFound_);
    st.set_develop_hidden(developHidden_);
    st.set_develop_locked(!developWritable_);
    requestIcons();
}

void OpenWithController::pushRows()
{
    const auto &st = ui_->global<OpenWithState>();
    std::vector<OpenWithRow> rows;
    int missing = 0;
    for (const auto &a : model_.apps) {
        OpenWithRow r{};
        r.name = ss(a.name.empty() ? std::string(nsp::tr("(no name)")) : a.name);
        r.found = exists(a.exe);
        r.enabled = a.enabled;
        r.custom = a.id == "custom";
        missing += !r.found && !r.custom;
        r.detail = ss(a.exe.empty() ? std::string(r.custom ? nsp::tr("No program chosen") : nsp::tr("Not found on this PC"))
                      : r.found   ? a.exe
                                  : nsp::trf("Not found: {}", a.exe));
        std::string problem = openwith::validate(a);
        if (problem.empty() && a.enabled && !reachable(model_.options, a))
            problem = nsp::tr("Doesn't show: the menu isn't on the places it's for.");
        r.problem = ss(problem);
        if (auto own = nativesText(size_t(&a - model_.apps.data())); !own.empty())
            r.own = ss(openwith::hidesNatives(a) ? nsp::trf("Also hides: {}", own)
                       : a.enabled             ? nsp::trf("Switching it off also hides: {}", own)
                       : a.keepOwn             ? nsp::trf("Keeps its own: {}", own)
                                               : nsp::trf("Off by default, its own still show: {}", own));
        auto it = icons_.find(lowerKey(iconSource(a)));
        if (it != icons_.end() && it->second) {
            r.icon = *it->second;
            r.has_icon = true;
        }
        rows.push_back(r);
    }
    st.set_rows(std::make_shared<slint::VectorModel<OpenWithRow>>(rows));
    st.set_missing_count(missing);
    st.set_selected(selected_);
    bool has = selected_ >= 0 && selected_ < int(model_.apps.size()) && !readOnly();
    st.set_can_up(has && selected_ > 0);
    st.set_can_down(has && selected_ + 1 < int(model_.apps.size()));
}

void OpenWithController::pushOthers()
{
    const auto &st = ui_->global<OpenWithState>();
    std::vector<OtherAppRow> rows;
    int hidden = 0, leftovers = 0;
    for (const auto &ref : otherRows()) {
        OtherAppRow r{};
        const otherapps::Item *item = ref.item >= 0 ? &others_[size_t(ref.item)] : nullptr;
        const openwith::Hidden *h = ref.hidden >= 0 ? &model_.hidden[size_t(ref.hidden)] : nullptr;
        r.shown = !h;
        hidden += h != nullptr;
        r.extension = item ? item->kind == "handler" : h->kind == "handler";
        r.gone = !item;
        r.hide_title = ss(h ? h->title : std::string{});
        if (item) {
            r.title = ss(r.extension && h && !h->title.empty() ? h->title : item->title);
            std::string where = otherapps::describeWhere(item->where);
            std::string program = item->program.empty() ? std::string{} : nss::narrow(fs::path(nss::widen(item->program)).filename().wstring());
            if (r.extension)
                r.detail = ss((program.empty() ? nsp::trf("Shell extension \"{}\"", item->name)
                                               : nsp::trf("Shell extension \"{}\" ({})", item->name, program)) +
                              (where.empty() ? "" : " · " + where));
            else
                r.detail = ss(where + (program.empty() ? "" : " · " + program));
            r.tag = item->shiftOnly ? nsp::tr("Shift only") : r.extension ? nsp::tr("Title shown by the app") : "";
            if (!item->missing.empty()) {
                // An uninstaller left it behind: the menu item does nothing.
                r.leftover = true;
                r.detail = ss(nsp::trf("Program not found: {}", item->missing));
                leftovers += r.shown;
            }
            auto it = icons_.find(lowerKey(item->program));
            if (!item->program.empty() && it != icons_.end() && it->second) {
                r.icon = *it->second;
                r.has_icon = true;
            }
        } else {
            r.title = ss(h->title.empty() ? h->name : h->title);
            r.detail = nsp::tr("No longer in the registry. The rule stays until you switch it back on.");
        }
        if (h && openwith::exactFind(h->title).empty())
            r.problem = ss(nss::trim(h->title).empty()
                               ? nsp::tr("Type the title the menu shows, or nothing is hidden.")
                               : nsp::tr("A title with \" or | can't be matched exactly, so nothing is hidden."));
        rows.push_back(r);
    }
    // Leftovers come first (otherRows()): a heading over them and over the rest.
    if (!rows.empty() && rows.front().leftover) {
        rows.front().section = nsp::tr("Leftovers from uninstalled programs");
        for (auto &r : rows)
            if (!r.leftover) {
                r.section = nsp::tr("Other items");
                break;
            }
    }
    st.set_others(std::make_shared<slint::VectorModel<OtherAppRow>>(rows));
    st.set_others_leftovers(leftovers);
    st.set_others_hidden(hidden);
    st.set_others_loaded(othersLoaded_);
}

void OpenWithController::pushForm()
{
    const auto &st = ui_->global<OpenWithState>();
    OpenWithForm f{};
    if (selected_ >= 0 && selected_ < int(model_.apps.size())) {
        const auto &a = model_.apps[size_t(selected_)];
        f.name = ss(a.name);
        f.exe = ss(a.exe);
        f.args = ss(a.args);
        f.icon = ss(a.icon);
        f.scope = int(a.scope);
        f.extensions = ss(a.extensions);
        f.admin = a.admin;
        f.custom = a.id == "custom";
        f.keep_own = a.keepOwn;
        f.own = ss(nativesText(size_t(selected_)));
    }
    st.set_form(f);
    st.set_form_serial(formSerial_);
}

void OpenWithController::pushOptions()
{
    const auto &st = ui_->global<OpenWithState>();
    const auto &o = model_.options;
    st.set_submenu(o.submenu);
    st.set_title(ss(o.title));
    st.set_show_files(o.files);
    st.set_show_folders(o.folders);
    st.set_show_background(o.background);
    st.set_show_desktop(o.desktop);
    st.set_options_serial(optionsSerial_);
    std::string problem;
    if (!o.files && !o.folders && !o.background && !o.desktop)
        problem = nsp::tr("Pick at least one place, or nothing shows.");
    else if (o.submenu && nss::trim(o.title).empty())
        problem = nsp::tr("Give the submenu a title.");
    st.set_options_problem(ss(problem));
}

void OpenWithController::pushPreview()
{
    const auto &st = ui_->global<OpenWithState>();
    const auto &o = model_.options;
    std::string kind = kKinds[std::clamp(previewKind_, 0, 2)];
    std::vector<OpenWithPreviewRow> items;
    for (const auto &a : model_.apps) {
        if (!a.enabled || !openwith::validate(a).empty() || !exists(a.exe) ||
            !openwith::showsFor(o, a, kind, kind == "file" ? ".txt" : ""))
            continue;
        OpenWithPreviewRow r{};
        r.kind = "item";
        r.title = ss(openwith::itemTitle(o, a));
        auto it = icons_.find(lowerKey(iconSource(a)));
        if (it != icons_.end() && it->second) {
            r.icon = *it->second;
            r.has_icon = true;
        }
        items.push_back(r);
    }
    // Ours go under WinRAR, else under "Open in new window", else first.
    std::vector<OpenWithPreviewRow> ours, root, flyout;
    if (o.submenu && !items.empty()) {
        OpenWithPreviewRow m{};
        m.kind = "menu";
        m.title = ss(o.title);
        m.open = true;
        ours.push_back(m);
        flyout = items;
    } else if (!o.submenu) {
        ours = items;
    }
    auto native = [&](const char *kind, const char *title) {
        OpenWithPreviewRow r{};
        r.kind = kind;
        r.title = title;
        root.push_back(r);
    };
    // Explorer's own items, in English until the anchor is found (kAnchors), then translated.
    if (kind == "file") {
        native("native", nsp::mark("Open"));
        native("native-menu", nsp::mark("Open with"));
        native("sep", "");
        native("native", nsp::mark("Cut"));
        native("native", nsp::mark("Copy"));
    } else if (kind == "dir") {
        native("native", nsp::mark("Open"));
        native("native", nsp::mark("Open in new window"));
        native("native", nsp::mark("Pin to Quick access"));
        native("sep", "");
        native("native", nsp::mark("Copy"));
    } else {
        native("native-menu", nsp::mark("View"));
        native("native-menu", nsp::mark("Sort by"));
        native("native", nsp::mark("Refresh"));
        native("sep", "");
        native("native", nsp::mark("Paste"));
    }
    size_t at = 0;
    bool anchored = false;
    for (auto anchor : openwith::kAnchors) {
        for (size_t i = 0; i < root.size() && !anchored; ++i)
            if (nss::iequals(str(root[i].title), anchor)) {
                at = i + 1;
                anchored = true;
            }
        if (anchored)
            break;
    }
    for (auto &r : root)
        if (auto k = str(r.kind); k == "native" || k == "native-menu")
            r.title = nsp::tr(str(r.title).c_str());
    root.insert(root.begin() + ptrdiff_t(at), ours.begin(), ours.end());
    st.set_preview(std::make_shared<slint::VectorModel<OpenWithPreviewRow>>(root));
    st.set_flyout(std::make_shared<slint::VectorModel<OpenWithPreviewRow>>(flyout));
    const char *empty = previewKind_ == 0   ? nsp::tr("Nothing of yours shows for a .txt file.")
                        : previewKind_ == 1 ? nsp::tr("Nothing of yours shows for a folder.")
                                            : nsp::tr("Nothing of yours shows for a folder's background.");
    st.set_preview_empty(items.empty() ? ss(empty) : slint::SharedString());
}

void OpenWithController::pushCode()
{
    const auto &st = ui_->global<OpenWithState>();
    auto text = openwith::generate(model_, env_, nss::Eol::LF);
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

void OpenWithController::pushTest()
{
    const auto &st = ui_->global<OpenWithState>();
    bool shown = testShown_ && selected_ >= 0 && selected_ < int(model_.apps.size());
    st.set_test_shown(shown);
    if (!shown)
        return;
    const auto &a = model_.apps[size_t(selected_)];
    const auto &o = model_.options;
    std::vector<OpenWithTestLine> lines;
    std::string home = nss::narrow(env_.userProfile.empty() ? std::wstring(L"C:\\Users\\you") : env_.userProfile);
    std::string docs = home + "\\Documents";
    std::string ext = ".txt";
    if (a.scope == openwith::Scope::Extensions) {
        auto list = menus::extensionsToFind(a.extensions);
        if (!list.empty())
            ext = list.substr(0, list.find('|'));
    }
    std::string note;
    if (a.exe.empty()) {
        note = nsp::tr("Pick the program first.");
    } else {
        bool multiple = openwith::usesAllSelected(a.args);
        if (openwith::showsFor(o, a, "file", ext)) {
            openwith::Sample s;
            s.paths = { docs + "\\notes" + ext };
            if (multiple)
                s.paths.push_back(docs + "\\todo" + ext);
            s.dir = docs;
            lines.push_back(OpenWithTestLine{ ss(multiple ? nsp::tr("Two files") : nsp::tr("A file")), ss(openwith::commandLine(a, s)) });
        }
        if (openwith::showsFor(o, a, "dir")) {
            openwith::Sample s;
            s.paths = { docs + "\\Projects" };
            s.dir = docs + "\\Projects";
            lines.push_back(OpenWithTestLine{ nsp::tr("A folder"), ss(openwith::commandLine(a, s)) });
        } else if (openwith::showsFor(o, a, "back")) {
            openwith::Sample s;
            s.paths = { docs };
            s.dir = docs;
            lines.push_back(OpenWithTestLine{ nsp::tr("A folder's background"), ss(openwith::commandLine(a, s)) });
        }
        if (lines.empty())
            note = nsp::tr("It doesn't show anywhere with the current settings, so there's nothing to run.");
        if (!exists(a.exe))
            note += (note.empty() ? "" : " ") + std::string(nsp::tr("The program isn't there, so Shell hides the item."));
        if (a.args.find('@') != std::string::npos)
            note += (note.empty() ? "" : " ") + std::string(nsp::tr("Parts starting with @ are filled in by Shell when the menu opens."));
        if (a.admin)
            note += (note.empty() ? "" : " ") + std::string(nsp::tr("It runs as administrator; Windows asks first."));
    }
    st.set_test_lines(std::make_shared<slint::VectorModel<OpenWithTestLine>>(lines));
    st.set_test_note(ss(note));
}

void OpenWithController::select(int index)
{
    selected_ = index >= 0 && index < int(model_.apps.size()) ? index : -1;
    testShown_ = false;
    ++formSerial_;
    pushRows();
    pushForm();
    pushTest();
}

void OpenWithController::move(int delta)
{
    if (readOnly() || selected_ < 0)
        return;
    int to = selected_ + delta;
    if (to < 0 || to >= int(model_.apps.size()))
        return;
    std::swap(model_.apps[size_t(selected_)], model_.apps[size_t(to)]);
    selected_ = to;
    edited(true);
}

void OpenWithController::setText(const std::string &field, const std::string &value)
{
    if (readOnly())
        return;
    if (field == "title") {
        model_.options.title = value;
        edited(false);
        return;
    }
    if (selected_ < 0 || selected_ >= int(model_.apps.size()))
        return;
    auto &a = model_.apps[size_t(selected_)];
    if (field == "name")
        a.name = value;
    else if (field == "exe")
        a.exe = unquotePath(value);
    else if (field == "args")
        a.args = value;
    else if (field == "icon")
        a.icon = unquotePath(value);
    else if (field == "extensions")
        a.extensions = value;
    else
        return;
    edited(false);
}

void OpenWithController::setChoice(const std::string &field, int value)
{
    if (readOnly() || value < 0)
        return;
    if (field == "scope" && selected_ >= 0 && selected_ < int(model_.apps.size())) {
        auto &a = model_.apps[size_t(selected_)];
        a.scope = openwith::Scope(std::min(value, int(openwith::Scope::Extensions)));
        if (a.scope == openwith::Scope::Extensions && a.extensions.empty())
            a.extensions = ".txt";
        ++formSerial_; // the extensions field appears with its value
    } else {
        return;
    }
    edited(false);
}

void OpenWithController::setFlag(const std::string &field, bool value)
{
    if (readOnly())
        return;
    auto &o = model_.options;
    if (field == "submenu")
        o.submenu = value;
    else if (field == "files")
        o.files = value;
    else if (field == "folders")
        o.folders = value;
    else if (field == "background")
        o.background = value;
    else if (field == "desktop")
        o.desktop = value;
    else if (field == "hide-develop") {
        if (!developWritable_)
            return;
        developHidden_ = value;
    } else if (field == "admin" && selected_ >= 0 && selected_ < int(model_.apps.size()))
        model_.apps[size_t(selected_)].admin = value;
    else if (field == "keep-own" && selected_ >= 0 && selected_ < int(model_.apps.size()))
        model_.apps[size_t(selected_)].keepOwn = value;
    else
        return;
    edited(false);
}

std::string OpenWithController::pickFile(const wchar_t *title, const wchar_t *filterName, const wchar_t *filterSpec,
                                         const std::string &start)
{
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return {};
    DWORD flags = 0;
    dlg->GetOptions(&flags);
    dlg->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    const std::wstring allFiles = nss::widen(nsp::tr("All files"));
    COMDLG_FILTERSPEC filters[] = { { filterName, filterSpec }, { allFiles.c_str(), L"*.*" } };
    dlg->SetFileTypes(UINT(std::size(filters)), filters);
    // Start next to the current program, else in Program Files.
    std::wstring folder;
    if (!start.empty())
        folder = fs::path(splitIconIndex(nss::widen(start)).first).parent_path().wstring();
    if (folder.empty() || GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES)
        folder = env_.programFiles;
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

void OpenWithController::save(Migrate migrate)
{
    if (saving_ || readOnly() || !dirty())
        return;
    if (migrate != Migrate::None && shellExe_.empty())
        return; // waits for a Save, which says why it can't
    if (shellExe_.empty()) {
        toast("error", nsp::tr("Can't check the config"), nsp::tr("shell.exe was not found, so nothing was saved."));
        return;
    }
    auto file = openWithFile();
    if (editor::isProtectedPath(file) || editor::isProtectedPath(root_)) {
        toast("error", nsp::tr("Not saved"), nsp::tr("Program Files is never written to."));
        return;
    }

    // "Open with: VS Code, Zed in "Open in""
    std::vector<std::string> names;
    for (const auto &a : model_.apps)
        if (a.enabled && !a.exe.empty() && openwith::validate(a).empty())
            names.push_back(a.name);
    std::string reason = "Open with: ";
    if (names.empty())
        reason += "no apps";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i == 4) {
            reason += " and " + std::to_string(names.size() - i) + " more";
            break;
        }
        reason += (i ? ", " : "") + names[i];
    }
    if (!names.empty())
        reason += model_.options.submenu ? " in \"" + model_.options.title + "\"" : std::string(" as top-level items");
    bool developChange = developFound_ && developHidden_ != developSavedHidden_;
    if (developChange)
        reason += developHidden_ ? "; hide Develop > Editors" : "; show Develop > Editors";
    if (!(model_.hidden == saved_.hidden)) {
        size_t n = 0;
        for (const auto &h : model_.hidden)
            n += !openwith::exactFind(h.title).empty();
        reason += "; other apps' items hidden: " + std::to_string(n);
    }
    // The switched-off apps whose own entries are hidden too.
    std::vector<std::string> hiding;
    for (size_t i = 0; i < model_.apps.size(); ++i) {
        const auto &a = model_.apps[i];
        if (a.exe.empty() || a.natives.empty() || !openwith::hidesNatives(a))
            continue;
        const auto *was = i < saved_.apps.size() && saved_.apps[i].id == a.id ? &saved_.apps[i] : nullptr;
        if (!was || !openwith::hidesNatives(*was) || !(was->natives == a.natives))
            hiding.push_back(a.name);
    }
    std::string hidingText;
    for (size_t i = 0; i < hiding.size(); ++i)
        hidingText += (i == 0 ? "" : i + 1 == hiding.size() ? " and " : ", ") + hiding[i];
    if (!hiding.empty())
        reason += "; hide " + hidingText + "'s own items";
    if (migrate == Migrate::Layout)
        reason = "Open with: moved the items under WinRAR / Open in new window";
    else if (migrate == Migrate::Own)
        reason = "Open with: hide the own menu items of " + (hidingText.empty() ? std::string("switched-off apps") : hidingText);

    auto model = model_;
    bool hideDevelop = developHidden_;
    saving_ = true;
    ui_->global<OpenWithState>().set_saving(true);
    worker_.post([this, model, reason, file, root = root_, exe = shellExe_, format = format_, env = env_,
                  developChange, hideDevelop, migrate, hiding, developPath = developPath_] {
        nss::SaveResult result;
        std::string error;
        // A hand edit that doesn't parse can't be read into the page: the file stays as it is.
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
            writes.push_back(nss::FileWrite{ file, openwith::generate(model, env, format.eol), format });
            if (menus::MenuWriter::ensureImported(*rootDoc, openwith::kRelativePath))
                writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
            if (developChange) {
                auto dev = nss::NssDocument::load(developPath, {}, &error);
                if (!dev || !openwith::setEditorsMenuEnabled(*dev, !hideDevelop))
                    result.error = nsp::trf("Couldn't change the Editors submenu in {}.", nss::narrow(developPath.wstring()));
                else
                    writes.push_back(nss::FileWrite{ developPath, dev->text(), dev->format() });
            }
            if (result.error.empty()) {
                nss::ConfigStore store(root);
                nss::SaveOptions options;
                options.reason = reason;
                options.validator = nss::makeShellCheckValidator(exe);
                result = store.save(std::move(writes), options);
            }
        }
        slint::invoke_from_event_loop([this, model, hideDevelop, migrate, hiding, result = std::move(result), root] {
            saving_ = false;
            ui_->global<OpenWithState>().set_saving(false);
            if (result.ok) {
                saved_ = model;
                fileExists_ = true;
                fileLegacy_ = false;
                developSavedHidden_ = hideDevelop;
                edited(false);
                if (migrate == Migrate::Own)
                    toast("ok", nsp::tr("Updated the switched-off apps' own items"),
                          nsp::trf("{} Snapshot {} has the file before.",
                                   hiding.empty() ? std::string(nsp::tr("Their menu entries changed in the registry."))
                                                  : nsp::trf("The menu no longer shows {}'s own entries.", joinNames(hiding)),
                                   result.snapshotId));
                else if (migrate == Migrate::Layout)
                    toast("ok", nsp::tr("Moved the Open with items"),
                          nsp::trf("They're now right under WinRAR (or \"Open in new window\"; else at the top). Snapshot {} has the old layout.",
                                   result.snapshotId));
                else
                    toast("ok", nsp::tr("Saved the Open with items"),
                          result.snapshotId.empty()
                              ? std::string(nsp::tr("No changes were needed."))
                              : nsp::trf("Snapshot {} was taken first. Right-click a file to see them.", result.snapshotId));
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
                toast("error", nsp::tr("Not saved: Shell would reject these items"), detail);
                return;
            }
            toast("error", nsp::tr("Couldn't save"), detail);
        });
    });
}
