#include "OtherApps.h"

#include "nss/Text.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace otherapps {

namespace {

// Windows' own verb names (lower case). Their titles and programs usually
// give them away too; this catches the ones with neither (pintohome...).
constexpr std::string_view kWindowsVerbs[] = {
    "open", "opennewwindow", "opennewtab", "opennewprocess", "explore", "find", "print", "printto", "edit",
    "runas", "runasuser", "cmd", "powershell", "pintohome", "pintohomefile", "pintostartscreen",
    "removeproperties", "properties", "openas", "opencontaining", "openfilelocation", "play", "preview",
    "updateencryptionsettings", "updateencryptionsettingswork", "change-passphrase", "change-pin",
    "encrypt-bde", "encrypt-bde-elev", "manage-bde", "resume-bde", "resume-bde-elev", "unlock-bde",
    "format", "eject", "wsl",
};

// Programs Windows starts by name alone (lower case).
constexpr std::string_view kWindowsPrograms[] = {
    "cmd.exe", "powershell.exe", "wsl.exe", "bash.exe", "explorer.exe", "control.exe",
    "rundll32.exe", "msiexec.exe", "notepad.exe", "mspaint.exe", "wscript.exe", "cscript.exe", "regedit.exe",
    "mmc.exe", "conhost.exe", "write.exe",
};

bool contains(const auto &list, std::string_view v)
{
    return std::find(std::begin(list), std::end(list), v) != std::end(list);
}

std::string replaceIcase(std::string s, std::string_view from, std::string_view to)
{
    auto lower = nss::toLower(s);
    auto f = nss::toLower(from);
    for (size_t at = 0; (at = lower.find(f, at)) != std::string::npos;) {
        s.replace(at, from.size(), to);
        lower.replace(at, from.size(), to);
        at += to.size();
    }
    return s;
}

bool isClsid(std::string_view s) { return s.size() >= 38 && s.front() == '{' && s.back() == '}'; }

std::string guessHandlerTitle(const Handler &h)
{
    if (!isClsid(h.name) && !nss::trim(h.name).empty())
        return std::string(nss::trim(h.name));
    if (!nss::trim(h.className).empty())
        return std::string(nss::trim(h.className));
    return h.clsid.empty() ? h.name : h.clsid;
}

std::string hive(bool user) { return user ? "HKCU\\Software\\Classes\\" : "HKCR\\"; }

} // namespace

const std::vector<Type> &types()
{
    static const std::vector<Type> list = {
        { "*", Files },
        { "AllFilesystemObjects", Files | Folders },
        { "Directory", Folders },
        { "Folder", Folders },
        { "Directory\\Background", Background },
        { "Drive", Drives },
    };
    return list;
}

std::string menuTitle(std::string_view raw)
{
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\t')
            break; // a shortcut key follows
        if (c == '&') {
            if (i + 1 < raw.size() && raw[i + 1] == '&') {
                out += '&';
                ++i;
            }
            continue;
        }
        out += c;
    }
    std::string_view t = nss::trim(out);
    while (!t.empty() && t.back() == '.')
        t = nss::trim(t.substr(0, t.size() - 1));
    return std::string(t);
}

std::string commandProgram(std::string_view command)
{
    auto c = nss::trim(command);
    if (c.empty())
        return {};
    if (c.front() == '"') {
        auto end = c.find('"', 1);
        return std::string(c.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1));
    }
    // Unquoted: up to the first ".exe" (paths may have spaces), else the first word.
    auto lower = nss::toLower(c);
    auto exe = lower.find(".exe");
    if (exe != std::string::npos)
        return std::string(c.substr(0, exe + 4));
    auto sp = c.find(' ');
    return std::string(c.substr(0, sp));
}

