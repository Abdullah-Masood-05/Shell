#include "ConfigUpdate.h"

#include "AppChannel.h"
#include "CarryOver.h"
#include "ShellOverrides.h"

#include "nss/NssDocument.h"
#include "nss/Text.h"

// The one place the app itself runs src/shared code: the upgrade of a per-user copy.
#include "../../shared/ConfigUpgrade.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace config_update {

namespace {

using json = nlohmann::json;

std::string str(const json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::vector<std::string> list(const json &j, const char *key)
{
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return out;
    for (const auto &v : *it)
        if (v.is_string())
            out.push_back(v.get<std::string>());
    return out;
}

} // namespace

bool shouldRun(std::string_view lastVersion, std::string_view currentVersion, bool forced)
{
    return forced || lastVersion != currentVersion;
}

bool isPersonal(std::string_view configSource, bool configExists)
{
    return configExists && (configSource == "appdata" || configSource == "registry");
}

std::optional<Report> parseReport(std::string_view text)
{
    auto j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("ok") || !j["ok"].is_boolean())
        return std::nullopt;
    Report r;
    r.ok = j["ok"].get<bool>();
    r.configDir = str(j, "configDir");
    r.installDir = str(j, "installDir");
    r.backup = str(j, "backup");
    r.updated = list(j, "updated");
    r.added = list(j, "added");
    r.modifiedKept = list(j, "modified_kept");
    r.unchanged = list(j, "unchanged");
    r.failed = list(j, "failed");
    r.error = str(j, "error");
    return r;
}

Report runLocal(const std::wstring &shippedDir, const std::wstring &configRoot, const std::vector<std::string> &take)
{
    std::wstring configDir, rootName = L"shell.nss";
    auto slash = configRoot.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        configDir = configRoot.substr(0, slash);
        rootName = configRoot.substr(slash + 1);
    }
    config_upgrade::Result result;
    if (!configDir.empty() && !config_upgrade::same_path(configDir, shippedDir)) {
        config_upgrade::Options options;
        options.root_name = rootName;
        options.take = take;
        result = config_upgrade::run(shippedDir, configDir, options);
    } else {
        configDir.clear();
        if (!take.empty()) {
            result.ok = false;
            result.error = "there is no per-user config";
        }
    }
    auto report = parseReport(config_upgrade::to_json(result, shippedDir, configDir));
    if (!report) {
        Report r;
        r.error = "the upgrade report couldn't be read";
        return r;
    }
    return *report;
}

Carried carryOver(const std::filesystem::path &configRoot, const std::filesystem::path &installDir,
                  const std::vector<std::string> &files, bool partial, const nss::Validator &validator)
{
    namespace fs = std::filesystem;
    Carried out;
    const auto configDir = configRoot.parent_path();
    std::vector<nss::FileWrite> writes;
    std::vector<overrides::Override> lines;
    std::optional<std::string> rootText;
    std::optional<nss::TextFormat> rootFormat;
    std::vector<std::string> done;
    size_t kept = 0;
    for (const auto &rel : files) {
        const auto path = (configDir / fs::path(nss::widen(rel))).lexically_normal();
        std::string user, shipped;
        if (!nss::readFile(path, user) || !nss::readFile(installDir / fs::path(nss::widen(rel)), shipped))
            continue;
        auto r = carryover::carry(rel, user, shipped, carryover::shippedBases(rel), partial);
        if (r.text.empty() || (!partial && !r.complete))
            continue;
        // Override lines only once the config can be checked: an older engine rejects the
        // statement, and then the whole config fails to load.
        if (!validator && !r.overrides.empty())
            continue;
        if (nss::pathKey(path) == nss::pathKey(configRoot)) {
            rootText = r.text;
            rootFormat = r.format;
        } else
            writes.push_back(nss::FileWrite{ path, r.text, r.format });
        lines.insert(lines.end(), r.overrides.begin(), r.overrides.end());
        kept += r.carried;
        done.push_back(rel);
    }
    if (done.empty())
        return out;

    if (!lines.empty()) {
        auto plan = overrides::planMapWrite(configRoot, [&](maprules::Rules &rules, std::string &) {
            carryover::addOverrides(rules, lines);
            return true;
        });
        if (!plan.ok) {
            out.ok = false;
            out.error = plan.error;
            return out;
        }
        for (auto &w : plan.writes) {
            if (nss::pathKey(w.path) == nss::pathKey(configRoot)) {
                // The root's import of the map file: on the new root when that is written too.
                if (!rootText) {
                    rootText = w.text;
                    rootFormat = w.format;
                }
                continue;
            }
            writes.push_back(std::move(w));
        }
        if (rootText) {
            auto doc = nss::NssDocument::fromText(*rootText, rootFormat.value_or(nss::defaultFormat()));
            maprules::ensureImported(doc);
            rootText = doc.text();
        }
    }
    if (rootText)
        writes.push_back(nss::FileWrite{ configRoot, *rootText, rootFormat });

    nss::ConfigStore store(configRoot);
    nss::SaveOptions options;
    options.reason = partial ? "Before using the new version of a menu file" : reasonCarry;
    options.validator = validator;
    auto result = store.save(writes, options);
    if (!result.ok) {
        out.ok = false;
        out.error = result.error;
        return out;
    }
    out.snapshot = result.snapshotId;
    out.files = std::move(done);
    out.kept = kept;
    return out;
}

std::wstring localShippedDir()
{
    if (!appchannel::isPerUserCopy())
        return {};
    auto dir = appchannel::selfPath().parent_path();
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / L"shell.nss", ec) ? dir.wstring() : std::wstring{};
}

} // namespace config_update
