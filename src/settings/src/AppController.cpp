#include "AppController.h"

#include "Dwm.h"
#include "Registry.h"
#include "Text.h"

#include <nlohmann/json.hpp>

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using text::narrow;
using text::widen;

namespace {

slint::SharedString ss(const std::string &s) { return slint::SharedString(std::string_view(s)); }

bool fileExists(const std::string &utf8)
{
    std::error_code ec;
    return !utf8.empty() && fs::is_regular_file(fs::path(widen(utf8)), ec);
}

slint::Color rgb(uint32_t c)
{
    return slint::Color::from_rgb_uint8(uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c));
}

// The Windows UAC shield as a Slint image (stock icon, so it matches the OS).
slint::Image shieldIcon()
{
    SHSTOCKICONINFO sii{ sizeof sii };
    if (FAILED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_LARGEICON, &sii)) || !sii.hIcon)
        return {};
    slint::Image image;
    ICONINFO ii{};
    if (GetIconInfo(sii.hIcon, &ii)) {
        BITMAP bm{};
        if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
            const int w = bm.bmWidth, h = bm.bmHeight;
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            std::vector<uint8_t> bgra(size_t(w) * h * 4);
            HDC dc = GetDC(nullptr);
            int lines = GetDIBits(dc, ii.hbmColor, 0, UINT(h), bgra.data(), &bi, DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            if (lines == h) {
                slint::SharedPixelBuffer<slint::Rgba8Pixel> buf{ uint32_t(w), uint32_t(h) };
                auto *px = buf.begin();
                for (size_t i = 0; i < size_t(w) * h; ++i)
                    px[i] = { bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4 + 0], bgra[i * 4 + 3] };
                image = slint::Image(buf);
            }
        }
        if (ii.hbmColor)
            DeleteObject(ii.hbmColor);
        if (ii.hbmMask)
            DeleteObject(ii.hbmMask);
    }
    DestroyIcon(sii.hIcon);
    return image;
}

std::string describeFailure(const CliResult &r)
{
    if (!r.started)
        return narrow(r.error);
    if (r.timedOut)
        return "shell.exe did not finish in time and was stopped.";
    auto j = nlohmann::json::parse(r.output, nullptr, false);
    if (j.is_object()) {
        for (const char *key : { "error", "message" }) {
            auto it = j.find(key);
            if (it != j.end() && it->is_string())
                return it->get<std::string>();
        }
    }
    std::string out = r.output;
    while (!out.empty() && isspace(uint8_t(out.back())))
        out.pop_back();
    if (!out.empty() && out.size() < 300 && out.front() != '{')
        return out;
    return "shell.exe exited with code " + std::to_string(long(r.exitCode)) + ".";
}

