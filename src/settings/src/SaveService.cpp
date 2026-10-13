#include "SaveService.h"

#include "LogReader.h"
#include "Status.h"
#include "Text.h"

#include "nss/ConfigStore.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"

#include <nlohmann/json.hpp>

#include <commctrl.h>
#include <shlobj.h>

#include <chrono>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using text::narrow;
using text::widen;

namespace {

slint::SharedString ss(const std::string &s) { return slint::SharedString(std::string_view(s)); }

std::string localTimestamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    return buf;
}

bool isUnder(const fs::path &path, const KNOWNFOLDERID &folder)
{
    PWSTR raw = nullptr;
    bool under = false;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &raw)) && raw) {
        auto base = nss::pathKey(fs::path(raw));
        auto p = nss::pathKey(path);
        under = !base.empty() && p.size() > base.size() && p.compare(0, base.size(), base) == 0 &&
                (p[base.size()] == '\\' || p[base.size()] == '/');
    }
    CoTaskMemFree(raw);
    return under;
}

bool isProtectedLocation(const fs::path &path)
{
    return isUnder(path, FOLDERID_ProgramFiles) || isUnder(path, FOLDERID_ProgramFilesX86) ||
           isUnder(path, FOLDERID_Windows);
}

// The first error of a `-config:check` JSON result, as "file:line: message".
std::string firstCheckError(const std::string &json)
{
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object())
        return json.size() < 200 ? json : std::string("the check failed");
    auto errors = j.find("errors");
    if (errors != j.end() && errors->is_array() && !errors->empty()) {
        const auto &e = errors->front();
        std::string out;
        if (e.contains("file") && e["file"].is_string())
            out = fs::path(widen(e["file"].get<std::string>())).filename().string();
        if (e.contains("line") && e["line"].is_number())
            out += (out.empty() ? "line " : ":") + std::to_string(e["line"].get<int>());
        if (e.contains("message") && e["message"].is_string())
            out += (out.empty() ? "" : ": ") + e["message"].get<std::string>();
        return out;
    }
    if (j.contains("error") && j["error"].is_string())
        return j["error"].get<std::string>();
    return "the check failed";
}

} // namespace

SaveService::SaveService(slint::ComponentHandle<AppWindow> ui, fs::path shellExe)
    : ui_(std::move(ui)), cli_(std::move(shellExe))
{
    const auto &st = ui_->global<SaveState>();
    st.on_save([this] { save(); });
    st.on_discard([this] { discard(); });
    st.on_create_user_config([this] { createUserConfig(); });
    st.on_cancel_user_config([this] { ui_->global<SaveState>().set_user_config_prompt(false); });
    st.on_dismiss_result([this] {
        resultTimer_.stop();
        setResult("", "");
    });
    publish();
}

SaveService::~SaveService()
{
    if (hwnd_)
        RemoveWindowSubclass(hwnd_, &SaveService::keyProc, 1);
}

void SaveService::attachWindow(HWND hwnd)
{
    hwnd_ = hwnd;
    SetWindowSubclass(hwnd_, &SaveService::keyProc, 1, reinterpret_cast<DWORD_PTR>(this));
}

