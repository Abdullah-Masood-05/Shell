#include "SnippetsController.h"

#include "EditorSession.h"
#include "FileIcon.h"
#include "Highlight.h"
#include "ShellCli.h"
#include "Tr.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>

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

namespace sp = snippetpreview;

// %TEMP%\NilesoftShellPro\snippet-preview\<id>: the scratch root, the snippet and the samples.
fs::path previewDir(const std::string &id)
{
    wchar_t buf[MAX_PATH + 1]{};
    DWORD n = GetTempPathW(MAX_PATH, buf);
    fs::path dir = n ? fs::path(std::wstring(buf, n)) : fs::temp_directory_path();
    return dir / L"NilesoftShellPro" / L"snippet-preview" / nss::widen(id);
}

std::wstring env(const wchar_t *name)
{
    wchar_t buf[MAX_PATH + 1]{};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

// What @sys.prog and the like stand for in an image path.
std::map<std::string, std::string> pathVariables(const fs::path &exe)
{
    std::map<std::string, std::string> v;
    auto put = [&](const char *name, const std::wstring &value) {
        if (!value.empty())
            v[name] = nss::narrow(value);
    };
    const auto windir = env(L"SystemRoot");
    put("sys.prog", env(L"ProgramFiles"));
    put("sys.prog32", env(L"ProgramFiles(x86)"));
    put("sys.programdata", env(L"ProgramData"));
    put("sys.dir", windir);
    put("sys.bin", windir.empty() ? std::wstring() : windir + L"\\System32");
    put("sys.root", env(L"SystemDrive"));
    put("user.appdata", env(L"APPDATA"));
    put("user.localappdata", env(L"LOCALAPPDATA"));
    put("user.dir", env(L"USERPROFILE"));
    put("user.home", env(L"USERPROFILE"));
    put("app.dir", exe.parent_path().wstring());
    return v;
}

// The sample a context's menu is read for: an empty file with the snippet's extension,
// a folder, or the system drive; nothing for the desktop and the taskbar.
fs::path prepareSample(sp::Context c, const fs::path &dir, const sp::Sample &sample)
{
    std::error_code ec;
    switch (c) {
    case sp::Context::File: {
        auto file = dir / (L"Sample" + nss::widen(sample.extension));
        if (!fs::exists(file, ec)) {
            std::ofstream out(file, std::ios::binary);
            if (sample.extension == ".zip") {
                // An empty archive (end of central directory only), so it opens as one.
                const char eocd[22] = { 'P', 'K', 5, 6 };
                out.write(eocd, sizeof eocd);
            }
        }
        return file;
    }
    case sp::Context::Folder:
    case sp::Context::Back: {
        auto folder = dir / L"Sample folder";
        fs::create_directories(folder, ec);
        return folder;
    }
    case sp::Context::Drive: {
        auto drive = env(L"SystemDrive");
        return fs::path((drive.empty() ? std::wstring(L"C:") : drive) + L"\\");
    }
    default:
        return {};
    }
}

void collectIcons(const std::vector<sp::Item> &items, std::vector<sp::Icon> &out)
{
    for (const auto &it : items) {
        if (it.icon.kind == sp::IconKind::File)
            out.push_back(it.icon);
        collectIcons(it.children, out);
    }
}

std::string iconKey(const sp::Icon &icon) { return icon.path + "," + std::to_string(icon.index); }

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

void SnippetsController::selectForPreview(const std::string &id, const std::string &context)
{
    wantedContext_ = context;
    select(id);
}

void SnippetsController::setTheme(const theme::Resolved &resolved)
{
    theme_ = resolved;
    if (!selected_.empty())
        pushPreview();
}

void SnippetsController::select(const std::string &id)
{
    if (id == selected_)
        return;
    selected_ = id;
    previewContext_ = 0;
    codeErrors_.erase(id); // a download that failed is tried again
    push();
    // Clicking through the list quickly reads only the one it stops on.
    previewDelay_.start(slint::TimerMode::SingleShot, 150ms, [this] { startPreview(); });
}

void SnippetsController::startPreview()
{
    const auto *s = catalog_ ? catalog_->find(selected_) : nullptr;
    if (!s || previews_.count(s->id) || previewing_.count(s->id))
        return;
    if (s->text.empty()) {
        // A remote snippet is downloaded and checked first, as for "Show code"; this
        // runs again once it's in.
        fetchCode(s->id);
        pushPreview();
        return;
    }
    if (shellExe_.empty()) {
        auto p = std::make_shared<Preview>();
        p->error = nsp::tr("shell.exe was not found");
        previews_[s->id] = p;
        pushPreview();
        return;
    }
    previewing_.insert(s->id);
    pushPreview();
    previewWorker_.post([this, id = s->id, text = s->text, exe = shellExe_, root = root_] {
        auto p = makePreview(id, text, exe, root);
        slint::invoke_from_event_loop([this, id, p] {
            previewing_.erase(id);
            previews_[id] = p;
            if (id == selected_)
                pushPreview();
        });
    });
}

std::shared_ptr<const SnippetsController::Preview> SnippetsController::makePreview(const std::string &id,
                                                                                   const std::string &text,
                                                                                   const fs::path &exe,
                                                                                   const fs::path &root)
{
    auto out = std::make_shared<Preview>();
    std::error_code ec;
    auto dir = previewDir(id);
    fs::create_directories(dir, ec);
    // The long form (no ABDULL~1): shell.exe names the snippet's file that way in the tree.
    if (wchar_t buf[MAX_PATH * 2]; DWORD n = GetLongPathNameW(dir.c_str(), buf, DWORD(std::size(buf))))
        if (n < std::size(buf))
            dir = fs::path(std::wstring(buf, n));
    auto write = [](const fs::path &file, const std::string &bytes) {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), std::streamsize(bytes.size()));
        return bool(f);
    };
    const auto snippetFile = dir / L"snippet.nss";
    if (!write(snippetFile, snippets::toCrlf(text))) {
        out->error = nsp::trf("Couldn't write {}", nss::narrow(snippetFile.wstring()));
        return out;
    }

    // The user's theme, images and language; their image definitions, then the snippet's.
    sp::ThemeSource source;
    auto read = [](const fs::path &p) -> std::optional<std::string> {
        std::string bytes;
        if (!nss::readFile(p, bytes))
            return std::nullopt;
        return bytes;
    };
    if (!root.empty())
        if (auto doc = nss::NssDocument::load(root)) {
            source = sp::themeSource(*doc, root.parent_path(), read);
            sp::collectImageDefs(doc->text(), out->defs);
            for (const auto &p : source.imports)
                if (auto bytes = read(p))
                    sp::collectImageDefs(*bytes, out->defs);
        }
    sp::collectImageDefs(text, out->defs);
    out->sample = sp::sampleFor(text);
    const auto snippetDoc = nss::NssDocument::fromText(snippets::normalize(text));

    struct Run
    {
        std::optional<menumap::Tree> tree;
        std::string error;
    };
    // Every context at once: each is a shell.exe of its own.
    auto runAll = [&](const sp::ThemeSource &src) {
        const auto rootFile = dir / L"root.nss";
        write(rootFile, sp::scratchRoot(src, "snippet.nss"));
        std::vector<std::future<Run>> jobs;
        for (auto c : sp::kContexts) {
            auto sample = prepareSample(c, dir, out->sample);
            jobs.push_back(std::async(std::launch::async, [exe, rootFile, c, sample] {
                std::vector<std::wstring> args{ L"-config:tree", rootFile.wstring(),
                                                L"-context:" + nss::widen(sp::contextName(c)) };
                if (!sample.empty())
                    args.push_back(L"-sample:" + sample.wstring());
                auto r = ShellCli(exe).run(args, 30000);
                Run run;
                if (!r.started)
                    run.error = nss::narrow(r.error);
                else if (r.timedOut)
                    run.error = nsp::tr("shell.exe took too long to read the menu and was stopped.");
                else {
                    std::string why;
                    run.tree = menumap::parseTree(r.output, &why);
                    if (!run.tree)
                        run.error = nsp::trf("shell.exe gave no menu ({}). It may be older than this app.", why);
                    else if (!run.tree->ok && !run.tree->errors.empty())
                        run.error = run.tree->errors.front();
                }
                return run;
            }));
        }
        std::vector<Run> runs;
        for (auto &j : jobs)
            runs.push_back(j.get());
        return runs;
    };
    auto anyOk = [](const std::vector<Run> &runs) {
        return std::any_of(runs.begin(), runs.end(), [](const Run &r) { return r.tree && r.tree->ok; });
    };

    auto runs = runAll(source);
    // The user's theme files shouldn't keep the snippet from showing: again without them.
    if (!anyOk(runs) && (!source.imports.empty() || !source.inlineTheme.empty() || source.lang))
        runs = runAll({});
    if (!anyOk(runs)) {
        for (const auto &r : runs)
            if (!r.error.empty()) {
                out->error = r.error;
                break;
            }
        if (out->error.empty())
            out->error = nsp::tr("shell.exe couldn't load the snippet");
        return out;
    }
    for (size_t i = 0; i < runs.size(); ++i) {
        if (!runs[i].tree || !runs[i].tree->ok)
            continue;
        auto items = sp::itemsOf(runs[i].tree->shell, snippetFile, snippetDoc, out->defs);
        if (!items.empty())
            out->contexts.push_back({ sp::kContexts[i], std::move(items) });
    }

    // Programs' icons are read here, so the UI thread only turns them into images.
    std::vector<sp::Icon> icons;
    for (const auto &c : out->contexts)
        collectIcons(c.items, icons);
    const auto vars = pathVariables(exe);
    for (const auto &icon : icons) {
        const auto key = iconKey(icon);
        if (out->files.count(key))
            continue;
        Preview::Pixels px;
        if (auto rgba = fileicon::extract(nss::widen(sp::expandPath(icon.path, vars)), icon.index, 32, px.width, px.height)) {
            px.rgba = std::move(*rgba);
            out->files[key] = std::move(px);
        }
    }
    return out;
}

