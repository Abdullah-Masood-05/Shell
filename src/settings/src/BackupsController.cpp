#include "BackupsController.h"

#include "EditorSession.h"

#include "nss/Text.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

slint::SharedString ss(std::string_view s) { return slint::SharedString(s); }
std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }

const char *statusName(backups::FileDiff::Status s)
{
    switch (s) {
    case backups::FileDiff::Modified: return "modified";
    case backups::FileDiff::Added: return "added";
    case backups::FileDiff::Removed: return "removed";
    default: return "same";
    }
}

} // namespace

BackupsController::BackupsController(slint::ComponentHandle<AppWindow> ui) : ui_(std::move(ui))
{
    ui_->global<BackupsState>().set_retention_text(ss(retentionText()));
    bindCallbacks();
    statusPoll_.start(slint::TimerMode::Repeated, 500ms, [this] { pollStatus(); });
}

BackupsController::~BackupsController() = default;

std::string BackupsController::retentionText() const
{
    return "Keeps the newest " + std::to_string(retention_.maxCount) + " snapshots, none older than " +
           std::to_string(retention_.maxAgeDays) + " days. Pinned snapshots are always kept.";
}

void BackupsController::toast(const std::string &kind, const std::string &title, const std::string &text)
{
    const auto &app = ui_->global<AppState>();
    app.set_toast_kind(ss(kind));
    app.set_toast_title(ss(title));
    app.set_toast_text(ss(text));
    app.set_toast_shown(true);
    toastTimer_.start(slint::TimerMode::SingleShot, kind == "error" ? 9s : 5s,
                      [this] { ui_->global<AppState>().set_toast_shown(false); });
}