StatusInfo toInfo(const ShellStatus &s)
{
    StatusInfo i{};
    i.loaded = true;
    i.version = ss(s.version);
    i.elevated = s.elevated;
    i.install_dir = ss(s.installDir);
    auto dll = s.activeDll();
    i.active_dll = ss(dll);
    i.active_dll_exists = fileExists(dll);
    i.reg_mode = ss(s.regMode);
    i.machine_registered = s.machineRegistered;
    i.user_registered = s.userRegistered;
    i.win11_applicable = s.win11Applicable;
    i.classic_menu = s.classicMenu;
    i.config_path = ss(s.configPath);
    i.config_source = ss(s.configSource);
    i.config_exists = s.configExists;
    i.user_config_dir = ss(s.userConfigDir);

    if (s.regMode == "user")
        i.reg_text = "Registered for you";
    else if (s.regMode == "machine")
        i.reg_text = "Registered for all users";
    else if (s.regMode == "both")
        i.reg_text = "Registered for you and for all users";
    else
        i.reg_text = "Not registered";

    const std::string &treatAs = s.userTreatAs != "absent" ? s.userTreatAs : s.machineTreatAs;
    if (!s.win11Applicable)
        i.win11_text = "Not applicable on this Windows version";
    else if (s.classicMenu)
        i.win11_text = treatAs == "foreign" ? "Classic menu (set by another app)" : "Classic menu (set by Shell)";
    else
        i.win11_text = "Windows 11 menu; Shell is under \"Show more options\"";

    if (s.configSource == "appdata")
        i.source_text = "Personal (%APPDATA%)";
    else if (s.configSource == "registry")
        i.source_text = "Custom path (registry)";
    else if (s.configSource == "install")
        i.source_text = "Shared (install folder)";
    else
        i.source_text = "Unknown";

    if (!s.conflictDlls().empty()) {
        i.health_kind = "error";
        i.health_title = "Two copies of Shell are registered";
        i.health_detail = "Explorer can load the other copy, and then Shell's menu doesn't appear.";
        i.pill_text = "Needs attention";
    } else if (s.regMode == "none") {
        i.health_kind = "error";
        i.health_title = "Shell is not enabled";
        i.health_detail = "The context menu isn't registered for you or for all users.";
        i.pill_text = "Not enabled";
    } else if (!i.active_dll_exists) {
        i.health_kind = "error";
        i.health_title = "shell.dll is missing";
        i.health_detail = "The registered DLL file can't be found. Register again to fix it.";
        i.pill_text = "Needs attention";
    } else if (!s.configExists) {
        i.health_kind = "error";
        i.health_title = "The config file is missing";
        i.health_detail = "Shell can't build its menu without shell.nss.";
        i.pill_text = "Needs attention";
    } else if (s.win11Applicable && s.classicMenu && treatAs == "foreign") {
        i.health_kind = "warning";
        i.health_title = "Shell is active";
        i.health_detail = "Another app set the Windows 11 classic-menu switch.";
        i.pill_text = "Shell is active";
    } else {
        i.health_kind = "ok";
        i.health_title = "Shell is active";
        i.health_detail = s.regMode == "user"    ? "Your context menu is powered by Nilesoft Shell."
                          : s.regMode == "both" ? "Active for you and for every user on this PC."
                                                : "Active for every user on this PC.";
        i.pill_text = "Shell is active";
    }
    return i;
}

std::vector<Issue> issuesFor(const ShellStatus &s, bool dllExists)
{
    std::vector<Issue> out;
    for (const auto &i : statusIssues(s, dllExists))
        out.push_back(Issue{ ss(i.kind), ss(i.title), ss(i.message), ss(i.action), ss(i.actionLabel) });
    return out;
}

// shell.exe in the install folder `-status` reports, else the one this app found.
ShellCli installCli(const ShellStatus *s, const ShellCli &fallback)
{
    if (s && !s->installDir.empty()) {
        auto exe = fs::path(widen(s->installDir)) / L"shell.exe";
        std::error_code ec;
        if (fs::is_regular_file(exe, ec))
            return ShellCli(exe);
    }
    return fallback;
}

} // namespace

AppController::AppController(slint::ComponentHandle<AppWindow> ui, AppOptions options)
    : ui_(std::move(ui)), options_(options), cli_(ShellCli::locate())
{
    const auto &state = ui_->global<AppState>();
    state.set_app_version(SETTINGS_VERSION);
    state.set_cli_available(cli_.available());
    state.set_theme_mode(int(registry::app::get(L"Theme").value_or(0) % 3));
    state.set_classic_menu_option(registry::app::get(L"ClassicMenu").value_or(1) != 0);
    state.set_shield_icon(shieldIcon());
    state.set_page(options_.startPage);
    bindCallbacks();
    applyTheme();
}

AppController::~AppController() = default;

