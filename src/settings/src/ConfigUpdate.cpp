#include "ConfigUpdate.h"

#include "AppChannel.h"

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

std::wstring localShippedDir()
{
    if (!appchannel::isPerUserCopy())
        return {};
    auto dir = appchannel::selfPath().parent_path();
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / L"shell.nss", ec) ? dir.wstring() : std::wstring{};
}

} // namespace config_update