void BackupsController::bindCallbacks()
{
    const auto &st = ui_->global<BackupsState>();
    st.on_activated([this] {
        pollStatus();
        if (!root_.empty())
            refresh();
    });
    st.on_select([this](int i) { select(i); });
    st.on_select_file([this](int i) { showFile(i); });
    st.on_create([this] { create(); });
    st.on_toggle_pin([this](int i) { togglePin(i); });
    st.on_ask_restore([this](int i) {
        if (i < 0 || i >= int(snapshots_.size()) || !available_)
            return;
        const auto &s = snapshots_[size_t(i)];
        confirmIndex_ = i;
        const auto &st = ui_->global<BackupsState>();
        st.set_confirm_text(ss("Your config files are put back as they were on " + backups::displayTime(s) + " (" +
                               std::to_string(s.files.size()) + " files, \"" + s.reason +
                               "\").\n\nA snapshot of your current files is taken first, so you can undo this. "
                               "Files added since then are left alone."));
        st.set_confirm_kind("restore");
    });
    st.on_ask_delete([this](int i) {
        if (i < 0 || i >= int(snapshots_.size()) || snapshots_[size_t(i)].pinned)
            return;
        const auto &s = snapshots_[size_t(i)];
        confirmIndex_ = i;
        const auto &st = ui_->global<BackupsState>();
        st.set_confirm_text(ss("The snapshot from " + backups::displayTime(s) + " (\"" + s.reason +
                               "\") is deleted from disk. This can't be undone."));
        st.set_confirm_kind("delete");
    });
    st.on_confirm([this] { confirm(); });
    st.on_cancel([this] {
        confirmIndex_ = -1;
        ui_->global<BackupsState>().set_confirm_kind("");
    });
    st.on_open_folder([this] {
        if (root_.empty())
            return;
        int sel = ui_->global<BackupsState>().get_selected();
        fs::path dir = sel >= 0 && sel < int(snapshots_.size()) ? snapshots_[size_t(sel)].dir
                                                                 : nss::ConfigStore(root_).backupsDir();
        worker_.post([dir] {
            std::error_code ec;
            fs::create_directories(dir, ec);
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
    });
}

void BackupsController::pollStatus()
{
    auto status = ui_->global<AppState>().get_status();
    if (!status.loaded)
        return;
    auto path = str(status.config_path);
    if (path == statusPath_)
        return;
    statusPath_ = path;
    root_ = fs::path(nss::widen(path)).lexically_normal();
    bool shared = str(status.config_source) == "install" || editor::isProtectedPath(root_);
    available_ = status.config_exists && !shared;
    const auto &st = ui_->global<BackupsState>();
    st.set_available(available_);
    st.set_unavailable_text(ss(shared ? std::string("The active config is the shared one in the install folder, which "
                                                    "this app never writes to. Create a personal config to get snapshots.")
                                      : "The config file doesn't exist: " + path));
    st.set_folder(ss(nss::narrow(nss::ConfigStore(root_).backupsDir().wstring())));
    refresh();
}

void BackupsController::refresh(const std::string &selectId)
{
    if (root_.empty())
        return;
    auto gen = ++gen_;
    const auto &st = ui_->global<BackupsState>();
    int sel = st.get_selected();
    std::string keep = !selectId.empty() ? selectId
                       : sel >= 0 && sel < int(snapshots_.size()) ? snapshots_[size_t(sel)].id
                                                                    : std::string{};
    worker_.post([this, gen, keep, root = root_] {
        nss::ConfigStore store(root);
        auto list = store.snapshots();
        std::vector<bool> current;
        bool matched = false;
        for (const auto &s : list) {
            // Only the newest snapshot equal to the files on disk is "Current".
            bool same = !matched && backups::matchesTree(store, s);
            matched = matched || same;
            current.push_back(same);
        }
        slint::invoke_from_event_loop([this, gen, keep, list = std::move(list), current = std::move(current)]() mutable {
            if (gen != gen_)
                return;
            snapshots_ = std::move(list);
            std::vector<SnapshotRow> rows;
            for (size_t i = 0; i < snapshots_.size(); ++i) {
                const auto &s = snapshots_[i];
                rows.push_back(SnapshotRow{ ss(s.id), ss(backups::displayTime(s)), ss(backups::relativeTime(s)),
                                            ss(s.reason), int(s.files.size()), s.pinned, bool(current[i]) });
            }
            const auto &st = ui_->global<BackupsState>();
            st.set_snapshots(std::make_shared<slint::VectorModel<SnapshotRow>>(rows));
            st.set_loaded(true);
            int index = snapshots_.empty() ? -1 : 0;
            for (size_t i = 0; i < snapshots_.size(); ++i)
                if (snapshots_[i].id == keep)
                    index = int(i);
            select(index);
        });
    });
}

void BackupsController::select(int index)
{
    const auto &st = ui_->global<BackupsState>();
    st.set_selected(index);
    st.set_diff_lines(std::make_shared<slint::VectorModel<DiffLineRow>>());
    st.set_diff_files(std::make_shared<slint::VectorModel<DiffFileRow>>());
    st.set_diff_file(-1);
    diff_.reset();
    if (index < 0 || index >= int(snapshots_.size())) {
        st.set_diff_summary("");
        return;
    }
    auto gen = ++diffGen_;
    auto snap = snapshots_[size_t(index)];
    st.set_diff_loading(true);
    st.set_diff_summary("Comparing with your files now…");
    worker_.post([this, gen, snap, root = root_] {
        nss::ConfigStore store(root);
        auto d = backups::diffSnapshot(store, snap);
        slint::invoke_from_event_loop([this, gen, d = std::move(d)]() mutable {
            if (gen != diffGen_)
                return;
            const auto &st = ui_->global<BackupsState>();
            st.set_diff_loading(false);
            int added = 0, removed = 0;
            // Only changed files are listed (they sort first).
            std::vector<DiffFileRow> rows;
            for (const auto &f : d.files) {
                if (f.status == backups::FileDiff::Same)
                    continue;
                rows.push_back(DiffFileRow{ ss(f.path), statusName(f.status), f.diff.added, f.diff.removed });
                added += f.diff.added;
                removed += f.diff.removed;
            }
            const auto unchanged = d.files.size() - size_t(d.changed);
            std::string summary;
            if (!d.error.empty())
                summary = d.error;
            else if (d.changed == 0)
                summary = "Identical to your files now (" + std::to_string(d.files.size()) + " files).";
            else
                summary = "Since this snapshot: " + std::to_string(d.changed) +
                          (d.changed == 1 ? " file changed" : " files changed") + " (+" + std::to_string(added) +
                          " −" + std::to_string(removed) + " lines), " + std::to_string(unchanged) +
                          " unchanged. Green lines are in your files now; red lines come back if you restore.";
            st.set_diff_summary(ss(summary));
            st.set_diff_files(std::make_shared<slint::VectorModel<DiffFileRow>>(rows));
            diff_ = std::move(d);
            showFile(diff_->changed > 0 ? 0 : -1); // rows match files: changed files sort first
        });
    });
}

void BackupsController::showFile(int index)
{
    const auto &st = ui_->global<BackupsState>();
    st.set_diff_file(index);
    auto rows = std::make_shared<slint::VectorModel<DiffLineRow>>();
    size_t maxColumns = 0;
    if (diff_ && index >= 0 && index < int(diff_->files.size())) {
        for (const auto &r : diff_->files[size_t(index)].diff.rows) {
            // Tabs as four spaces, as in the editor's read view.
            std::string text;
            for (char c : r.text)
                text += c == '\t' ? std::string("    ") : std::string(1, c);
            maxColumns = std::max(maxColumns, text.size());
            rows->push_back(DiffLineRow{ int(r.kind), r.oldLine, r.newLine, ss(text) });
        }
    }
    st.set_diff_max_columns(int(std::min<size_t>(maxColumns + 2, 2000)));
    st.set_diff_lines(rows);
}

void BackupsController::create()
{
    if (!available_)
        return;
    const auto &st = ui_->global<BackupsState>();
    st.set_working(true);
    worker_.post([this, root = root_, retention = retention_] {
        nss::ConfigStore store(root);
        std::string error;
        auto snap = store.snapshot("Manual snapshot", &error);
        if (snap)
            store.applyRetention(retention);
        slint::invoke_from_event_loop([this, snap, error] {
            ui_->global<BackupsState>().set_working(false);
            if (!snap) {
                toast("error", "Couldn't create a snapshot", error);
                return;
            }
            toast("ok", "Snapshot created", std::to_string(snap->files.size()) + " files saved as " + snap->id + ".");
            refresh(snap->id);
        });
    });
}

void BackupsController::togglePin(int index)
{
    if (index < 0 || index >= int(snapshots_.size()))
        return;
    auto id = snapshots_[size_t(index)].id;
    bool pin = !snapshots_[size_t(index)].pinned;
    worker_.post([this, root = root_, id, pin] {
        bool ok = nss::ConfigStore(root).pin(id, pin);
        slint::invoke_from_event_loop([this, ok, id, pin] {
            if (!ok)
                toast("error", pin ? "Couldn't pin the snapshot" : "Couldn't unpin the snapshot");
            refresh(id);
        });
    });
}

void BackupsController::confirm()
{
    const auto &st = ui_->global<BackupsState>();
    auto kind = str(st.get_confirm_kind());
    st.set_confirm_kind("");
    int index = confirmIndex_;
    confirmIndex_ = -1;
    if (index < 0 || index >= int(snapshots_.size()))
        return;
    auto snap = snapshots_[size_t(index)];
    st.set_working(true);
    if (kind == "restore") {
        worker_.post([this, root = root_, snap] {
            nss::ConfigStore store(root);
            std::string error;
            bool ok = store.restore(snap.id, &error);
            slint::invoke_from_event_loop([this, ok, error, snap] {
                ui_->global<BackupsState>().set_working(false);
                if (ok)
                    toast("ok", "Restored the snapshot from " + backups::displayTime(snap),
                          "Your previous files were snapshotted first.");
                else
                    toast("error", "Couldn't restore the snapshot", error);
                refresh(snap.id);
            });
        });
    } else if (kind == "delete") {
        worker_.post([this, root = root_, snap] {
            std::string error;
            bool ok = backups::deleteSnapshot(nss::ConfigStore(root), snap.id, &error);
            slint::invoke_from_event_loop([this, ok, error] {
                ui_->global<BackupsState>().set_working(false);
                if (ok)
                    toast("ok", "Snapshot deleted");
                else
                    toast("error", "Couldn't delete the snapshot", error);
                refresh();
            });
        });
    } else {
        st.set_working(false);
    }
}
