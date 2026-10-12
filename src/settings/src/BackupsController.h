#pragma once

// The Backups page (ui/pages/backups.slint, BackupsState): the snapshot
// timeline from nss::ConfigStore, pin/unpin, restore and delete with a
// confirmation, and a per-file line diff of a snapshot against the current
// files. Disk work runs on its own Worker.

#include "app.h"

#include "BackupsModel.h"
#include "Worker.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class BackupsController
{
public:
    explicit BackupsController(slint::ComponentHandle<AppWindow> ui);
    ~BackupsController();

private:
    void bindCallbacks();
    void pollStatus();
    void refresh(const std::string &selectId = {});
    void select(int index);
    void showFile(int index);
    void create();
    void togglePin(int index);
    void confirm();
    void toast(const std::string &kind, const std::string &title, const std::string &text = {});
    std::string retentionText() const;

    slint::ComponentHandle<AppWindow> ui_;
    std::filesystem::path root_;
    std::string statusPath_;
    bool available_ = false;
    std::vector<nss::SnapshotInfo> snapshots_;
    std::optional<backups::SnapshotDiff> diff_;
    std::string diffId_;
    int confirmIndex_ = -1;
    uint64_t gen_ = 0;
    uint64_t diffGen_ = 0;
    nss::RetentionPolicy retention_;

    slint::Timer statusPoll_;
    slint::Timer toastTimer_;
    Worker worker_; // last: joined first
};