void AppController::bindCallbacks()
{
    const auto &state = ui_->global<AppState>();
    state.on_refresh([this] { refresh(false); });
    state.on_action([this](slint::SharedString id) { runAction(std::string(id.data(), id.size())); });
    state.on_set_theme([this](int mode) { setTheme(mode); });
    state.on_open_url([](slint::SharedString url) {
        auto w = widen(std::string_view(url.data(), url.size()));
        if (w.rfind(L"https://", 0) == 0)
            ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    state.on_open_path([this](slint::SharedString path) { openPath(std::string(path.data(), path.size())); });
    // The first-run dialog's callbacks are in OnboardingController.
    state.on_dismiss_toast([this] {
        toastTimer_.stop();
        ui_->global<AppState>().set_toast_shown(false);
    });
}

void AppController::attachWindow(HWND hwnd)
{
    hwnd_ = hwnd;
    dwm::applyIcon(hwnd_);
    dwm::applyTitleBar(hwnd_, lastDark_);
    // Follow Windows' app mode and accent while "System" is selected.
    themePoll_.start(slint::TimerMode::Repeated, 2s, [this] {
        bool dark = !registry::appsUseLightTheme();
        auto accent = registry::accentColor();
        int mode = ui_->global<AppState>().get_theme_mode();
        bool effective = mode == 2 || (mode == 0 && dark);
        if (effective != lastDark_ || accent != lastAccent_)
            applyTheme();
    });
}

void AppController::start() { refresh(false); }

void AppController::applyTheme()
{
    const auto &state = ui_->global<AppState>();
    int mode = state.get_theme_mode();
    bool dark = mode == 2 || (mode == 0 && !registry::appsUseLightTheme());
    const auto &tokens = ui_->global<Tokens>();
    if (auto a = registry::accentFor(true))
        tokens.set_accent_on_dark(rgb(*a));
    if (auto a = registry::accentFor(false))
        tokens.set_accent_on_light(rgb(*a));
    lastAccent_ = registry::accentColor();
    lastDark_ = dark;
    ui_->invoke_apply_theme(dark);
    dwm::applyTitleBar(hwnd_, dark);
}

void AppController::setTheme(int mode)
{
    mode = std::clamp(mode, 0, 2);
    ui_->global<AppState>().set_theme_mode(mode);
    registry::app::set(L"Theme", DWORD(mode));
    applyTheme();
}

void AppController::setBusy(bool busy, const std::string &label)
{
    busy_ = busy;
    const auto &state = ui_->global<AppState>();
    state.set_busy(busy);
    state.set_busy_label(ss(label));
}

void AppController::showToast(const std::string &kind, const std::string &title, const std::string &body)
{
    const auto &state = ui_->global<AppState>();
    state.set_toast_kind(ss(kind));
    state.set_toast_title(ss(title));
    state.set_toast_text(ss(body));
    state.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

AppController::Snapshot AppController::collect() const
{
    Snapshot snap;
    CliResult r;
    if (!options_.statusFile.empty()) {
        // --status-json <file> (development): Home shows this instead of `shell.exe -status`.
        r.started = true;
        r.exitCode = 0;
        std::ifstream in(options_.statusFile, std::ios::binary);
        r.output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
        r = cli_.run({ L"-status" }, 10000);
    }
    if (!r.started || r.timedOut) {
        snap.error = describeFailure(r);
        return snap;
    }
    std::string err;
    snap.status = parseStatus(r.output, err);
    if (!snap.status) {
        snap.error = r.exitCode != 0 ? describeFailure(r) : err;
        return snap;
    }
    auto dll = snap.status->activeDll();
    if (!dll.empty()) {
        auto log = logreader::logPathFor(fs::path(widen(snap.status->activeLog())), fs::path(widen(dll)));
        std::error_code ec;
        if (fs::is_regular_file(log, ec)) {
            snap.logPath = narrow(log.wstring());
            snap.log = logreader::readTail(log);
        }
    }
    return snap;
}

void AppController::refresh(bool quiet)
{
    if (busy_)
        return;
    if (!cli_.available()) {
        const auto &state = ui_->global<AppState>();
        std::vector<Issue> issues{ Issue{ "error", "shell.exe was not found",
                                          "Install Nilesoft Shell, or put settings.exe next to shell.exe.", "", "" } };
        state.set_issues(std::make_shared<slint::VectorModel<Issue>>(issues));
        StatusInfo info{};
        info.health_kind = "error";
        info.health_title = "Nilesoft Shell was not found";
        info.health_detail = "settings.exe looks for shell.exe next to itself and next to the registered shell.dll.";
        info.pill_text = "Not found";
        info.loaded = true;
        state.set_status(info);
        return;
    }
    setBusy(true, "Checking status…");
    worker_.post([this, quiet] {
        auto snap = collect();
        slint::invoke_from_event_loop([this, quiet, snap = std::move(snap)]() mutable {
            setBusy(false);
            applySnapshot(std::move(snap), quiet);
        });
    });
}

void AppController::applySnapshot(Snapshot snap, bool quiet)
{
    const auto &state = ui_->global<AppState>();
    if (!snap.status) {
        std::vector<Issue> issues{ Issue{ "error", "Couldn't read the Shell status", ss(snap.error), "refresh", "Try again" } };
        state.set_issues(std::make_shared<slint::VectorModel<Issue>>(issues));
        if (!quiet)
            showToast("error", "Couldn't read the Shell status", snap.error);
        return;
    }
    status_ = snap.status;
    auto info = toInfo(*status_);
    state.set_status(info);
    state.set_issues(std::make_shared<slint::VectorModel<Issue>>(issuesFor(*status_, info.active_dll_exists)));

    std::vector<Diagnostic> diags;
    diags.reserve(snap.log.size());
    for (const auto &e : snap.log)
        diags.push_back(Diagnostic{ ss(e.level), ss(e.time), ss(e.message), ss(e.file), e.line, e.column });
    state.set_diagnostics(std::make_shared<slint::VectorModel<Diagnostic>>(diags));
    state.set_log_path(ss(snap.logPath));

    if (firstStatus_) {
        firstStatus_ = false;
        // A custom config path in the registry is the user's own config too.
        bool ownConfig = status_->configSource == "appdata" || status_->configSource == "registry";
        bool needsSetup = !ownConfig || status_->regMode == "none";
        bool dismissed = registry::app::get(L"OnboardingDismissed").value_or(0) != 0;
        if (options_.onboardingStep >= 0) {
            state.set_onboarding_step(std::clamp(options_.onboardingStep, 0, 2));
            state.set_onboarding_shown(true);
        } else if (needsSetup && !dismissed && !options_.noOnboarding) {
            state.set_onboarding_step(0);
            state.set_onboarding_shown(true);
        }
    }
}

void AppController::runAction(const std::string &id)
{
    if (busy_)
        return;
    if (id == "refresh") {
        refresh(false);
        return;
    }
    if (id == "open-config-folder") {
        std::string target = status_ ? status_->configPath : std::string{};
        if (target.empty() && status_)
            target = status_->userConfigDir;
        worker_.post([target] {
            fs::path p(widen(target));
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) {
                // Open the folder with the file selected.
                if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(p.c_str())) {
                    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                    ILFree(pidl);
                    return;
                }
            }
            auto dir = fs::is_directory(p, ec) ? p : p.parent_path();
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        return;
    }

    struct Spec
    {
        std::string label;
        std::vector<std::wstring> args;
        bool elevated = false;
        DWORD timeout = 30000;
        std::string done;
        std::string hint;
        bool restartAfter = false; // then `shell.exe -restart`
    };
    const bool treat = status_ && status_->win11Applicable &&
                       ui_->global<AppState>().get_classic_menu_option();
    Spec spec;
    if (id == "restart")
        spec = { "Restarting Explorer…", { L"-restart" }, false, 45000, "Explorer restarted", "" };
    else if (id == "register-user") {
        spec = { "Registering for you…", { L"-register", L"-user" }, false, 30000, "Shell is enabled for you",
                 "Restart Explorer to load it everywhere." };
        if (treat)
            spec.args.push_back(L"-treat");
    } else if (id == "unregister-user")
        spec = { "Unregistering…", { L"-unregister", L"-user" }, false, 30000, "Shell is disabled for you",
                 "Restart Explorer to unload it." };
    else if (id == "config-init")
        spec = { "Creating your config…", { L"-config:init" }, false, 30000, "Personal config created",
                 "Shell now reads it from %APPDATA%\\Nilesoft\\Shell." };
    else if (id == "register-machine") {
        spec = { "Waiting for administrator approval…", { L"-register" }, true, 120000,
                 "Registered for all users", "Restart Explorer to load it." };
        if (treat)
            spec.args.push_back(L"-treat");
    } else if (id == "unregister-machine")
        spec = { "Waiting for administrator approval…", { L"-unregister" }, true, 120000,
                 "Unregistered for all users", "Restart Explorer to unload it." };
    else if (id == "fix-conflicts") {
        // Register this copy for all users again, which takes the CLSIDs back from the
        // other copy, then restart Explorer so it drops the old dll.
        spec = { "Waiting for administrator approval…", { L"-register" }, true, 120000,
                 "Only this copy is registered now", "Explorer was restarted.", true };
        if (treat)
            spec.args.push_back(L"-treat");
    } else
        return;

    setBusy(true, spec.label);
    const auto &state = ui_->global<AppState>();
    state.set_action_id(ss(id));
    state.set_action_error("");
    HWND owner = hwnd_;
    // The fix runs the shell.exe of the install folder -status reports.
    ShellCli cli = id == "fix-conflicts" ? installCli(status_ ? &*status_ : nullptr, cli_) : cli_;
    worker_.post([this, spec, owner, cli, id] {
        CliResult r = spec.elevated ? cli.runElevated(spec.args, owner, spec.timeout) : cli.run(spec.args, spec.timeout);
        std::string failure = r.ok() ? std::string{} : describeFailure(r);
        bool cancelled = r.cancelled;
        if (failure.empty() && spec.restartAfter) {
            slint::invoke_from_event_loop([this] { setBusy(true, "Restarting Explorer…"); });
            auto restart = cli.run({ L"-restart" }, 45000);
            if (!restart.ok())
                failure = "Registered, but Explorer didn't restart: " + describeFailure(restart);
        }
        auto snap = collect();
        // The other copy can still be there (it re-registers itself, or the core is older).
        if (failure.empty() && id == "fix-conflicts" && snap.status) {
            auto others = snap.status->conflictDlls();
            if (!others.empty())
                failure = "Another copy is still registered: " + others[0] +
                          ". Uninstall it from Settings > Apps, then try again.";
        }
        slint::invoke_from_event_loop([this, spec, failure, cancelled, snap = std::move(snap)]() mutable {
            setBusy(false);
            const auto &st = ui_->global<AppState>();
            if (cancelled) {
                st.set_action_error("The administrator prompt was cancelled. Nothing was changed.");
                showToast("info", "Cancelled", failure);
            } else if (failure.empty()) {
                showToast("ok", spec.done, spec.hint);
            } else {
                st.set_action_error(ss(failure));
                showToast("error", "That didn't work", failure);
            }
            bool configChanged = failure.empty() && !cancelled && st.get_action_id() == "config-init";
            applySnapshot(std::move(snap), true);
            if (configChanged)
                for (auto &fn : configChanged_)
                    fn();
        });
    });
}

void AppController::openPath(const std::string &path)
{
    if (path.empty())
        return;
    auto w = widen(path);
    worker_.post([w] {
        auto rc = INT_PTR(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (rc == SE_ERR_NOASSOC || rc == SE_ERR_ASSOCINCOMPLETE) {
            // .nss and .log may have no handler; fall back to Notepad.
            auto quoted = ShellCli::quote(w);
            ShellExecuteW(nullptr, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
        }
    });
}

void AppController::saveWindowPlacement()
{
    const auto &win = ui_->window();
    registry::app::Placement p;
    p.maximized = win.is_maximized();
    auto pos = win.position();
    auto size = win.size();
    p.x = pos.x;
    p.y = pos.y;
    p.width = int(size.width);
    p.height = int(size.height);
    bool minimized = hwnd_ && IsIconic(hwnd_);
    registry::app::savePlacement(p, p.maximized || minimized);
}