LRESULT CALLBACK SaveService::keyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    if (msg == WM_KEYDOWN && wp == 'S' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0 &&
        GetKeyState(VK_SHIFT) >= 0) {
        auto *self = reinterpret_cast<SaveService *>(data);
        slint::invoke_from_event_loop([self] { self->saveFromKeyboard(); });
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void SaveService::saveFromKeyboard()
{
    // The Config Editor has its own files and its own Save; route Ctrl+S there.
    const int page = ui_->global<AppState>().get_page();
    if (page == ui_->global<Pages>().get_editor())
        ui_->global<EditorState>().invoke_save();
    else if (page == ui_->global<Pages>().get_openwith())
        ui_->global<OpenWithState>().invoke_save(); // its own file, like the Config Editor
    else
        save();
}

void SaveService::reloadConfig()
{
    if (configLoading_) {
        configAgain_ = true; // a save landed meanwhile: read again once this one is in
        return;
    }
    configLoading_ = true;
    worker_.post([this] {
        auto t = queryTarget();
        std::shared_ptr<const nss::ConfigGraph> graph;
        if (t.ok) {
            nss::GraphOptions options;
            options.appDir = t.installDir;
            graph = std::make_shared<nss::ConfigGraph>(nss::ConfigGraph::load(t.root, options));
        }
        slint::invoke_from_event_loop([this, t, graph] {
            configLoading_ = false;
            target_ = t;
            graph_ = graph;
            overridesPath_ = t.ok ? (t.root.parent_path() / fs::path(widen(nss::OverridesWriter::relativePath)))
                                        .lexically_normal()
                                  : fs::path{};
            publish();
            if (configAgain_) {
                configAgain_ = false;
                reloadConfig();
                return;
            }
            for (auto &fn : config_)
                fn();
        });
    });
}

SaveService::Origins SaveService::origins(const std::string &key) const
{
    Origins o;
    if (!graph_)
        return o;
    const auto *ev = graph_->find(key);
    if (!ev)
        return o;
    const auto appKey = nss::pathKey(overridesPath_);
    for (const auto &origin : ev->chain) {
        if (nss::pathKey(origin.file) == appKey)
            o.app = &origin;
        else
            o.file = &origin;
    }
    return o;
}

std::optional<std::string> SaveService::current(const std::string &key) const
{
    auto o = origins(key);
    if (const auto *p = pending_.find(key)) {
        if (p->literal)
            return p->literal;
        return o.file ? std::optional<std::string>(o.file->value) : std::nullopt;
    }
    if (o.app)
        return o.app->value;
    if (o.file)
        return o.file->value;
    return std::nullopt;
}

std::string SaveService::where(const nss::ValueOrigin &o)
{
    return narrow(o.file.filename().wstring()) + ", line " + std::to_string(o.line);
}

std::string SaveService::originText(const std::string &key) const
{
    auto o = origins(key);
    const auto *pending = pending_.find(key);
    if (pending && pending->literal)
        return "Changed here · not saved yet";
    if (pending)
        return o.file ? "Back to " + where(*o.file) + " · not saved yet" : "Back to the built-in default · not saved yet";
    if (o.app)
        return o.file ? "Set by this app · " + where(*o.file) + " has " + o.file->value : "Set by this app";
    if (o.file)
        return (o.file->literal ? "From " : "") + where(*o.file);
    return "Built-in default";
}

std::vector<std::string> SaveService::set(std::string_view key, std::string_view literal, std::string_view page)
{
    // The value already saved (this app's, else the files'): nothing to change, so drop
    // a pending change instead of staging a no-op (e.g. clicking back to the theme in use).
    auto o = origins(std::string(key));
    const auto *saved = o.app ? o.app : o.file;
    const bool wasPending = pending_.find(key) != nullptr;
    auto errors = pending_.stage(key, literal, page, saved ? std::optional<std::string>(saved->value) : std::nullopt);
    if (errors.empty() && (wasPending || pending_.find(key)))
        notifyChanged();
    return errors;
}

void SaveService::reset(std::string_view key, std::string_view page)
{
    pending_.reset(key, page);
    notifyChanged();
}

void SaveService::forget(std::string_view key)
{
    pending_.forget(key);
    notifyChanged();
}

void SaveService::edit(const std::function<void(PendingChanges &)> &fn)
{
    if (saving_)
        return;
    fn(pending_);
    notifyChanged();
}

void SaveService::discard()
{
    if (saving_ || pending_.empty())
        return;
    pending_.discard();
    setResult("", "");
    notifyChanged();
}

void SaveService::notifyChanged()
{
    publish();
    for (auto &fn : changed_)
        fn();
}

void SaveService::publish()
{
    const auto &st = ui_->global<SaveState>();
    st.set_unsaved(int(pending_.count()));
    st.set_saving(saving_);
    st.set_config_path(ss(narrow(target_.root.wstring())));
    if (!pending_.empty() && st.get_result_kind() == "ok")
        setResult("", "");
}

void SaveService::setResult(const std::string &kind, const std::string &text)
{
    const auto &st = ui_->global<SaveState>();
    st.set_result_kind(ss(kind));
    st.set_result_text(ss(text));
    resultTimer_.stop();
    if (kind == "ok" || kind == "info")
        resultTimer_.start(slint::TimerMode::SingleShot, 6s, [this] {
            if (pending_.empty())
                setResult("", "");
        });
}

ConfigTarget SaveService::queryTarget() const
{
    ConfigTarget t;
    auto r = cli_.run({ L"-status" }, 10000);
    if (!r.started || r.timedOut) {
        t.error = r.started ? "shell.exe -status did not finish in time." : narrow(r.error);
        return t;
    }
    std::string err;
    auto status = parseStatus(r.output, err);
    if (!status) {
        t.error = err;
        return t;
    }
    t.root = fs::path(widen(status->configPath));
    t.source = status->configSource;
    t.installDir = fs::path(widen(status->installDir));
    if (t.installDir.empty())
        t.installDir = cli_.exe().parent_path();
    auto dll = status->activeDll();
    if (!dll.empty())
        t.logPath = logreader::logPathFor(fs::path(widen(status->activeLog())), fs::path(widen(dll)));
    std::error_code ec;
    if (t.root.empty() || !status->configExists || !fs::is_regular_file(t.root, ec)) {
        t.error = "The config file was not found. Create a personal config from Home.";
        return t;
    }
    t.ok = true;
    return t;
}

void SaveService::loadTarget(std::function<void(ConfigTarget)> done)
{
    worker_.post([this, done = std::move(done)] {
        auto t = queryTarget();
        slint::invoke_from_event_loop([this, t, done]() mutable {
            target_ = t;
            publish();
            done(std::move(t));
        });
    });
}

void SaveService::save()
{
    if (saving_ || pending_.empty())
        return;
    if (!cli_.available()) {
        const std::string text = "shell.exe was not found, so the config can't be checked before saving.";
        setResult("error", text);
        notifyResult({ false, false, text });
        return;
    }
    saving_ = true;
    ui_->global<SaveState>().set_saving_label("Checking with shell.exe…");
    setResult("", "");
    publish();
    worker_.post([this, snapshot = pending_] {
        auto outcome = runSave(snapshot);
        slint::invoke_from_event_loop([this, outcome = std::move(outcome)]() mutable { finish(std::move(outcome)); });
    });
}

SaveService::Outcome SaveService::runSave(const PendingChanges &snapshot) const
{
    Outcome out;
    out.saved = snapshot.items();
    out.target = queryTarget();
    if (!out.target.ok) {
        out.message = out.target.error;
        return out;
    }
    const auto &root = out.target.root;
    if (out.target.source == "install" || isProtectedLocation(root)) {
        out.needsUserConfig = true;
        return out;
    }

    auto plan = planSave(root, snapshot);
    if (!plan.ok) {
        out.message = plan.error;
        return out;
    }
    if (plan.writes.empty()) {
        out.ok = true;
        out.nothingToDo = true;
        return out;
    }

    nss::SaveOptions options;
    options.reason = snapshot.describe();
    options.validator = nss::makeShellCheckValidator(cli_.exe());
    const fs::path logPath = out.target.logPath;
    auto check = options.validator;
    options.verify = [check, logPath](const fs::path &realRoot) -> nss::CheckResult {
        auto r = check(realRoot);
        if (!r.ok)
            return r;
        // shell.dll reloads the config on the next menu; watch its log briefly in case
        // a menu opens meanwhile and the load fails there.
        std::error_code ec;
        if (logPath.empty() || !fs::exists(logPath, ec))
            return r;
        const std::string since = localTimestamp();
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(250ms);
            for (const auto &e : logreader::readTail(logPath, 20)) {
                if (e.time < since)
                    break; // newest first
                if (e.level == "error" && (!e.file.empty() || e.line > 0))
                    return nss::CheckResult{ false, e.message };
            }
        }
        return r;
    };

    nss::ConfigStore store(root);
    auto result = store.save(plan.writes, options);
    out.ok = result.ok;
    out.rolledBack = result.rolledBack;
    out.rejected = !result.check.ok;
    if (!result.ok) {
        std::string detail = result.check.message.empty() ? result.error : firstCheckError(result.check.message);
        if (!result.check.ok && !result.check.message.empty() && result.check.message.front() != '{')
            detail = result.check.message;
        out.message = detail.empty() ? result.error : detail;
    }
    return out;
}

