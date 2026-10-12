#include "Status.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

const json &at(const json &j, const char *key)
{
    static const json null;
    if (!j.is_object())
        return null;
    auto it = j.find(key);
    return it == j.end() ? null : *it;
}

std::string str(const json &j, const char *key)
{
    const auto &v = at(j, key);
    return v.is_string() ? v.get<std::string>() : std::string{};
}

bool boolean(const json &j, const char *key)
{
    const auto &v = at(j, key);
    return v.is_boolean() && v.get<bool>();
}

} // namespace

std::string ShellStatus::activeDll() const
{
    if (userRegistered && !userDll.empty())
        return userDll;
    if (machineRegistered && !machineDll.empty())
        return machineDll;
    return dllPath;
}

std::optional<ShellStatus> parseStatus(const std::string &text, std::string &error)
{
    auto j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = text.empty() ? "shell.exe printed nothing." : "shell.exe printed invalid JSON.";
        return std::nullopt;
    }
    ShellStatus s;
    s.version = str(j, "version");
    s.elevated = boolean(j, "elevated");
    s.installDir = str(j, "installDir");

    const auto &dll = at(j, "dll");
    s.dllPath = str(dll, "path");
    s.dllExists = boolean(dll, "exists");

    const auto &reg = at(j, "registration");
    if (auto mode = str(reg, "mode"); !mode.empty())
        s.regMode = mode;
    s.machineRegistered = boolean(at(reg, "machine"), "registered");
    s.machineDll = str(at(reg, "machine"), "dll");
    s.userRegistered = boolean(at(reg, "user"), "registered");
    s.userDll = str(at(reg, "user"), "dll");

    const auto &w11 = at(j, "win11");
    s.win11Applicable = boolean(w11, "applicable");
    s.classicMenu = boolean(w11, "classicMenu");
    if (auto v = str(w11, "machineTreatAs"); !v.empty())
        s.machineTreatAs = v;
    if (auto v = str(w11, "userTreatAs"); !v.empty())
        s.userTreatAs = v;

    const auto &cfg = at(j, "config");
    s.configPath = str(cfg, "path");
    s.configSource = str(cfg, "source");
    s.configExists = boolean(cfg, "exists");
    s.configRegistryValue = str(cfg, "registryValue");
    s.userConfigDir = str(j, "userConfigDir");
    return s;
}