namespace {

// A command line's first part and the rest: as commandProgram(), plus what follows.
std::pair<std::string, std::string_view> splitCommand(std::string_view c)
{
    c = nss::trim(c);
    if (c.empty())
        return {};
    if (c.front() == '"') {
        auto end = c.find('"', 1);
        if (end == std::string_view::npos)
            return { std::string(c.substr(1)), {} };
        return { std::string(c.substr(1, end - 1)), nss::trim(c.substr(end + 1)) };
    }
    auto exe = nss::toLower(c).find(".exe");
    auto stop = exe != std::string::npos ? exe + 4 : c.find(' ');
    if (stop == std::string_view::npos || stop >= c.size())
        return { std::string(c), {} };
    return { std::string(c.substr(0, stop)), nss::trim(c.substr(stop)) };
}

// "C:\\x\\rundll32.exe" -> "rundll32"
std::string programName(std::string_view program)
{
    auto slash = program.find_last_of("\\/");
    auto name = nss::toLower(program.substr(slash == std::string_view::npos ? 0 : slash + 1));
    if (name.ends_with(".exe"))
        name.resize(name.size() - 4);
    return name;
}

} // namespace

std::string expandVars(std::string_view text, const std::function<std::string(std::string_view)> &env)
{
    std::string out(text);
    if (!env)
        return out;
    for (size_t at = 0; (at = out.find('%', at)) != std::string::npos;) {
        auto end = out.find('%', at + 1);
        if (end == std::string::npos)
            break;
        auto name = std::string_view(out).substr(at + 1, end - at - 1);
        // A variable's name: "ProgramFiles(x86)", "LOCALAPPDATA". "%1", "%V" and "%*" aren't.
        bool plausible = name.size() >= 2 && !std::isdigit(static_cast<unsigned char>(name[0])) &&
                         name.find_first_of(" \t\"\\/%:*") == std::string_view::npos;
        std::string value = plausible ? env(name) : std::string{};
        if (value.empty()) {
            at = plausible ? end + 1 : at + 1;
            continue;
        }
        out.replace(at, end - at + 1, value);
        at += value.size();
    }
    return out;
}

bool isFullPath(std::string_view path)
{
    path = nss::trim(path);
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
        (path[2] == '\\' || path[2] == '/'))
        return true;
    return path.size() >= 5 && path.substr(0, 2) == "\\\\" && path[2] != '?' && path[2] != '.';
}

std::string commandTarget(std::string_view command, const std::function<std::string(std::string_view)> &env)
{
    auto expanded = expandVars(nss::trim(command), env);
    auto [program, rest] = splitCommand(expanded);
    auto name = programName(program);
    std::string file;
    if (name == "rundll32") {
        // rundll32 "C:\x\a.dll",Entry  /  rundll32 C:\x\a.dll,Entry
        if (!rest.empty() && rest.front() == '"') {
            auto end = rest.find('"', 1);
            file = std::string(rest.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1));
        } else {
            file = std::string(rest.substr(0, rest.find(',')));
        }
    } else if (name == "cmd") {
        // cmd.exe /s /c ""C:\x\run.cmd" "%V"": the program after /c or /k.
        while (!rest.empty() && rest.front() == '/') {
            auto sw = nss::toLower(rest.substr(0, rest.find(' ')));
            rest = nss::trim(rest.substr(std::min(rest.size(), sw.size())));
            if (sw == "/c" || sw == "/k")
                break;
        }
        while (rest.size() >= 2 && rest[0] == '"' && rest[1] == '"')
            rest = rest.substr(1); // /c ""C:\x y\run.cmd" args": the outer quote
        file = splitCommand(rest).first;
    } else if (name == "wscript" || name == "cscript" || name == "mshta") {
        // The first argument that isn't a switch: the script.
        while (!rest.empty() && rest.front() == '/') {
            auto sp = rest.find(' ');
            rest = sp == std::string_view::npos ? std::string_view{} : nss::trim(rest.substr(sp));
        }
        file = splitCommand(rest).first;
    }
    file = std::string(nss::trim(file));
    return !file.empty() && isFullPath(file) ? file : program;
}

