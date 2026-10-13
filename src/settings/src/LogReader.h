#pragma once

// Reads the end of shell.log (written by shell.dll next to itself) and turns its
// "YYYY-MM-DD HH:MM:SS [level] message" lines into diagnostics. Parser errors carry
// "line[x] column[y]" and the file in quotes.

#include <filesystem>
#include <string>
#include <vector>

struct LogEntry
{
    std::string time;    // "2026-10-12 20:31:07"
    std::string level;   // error, warning, info, debug
    std::string message; // UTF-8
    std::string file;    // from the quoted path in parser messages, if any
    int line = 0;
    int column = 0;
};

namespace logreader {

// shell.log next to the dll; if there is none there but there is one in
// %LOCALAPPDATA%\Nilesoft\Shell (where shell.dll logs when it cannot write next to
// itself, e.g. in Program Files), that one.
std::filesystem::path logPathForDll(const std::filesystem::path &dll);

// The log to read: `reported` (shell -status "log.path") if set, else logPathForDll.
std::filesystem::path logPathFor(const std::filesystem::path &reported, const std::filesystem::path &dll);

// The newest maxEntries entries of level error/warning/info, newest first.
// Reads at most maxBytes from the end of the file. Handles UTF-16 LE (with BOM) and UTF-8.
std::vector<LogEntry> readTail(const std::filesystem::path &log, size_t maxEntries = 40,
                               size_t maxBytes = 256 * 1024);

// Parses one log line; false if it is not a timestamped entry.
bool parseLine(const std::string &line, LogEntry &out);

} // namespace logreader
