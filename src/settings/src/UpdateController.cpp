#include "UpdateController.h"

#include "Registry.h"
#include "ShellCli.h"
#include "Status.h"
#include "Text.h"

#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;
using text::narrow;
using text::widen;

namespace {

slint::SharedString ss(const std::string &s) { return slint::SharedString(std::string_view(s)); }

constexpr std::time_t kDay = 24 * 60 * 60;

fs::path selfPath()
{
    std::wstring buf(MAX_PATH, L'\0');
    while (true) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

fs::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p)
        out = p;
    CoTaskMemFree(p);
    return out;
}

fs::path tempDir()
{
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return n ? fs::path(std::wstring(buf, n)) : fs::path();
}

constexpr const wchar_t *kHelperPrefix = L"NilesoftShellPro-update-";

// The helper runs from a copy in %TEMP%, so the installed exe can be replaced.
fs::path copySelfToTemp(std::string &error)
{
    auto self = selfPath();
    auto dir = tempDir();
    if (self.empty() || dir.empty()) {
        error = "Couldn't find this app's own file or the temporary folder.";
        return {};
    }
    auto copy = dir / (std::wstring(kHelperPrefix) + std::to_wstring(GetCurrentProcessId()) + L".exe");
    if (!CopyFileW(self.c_str(), copy.c_str(), FALSE)) {
        error = "Couldn't copy the app to the temporary folder.";
        return {};
    }
    return copy;
}

// Helper copies left by earlier updates.
void cleanupHelpers()
{
    auto dir = tempDir();
    std::error_code ec;
    if (dir.empty())
        return;
    const std::wstring mine = std::wstring(kHelperPrefix) + std::to_wstring(GetCurrentProcessId()) + L".exe";
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        auto name = e.path().filename().wstring();
        if (name.rfind(kHelperPrefix, 0) == 0 && e.path().extension() == L".exe" && _wcsicmp(name.c_str(), mine.c_str()) != 0)
            DeleteFileW(e.path().c_str());
    }
}

struct Detected
{
    updater::InstallType type = updater::InstallType::Portable;
    fs::path installDir;
};

// MSI or portable, from where this exe is and where the all-users registration points.
Detected detectInstall()
{
    Detected d;
    auto self = selfPath();
    d.installDir = self.parent_path();
    std::string machineDll;
    ShellCli cli(ShellCli::locate());
    if (cli.available()) {
        auto r = cli.run({ L"-status" }, 10000);
        std::string err;
        if (r.ok())
            if (auto s = parseStatus(r.output, err); s && s->machineRegistered)
                machineDll = s->machineDll;
    }
    d.type = updater::detectInstallType(self, knownFolder(FOLDERID_ProgramFiles), machineDll);
    return d;
}

