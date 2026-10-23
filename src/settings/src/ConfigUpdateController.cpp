#include "ConfigUpdateController.h"

#include "Diff.h"
#include "Registry.h"
#include "Text.h"
#include "Tr.h"

#include "nss/Text.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }
std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }

// The snapshot reasons shell.exe writes (src/shared/ConfigUpgrade.h), here so they get
// translated: the Backups page shows them through nsp::tr.
[[maybe_unused]] constexpr const char *snapshotReasons[] = {
    nsp::mark("Before updating the menu files"),
    nsp::mark("Before using the new version of a menu file"),
};

std::string decodedText(const std::string &bytes)
{
    auto dec = nss::decode(bytes);
    return dec.ok ? dec.text : bytes;
}

std::string failureText(const CliResult &r, const std::optional<config_update::Report> &report)
{
    if (report && !report->error.empty())
        return report->error;
    if (!r.started)
        return text::narrow(r.error);
    if (r.timedOut)
        return nsp::tr("shell.exe did not finish in time and was stopped.");
    return nsp::trf("shell.exe exited with code {}.", (long long)r.exitCode);
}

} // namespace

ConfigUpdateController::ConfigUpdateController(slint::ComponentHandle<AppWindow> ui, bool enabled, bool forced)
    : ui_(std::move(ui)), cli_(ShellCli::locate()), shipped_(config_update::localShippedDir()), enabled_(enabled),
      forced_(forced)
{
    bindCallbacks();
    if (enabled_)
        statusPoll_.start(slint::TimerMode::Repeated, 300ms, [this] { pollStatus(); });
    nsp::onLanguageChanged([this] { pushText(); });
}

ConfigUpdateController::~ConfigUpdateController() = default;

void ConfigUpdateController::bindCallbacks()
{
    const auto &st = ui_->global<ConfigUpdateState>();
    st.on_dismiss_updated([this] { ui_->global<ConfigUpdateState>().set_updated_shown(false); });
    st.on_dismiss_kept([this] { ui_->global<ConfigUpdateState>().set_kept_shown(false); });
    st.on_compare([this](const slint::SharedString &path) { compare(str(path)); });
    st.on_close_compare([this] {
        ++compareGen_;
        comparePath_.clear();
        ui_->global<ConfigUpdateState>().set_compare_shown(false);
    });
    st.on_take([this](const slint::SharedString &path) { take(str(path)); });
}

void ConfigUpdateController::pollStatus()
{
    auto status = ui_->global<AppState>().get_status();
    if (!status.loaded)
        return;
    const bool personal = config_update::isPersonal(str(status.config_source), status.config_exists);
    const std::string root = str(status.config_path);

    if (checked_) {
        // A per-user copy of the app only: a personal config made since (from Home or the
        // first-run dialog) got the install folder's older files; bring them up to this version.
        if (personal && root != upgradedRoot_ && !running_)
            run(root);
        return;
    }
    checked_ = true;
    statusPoll_.stop();
    if (!shipped_.empty())
        statusPoll_.start(slint::TimerMode::Repeated, 2s, [this] { pollStatus(); });
    upgradedRoot_ = personal ? root : std::string();

    const std::string current = SETTINGS_VERSION;
    auto last = registry::app::getString(config_update::versionValue).value_or(L"");
    if (!config_update::shouldRun(text::narrow(last), current, forced_))
        return;

    if (!personal) {
        // The shared config in the install folder: nothing per user to bring up to date.
        registry::app::setString(config_update::versionValue, text::widen(current));
        return;
    }
    run(root);
}

std::optional<config_update::Report> ConfigUpdateController::upgrade(const ShellCli &cli, const std::wstring &shipped,
                                                                     const std::string &root,
                                                                     const std::vector<std::string> &take, CliResult &r)
{
    if (!shipped.empty()) {
        // A per-user copy of the app: its own shell.nss and imports\ are this version's.
        r.started = true;
        return config_update::runLocal(shipped, text::widen(root), take);
    }
    std::vector<std::wstring> args{ L"-config:upgrade" };
    for (const auto &t : take)
        args.push_back(L"-take:" + text::widen(t));
    r = cli.run(args, 60000);
    return config_update::parseReport(r.output);
}

void ConfigUpdateController::run(const std::string &root)
{
    if (running_ || (shipped_.empty() && !cli_.available()))
        return;
    running_ = true;
    upgradedRoot_ = root;
    worker_.post([this, cli = cli_, shipped = shipped_, root] {
        CliResult r;
        auto report = upgrade(cli, shipped, root, {}, r);
        slint::invoke_from_event_loop([this, r, report] {
            running_ = false;
            if (!report || !r.started || r.timedOut) {
                // Tried again at the next start.
                return;
            }
            // Files that couldn't be written are tried again at the next start; what
            // did get done is shown either way.
            if (report->ok)
                registry::app::setString(config_update::versionValue, text::widen(SETTINGS_VERSION));
            show(*report);
            if (report->changed())
                notifyFilesChanged();
        });
    });
}

void ConfigUpdateController::show(const config_update::Report &report)
{
    configDir_ = report.configDir;
    installDir_ = report.installDir;
    changed_ = report.changed();
    kept_ = report.modifiedKept;
    pushText();
    const auto &st = ui_->global<ConfigUpdateState>();
    st.set_updated_shown(changed_ > 0);
    st.set_kept_shown(!kept_.empty());
}

