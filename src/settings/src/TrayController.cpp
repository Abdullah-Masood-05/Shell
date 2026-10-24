#include "TrayController.h"

#include "AppChannel.h"
#include "Registry.h"
#include "Text.h"
#include "Tr.h"

#include <windows.h>

#include <chrono>
#include <ctime>

using namespace std::chrono_literals;
using text::narrow;
using text::widen;

namespace {

constexpr const wchar_t *kNotified = L"TrayNotifiedVersion";

std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }

bool offered(const std::string &state)
{
    return state == "available" || state == "downloading" || state == "ready" || state == "installing";
}

// Like Windows does for a minimised window: the pages go back to the system and come
// back when they are used again. While the app sits in the tray that is most of them.
void trimWorkingSet() { SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1)); }

} // namespace

TrayController::TrayController(slint::ComponentHandle<AppWindow> ui, UpdateController &updates, Hooks hooks, bool persist)
    : ui_(std::move(ui)), updates_(updates), hooks_(std::move(hooks)), persist_(persist)
{
    prefs_ = startup::load(store_);
    pushPrefs();
    bindCallbacks();
}

TrayController::~TrayController() = default;

std::wstring TrayController::exePath() const { return appchannel::launcherPath().wstring(); }

void TrayController::pushPrefs()
{
    const auto &st = ui_->global<TrayState>();
    st.set_keep_in_tray(prefs_.keepInTray);
    st.set_start_with_windows(prefs_.startsWithWindows());
    st.set_off_in_windows(prefs_.keepInTray && prefs_.startWithWindows && prefs_.disabledInWindows);
}

void TrayController::bindCallbacks()
{
    const auto &st = ui_->global<TrayState>();
    st.on_set_keep_in_tray([this](bool on) {
        if (persist_)
            startup::setKeepInTray(store_, exePath(), on);
        prefs_ = startup::load(store_);
        if (!persist_)
            prefs_.keepInTray = on;
        if (icon_)
            icon_->setVisible(prefs_.keepInTray);
        pushPrefs();
    });
    st.on_set_start_with_windows([this](bool on) {
        if (persist_)
            startup::setStartWithWindows(store_, exePath(), on);
        prefs_ = startup::load(store_);
        if (!persist_)
            prefs_.startWithWindows = on;
        pushPrefs();
    });
}

bool TrayController::start(bool background)
{
    background_ = background;
    if (persist_)
        startup::onStartup(store_, exePath(), appchannel::appRoot().wstring());
    prefs_ = startup::load(store_);
    pushPrefs();

    // The icon's messages arrive inside the message loop; the UI is changed from Slint's
    // own turn of it.
    TrayIcon::Handlers h;
    h.open = [this] { slint::invoke_from_event_loop([this] { hooks_.show(-1); }); };
    h.balloonClicked = [this] { slint::invoke_from_event_loop([this] { hooks_.show(0); }); }; // Home: the update banner
    h.command = [this](TrayIcon::Command c) { slint::invoke_from_event_loop([this, c] { command(c); }); };
    h.label = [this](TrayIcon::Command c) { return label(c); };
    h.quit = [this] { slint::invoke_from_event_loop([this] { hooks_.quit(); }); };
    icon_ = std::make_unique<TrayIcon>(std::move(h));
    // The title says which version runs: a newer copy that starts asks this one to quit.
    if (!icon_->create(appchannel::instanceTitle(updater::Version::parse(SETTINGS_VERSION).value_or(updater::Version{})))) {
        icon_.reset();
        return false;
    }
    icon_->setVisible(prefs_.keepInTray);

    // Update checks while the app runs: a look at the clock every half hour (a timer, no
    // polling in between), and a first one soon after a start with Windows.
    schedule_.start(slint::TimerMode::Repeated, background::kTick, [this] { tick(); });
    if (background) {
        firstCheck_.start(slint::TimerMode::SingleShot, background::kFirstCheck, [this] { tick(); });
        trim_.start(slint::TimerMode::SingleShot, 20s, [] { trimWorkingSet(); });
    }
    return true;
}

void TrayController::windowHidden()
{
    trim_.start(slint::TimerMode::SingleShot, 3s, [] { trimWorkingSet(); });
}