std::string formatSize(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(buf, sizeof buf, "%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    return buf;
}

const char *kMonths[] = { "January", "February", "March",     "April",   "May",      "June",
                          "July",    "August",   "September", "October", "November", "December" };

// "2026-10-14T09:00:00Z" -> "14 October 2026"
std::string formatDate(const std::string &iso)
{
    int y = 0, m = 0, d = 0;
    if (sscanf_s(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12)
        return {};
    return std::to_string(d) + " " + kMonths[m - 1] + " " + std::to_string(y);
}

std::string formatChecked(std::time_t t)
{
    if (t <= 0)
        return "Never";
    std::tm when{}, now{};
    std::time_t n = std::time(nullptr);
    localtime_s(&when, &t);
    localtime_s(&now, &n);
    char hm[16];
    std::snprintf(hm, sizeof hm, "%02d:%02d", when.tm_hour, when.tm_min);
    std::tm yesterday = now;
    yesterday.tm_mday -= 1;
    std::mktime(&yesterday);
    if (when.tm_year == now.tm_year && when.tm_yday == now.tm_yday)
        return std::string("Today at ") + hm;
    if (when.tm_year == yesterday.tm_year && when.tm_yday == yesterday.tm_yday)
        return std::string("Yesterday at ") + hm;
    return std::to_string(when.tm_mday) + " " + kMonths[when.tm_mon] + " " + std::to_string(when.tm_year + 1900);
}

bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

bool startProcess(const fs::path &exe, const std::wstring &args)
{
    std::wstring cmd = ShellCli::quote(exe.wstring()) + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si,
                        &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Starts a program as the signed-in user from an elevated process, through Explorer's
// desktop (IShellDispatch2::ShellExecute runs in Explorer, unelevated).
bool startUnelevated(const fs::path &exe, const std::wstring &args)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IShellWindows> windows;
        ComPtr<IDispatch> desktop;
        ComPtr<IServiceProvider> sp;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IDispatch> background;
        ComPtr<IShellFolderViewDual> folderView;
        ComPtr<IDispatch> app;
        ComPtr<IShellDispatch2> shell;
        VARIANT empty{};
        long hwnd = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))) &&
            windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop) == S_OK &&
            desktop && SUCCEEDED(desktop.As(&sp)) &&
            SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
            SUCCEEDED(browser->QueryActiveShellView(&view)) &&
            SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) &&
            SUCCEEDED(background.As(&folderView)) && SUCCEEDED(folderView->get_Application(&app)) &&
            SUCCEEDED(app.As(&shell))) {
            BSTR file = SysAllocString(exe.c_str());
            VARIANT vArgs{}, vDir{}, vOp{}, vShow{};
            vArgs.vt = VT_BSTR;
            vArgs.bstrVal = SysAllocString(args.c_str());
            vDir.vt = VT_BSTR;
            vDir.bstrVal = SysAllocString(exe.parent_path().c_str());
            vOp.vt = VT_BSTR;
            vOp.bstrVal = SysAllocString(L"open");
            vShow.vt = VT_I4;
            vShow.lVal = SW_SHOWNORMAL;
            ok = SUCCEEDED(shell->ShellExecute(file, vArgs, vDir, vOp, vShow));
            SysFreeString(file);
            VariantClear(&vArgs);
            VariantClear(&vDir);
            VariantClear(&vOp);
        }
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    return ok;
}

// Relaunches the app; from an elevated helper, as the signed-in user. Explorer may be
// restarting (the MSI restarts it), so this retries for a while.
void relaunch(const fs::path &exe, const std::wstring &args)
{
    if (!isElevated()) {
        startProcess(exe, args);
        return;
    }
    for (int i = 0; i < 30; ++i) {
        if (startUnelevated(exe, args))
            return;
        Sleep(1000);
    }
    startProcess(exe, args); // no desktop to go through: start it as is
}

void waitForExit(DWORD pid)
{
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(h, 60000);
        CloseHandle(h);
    }
}

} // namespace

// --- the helper ---------------------------------------------------------------------------

