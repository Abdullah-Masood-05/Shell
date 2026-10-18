#include "ConfigUpdate.h"

#include <nlohmann/json.hpp>

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

} // namespace config_update
