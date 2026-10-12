#pragma once

// The JSON printed by `shell.exe -status`, parsed into a plain struct.

#include <optional>
#include <string>

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

    // The dll Explorer actually loads: the user one wins over the machine one.
    std::string activeDll() const;
};

// Returns the parsed status, or nullopt and an error message.
std::optional<ShellStatus> parseStatus(const std::string &json, std::string &error);