void SaveService::finish(Outcome outcome)
{
    saving_ = false;
    if (outcome.target.ok)
        target_ = outcome.target;

    if (outcome.needsUserConfig) {
        ui_->global<SaveState>().set_user_config_prompt(true);
        publish();
        notifyResult({ false, true, {} });
        return;
    }
    std::string text;
    if (outcome.ok) {
        // Drop what was saved, unless it was edited again while saving.
        for (const auto &[key, change] : outcome.saved) {
            const auto *now = pending_.find(key);
            if (now && now->literal == change.literal)
                pending_.forget(key);
        }
        text = outcome.nothingToDo ? "Nothing to save · the files already match" : "Saved · Shell reloads on next right-click";
    } else if (outcome.rolledBack) {
        text = "Shell rejected the config, rolled back: " + outcome.message;
    } else if (outcome.rejected) {
        text = "Shell rejected the config, nothing was written: " + outcome.message;
    } else {
        text = "Not saved: " + outcome.message;
    }
    setResult(outcome.ok ? "ok" : "error", text);
    publish();
    for (auto &fn : changed_)
        fn();
    if (outcome.ok && !outcome.nothingToDo) {
        for (auto &fn : saved_)
            fn();
        reloadConfig();
    }
    notifyResult({ outcome.ok, false, text });
}

