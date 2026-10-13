#include "OpenWithController.h"

#include "ConfigRules.h"
#include "EditorSession.h"
#include "Highlight.h"
#include "MenuWriter.h"
#include "ShellCli.h"

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
    HICON icon = nullptr;
    if (SHDefExtractIconW(path.c_str(), index, 0, &icon, nullptr, 32) != S_OK || !icon)
        return std::nullopt;
    std::optional<std::vector<uint8_t>> out;
    ICONINFO ii{};
    if (GetIconInfo(icon, &ii)) {
        BITMAP bm{};
        if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
            const int bw = bm.bmWidth, bh = bm.bmHeight;
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = bw;
            bi.bmiHeader.biHeight = -bh; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            std::vector<uint8_t> bgra(size_t(bw) * size_t(bh) * 4);
            HDC dc = GetDC(nullptr);
            int lines = GetDIBits(dc, ii.hbmColor, 0, UINT(bh), bgra.data(), &bi, DIB_RGB_COLORS);
            bool anyAlpha = false;
            for (size_t i = 3; i < bgra.size(); i += 4)
                anyAlpha |= bgra[i] != 0;
            std::vector<uint8_t> mask;
            if (lines == bh && !anyAlpha && ii.hbmMask) {
                // An icon without alpha: the AND mask says what's transparent.
                mask.resize(bgra.size());
                GetDIBits(dc, ii.hbmMask, 0, UINT(bh), mask.data(), &bi, DIB_RGB_COLORS);
            }
            ReleaseDC(nullptr, dc);
            if (lines == bh) {
                std::vector<uint8_t> rgba(bgra.size());
                for (size_t i = 0; i < size_t(bw) * size_t(bh); ++i) {
                    rgba[i * 4 + 0] = bgra[i * 4 + 2];
                    rgba[i * 4 + 1] = bgra[i * 4 + 1];
                    rgba[i * 4 + 2] = bgra[i * 4 + 0];
                    rgba[i * 4 + 3] = anyAlpha ? bgra[i * 4 + 3] : (mask.empty() || mask[i * 4] == 0 ? 255 : 0);
                }
                w = uint32_t(bw);
                h = uint32_t(bh);
                out = std::move(rgba);
            }
        }
        if (ii.hbmColor)
            DeleteObject(ii.hbmColor);
        if (ii.hbmMask)
            DeleteObject(ii.hbmMask);
    }
    DestroyIcon(icon);
    return out;
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
        model_.apps[size_t(index)].enabled = on;
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
        a.name = "New app";
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
        auto picked = pickFile(L"Choose the program", L"Programs", L"*.exe;*.cmd;*.bat;*.com", a.exe);
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
        a.enabled = true;
        edited(true);
    });
    st.on_browse_icon([this] {
        if (readOnly() || selected_ < 0 || selected_ >= int(model_.apps.size()))
            return;
        auto &a = model_.apps[size_t(selected_)];
        auto picked = pickFile(L"Choose an icon", L"Icons and programs", L"*.ico;*.exe;*.dll", a.icon.empty() ? a.exe : a.icon);
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
    return !(model_ == saved_) || developHidden_ != developSavedHidden_;
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
        st.set_load_error(ss("The config file doesn't exist: " + path));
        return;
    }
    root_ = fs::path(nss::widen(path)).lexically_normal();
    shared_ = str(status.config_source) == "install" || editor::isProtectedPath(root_);
    st.set_shared_config(shared_);
    st.set_load_error("");
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
            nss::TextFormat format = nss::defaultFormat();
            std::string notice;
            std::string error;
            bool importOff = false;
            fs::path developPath;
            bool developFound = false;
            bool developHidden = false;
            bool developWritable = false;
        };
        auto out = std::make_shared<Loaded>();
        out->env = openwith::Env::current();
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
                if (!parsed.warnings.empty())
                    out->notice = std::to_string(parsed.warnings.size()) +
                                  " line(s) of the file weren't written by this page and will be replaced on save. First: " +
                                  parsed.warnings[0];
            } else {
                out->error = "Couldn't read " + nss::narrow(file.wstring()) + ": " + error;
            }
        }
        out->model = openwith::merge(saved, detected);
        out->saved = out->model;
        if (!saved)
            for (auto &a : out->saved.apps)
                a.enabled = false; // nothing is in the menu until the first save

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
            model_ = out->model;
            saved_ = out->saved;
            fileExists_ = out->fileExists;
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
            st.set_notice(ss(out->notice));
            st.set_load_error(ss(out->error));
            st.set_import_off(out->importOff);
            st.set_file_label(ss(nss::narrow(file.lexically_relative(root.parent_path()).generic_wstring())));
            ++optionsSerial_;
            edited(true);
            requestIcons();
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
    for (const auto &a : model_.apps) {
        auto src = iconSource(a);
        if (src.empty())
            continue;
        auto key = lowerKey(src);
        if (icons_.count(key) || iconsPending_.count(key))
            continue;
        iconsPending_[key] = true;
        wanted.push_back(src);
    }
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
        });
    });
}

