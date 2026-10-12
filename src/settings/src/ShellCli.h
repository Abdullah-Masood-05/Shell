#pragma once

// Runs shell.exe (the core's command-line front end) and captures its JSON output.
// shell.exe is a GUI-subsystem program, so -silent is always passed.

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

struct CliResult
{
    bool started = false;   // CreateProcess / ShellExecuteEx succeeded
    bool timedOut = false;  // killed after the timeout
    bool cancelled = false; // the user declined the UAC prompt
    DWORD exitCode = DWORD(-1);
    std::string output;     // stdout, UTF-8
    std::wstring error;     // launch error text

    bool ok() const { return started && !timedOut && exitCode == 0; }
};

class ShellCli
{
public:
    // shell.exe next to settings.exe, else next to the registered shell.dll
    // (HKCU first, then HKLM). Empty if neither exists.
    static std::filesystem::path locate();

    explicit ShellCli(std::filesystem::path exe) : exe_(std::move(exe)) {}

    const std::filesystem::path &exe() const { return exe_; }
    bool available() const;

    // Runs without a window and waits up to timeoutMs. "-silent" is added.
    CliResult run(const std::vector<std::wstring> &args, DWORD timeoutMs = 15000) const;

    // Runs elevated through the UAC prompt (ShellExecuteEx "runas"). Output is not
    // captured; only the exit code is. Used only by the machine-wide registration.
    CliResult runElevated(const std::vector<std::wstring> &args, HWND owner,
                          DWORD timeoutMs = 120000) const;

    static std::wstring quote(const std::wstring &arg);

private:
    std::wstring commandLine(const std::vector<std::wstring> &args, bool withExe) const;

    std::filesystem::path exe_;
};
