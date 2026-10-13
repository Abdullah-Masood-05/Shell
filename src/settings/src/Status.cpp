#include "Status.h"

#include "Text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

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

// Case-insensitive, separator-insensitive path comparison.
bool samePath(const std::string &a, const std::string &b)
{
    if (a.empty() || b.empty())
        return false;
    auto key = [](const std::string &s) {
        auto w = std::filesystem::path(text::widen(s)).lexically_normal().wstring();
        CharLowerBuffW(w.data(), DWORD(w.size()));
        return w;
    };
    return key(a) == key(b);
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

std::string ShellStatus::activeLog() const
{
    const auto active = activeDll();
    const bool same = active.size() == dllPath.size() &&
                      std::equal(active.begin(), active.end(), dllPath.begin(), [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
    return same ? logPath : std::string{};
}

std::vector<std::string> ShellStatus::conflictDlls() const
{
    // shell.exe lists registrations naming a dll other than installDir's; the same
    // copy under several CLSIDs or views is one copy.
    std::vector<std::string> out;
    for (const auto &c : conflicts) {
        if (c.dll.empty() || samePath(c.dll, dllPath))
            continue;
        bool seen = false;
        for (const auto &d : out)
            seen = seen || samePath(d, c.dll);
        if (!seen)
            out.push_back(c.dll);
    }
    return out;
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

    if (const auto &list = at(j, "conflicts"); list.is_array())
        for (const auto &c : list) {
            if (!c.is_object())
                continue;
            ShellStatus::Conflict conflict;
            conflict.clsid = str(c, "clsid");
            conflict.role = str(c, "role");
            conflict.scope = str(c, "scope");
            conflict.dll = str(c, "dll");
            conflict.exists = boolean(c, "exists");
            s.conflicts.push_back(std::move(conflict));
        }
    s.logPath = str(at(j, "log"), "path");
    return s;
}

std::vector<StatusIssue> statusIssues(const ShellStatus &s, bool dllExists)
{
    std::vector<StatusIssue> out;
    auto add = [&](const char *kind, std::string title, std::string msg, const char *action = "",
                   const char *label = "") {
        out.push_back(StatusIssue{ kind, std::move(title), std::move(msg), action, label });
    };

    // Two registered copies: Explorer may load the other one, and the menu doesn't appear.
    auto others = s.conflictDlls();
    if (!others.empty()) {
        std::string msg = "Two copies stop the menu from appearing.";
        bool gone = false;
        for (const auto &c : s.conflicts)
            gone = gone || (samePath(c.dll, others[0]) && !c.exists);
        if (gone)
            msg += " Its file no longer exists; only the registration is left.";
        for (size_t i = 1; i < others.size(); ++i)
            msg += (i == 1 ? " Also registered: " : ", ") + others[i];
        add("error", "Another copy of Nilesoft Shell is registered: " + others[0], msg, "fix-conflicts",
            "Fix (needs administrator)");
    }

    if (s.regMode == "none")
        add("error", "Shell isn't enabled", "Enable it for your account; no administrator rights are needed.",
            "register-user", "Enable for me");
    else if (!dllExists)
        add("error", "The registered shell.dll is missing", "Register again from the install folder to repair it.",
            "register-user", "Register for me");
    else if (others.empty() && s.regMode == "both" && !samePath(s.userDll, s.machineDll))
        add("warning", "Shell is registered twice",
            "Your own registration (" + s.userDll + ") takes priority, so the all-users copy (" + s.machineDll +
                ") isn't used for you.",
            "unregister-user", "Unregister for me");

    if (!s.configExists)
        add("error", "The config file is missing", "Create a personal config to get the default menu back.",
            "config-init", "Create personal config");
    else if (s.configSource == "install")
        add("info", "You're using the shared config", "A personal copy in %APPDATA% is kept per user and survives updates.",
            "config-init", "Use per-user config");
    return out;
}