std::wstring TrayController::label(TrayIcon::Command c) const
{
    switch (c) {
    case TrayIcon::Command::Open:
        return widen(nsp::tr("Open Nilesoft Shell Pro"));
    case TrayIcon::Command::CheckUpdates:
        return widen(nsp::tr("Check for updates"));
    case TrayIcon::Command::RestartExplorer:
        return widen(nsp::tr("Restart Explorer"));
    case TrayIcon::Command::Quit:
        return widen(nsp::tr("Quit"));
    }
    return {};
}

void TrayController::command(TrayIcon::Command c)
{
    switch (c) {
    case TrayIcon::Command::Open:
        hooks_.show(-1);
        break;
    case TrayIcon::Command::CheckUpdates:
        check(true);
        break;
    case TrayIcon::Command::RestartExplorer:
        // The same as the button on Home (shell.exe -restart).
        ui_->global<AppState>().invoke_action("restart");
        break;
    case TrayIcon::Command::Quit:
        hooks_.quit();
        break;
    }
}

void TrayController::tick()
{
    if (!icon_ || !icon_->visible())
        return; // no tray: UpdateController's own checks while the window is open
    const std::string state = updates_.summary().state;
    if (state == "checking") {
        watchCheck(false);
        return;
    }
    if (offered(state))
        afterCheck(false); // found by UpdateController's own check
    if (state == "downloading" || state == "ready" || state == "installing" || !ui_->global<UpdateState>().get_auto_check())
        return;
    const std::time_t last = std::time_t(registry::app::get(L"UpdateLastCheck").value_or(0));
    if (background::checkDue(std::time(nullptr), last))
        check(false);
}

void TrayController::check(bool manual)
{
    manualPending_ = manualPending_ || manual;
    const std::string state = updates_.summary().state;
    // An automatic check is like the daily one (quiet failures, the automatic download
    // and install of an update that needs no administrator rights); Check for updates in
    // the tray menu is like Check now.
    if (state != "checking" && state != "downloading" && state != "installing")
        updates_.checkForUpdates(manual);
    watchCheck(manual);
}

// The check runs on UpdateController's worker; its result shows in summary().state.
// Looked at twice a second while it runs (a few seconds), for two minutes at most.
void TrayController::watchCheck(bool)
{
    watchTicks_ = 0;
    if (watch_.running())
        return;
    watch_.start(slint::TimerMode::Repeated, 500ms, [this] {
        const std::string state = updates_.summary().state;
        if (state == "checking" && ++watchTicks_ < 240)
            return;
        watch_.stop();
        const bool manual = manualPending_;
        manualPending_ = false;
        afterCheck(manual);
    });
}

void TrayController::afterCheck(bool manual)
{
    if (!icon_)
        return;
    const UpdateSummary sum = updates_.summary();
    if (offered(sum.state) && !sum.version.empty()) {
        const std::string &version = sum.version;
        const std::string last = narrow(registry::app::getString(kNotified).value_or(L""));
        const std::string skipped = narrow(registry::app::getString(L"UpdateSkipVersion").value_or(L""));
        if (!manual && !background::shouldNotify(version, last, skipped))
            return;
        // With the window open, the banner on Home says it already.
        const bool windowOpen = ui_->window().is_visible() && !manual;
        // A core update is only installed with the user's click on Home (it asks for
        // administrator approval there); the notification says so up front.
        const std::string body =
            sum.needsElevation
                ? nsp::trf("Nilesoft Shell Pro {} is available. Installing it needs administrator approval. Click to see what's new.",
                           version)
                : nsp::trf("Nilesoft Shell Pro {} is available. Click to see what's new.", version);
        if (windowOpen || icon_->notify(widen(nsp::tr("Update available")), widen(body)))
            if (persist_)
                registry::app::setString(kNotified, widen(version));
        return;
    }
    if (!manual)
        return;
    const std::string message = str(ui_->global<UpdateState>().get_message());
    if (sum.state == "error")
        icon_->notify(widen(nsp::tr("Couldn't check for updates")), widen(message));
    else
        icon_->notify(L"Nilesoft Shell Pro", widen(message.empty() ? std::string(nsp::tr("You have the latest version.")) : message));
}
