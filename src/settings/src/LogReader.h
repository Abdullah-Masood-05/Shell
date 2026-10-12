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

std::filesystem::path logPathForDll(const std::filesystem::path &dll);

// The newest maxEntries entries of level error/warning/info, newest first.
// Reads at most maxBytes from the end of the file. Handles UTF-16 LE (with BOM) and UTF-8.
std::vector<LogEntry> readTail(const std::filesystem::path &log, size_t maxEntries = 40,
                               size_t maxBytes = 256 * 1024);

// Parses one log line; false if it is not a timestamped entry.
bool parseLine(const std::string &line, LogEntry &out);

} // namespace logreader