slint::Image SnippetsController::iconImage(const sp::Icon &icon, const Preview &p)
{
    if (icon.kind == sp::IconKind::File) {
        auto it = p.files.find(iconKey(icon));
        if (it == p.files.end() || it->second.rgba.empty())
            return {};
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(
            it->second.width, it->second.height, reinterpret_cast<const slint::Rgba8Pixel *>(it->second.rgba.data()));
        return slint::Image(buf);
    }
    if (icon.kind != sp::IconKind::Svg)
        return {};
    const auto svg = sp::svgFor(icon.svg, p.defs, theme_.imageColor, !theme_.dark);
    if (auto it = svgImages_.find(svg); it != svgImages_.end())
        return it->second;
    auto image = slint::Image::load_from_data(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(svg.data()), svg.size()), "svg");
    svgImages_[svg] = image;
    return image;
}

void SnippetsController::pushPreview()
{
    const auto &st = ui_->global<SnippetsState>();
    const auto *s = catalog_ ? catalog_->find(selected_) : nullptr;
    st.set_selected(ss(s ? selected_ : std::string()));
    if (!s) {
        st.set_preview_state("");
        return;
    }
    st.set_preview_title(ss(s->titleIn(nsp::currentLanguage())));
    st.set_preview_note("");
    auto setState = [&](const char *state, const std::string &message) {
        st.set_preview_state(state);
        st.set_preview_message(ss(message));
    };
    auto showCode = [&](const std::string &message) {
        int columns = 0;
        st.set_preview_code(code(*s, columns));
        st.set_preview_code_columns(columns);
        setState("code", message);
    };

    // Selected before the catalog was in (--snippets-select), or after an error was
    // cleared: start it now.
    const bool waiting = s->text.empty() ? !fetching_.count(s->id) && !codeErrors_.count(s->id)
                                         : !previews_.count(s->id) && !previewing_.count(s->id);
    if (waiting && !previewDelay_.running())
        previewDelay_.start(slint::TimerMode::SingleShot, 150ms, [this] { startPreview(); });

    if (s->text.empty()) {
        if (auto e = codeErrors_.find(s->id); e != codeErrors_.end())
            setState("message", nsp::trf("No preview: the snippet couldn't be downloaded. {}", e->second));
        else
            setState("loading", nsp::tr("Downloading the snippet…"));
        return;
    }
    auto found = previews_.find(s->id);
    if (found == previews_.end()) {
        setState("loading", nsp::tr("Loading preview…"));
        return;
    }
    const auto &p = *found->second;
    if (!p.error.empty()) {
        showCode(nsp::trf("No preview ({}). This is the snippet's code.", p.error));
        return;
    }
    if (p.contexts.empty()) {
        showCode(nsp::tr("This snippet adds no items of its own: it changes or hides other items, or works elsewhere. This is its code."));
        return;
    }

    // --snippets-context: the first time this preview shows.
    if (!wantedContext_.empty()) {
        for (size_t i = 0; i < p.contexts.size(); ++i)
            if (wantedContext_ == sp::contextName(p.contexts[i].context))
                previewContext_ = int(i);
        wantedContext_.clear();
    }
    previewContext_ = std::clamp(previewContext_, 0, int(p.contexts.size()) - 1);
    const auto &ctx = p.contexts[size_t(previewContext_)];

    std::vector<slint::SharedString> labels;
    for (const auto &c : p.contexts)
        labels.push_back(ss(nsp::tr(sp::contextLabel(c.context))));
    st.set_preview_contexts(std::make_shared<slint::VectorModel<slint::SharedString>>(labels));
    st.set_preview_context(previewContext_);
    st.set_preview_pane(ctx.context == sp::Context::Desktop || ctx.context == sp::Context::Taskbar ? 0 : 1);
    if (ctx.context == sp::Context::File && p.sample.fromSnippet)
        st.set_preview_note(ss(nsp::trf("Shown for a {} file.", p.sample.extension)));

    const auto composed = sp::compose(ctx.context, ctx.items, [](const char *msgid) { return std::string(nsp::tr(msgid)); });
    const auto m = theme::rowMetrics(theme_);
    const std::string iconFont = str(ui_->global<AppearanceState>().get_icon_font());
    auto build = [&](const sp::Menu &menu, int open) {
        auto items = std::make_shared<slint::VectorModel<PreviewItem>>();
        float y = m.top;
        for (const auto &r : menu.rows) {
            PreviewItem it{};
            it.separator = r.separator;
            it.label = ss(r.label);
            it.disabled = r.disabled;
            it.sub = r.sub;
            it.own = r.own;
            it.placeholder = r.placeholder;
            if (r.icon.kind == sp::IconKind::Glyph) {
                it.glyph = ss(r.icon.glyph);
                it.glyph_font = ss(r.icon.font == sp::GlyphFont::Shell ? std::string("Nilesoft.Shell") : iconFont);
            } else if (r.icon.kind != sp::IconKind::None) {
                it.icon = iconImage(r.icon, p);
                it.has_icon = it.icon.size().width > 0;
            }
            it.y = y;
            it.h = r.separator ? m.sep : m.row;
            y += it.h;
            items->push_back(it);
        }
        PreviewMenu out{};
        out.items = items;
        out.height = y + m.bottom;
        out.open = open;
        return out;
    };
    st.set_preview_menu(build(composed.main, composed.open));
    auto subs = std::make_shared<slint::VectorModel<PreviewMenu>>();
    for (const auto &sub : composed.subs)
        subs->push_back(build(sub, -1));
    st.set_preview_subs(subs);
    setState("menu", "");
}