bool isWindowsFile(std::string_view path, std::string_view systemRoot)
{
    std::string s(nss::trim(path));
    if (!s.empty() && s.front() == '@')
        s.erase(0, 1); // "@shell32.dll,-8506"
    auto comma = s.rfind(',');
    if (comma != std::string::npos && s.find_first_not_of("-0123456789 ", comma + 1) == std::string::npos)
        s.resize(comma);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    std::string root(nss::trim(systemRoot));
    while (!root.empty() && (root.back() == '\\' || root.back() == '/'))
        root.pop_back();
    s = replaceIcase(s, "%SystemRoot%", root);
    s = replaceIcase(s, "%windir%", root);
    std::replace(s.begin(), s.end(), '/', '\\');
    auto lower = nss::toLower(s);
    if (lower.empty())
        return false;
    if (lower.find('\\') == std::string::npos) {
        // A bare name: Windows finds it in System32.
        return contains(kWindowsPrograms, lower) || lower.ends_with(".dll") || lower.ends_with(".mui");
    }
    auto lroot = nss::toLower(root);
    if (!lroot.empty() && lower.rfind(lroot + "\\", 0) == 0)
        return lower.find("\\driverstore\\") == std::string::npos; // drivers come from their vendors
    for (std::string_view part : { "\\windows defender\\", "\\windows sidebar\\", "\\windows nt\\", "\\windows mail\\",
                                   "\\windows media player\\", "\\windows photo viewer\\" })
        if (lower.find(part) != std::string::npos)
            return true;
    return false;
}