void ConfigUpdateController::pushText()
{
    const auto &st = ui_->global<ConfigUpdateState>();
    st.set_updated_title(ss(nsp::trn("Updated {n} configuration file to this version",
                                     "Updated {n} configuration files to this version", (long long)changed_)));
    st.set_updated_text(ss(nsp::tr("They came with Nilesoft Shell and you hadn't changed them. Your old copies are saved in Backups.")));
    st.set_kept_title(ss(nsp::trn("{n} configuration file wasn't updated because you changed it",
                                  "{n} configuration files weren't updated because you changed them",
                                  (long long)kept_.size())));
    st.set_kept_text(ss(nsp::tr("Your changes are kept. Compare a file with the new version, or use the new version instead. Your copy is saved in Backups first.")));
    std::vector<slint::SharedString> rows;
    for (const auto &p : kept_)
        rows.push_back(ss(p));
    st.set_kept_files(std::make_shared<slint::VectorModel<slint::SharedString>>(rows));
    if (!comparePath_.empty()) {
        st.set_compare_title(ss(nsp::trf("{}: your copy and the new version", comparePath_)));
        st.set_compare_summary(ss(nsp::tr("Red lines are only in your copy. Green lines are in the new version.")));
    }
}

void ConfigUpdateController::compare(const std::string &path)
{
    if (configDir_.empty() || installDir_.empty())
        return;
    comparePath_ = path;
    auto gen = ++compareGen_;
    const auto &st = ui_->global<ConfigUpdateState>();
    st.set_compare_path(ss(path));
    st.set_compare_lines(std::make_shared<slint::VectorModel<DiffLineRow>>());
    st.set_compare_loading(true);
    st.set_compare_shown(true);
    pushText();

    auto rel = fs::path(nss::widen(path));
    auto mine = fs::path(nss::widen(configDir_)) / rel;
    auto shipped = fs::path(nss::widen(installDir_)) / rel;
    worker_.post([this, gen, path, mine, shipped] {
        std::string oldBytes, newBytes;
        bool ok = nss::readFile(mine, oldBytes) && nss::readFile(shipped, newBytes);
        auto d = ok ? diff::unified(decodedText(oldBytes), decodedText(newBytes)) : diff::Unified{};
        slint::invoke_from_event_loop([this, gen, ok, path, d = std::move(d)] {
            if (gen != compareGen_)
                return;
            const auto &st = ui_->global<ConfigUpdateState>();
            st.set_compare_loading(false);
            if (!ok) {
                st.set_compare_summary(ss(nsp::trf("Couldn't read {}.", path)));
                return;
            }
            auto rows = std::make_shared<slint::VectorModel<DiffLineRow>>();
            size_t maxColumns = 0;
            for (const auto &r : d.rows) {
                // Tabs as four spaces, as on the Backups page.
                std::string line;
                for (char c : r.text)
                    line += c == '\t' ? std::string("    ") : std::string(1, c);
                maxColumns = std::max(maxColumns, line.size());
                rows->push_back(DiffLineRow{ int(r.kind), r.oldLine, r.newLine, ss(line) });
            }
            st.set_compare_max_columns(int(std::min<size_t>(maxColumns + 2, 2000)));
            st.set_compare_lines(rows);
        });
    });
}

void ConfigUpdateController::take(const std::string &path)
{
    const auto &st = ui_->global<ConfigUpdateState>();
    if (st.get_working() || (shipped_.empty() && !cli_.available()))
        return;
    st.set_working(true);
    const std::string root = str(ui_->global<AppState>().get_status().config_path);
    worker_.post([this, path, root, cli = cli_, shipped = shipped_] {
        CliResult r;
        auto report = upgrade(cli, shipped, root, { path }, r);
        bool done = report && std::any_of(report->updated.begin(), report->updated.end(),
                                          [&](const std::string &p) { return _stricmp(p.c_str(), path.c_str()) == 0; });
        std::string failure = done ? std::string{} : failureText(r, report);
        slint::invoke_from_event_loop([this, path, done, failure, report] {
            const auto &st = ui_->global<ConfigUpdateState>();
            st.set_working(false);
            if (!done) {
                toast("error", nsp::tr("Couldn't use the new version"), failure);
                return;
            }
            kept_.erase(std::remove_if(kept_.begin(), kept_.end(),
                                       [&](const std::string &p) { return _stricmp(p.c_str(), path.c_str()) == 0; }),
                        kept_.end());
            if (comparePath_ == path) {
                ++compareGen_;
                comparePath_.clear();
                st.set_compare_shown(false);
            }
            pushText();
            st.set_kept_shown(!kept_.empty());
            toast("ok", nsp::trf("Now using the new version of {}", path), nsp::tr("Your copy is saved in Backups."));
            notifyFilesChanged();
        });
    });
}

void ConfigUpdateController::toast(const std::string &kind, const std::string &title, const std::string &text)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(text));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

void ConfigUpdateController::notifyFilesChanged()
{
    for (auto &fn : filesChanged_)
        fn();
}
