#pragma once

// Pending theme/settings changes shared by every page, and how they turn into
// the app-owned imports/settings-app.nss (see SaveService for the transaction).
// No Slint and no processes here, so the mapping is unit-tested
// (tests/test_pending.cpp).

#include "nss/ConfigStore.h"
#include "nss/OverridesWriter.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

class PendingChanges
{
public:
    struct Change
    {
        std::optional<std::string> literal; // nullopt: drop the app's override ("reset to file")
        std::string page;                   // "Appearance", for the snapshot reason
    };

    // Validates `literal` with ThemeSchema first; returns the errors (empty when it
    // was accepted). The key is stored in canonical form. Expressions are accepted
    // only for keys that take one (ValueType::Any, e.g. settings.exclude.where);
    // shell.exe -config:check judges them when the change is saved.
    std::vector<std::string> set(std::string_view key, std::string_view literal, std::string_view page = {});
    // set(), unless `literal` is the value already saved (`saved`: this app's value,
    // else the files'; nullopt for the built-in default): then the key's pending
    // change is dropped instead of staging a no-op. What every page's edit does.
    std::vector<std::string> stage(std::string_view key, std::string_view literal, std::string_view page,
                                   const std::optional<std::string> &saved);
    void reset(std::string_view key, std::string_view page = {});
    // Forgets the pending change for `key` (the value goes back to the saved one).
    void forget(std::string_view key);
    void discard() { changes_.clear(); }

    size_t count() const { return changes_.size(); }
    bool empty() const { return changes_.empty(); }
    const std::map<std::string, Change> &items() const { return changes_; }
    const Change *find(std::string_view key) const;

    // The overrides after the pending changes are applied to `existing`.
    nss::Overrides apply(const nss::Overrides &existing) const;

    // "Appearance: background.color, item.radius" (the snapshot reason).
    std::string describe() const;

private:
    std::map<std::string, Change> changes_;
};

// Two value texts are the same literal ("modern" and 'modern', 8 and 8.0, #fff and
// #FFFFFF); expressions compare as written. Picking the value already saved drops
// the pending change instead of staging a no-op (SaveService::set).
bool sameValue(std::string_view a, std::string_view b);

// What a save writes, computed from the current files.
struct SavePlan
{
    bool ok = false;
    std::string error;
    std::filesystem::path overridesPath; // <root folder>/imports/settings-app.nss
    nss::Overrides before;               // what settings-app.nss held
    nss::Overrides after;
    bool rootChanged = false;            // the import line had to be added or moved
    std::vector<nss::FileWrite> writes;  // settings-app.nss first, then the root
};

SavePlan planSave(const std::filesystem::path &root, const PendingChanges &pending);
