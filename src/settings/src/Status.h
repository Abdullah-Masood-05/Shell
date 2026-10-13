#pragma once

// The JSON printed by `shell.exe -status`, parsed into a plain struct, and the
// problems Home shows for it. No Slint here (tests/test_status.cpp).

#include <optional>
#include <string>
#include <vector>

struct ShellStatus
{
    std::string version;
    bool elevated = false;
    std::string installDir;

    std::string dllPath;
    bool dllExists = false;

    std::string regMode = "none"; // none | user | machine | both
    bool machineRegistered = false;
    std::string machineDll;
    bool userRegistered = false;
    std::string userDll;

    bool win11Applicable = false;
    bool classicMenu = false;
    std::string machineTreatAs = "absent"; // ours | foreign | absent
    std::string userTreatAs = "absent";

    std::string configPath;
    std::string configSource; // registry | appdata | install
    bool configExists = false;
    std::string configRegistryValue;
    std::string userConfigDir;

    // Another copy of Shell: a registration of one of its CLSIDs naming a dll other
    // than installDir\shell.dll ("conflicts"; absent in older shell.exe builds, which
    // means none).
    struct Conflict
    {
        std::string clsid;
        std::string role;  // contextMenu | iconOverlay | folderExtensions
        std::string scope; // machine | user | machineWow64 | userWow64
        std::string dll;
        bool exists = false;
    };
    std::vector<Conflict> conflicts;

    // "log.path": where installDir's shell.dll logs for this user (next to it, or
    // %LOCALAPPDATA%\Nilesoft\Shell when that folder is not writable).
    std::string logPath;

    // The dll Explorer actually loads: the user one wins over the machine one.
    std::string activeDll() const;

    // logPath when the active dll is installDir's, else empty (unknown).
    std::string activeLog() const;

    // The distinct conflicting dll paths, in report order.
    std::vector<std::string> conflictDlls() const;
};

// Returns the parsed status, or nullopt and an error message.
std::optional<ShellStatus> parseStatus(const std::string &json, std::string &error);

// A problem worth a banner on Home. `action` is an AppState.action() id.
struct StatusIssue
{
    std::string kind; // error | warning | info
    std::string title;
    std::string message;
    std::string action;
    std::string actionLabel;
};

// The banners for a status, most important first. `dllExists`: the active dll is on disk.
std::vector<StatusIssue> statusIssues(const ShellStatus &s, bool dllExists);