void OpenWithController::edited(bool reloadForm)
{
    if (reloadForm)
        ++formSerial_;
    pushRows();
    pushForm();
    pushOptions();
    pushPreview();
    pushCode();
    pushTest();
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
        r.name = ss(a.name.empty() ? std::string("(no name)") : a.name);
        r.found = exists(a.exe);
        r.enabled = a.enabled;
        r.custom = a.id == "custom";
        missing += !r.found && !r.custom;
        r.detail = ss(a.exe.empty() ? std::string(r.custom ? "No program chosen" : "Not found on this PC")
                      : r.found   ? a.exe
                                  : "Not found: " + a.exe);
        std::string problem = openwith::validate(a);
        if (problem.empty() && a.enabled && !reachable(model_.options, a))
            problem = "Doesn't show: the menu isn't on the places it's for.";
        r.problem = ss(problem);
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
        problem = "Pick at least one place, or nothing shows.";
    else if (o.submenu && nss::trim(o.title).empty())
        problem = "Give the submenu a title.";
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
    std::vector<OpenWithPreviewRow> root, flyout;
    if (o.submenu && !items.empty()) {
        OpenWithPreviewRow m{};
        m.kind = "menu";
        m.title = ss(o.title);
        m.open = true;
        root.push_back(m);
        flyout = items;
    } else if (!o.submenu) {
        root = items;
    }
    st.set_preview(std::make_shared<slint::VectorModel<OpenWithPreviewRow>>(root));
    st.set_flyout(std::make_shared<slint::VectorModel<OpenWithPreviewRow>>(flyout));
    const char *what = previewKind_ == 0 ? "a .txt file" : previewKind_ == 1 ? "a folder" : "a folder's background";
    st.set_preview_empty(ss(std::string("Nothing shows for ") + what + "."));
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
        note = "Pick the program first.";
    } else {
        bool multiple = openwith::usesAllSelected(a.args);
        if (openwith::showsFor(o, a, "file", ext)) {
            openwith::Sample s;
            s.paths = { docs + "\\notes" + ext };
            if (multiple)
                s.paths.push_back(docs + "\\todo" + ext);
            s.dir = docs;
            lines.push_back(OpenWithTestLine{ ss(multiple ? "Two files" : "A file"), ss(openwith::commandLine(a, s)) });
        }
        if (openwith::showsFor(o, a, "dir")) {
            openwith::Sample s;
            s.paths = { docs + "\\Projects" };
            s.dir = docs + "\\Projects";
            lines.push_back(OpenWithTestLine{ "A folder", ss(openwith::commandLine(a, s)) });
        } else if (openwith::showsFor(o, a, "back")) {
            openwith::Sample s;
            s.paths = { docs };
            s.dir = docs;
            lines.push_back(OpenWithTestLine{ "A folder's background", ss(openwith::commandLine(a, s)) });
        }
        if (lines.empty())
            note = "It doesn't show anywhere with the current settings, so there's nothing to run.";
        if (!exists(a.exe))
            note += (note.empty() ? "" : " ") + std::string("The program isn't there, so Shell hides the item.");
        if (a.args.find('@') != std::string::npos)
            note += (note.empty() ? "" : " ") + std::string("Parts starting with @ are filled in by Shell when the menu opens.");
        if (a.admin)
            note += (note.empty() ? "" : " ") + std::string("It runs as administrator; Windows asks first.");
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
    if (field == "layout") {
        model_.options.submenu = value == 0;
    } else if (field == "scope" && selected_ >= 0 && selected_ < int(model_.apps.size())) {
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
    if (field == "files")
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
    COMDLG_FILTERSPEC filters[] = { { filterName, filterSpec }, { L"All files", L"*.*" } };
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

void OpenWithController::save()
{
    if (saving_ || readOnly() || !dirty())
        return;
    if (shellExe_.empty()) {
        toast("error", "Can't check the config", "shell.exe was not found, so nothing was saved.");
        return;
    }
    auto file = openWithFile();
    if (editor::isProtectedPath(file) || editor::isProtectedPath(root_)) {
        toast("error", "Not saved", "Program Files is never written to.");
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

    auto model = model_;
    bool hideDevelop = developHidden_;
    saving_ = true;
    ui_->global<OpenWithState>().set_saving(true);
    worker_.post([this, model, reason, file, root = root_, exe = shellExe_, format = format_, env = env_,
                  developChange, hideDevelop, developPath = developPath_] {
        nss::SaveResult result;
        std::string error;
        auto rootDoc = nss::NssDocument::load(root, {}, &error);
        if (!rootDoc) {
            result.error = "Couldn't read " + nss::narrow(root.wstring()) + ": " + error;
        } else {
            std::vector<nss::FileWrite> writes;
            writes.push_back(nss::FileWrite{ file, openwith::generate(model, env, format.eol), format });
            if (menus::MenuWriter::ensureImported(*rootDoc, openwith::kRelativePath))
                writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
            if (developChange) {
                auto dev = nss::NssDocument::load(developPath, {}, &error);
                if (!dev || !openwith::setEditorsMenuEnabled(*dev, !hideDevelop))
                    result.error = "Couldn't change the Editors submenu in " + nss::narrow(developPath.wstring()) + ".";
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
        slint::invoke_from_event_loop([this, model, hideDevelop, result = std::move(result), root] {
            saving_ = false;
            ui_->global<OpenWithState>().set_saving(false);
            if (result.ok) {
                saved_ = model;
                fileExists_ = true;
                developSavedHidden_ = hideDevelop;
                edited(false);
                toast("ok", "Saved the Open with items",
                      result.snapshotId.empty() ? std::string("No changes were needed.")
                                                : "Snapshot " + result.snapshotId +
                                                      " was taken first. Right-click a file to see them.");
                return;
            }
            std::string detail = result.error;
            if (!result.check.ok && !result.check.message.empty()) {
                auto outcome = editor::parseCheck(result.check.message, {}, root.parent_path());
                for (const auto &i : outcome.issues)
                    if (i.error) {
                        detail = i.message + (i.line > 0 ? " (line " + std::to_string(i.line) + ")" : std::string{});
                        break;
                    }
                toast("error", "Not saved: Shell would reject these items", detail);
                return;
            }
            toast("error", "Couldn't save", detail);
        });
    });
}