std::string describeWhere(unsigned where)
{
    std::vector<std::string> parts;
    if (where & Files)
        parts.push_back("files");
    if (where & Folders)
        parts.push_back("folders");
    if (where & Background)
        parts.push_back("folder background");
    if (where & Drives)
        parts.push_back("drives");
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
        out += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    if (!out.empty())
        out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::vector<Item> collect(const Scan &scan, const Resolver &resolve, std::string_view systemRoot, const FileSystem &files)
{
    auto whereOf = [](const std::string &type) {
        for (const auto &t : types())
            if (nss::iequals(t.key, type))
                return t.where;
        return 0u;
    };
    std::vector<Item> items;
    std::map<std::string, size_t> byTitle;  // verbs: lower-case title -> index
    std::map<std::string, size_t> byHandler; // handlers: lower-case CLSID (or name) -> index
    // Per item: how many of its keys have their file, and the first one that's gone.
    struct Check
    {
        int found = 0;
        std::string missing;
    };
    std::vector<Check> checks;
    auto check = [&](size_t item, const std::string &file) {
        if (!files.exists || !isFullPath(file))
            return; // can't tell: a bare name is looked up on PATH or in System32
        if (checks.size() <= item)
            checks.resize(item + 1);
        if (files.exists(file))
            ++checks[item].found;
        else if (checks[item].missing.empty())
            checks[item].missing = file;
    };

    for (const auto &v : scan.verbs) {
        if (v.hiddenByApp || v.key.empty())
            continue;
        auto key = nss::toLower(v.key);
        if (contains(kWindowsVerbs, key) || key.rfind("windows.", 0) == 0)
            continue;
        std::string raw = !nss::trim(v.muiVerb).empty() ? v.muiVerb : !nss::trim(v.text).empty() ? v.text : v.key;
        raw = std::string(nss::trim(raw));
        if (raw.empty())
            continue;
        if (raw.front() == '@') {
            if (isWindowsFile(raw, systemRoot))
                continue; // "@shell32.dll,-8506": Windows' own string
            raw = resolve ? resolve(raw) : std::string{};
        }
        auto program = commandProgram(v.command);
        auto target = commandTarget(v.command, files.env);
        if (!program.empty() && isWindowsFile(program, systemRoot)) {
            // Windows' own, unless it only runs another program's file:
            // rundll32 "C:\Program Files\App\menu.dll",Run.
            if (!isFullPath(target) || isWindowsFile(target, systemRoot) || nss::iequals(target, program))
                continue;
            program = target;
        }
        if (program.empty() && !v.icon.empty()) {
            program = std::string(nss::trim(v.icon));
            if (program.size() >= 2 && program.front() == '"')
                program = commandProgram(program);
            auto comma = program.rfind(',');
            if (comma != std::string::npos)
                program.resize(comma);
        }
        auto title = menuTitle(raw);
        if (title.empty())
            continue; // a string that can't be read: nothing to show or match
        if (v.command.empty() && !v.submenu && v.icon.empty() && nss::trim(v.muiVerb).empty() && nss::trim(v.text).empty())
            continue; // a bare key, no command: Windows shows nothing for it
        auto k = nss::toLower(title);
        auto it = byTitle.find(k);
        if (it == byTitle.end()) {
            Item item;
            item.kind = "verb";
            item.name = v.key;
            item.title = title;
            item.program = program;
            item.shiftOnly = v.extended;
            it = byTitle.emplace(k, items.size()).first;
            items.push_back(std::move(item));
        }
        auto &item = items[it->second];
        item.where |= whereOf(v.type);
        item.shiftOnly = item.shiftOnly && v.extended; // shown without Shift anywhere: not shift-only
        item.submenu = item.submenu || v.submenu;
        if (item.program.empty())
            item.program = program;
        item.keys.push_back(hive(v.user) + v.type + "\\shell\\" + v.key);
        if (!nss::trim(v.command).empty())
            check(it->second, target);
    }

    for (const auto &h : scan.handlers) {
        auto lname = nss::toLower(h.name + " " + h.className);
        if (lname.find("nilesoft") != std::string::npos)
            continue; // Shell itself
        if (nss::trim(h.server).empty())
            continue; // nothing Explorer can load, so nothing shows
        if (isWindowsFile(h.server, systemRoot))
            continue;
        auto k = nss::toLower(h.clsid.empty() ? h.name : h.clsid);
        auto it = byHandler.find(k);
        if (it == byHandler.end()) {
            Item item;
            item.kind = "handler";
            item.name = guessHandlerTitle(h);
            item.title = menuTitle(item.name);
            item.titleKnown = false;
            item.program = h.server;
            it = byHandler.emplace(k, items.size()).first;
            items.push_back(std::move(item));
        }
        auto &item = items[it->second];
        item.where |= whereOf(h.type);
        item.keys.push_back(hive(h.user) + h.type + "\\shellex\\ContextMenuHandlers\\" + h.name);
        std::string server(nss::trim(h.server));
        if (server.size() >= 2 && server.front() == '"' && server.back() == '"')
            server = server.substr(1, server.size() - 2);
        check(it->second, expandVars(server, files.env));
    }

    for (size_t i = 0; i < checks.size() && i < items.size(); ++i)
        if (checks[i].found == 0)
            items[i].missing = checks[i].missing;
    for (auto &item : items) {
        std::sort(item.keys.begin(), item.keys.end());
        item.keys.erase(std::unique(item.keys.begin(), item.keys.end()), item.keys.end());
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        return nss::toLower(a.title) < nss::toLower(b.title);
    });
    return items;
}

// --- The registry (read-only) ---------------------------------------------------

namespace {

std::wstring readValue(HKEY key, const wchar_t *sub, const wchar_t *name, bool *present = nullptr)
{
    DWORD type = 0, size = 0;
    if (present)
        *present = false;
    if (RegGetValueW(key, sub, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, nullptr, &size) !=
        ERROR_SUCCESS) {
        // A value of another type (Extended is often an empty REG_SZ, but not always).
        if (present && RegGetValueW(key, sub, name, RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
            *present = true;
        return {};
    }
    if (present)
        *present = true;
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    size = DWORD(s.size() * sizeof(wchar_t));
    if (RegGetValueW(key, sub, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, s.data(), &size) !=
        ERROR_SUCCESS)
        return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    if (type == REG_EXPAND_SZ) {
        wchar_t buf[2048];
        DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, DWORD(std::size(buf)));
        if (n > 0 && n <= std::size(buf))
            s = buf;
    }
    return s;
}

bool hasValue(HKEY key, const wchar_t *name)
{
    bool present = false;
    readValue(key, nullptr, name, &present);
    return present;
}

std::vector<std::wstring> subkeys(HKEY key)
{
    std::vector<std::wstring> out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = DWORD(std::size(name));
        auto rc = RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc == ERROR_SUCCESS)
            out.emplace_back(name, len);
    }
    return out;
}