void SaveService::notifyResult(const Result &result)
{
    // A listener may add listeners; iterate over a copy.
    auto listeners = result_;
    for (auto &fn : listeners)
        fn(result);
}

void SaveService::createUserConfig()
{
    const auto &st = ui_->global<SaveState>();
    st.set_user_config_prompt(false);
    if (saving_)
        return;
    saving_ = true;
    st.set_saving_label("Creating your personal config…");
    publish();
    worker_.post([this] {
        auto r = cli_.run({ L"-config:init" }, 30000);
        bool ok = r.ok();
        std::string err;
        if (!ok) {
            // shell.exe prints {"error": "..."} on failure; show the message, not the JSON.
            auto j = nlohmann::json::parse(r.output, nullptr, false);
            if (j.is_object() && j.contains("error") && j["error"].is_string())
                err = j["error"].get<std::string>();
            else if (!r.started)
                err = narrow(r.error);
            else if (r.timedOut)
                err = "shell.exe did not finish in time.";
            else
                err = r.output.empty() ? "shell.exe exited with code " + std::to_string(long(r.exitCode)) + "."
                                       : r.output;
        }
        slint::invoke_from_event_loop([this, ok, err] {
            saving_ = false;
            ui_->global<AppState>().invoke_refresh(); // Home shows the new source
            if (!ok) {
                setResult("error", "Couldn't create the personal config: " + err);
                publish();
                notifyResult({ false, false, "Couldn't create the personal config: " + err });
                return;
            }
            publish();
            for (auto &fn : saved_)
                fn();
            reloadConfig(); // pages read the new root
            save();
        });
    });
}
