#include "LogReader.h"

#include "Text.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace logreader {

fs::path logPathForDll(const fs::path &dll)
{
    auto beside = dll.parent_path() / L"shell.log";
    std::error_code ec;
    if (fs::is_regular_file(beside, ec))
        return beside;
    wchar_t local[MAX_PATH]{};
    auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return beside;
    auto fallback = fs::path(local) / L"Nilesoft" / L"Shell" / L"shell.log";
    return fs::is_regular_file(fallback, ec) ? fallback : beside;
}

fs::path logPathFor(const fs::path &reported, const fs::path &dll)
{
    return reported.empty() ? logPathForDll(dll) : reported;
}

bool parseLine(const std::string &line, LogEntry &out)
{
    // 2026-10-12 20:31:07 [error] line[3] column[9], unexpected token "C:\...\shell.nss"
    static const std::regex entry(R"(^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+\[(\w+)\]\s?(.*)$)");
    static const std::regex position(R"(line\[(\d+)\]\s*column\[(\d+)\])");
    static const std::regex quoted(R"re(["']([^"']+\.nss)["'])re", std::regex::icase);

    std::smatch m;
    if (!std::regex_match(line, m, entry))
        return false;
    out = {};
    out.time = m[1].str();
    out.level = m[2].str();
    // shell.dll's handler-timing trace ("12ms<TAB>{CLSID}<TAB>") is written without line
    // breaks and lands after messages; strip it and any control characters.
    static const std::regex trace(R"(\d+(?:ms)?\t\{[0-9A-Fa-f-]+\}\t?)");
    out.message = std::regex_replace(m[3].str(), trace, "");
    std::erase_if(out.message, [](char c) { return uint8_t(c) < 0x20; });
    while (!out.message.empty() && out.message.back() == ' ')
        out.message.pop_back();
    std::smatch p;
    if (std::regex_search(out.message, p, position)) {
        out.line = std::stoi(p[1].str());
        out.column = std::stoi(p[2].str());
    }
    if (std::regex_search(out.message, p, quoted))
        out.file = p[1].str();
    return true;
}

std::vector<LogEntry> readTail(const fs::path &log, size_t maxEntries, size_t maxBytes)
{
    std::vector<LogEntry> out;
    std::ifstream in(log, std::ios::binary);
    if (!in)
        return out;
    in.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(in.tellg());
    if (size == 0)
        return out;

    char bom[2]{};
    in.seekg(0);
    in.read(bom, 2);
    const bool utf16 = size >= 2 && uint8_t(bom[0]) == 0xFF && uint8_t(bom[1]) == 0xFE;

    size_t start = size > maxBytes ? size - maxBytes : 0;
    if (utf16 && (start % 2))
        ++start; // keep UTF-16 code units aligned (the BOM is 2 bytes)
    if (utf16 && start < 2)
        start = 2;
    std::string raw(size - start, '\0');
    in.seekg(std::streamoff(start));
    in.read(raw.data(), std::streamsize(raw.size()));
    raw.resize(size_t(in.gcount()));

    std::string textUtf8;
    if (utf16) {
        std::wstring w(raw.size() / 2, L'\0');
        memcpy(w.data(), raw.data(), w.size() * 2);
        textUtf8 = text::narrow(w);
    } else {
        textUtf8 = std::move(raw);
        if (start == 0 && textUtf8.rfind("\xEF\xBB\xBF", 0) == 0)
            textUtf8.erase(0, 3);
    }

    // When reading from the middle, the first line is probably cut; drop it.
    size_t pos = 0;
    if (start > (utf16 ? 2u : 0u)) {
        auto nl = textUtf8.find('\n');
        pos = nl == std::string::npos ? textUtf8.size() : nl + 1;
    }

    std::vector<LogEntry> all;
    while (pos < textUtf8.size()) {
        auto nl = textUtf8.find('\n', pos);
        auto line = textUtf8.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? textUtf8.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        LogEntry e;
        if (!parseLine(line, e))
            continue;
        if (e.level == "error" || e.level == "warning" || e.level == "info")
            all.push_back(std::move(e));
    }

    const size_t n = std::min(maxEntries, all.size());
    out.assign(std::make_move_iterator(all.end() - std::ptrdiff_t(n)), std::make_move_iterator(all.end()));
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace logreader