struct Key
{
    HKEY h = nullptr;
    Key(HKEY root, const std::wstring &path)
    {
        if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
            h = nullptr;
    }
    ~Key()
    {
        if (h)
            RegCloseKey(h);
    }
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;
    explicit operator bool() const { return h != nullptr; }
};

} // namespace

Scan scan()
{
    Scan s;
    for (bool user : { false, true }) {
        HKEY root = user ? HKEY_CURRENT_USER : HKEY_CLASSES_ROOT;
        std::wstring prefix = user ? L"Software\\Classes\\" : L"";
        for (const auto &t : types()) {
            auto type = nss::widen(t.key);
            if (Key shell{ root, prefix + type + L"\\shell" }) {
                for (const auto &name : subkeys(shell.h)) {
                    Key k{ shell.h, name };
                    if (!k)
                        continue;
                    Verb v;
                    v.type = t.key;
                    v.user = user;
                    v.key = nss::narrow(name);
                    v.muiVerb = nss::narrow(readValue(k.h, nullptr, L"MUIVerb"));
                    v.text = nss::narrow(readValue(k.h, nullptr, L""));
                    v.command = nss::narrow(readValue(k.h, L"command", L""));
                    v.icon = nss::narrow(readValue(k.h, nullptr, L"Icon"));
                    v.submenu = hasValue(k.h, L"SubCommands") || hasValue(k.h, L"ExtendedSubCommandsKey");
                    v.extended = hasValue(k.h, L"Extended");
                    v.hiddenByApp = hasValue(k.h, L"ProgrammaticAccessOnly") || hasValue(k.h, L"LegacyDisable");
                    s.verbs.push_back(std::move(v));
                }
            }
            if (Key handlers{ root, prefix + type + L"\\shellex\\ContextMenuHandlers" }) {
                for (const auto &name : subkeys(handlers.h)) {
                    Handler h;
                    h.type = t.key;
                    h.user = user;
                    h.name = nss::narrow(name);
                    auto clsid = name.size() >= 38 && name.front() == L'{' ? name : readValue(handlers.h, name.c_str(), L"");
                    h.clsid = nss::narrow(clsid);
                    if (!clsid.empty()) {
                        h.className = nss::narrow(readValue(HKEY_CLASSES_ROOT, (L"CLSID\\" + clsid).c_str(), L""));
                        h.server = nss::narrow(readValue(HKEY_CLASSES_ROOT, (L"CLSID\\" + clsid + L"\\InprocServer32").c_str(), L""));
                    }
                    s.handlers.push_back(std::move(h));
                }
            }
        }
    }
    return s;
}

FileSystem realFiles()
{
    FileSystem f;
    f.env = [](std::string_view name) {
        wchar_t buf[32768];
        DWORD n = GetEnvironmentVariableW(nss::widen(name).c_str(), buf, DWORD(std::size(buf)));
        return n > 0 && n < std::size(buf) ? nss::narrow(std::wstring(buf, n)) : std::string{};
    };
    f.exists = [](const std::string &path) {
        DWORD a = GetFileAttributesW(nss::widen(path).c_str());
        if (a != INVALID_FILE_ATTRIBUTES)
            return !(a & FILE_ATTRIBUTE_DIRECTORY);
        // Only a file that's surely not there is gone: a drive that's offline
        // or a folder that can't be read says nothing.
        DWORD e = GetLastError();
        return e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
    };
    return f;
}

std::string resolveIndirect(const std::string &text)
{
    wchar_t out[512] = {};
    if (SHLoadIndirectString(nss::widen(text).c_str(), out, UINT(std::size(out)), nullptr) != S_OK)
        return {};
    return nss::narrow(out);
}

std::string systemRoot()
{
    wchar_t buf[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryW(buf, UINT(std::size(buf)));
    return n > 0 && n < std::size(buf) ? nss::narrow(buf) : std::string("C:\\Windows");
}

} // namespace otherapps