int runUpdateHelper()
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return -1;
    struct Free
    {
        LPWSTR *a;
        ~Free() { LocalFree(a); }
    } free{ argv };
    if (argc < 5)
        return -1;
    const bool portable = _wcsicmp(argv[1], L"--apply-update") == 0;
    const bool msi = _wcsicmp(argv[1], L"--apply-msi") == 0;
    if (!portable && !msi)
        return -1;

    const fs::path source = argv[2];
    const fs::path installDir = argv[3];
    const DWORD pid = wcstoul(argv[4], nullptr, 10);
    const fs::path app = installDir / L"NilesoftShellPro.exe";
    waitForExit(pid);

    if (portable) {
        auto r = updater::applyPortable(source, installDir);
        std::error_code ec;
        fs::remove_all(source, ec);
        relaunch(app, r.ok ? L"--updated portable" : L"--update-failed " + ShellCli::quote(widen(r.error)));
        return r.ok ? 0 : 1;
    }

    // MSI: this copy runs elevated (the app asked for it), so msiexec needs no prompt of its own.
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const fs::path msiexec = fs::path(sys) / L"msiexec.exe";
    std::wstring cmd = ShellCli::quote(msiexec.wstring()) + L" /i " + ShellCli::quote(source.wstring()) +
                       L" /passive /norestart /l*v " + ShellCli::quote(source.wstring() + L".log");
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    DWORD code = DWORD(-1);
    if (CreateProcessW(msiexec.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    std::wstring args;
    if (code == ERROR_SUCCESS || code == ERROR_SUCCESS_REBOOT_REQUIRED || code == ERROR_SUCCESS_REBOOT_INITIATED)
        args = L"--updated msi";
    else if (code == ERROR_INSTALL_USEREXIT)
        args = L"--update-cancelled";
    else if (code == DWORD(-1))
        args = L"--update-failed " + ShellCli::quote(L"msiexec.exe couldn't be started.");
    else if (code == ERROR_INSTALL_ALREADY_RUNNING)
        args = L"--update-failed " + ShellCli::quote(L"Another installation is in progress. Try again when it has finished.");
    else
        args = L"--update-failed " + ShellCli::quote(L"The installer stopped with code " + std::to_wstring(code) +
                                                     L". Its log is " + source.wstring() + L".log.");
    relaunch(app, args);
    return code == ERROR_SUCCESS ? 0 : 1;
}

// --- the controller -------------------------------------------------------------------------

UpdateController::UpdateController(slint::ComponentHandle<AppWindow> ui, UpdateOptions options,
                                   std::function<void()> quit)
    : ui_(std::move(ui)), options_(std::move(options)), quit_(std::move(quit))
{
    current_ = updater::Version::parse(SETTINGS_VERSION).value_or(updater::Version{});
    auto ch = registry::app::get(L"UpdateChannel");
    channel_ = ch ? (*ch == 1 ? updater::Channel::Beta : updater::Channel::Stable) : updater::defaultChannel(current_);
    autoCheck_ = registry::app::get(L"UpdateAutoCheck").value_or(1) != 0;
    autoInstall_ = registry::app::get(L"UpdateAutoInstall").value_or(0) != 0;
    skipped_ = narrow(registry::app::getString(L"UpdateSkipVersion").value_or(L""));
    lastCheck_ = std::time_t(registry::app::get(L"UpdateLastCheck").value_or(0));

    const auto &st = ui_->global<UpdateState>();
    st.set_current_version(ss(current_.str()));
    st.set_channel(channel_ == updater::Channel::Beta ? 1 : 0);
    st.set_auto_check(autoCheck_);
    st.set_auto_install(autoInstall_);
    showLastChecked();
    bindCallbacks();
}

UpdateController::~UpdateController() { cancel_ = true; }

void UpdateController::bindCallbacks()
{
    const auto &st = ui_->global<UpdateState>();
    st.on_check_now([this] { check(true); });
    st.on_install([this] { install(); });
    st.on_skip([this] { skip(); });
    st.on_cancel([this] { cancel_ = true; });
    st.on_open_notes([this] {
        if (!found_)
            return;
        auto url = widen(found_->htmlUrl);
        if (url.rfind(L"https://github.com/", 0) == 0)
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    st.on_set_channel([this](int c) {
        channel_ = c == 1 ? updater::Channel::Beta : updater::Channel::Stable;
        ui_->global<UpdateState>().set_channel(c == 1 ? 1 : 0);
        if (options_.persist)
            registry::app::set(L"UpdateChannel", c == 1 ? 1 : 0);
        if (!working_ && !releases_.empty())
            reselect();
    });
    st.on_set_auto_check([this](bool on) {
        autoCheck_ = on;
        ui_->global<UpdateState>().set_auto_check(on);
        if (options_.persist)
            registry::app::set(L"UpdateAutoCheck", on ? 1 : 0);
    });
    st.on_set_auto_install([this](bool on) {
        autoInstall_ = on;
        ui_->global<UpdateState>().set_auto_install(on);
        if (options_.persist)
            registry::app::set(L"UpdateAutoInstall", on ? 1 : 0);
        if (on && found_ && !working_ && ready_.empty())
            download(false, true);
    });
    st.on_dismiss_notice([this] { ui_->global<UpdateState>().set_notice_shown(false); });
}

void UpdateController::start()
{
    const auto current = current_;
    const auto dir = selfPath().parent_path();
    worker_.post([current, dir] {
        updater::cleanupDownloads(current);
        updater::cleanupOldFiles(dir);
        cleanupHelpers();
    });

    // What the helper that just ran has to say.
    if (options_.updated == L"portable")
        notice("ok", "Updated to " + current_.str(),
               "Restart Explorer to load the new shell.dll; until then it keeps using the old one.", "restart");
    else if (options_.updated == L"msi")
        notice("ok", "Updated to " + current_.str(), "Nilesoft Shell Pro " + current_.str() + " is installed.", "");
    else if (!options_.updateFailed.empty())
        notice("error", "The update wasn't installed", narrow(options_.updateFailed), "");
    else if (options_.updateCancelled)
        notice("info", "The update was cancelled", "Nothing was changed.", "");

    auto due = [this] {
        std::time_t now = std::time(nullptr);
        return lastCheck_ <= 0 || lastCheck_ > now || now - lastCheck_ >= kDay;
    };
    if (options_.checkNow) {
        startTimer_.start(slint::TimerMode::SingleShot, 300ms, [this] { check(true); });
    } else if (options_.allowAutoCheck && autoCheck_ && due()) {
        startTimer_.start(slint::TimerMode::SingleShot, 5s, [this] { check(false); });
    }
    // The app can stay open for days: look again once a day.
    if (options_.allowAutoCheck)
        dailyTimer_.start(slint::TimerMode::Repeated, 1h, [this, due] {
            if (autoCheck_ && due() && !working_ && ready_.empty())
                check(false);
        });
}

void UpdateController::setState(const std::string &state, const std::string &message, const std::string &kind)
{
    const auto &st = ui_->global<UpdateState>();
    st.set_state(ss(state));
    st.set_message(ss(message));
    st.set_message_kind(ss(kind));
}

void UpdateController::notice(const std::string &kind, const std::string &title, const std::string &body,
                              const std::string &action)
{
    const auto &st = ui_->global<UpdateState>();
    st.set_notice_kind(ss(kind));
    st.set_notice_title(ss(title));
    st.set_notice_text(ss(body));
    st.set_notice_action(ss(action));
    st.set_notice_shown(true);
}

void UpdateController::showLastChecked()
{
    ui_->global<UpdateState>().set_last_checked(ss(formatChecked(lastCheck_)));
}

void UpdateController::showRelease()
{
    const auto &st = ui_->global<UpdateState>();
    st.set_install_kind(installType_ == updater::InstallType::Msi ? "msi" : "portable");
    st.set_needs_admin(installType_ == updater::InstallType::Msi);
    if (!found_) {
        st.set_latest_version("");
        st.set_released("");
        st.set_asset_size("");
        return;
    }
    st.set_latest_version(ss(found_->version.str()));
    st.set_released(ss(formatDate(found_->publishedAt)));
    const auto *asset = updater::chooseAsset(*found_, installType_, updater::nativeArch());
    st.set_asset_size(ss(asset && asset->size ? formatSize(asset->size) : std::string{}));
}

void UpdateController::check(bool manual)
{
    if (working_)
        return;
    working_ = true;
    setState("checking", "Checking for updates…");
    auto feed = updater::feedOverride();
    worker_.post([this, manual, feed] {
        auto detected = detectInstall();
        auto result = updater::fetchReleases(feed);
        slint::invoke_from_event_loop([this, manual, detected, result = std::move(result)]() mutable {
            working_ = false;
            installType_ = detected.type;
            installDir_ = detected.installDir;
            applyCheck(std::move(result), detected.type, manual);
        });
    });
}

void UpdateController::applyCheck(updater::FeedResult feed, updater::InstallType, bool manual)
{
    if (!feed.ok) {
        // Automatic checks fail quietly and try again next time.
        if (!manual) {
            setState("idle");
            return;
        }
        std::string msg = feed.offline       ? "You seem to be offline, so updates couldn't be checked."
                          : feed.rateLimited ? "GitHub's limit for update checks was reached. Try again in an hour."
                                             : "Couldn't check for updates: " + feed.error;
        setState("error", msg, "error");
        return;
    }
    lastCheck_ = std::time(nullptr);
    if (options_.persist && !feed.local)
        registry::app::set(L"UpdateLastCheck", DWORD(lastCheck_));
    showLastChecked();
    releases_ = std::move(feed.releases);
    localFeed_ = feed.local;
    reselect();
    if (found_ && autoInstall_ && ready_.empty())
        download(false, true);
}

void UpdateController::reselect()
{
    auto next = updater::selectUpdate(releases_, current_, channel_, skipped_);
    if (!next || !found_ || found_->version.str() != next->version.str()) {
        ready_.clear();
        readyVersion_.clear();
    }
    found_ = std::move(next);
    showRelease();
    if (!found_) {
        setState("up-to-date", "You have the latest version.");
        return;
    }
    if (!updater::chooseAsset(*found_, installType_, updater::nativeArch()))
        setState("available", "This release has no download for this kind of install. Get it from the release page.",
                 "error");
    else if (!ready_.empty())
        setState("ready");
    else
        setState("available");
}

void UpdateController::download(bool thenInstall, bool quiet)
{
    if (working_ || !found_)
        return;
    const auto *asset = updater::chooseAsset(*found_, installType_, updater::nativeArch());
    if (!asset) {
        setState("available", "This release has no download for this kind of install. Get it from the release page.",
                 "error");
        return;
    }
    auto root = updater::updatesRoot();
    if (root.empty()) {
        setState("available", "Couldn't find %LOCALAPPDATA%.", "error");
        return;
    }
    working_ = true;
    cancel_ = false;
    const auto &st = ui_->global<UpdateState>();
    st.set_progress(0);
    st.set_progress_text(ss(asset->size ? "0 of " + formatSize(asset->size) : std::string("Starting…")));
    setState("downloading");

    auto release = *found_;
    auto chosen = *asset;
    auto dir = root / widen(release.version.str());
    const bool portable = installType_ == updater::InstallType::Portable;
    const bool allowLocal = localFeed_;
    worker_.post([this, release, chosen, dir, portable, allowLocal, thenInstall, quiet] {
        auto last = std::chrono::steady_clock::now() - 1s;
        auto progress = [this, &last](uint64_t done, uint64_t total) {
            auto now = std::chrono::steady_clock::now();
            if (now - last >= 100ms || (total && done == total)) {
                last = now;
                float f = total ? float(double(done) / double(total)) : 0.f;
                std::string label = total ? formatSize(done) + " of " + formatSize(total) : formatSize(done);
                slint::invoke_from_event_loop([this, f, label] {
                    const auto &s = ui_->global<UpdateState>();
                    s.set_progress(f);
                    s.set_progress_text(ss(label));
                });
            }
            return !cancel_.load();
        };
        auto r = updater::downloadAndVerify(release, chosen, dir, progress, allowLocal);
        fs::path ready;
        std::string error = r.error;
        if (r.ok && portable) {
            auto staging = dir / L"staging";
            std::error_code ec;
            fs::remove_all(staging, ec);
            if (updater::extractZip(r.file, staging, error))
                ready = staging;
        } else if (r.ok) {
            ready = r.file;
        }
        const bool cancelled = r.cancelled;
        const std::string version = release.version.str();
        slint::invoke_from_event_loop([this, ready, error, cancelled, version, thenInstall, quiet] {
            working_ = false;
            if (cancelled) {
                setState("available", "Download cancelled.");
                return;
            }
            if (ready.empty()) {
                setState("available", quiet ? "The update couldn't be downloaded in the background: " + error : error,
                         "error");
                return;
            }
            ready_ = ready;
            readyVersion_ = version;
            setState("ready");
            if (thenInstall)
                handOver();
        });
    });
}

void UpdateController::install()
{
    if (working_ || !found_)
        return;
    if (ui_->global<SaveState>().get_unsaved() > 0) {
        setState(ready_.empty() ? "available" : "ready",
                 "Save or discard your changes first: the app closes to install the update.", "error");
        return;
    }
    if (!ready_.empty() && readyVersion_ == found_->version.str())
        handOver();
    else
        download(true, false);
}

void UpdateController::handOver()
{
    if (ui_->global<SaveState>().get_unsaved() > 0) {
        setState("ready", "Save or discard your changes first: the app closes to install the update.", "error");
        return;
    }
    std::string error;
    auto helper = copySelfToTemp(error);
    if (helper.empty()) {
        setState("ready", error, "error");
        return;
    }
    const std::wstring args = ShellCli::quote(ready_.wstring()) + L" " + ShellCli::quote(installDir_.wstring()) + L" " +
                              std::to_wstring(GetCurrentProcessId());

    if (installType_ == updater::InstallType::Portable) {
        STARTUPINFOW si{ sizeof si };
        PROCESS_INFORMATION pi{};
        std::wstring cmd = ShellCli::quote(helper.wstring()) + L" --apply-update " + args;
        if (!CreateProcessW(helper.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            helper.parent_path().c_str(), &si, &pi)) {
            DeleteFileW(helper.c_str());
            setState("ready", "Couldn't start the update helper.", "error");
            return;
        }
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        setState("installing", "Installing the update…");
        quit_();
        return;
    }

    // MSI: one administrator prompt, for the helper that runs msiexec once this app has closed.
    working_ = true;
    setState("installing", "Waiting for administrator approval…");
    HWND owner = hwnd_;
    const std::wstring params = L"--apply-msi " + args;
    worker_.post([this, helper, params, owner] {
        SHELLEXECUTEINFOW sei{ sizeof sei };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.hwnd = owner;
        sei.lpVerb = L"runas";
        sei.lpFile = helper.c_str();
        sei.lpParameters = params.c_str();
        auto dir = helper.parent_path().wstring();
        sei.lpDirectory = dir.c_str();
        sei.nShow = SW_HIDE;
        bool ok = ShellExecuteExW(&sei) != FALSE;
        DWORD err = ok ? 0 : GetLastError();
        if (ok && sei.hProcess) {
            AllowSetForegroundWindow(GetProcessId(sei.hProcess));
            CloseHandle(sei.hProcess);
        }
        if (!ok)
            DeleteFileW(helper.c_str());
        slint::invoke_from_event_loop([this, ok, err] {
            working_ = false;
            if (ok) {
                setState("installing", "Installing the update…");
                quit_();
            } else if (err == ERROR_CANCELLED) {
                setState("ready", "The administrator prompt was cancelled. Nothing was changed.", "info");
            } else {
                setState("ready", "Couldn't start the installer (error " + std::to_string(err) + ").", "error");
            }
        });
    });
}

void UpdateController::skip()
{
    if (!found_ || working_)
        return;
    skipped_ = found_->version.str();
    if (options_.persist)
        registry::app::setString(L"UpdateSkipVersion", widen(skipped_));
    auto version = skipped_;
    found_.reset();
    ready_.clear();
    readyVersion_.clear();
    showRelease();
    setState("up-to-date", "Version " + version + " is skipped. Newer versions will still show up.");
}