void SnippetsController::addTexts(const std::map<std::string, std::string> &texts)
{
    if (!catalog_ || texts.empty())
        return;
    auto next = std::make_shared<snippets::Catalog>(*catalog_);
    for (auto &s : next->snippets)
        if (auto it = texts.find(s.id); it != texts.end() && s.text.empty())
            s.text = it->second;
    catalog_ = next;
}

void SnippetsController::fetchCode(const std::string &id)
{
    const auto *s = catalog_ ? catalog_->find(id) : nullptr;
    if (!s || !s->remote() || !s->text.empty() || fetching_.count(id))
        return;
    fetching_.insert(id);
    codeErrors_.erase(id);
    worker_.post([this, snippet = *s] {
        auto f = snippets::fetchText(snippet);
        slint::invoke_from_event_loop([this, id = snippet.id, f = std::move(f)] {
            fetching_.erase(id);
            if (f.ok)
                addTexts({ { id, f.text } });
            else
                codeErrors_[id] = f.error;
            push();
            if (f.ok && id == selected_)
                startPreview();
        });
    });
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
        if (!expanded_.erase(key)) {
            expanded_.insert(key);
            fetchCode(key);
        }
        push();
    });

    st.on_create_user_config([this] { ui_->global<AppState>().invoke_action("config-init"); });
    st.on_select([this](slint::SharedString id) { select(str(id)); });
    st.on_set_preview_context([this](int index) {
        previewContext_ = index;
        pushPreview();
    });
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
    previews_.clear(); // made with the previous config's theme
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
            catalog = std::make_shared<const snippets::Catalog>(snippets::loadAll());
        auto out = std::make_shared<Loaded>();
        if (!root.empty()) {
            const auto dir = root.parent_path();
            std::string bytes;
            if (nss::readFile(dir / nss::widen(snippets::kListPath), bytes))
                out->files.list = bytes;
            for (const auto &s : catalog->snippets) {
                std::string copy;
                if (nss::readFile(dir / nss::widen(snippets::copyPath(s.id)), copy)) {
                    // A remote snippet's copy that is still the checked file is its text:
                    // no download needed to show its code.
                    if (s.remote() && s.text.empty() && snippets::hashText(copy) == s.sha256)
                        out->texts[s.id] = snippets::normalize(copy);
                    out->files.copies[s.id] = copy;
                }
                else
                    out->files.copies[s.id] = std::nullopt;
            }
        }
        // The refresh needs the text of remote snippets whose copy is outdated or missing:
        // download those now (a failure only means they wait for the next start).
        if (refresh && allowWrites_ && !root.empty())
            for (const auto &s : catalog->snippets) {
                if (!s.remote() || !s.text.empty())
                    continue;
                auto state = snippets::copyState(s, out->files);
                if (state != snippets::CopyState::Outdated && state != snippets::CopyState::Missing)
                    continue;
                auto f = snippets::fetchText(s);
                if (f.ok)
                    out->texts[s.id] = std::move(f.text);
            }
        auto probe = snippets::systemProbe();
        for (const auto &s : catalog->snippets)
            for (const auto &r : s.needs)
                out->requirements[s.id].push_back(snippets::checkRequirement(r, probe));
        slint::invoke_from_event_loop([this, gen, catalog, out, root, refresh] {
            if (gen != gen_)
                return;
            catalog_ = catalog;
            addTexts(out->texts);
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
            auto lines = expanded && !s.text.empty() ? code(s, columns) : std::make_shared<slint::VectorModel<CodeLine>>();
            std::string codeStatus;
            if (expanded && s.text.empty()) {
                if (auto e = codeErrors_.find(s.id); e != codeErrors_.end())
                    codeStatus = e->second;
                else
                    codeStatus = nsp::tr("Downloading the code…");
            }
            const char *label = snippets::categoryLabel(s.category);
            rows.push_back(SnippetRow{
                ss(s.id), ss(title), ss(description), ss(s.author), ss(s.authorUrl), s.remote(),
                ss(label ? std::string(nsp::tr(label)) : s.category), enabled,
                // A snippet that is on can always be turned off.
                needs.empty() || enabled, ss(needs),
                ss(s.risk == snippets::Risk::Admin ? "admin" : s.risk == snippets::Risk::Network ? "network" : ""),
                ss(note), expanded, lines, columns, ss(codeStatus), s.id == selected_ });
        }
    }
    // The first one shows in the preview until another is clicked.
    if (selected_.empty() && !rows.empty()) {
        selected_ = str(rows.front().id);
        rows.front().selected = true;
        previewDelay_.start(slint::TimerMode::SingleShot, 150ms, [this] { startPreview(); });
    }
    st.set_rows(std::make_shared<slint::VectorModel<SnippetRow>>(rows));
    st.set_summary(ss(catalog_ ? nsp::trn("{n} of {0} on", "{n} of {0} on", on, total) : std::string()));
    st.set_empty_text(ss(!catalog_ ? std::string(nsp::tr("Loading…"))
                         : catalog_->snippets.empty() ? std::string(nsp::tr("No snippets are available."))
                                                      : std::string(nsp::tr("No snippet matches the search."))));
    st.set_busy(busy_);
    st.set_busy_id(ss(busyId_));
    pushPreview();
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
    // A remote snippet is downloaded (and checked against its hash) first; nothing is
    // written when that fails.
    if (on && s->remote() && s->text.empty()) {
        busy_ = true;
        busyId_ = id;
        push();
        worker_.post([this, snippet = *s] {
            auto f = snippets::fetchText(snippet);
            slint::invoke_from_event_loop([this, id = snippet.id, title = snippet.titleIn(nsp::currentLanguage()),
                                           f = std::move(f)] {
                busy_ = false;
                busyId_.clear();
                if (!f.ok) {
                    toast("error", nsp::trf("Couldn't turn on {}", title), f.error + " " + nsp::tr("Nothing was changed."));
                    push();
                    return;
                }
                addTexts({ { id, f.text } });
                codeErrors_.erase(id);
                setNow(id, true);
            });
        });
        return;
    }
    setNow(id, on);
}

void SnippetsController::setNow(const std::string &id, bool on)
{
    const auto *s = catalog_ ? catalog_->find(id) : nullptr;
    if (!s)
        return;
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
