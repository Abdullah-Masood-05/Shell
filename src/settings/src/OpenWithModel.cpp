#include "OpenWithModel.h"

#include "MenuModel.h"
#include "MenuWriter.h"
#include "Tr.h"

#include "nss/Literal.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace openwith {

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t'; }

std::wstring envVar(const wchar_t *name)
{
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring{};
}

std::wstring expandEnv(const std::wstring &s)
{
    if (s.find(L'%') == std::wstring::npos)
        return s;
    wchar_t buf[32768];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, DWORD(std::size(buf)));
    return n > 0 && n <= std::size(buf) ? std::wstring(buf) : s;
}

std::wstring lowerW(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

// "IntelliJ IDEA 2024.10" sorts after "IntelliJ IDEA 2024.9".
bool naturalLess(const std::wstring &a, const std::wstring &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::iswdigit(a[i]) && std::iswdigit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::iswdigit(a[i2]))
                ++i2;
            while (j2 < b.size() && std::iswdigit(b[j2]))
                ++j2;
            auto na = a.substr(i, i2 - i), nb = b.substr(j, j2 - j);
            na.erase(0, std::min(na.find_first_not_of(L'0'), na.size()));
            nb.erase(0, std::min(nb.find_first_not_of(L'0'), nb.size()));
            if (na.size() != nb.size())
                return na.size() < nb.size();
            if (na != nb)
                return na < nb;
            i = i2;
            j = j2;
            continue;
        }
        wchar_t ca = wchar_t(std::towlower(a[i])), cb = wchar_t(std::towlower(b[j]));
        if (ca != cb)
            return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

// The first existing file for a path whose components may hold '*' (newest
// version first when several match).
std::wstring resolveGlob(const std::wstring &pattern)
{
    if (pattern.find(L'*') == std::wstring::npos)
        return fileExists(pattern) ? pattern : std::wstring{};
    auto star = pattern.find(L'*');
    auto sepBefore = pattern.rfind(L'\\', star);
    auto sepAfter = pattern.find(L'\\', star);
    if (sepBefore == std::wstring::npos)
        return {};
    std::wstring dir = pattern.substr(0, sepBefore);
    std::wstring component = pattern.substr(sepBefore + 1, sepAfter == std::wstring::npos ? std::wstring::npos
                                                                                            : sepAfter - sepBefore - 1);
    std::wstring rest = sepAfter == std::wstring::npos ? std::wstring{} : pattern.substr(sepAfter);
    bool last = rest.empty();

    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((dir + L"\\" + component).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (last != isDir)
            names.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) { return naturalLess(b, a); });
    for (const auto &n : names) {
        auto found = resolveGlob(dir + L"\\" + n + rest);
        if (!found.empty())
            return found;
    }
    return {};
}

std::wstring expandTokens(std::string_view candidate, const Env &env)
{
    std::wstring s = nss::widen(candidate);
    struct T
    {
        const wchar_t *token;
        const std::wstring *value;
    };
    const T tokens[] = { { L"%LOCALAPPDATA%", &env.localAppData },     { L"%ProgramFiles(x86)%", &env.programFilesX86 },
                         { L"%ProgramFiles%", &env.programFiles },     { L"%WINDIR%", &env.windir },
                         { L"%USERPROFILE%", &env.userProfile } };
    for (const auto &t : tokens) {
        auto at = s.find(t.token);
        if (at == std::wstring::npos)
            continue;
        if (t.value->empty())
            return {};
        s.replace(at, wcslen(t.token), *t.value);
    }
    return s.find(L'%') == std::wstring::npos ? s : std::wstring{};
}

Preset preset(std::string id, std::string name, std::string args, std::vector<std::string> candidates,
              std::vector<Preset::Probe> probes, Scope scope = Scope::All, bool suggested = true)
{
    Preset p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.args = std::move(args);
    p.candidates = std::move(candidates);
    p.probes = std::move(probes);
    p.scope = scope;
    p.suggested = suggested;
    return p;
}

// A JetBrains IDE: standalone installs, Toolbox 2.x installs, then the
// Toolbox's shell scripts.
Preset jetbrains(std::string id, std::string name, const std::string &folder, const std::string &exe,
                 const std::string &script)
{
    return preset(std::move(id), std::move(name), "{path}",
                  { "%ProgramFiles%\\JetBrains\\" + folder + "*\\bin\\" + exe,
                    "%LOCALAPPDATA%\\Programs\\" + folder + "*\\bin\\" + exe,
                    "%LOCALAPPDATA%\\JetBrains\\Toolbox\\scripts\\" + script },
                  { { exe, "" }, { script, "" } });
}

std::string_view stripPrefix(std::string_view s, std::string_view prefix)
{
    return s.substr(0, prefix.size()) == prefix ? s.substr(prefix.size()) : s;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to)
{
    if (from.empty())
        return s;
    size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) {
        s.replace(at, from.size(), to);
        at += to.size();
    }
    return s;
}

std::string quoteArg(const std::string &s) { return "\"" + s + "\""; }

std::string folderOf(std::string_view path)
{
    auto slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? std::string{} : std::string(path.substr(0, slash));
}

std::string nameOf(std::string_view path)
{
    auto slash = path.find_last_of("\\/");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

const char *scopeWord(Scope s)
{
    switch (s) {
    case Scope::All: return "all";
    case Scope::Files: return "files";
    case Scope::Folders: return "folders";
    case Scope::Extensions: return "ext";
    }
    return "all";
}

std::vector<std::string> kindsOf(const Options &o)
{
    std::vector<std::string> k;
    if (o.files)
        k.push_back("file");
    if (o.folders)
        k.push_back("dir");
    if (o.background)
        k.push_back("back");
    if (o.desktop)
        k.push_back("desktop");
    return k;
}

std::string join(const std::vector<std::string> &v, std::string_view sep)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
        out += (i ? std::string(sep) : std::string{}) + v[i];
    return out;
}

// The kinds an app's item shows for, within the menu's locations.
std::vector<std::string> itemKinds(const Options &o, const App &a)
{
    std::vector<std::string> out;
    for (const auto &k : kindsOf(o)) {
        bool ok = a.scope == Scope::All || ((a.scope == Scope::Files || a.scope == Scope::Extensions) && k == "file") ||
                  (a.scope == Scope::Folders && k != "file");
        if (ok)
            out.push_back(k);
    }
    return out;
}

// ".py .js" -> "*.py|*.js" (ends with); "" when nothing usable.
std::string extensionsToFind(std::string_view ext)
{
    auto list = menus::extensionsToFind(ext);
    if (list.empty())
        return {};
    return "*" + replaceAll(list, "|", "|*");
}

std::string findToExtensions(std::string_view find)
{
    std::string out = replaceAll(std::string(find), "*", "");
    std::replace(out.begin(), out.end(), '|', ' ');
    return out;
}

// The quoted .nss form of a path (or an '@...' text): single quotes, else
// backticks, else double quotes (which don't expand '@', so they hold the
// absolute path).
std::string nssPathLiteral(const std::string &absolute, const Env &env)
{
    auto p = toNssPath(absolute, env);
    if (p.find('\'') == std::string::npos)
        return "'" + p + "'";
    if (p.find('`') == std::string::npos)
        return "`" + p + "`";
    return menus::MenuWriter::plainString(absolute);
}

// "  // x" -> "x" (the comment marker and one space after it); nullopt when
// the line isn't a lone line comment.
std::optional<std::string> commentBody(std::string_view line)
{
    size_t p = 0;
    while (p < line.size() && isBlank(line[p]))
        ++p;
    if (line.substr(p, 2) != "//")
        return std::nullopt;
    size_t q = p + 2;
    if (q < line.size() && line[q] == ' ')
        ++q;
    return std::string(line.substr(q));
}

std::vector<std::string> splitLines(std::string_view text)
{
    std::vector<std::string> lines;
    size_t b = 0;
    for (size_t k = 0; k <= text.size(); ++k) {
        if (k == text.size() || text[k] == '\n') {
            auto l = text.substr(b, k - b);
            if (!l.empty() && l.back() == '\r')
                l.remove_suffix(1);
            if (k < text.size() || !l.empty())
                lines.emplace_back(l);
            b = k + 1;
        }
    }
    return lines;
}

// Brace depth change of a line, outside strings.
int braceDelta(std::string_view s, bool *opened = nullptr)
{
    int d = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"' && i + 1 < s.size())
                ++i;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
            break;
        if (c == '"' || c == '\'' || c == '`')
            quote = c;
        else if (c == '{') {
            ++d;
            if (opened)
                *opened = true;
        } else if (c == '}')
            --d;
    }
    return d;
}

bool isEditorsMenuLine(std::string_view s)
{
    auto t = nss::trim(s);
    if (t.substr(0, 5) != "menu(" && t.substr(0, 5) != "menu ")
        return false;
    auto at = t.find("title");
    if (at == std::string_view::npos)
        return false;
    auto rest = nss::trim(t.substr(at + 5));
    if (rest.empty() || rest.front() != '=')
        return false;
    rest = nss::trim(rest.substr(1));
    return rest.substr(0, 11) == "loc.editors" &&
           (rest.size() == 11 || !nss::NssLexer::isIdentChar(rest[11]));
}

const nss::Node *findEditorsNode(const nss::NssDocument &doc, const std::vector<nss::Node> &nodes)
{
    for (const auto &n : nodes) {
        if (n.kind != nss::NodeKind::MenuDecl)
            continue;
        if (n.menuKind == nss::MenuKind::Menu) {
            for (const auto &p : n.props)
                if (p.name == "title" && p.hasValue && nss::trim(doc.slice(p.valueSpan)) == "loc.editors")
                    return &n;
            if (auto inner = findEditorsNode(doc, n.children))
                return inner;
        }
    }
    return nullptr;
}

std::vector<size_t> lineStarts(std::string_view text)
{
    std::vector<size_t> starts{ 0 };
    for (size_t k = 0; k < text.size(); ++k)
        if (text[k] == '\n' || (text[k] == '\r' && (k + 1 >= text.size() || text[k + 1] != '\n')))
            starts.push_back(k + 1);
    return starts;
}

size_t lineEnd(std::string_view text, const std::vector<size_t> &starts, size_t line)
{
    size_t b = starts[line - 1];
    auto e = text.find_first_of("\r\n", b);
    return e == std::string_view::npos ? text.size() : e;
}

// A batch file (a JetBrains Toolbox script): Shell would flash a console for it.
bool isScript(std::string_view path)
{
    auto lower = nss::toLower(path);
    if (lower.size() < 4)
        return false;
    auto ext = std::string_view(lower).substr(lower.size() - 4);
    return ext == ".cmd" || ext == ".bat";
}

// The program a launcher script starts: the first quoted "...exe" in it that
// exists. Toolbox scripts look like: start "" "C:\...\bin\pycharm64.exe" %*
std::string scriptTarget(const std::string &script)
{
    std::string text;
    if (!nss::readFile(fs::path(nss::widen(script)), text) || text.size() > 64 * 1024)
        return {};
    size_t at = 0;
    while ((at = text.find('"', at)) != std::string::npos) {
        auto end = text.find('"', at + 1);
        if (end == std::string::npos)
            break;
        auto inner = text.substr(at + 1, end - at - 1);
        auto lower = nss::toLower(inner);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0 && fileExists(nss::widen(inner)))
            return inner;
        at = end + 1;
    }
    return {};
}

} // namespace

// --- Env --------------------------------------------------------------------

Env Env::current()
{
    Env e;
    e.localAppData = envVar(L"LOCALAPPDATA");
    e.programFiles = envVar(L"ProgramW6432");
    if (e.programFiles.empty())
        e.programFiles = envVar(L"ProgramFiles");
    e.programFilesX86 = envVar(L"ProgramFiles(x86)");
    e.windir = envVar(L"WINDIR");
    if (e.windir.empty())
        e.windir = envVar(L"SystemRoot");
    e.userProfile = envVar(L"USERPROFILE");
    auto path = envVar(L"PATH");
    size_t b = 0;
    while (b <= path.size()) {
        auto semi = path.find(L';', b);
        auto part = path.substr(b, semi == std::wstring::npos ? std::wstring::npos : semi - b);
        part.erase(std::remove(part.begin(), part.end(), L'"'), part.end());
        while (!part.empty() && (part.back() == L'\\' || part.back() == L' '))
            part.pop_back();
        if (!part.empty())
            e.path.push_back(expandEnv(part));
        if (semi == std::wstring::npos)
            break;
        b = semi + 1;
    }
    return e;
}

bool fileExists(const std::wstring &path)
{
    if (path.empty())
        return false;
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring findOnPath(std::wstring_view name, const std::vector<std::wstring> &path)
{
    if (name.empty() || name.find_first_of(L"\\/") != std::wstring_view::npos)
        return {};
    std::vector<std::wstring> names;
    if (name.find(L'.') != std::wstring_view::npos) {
        names.emplace_back(name);
    } else {
        auto ext = envVar(L"PATHEXT");
        if (ext.empty())
            ext = L".COM;.EXE;.BAT;.CMD";
        size_t b = 0;
        while (b <= ext.size()) {
            auto semi = ext.find(L';', b);
            auto e = ext.substr(b, semi == std::wstring::npos ? std::wstring::npos : semi - b);
            if (!e.empty())
                names.push_back(std::wstring(name) + lowerW(e));
            if (semi == std::wstring::npos)
                break;
            b = semi + 1;
        }
    }
    for (const auto &dir : path)
        for (const auto &n : names) {
            auto candidate = dir + L"\\" + n;
            if (fileExists(candidate))
                return candidate;
        }
    return {};
}

// --- Presets ------------------------------------------------------------------

const std::vector<Preset> &presets()
{
    static const std::vector<Preset> list = [] {
        std::vector<Preset> v;
        v.push_back(preset("vscode", "VS Code", "{path}",
                           { "%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe",
                             "%ProgramFiles%\\Microsoft VS Code\\Code.exe",
                             "%USERPROFILE%\\scoop\\apps\\vscode\\current\\Code.exe" },
                           { { "code.cmd", "..\\Code.exe" } }));
        v.push_back(preset("vscode-insiders", "VS Code Insiders", "{path}",
                           { "%LOCALAPPDATA%\\Programs\\Microsoft VS Code Insiders\\Code - Insiders.exe",
                             "%ProgramFiles%\\Microsoft VS Code Insiders\\Code - Insiders.exe" },
                           { { "code-insiders.cmd", "..\\Code - Insiders.exe" } }));
        v.push_back(preset("zed", "Zed", "{path}",
                           { "%LOCALAPPDATA%\\Programs\\Zed\\Zed.exe", "%ProgramFiles%\\Zed\\Zed.exe",
                             "%USERPROFILE%\\scoop\\apps\\zed\\current\\zed.exe" },
                           { { "zed.exe", "..\\Zed.exe" } }));
        v.push_back(preset("cursor", "Cursor", "{path}",
                           { "%LOCALAPPDATA%\\Programs\\cursor\\Cursor.exe", "%ProgramFiles%\\Cursor\\Cursor.exe" },
                           { { "cursor.cmd", "..\\..\\..\\Cursor.exe" } }));
        v.push_back(preset("windsurf", "Windsurf", "{path}",
                           { "%LOCALAPPDATA%\\Programs\\Windsurf\\Windsurf.exe",
                             "%ProgramFiles%\\Windsurf\\Windsurf.exe" },
                           { { "windsurf.cmd", "..\\Windsurf.exe" } }));
        v.push_back(preset("sublime", "Sublime Text", "{path}",
                           { "%ProgramFiles%\\Sublime Text\\sublime_text.exe",
                             "%ProgramFiles%\\Sublime Text 3\\sublime_text.exe",
                             "%USERPROFILE%\\scoop\\apps\\sublime-text\\current\\sublime_text.exe" },
                           { { "subl.exe", "sublime_text.exe" } }));
        v.push_back(preset("notepad++", "Notepad++", "{path}",
                           { "%ProgramFiles%\\Notepad++\\notepad++.exe", "%ProgramFiles(x86)%\\Notepad++\\notepad++.exe" },
                           { { "notepad++.exe", "" } }, Scope::Files));
        v.push_back(jetbrains("idea", "IntelliJ IDEA", "IntelliJ IDEA", "idea64.exe", "idea.cmd"));
        v.push_back(jetbrains("pycharm", "PyCharm", "PyCharm", "pycharm64.exe", "pycharm.cmd"));
        v.push_back(jetbrains("webstorm", "WebStorm", "WebStorm", "webstorm64.exe", "webstorm.cmd"));
        v.push_back(jetbrains("rider", "Rider", "JetBrains Rider", "rider64.exe", "rider.cmd"));
        v.push_back(jetbrains("clion", "CLion", "CLion", "clion64.exe", "clion.cmd"));
        v.push_back(jetbrains("goland", "GoLand", "GoLand", "goland64.exe", "goland.cmd"));
        v.push_back(jetbrains("phpstorm", "PhpStorm", "PhpStorm", "phpstorm64.exe", "phpstorm.cmd"));
        v.push_back(jetbrains("rustrover", "RustRover", "RustRover", "rustrover64.exe", "rustrover.cmd"));
        v.push_back(preset("nvim-qt", "Neovim Qt", "{path}", { "%ProgramFiles%\\Neovim\\bin\\nvim-qt.exe" },
                           { { "nvim-qt.exe", "" } }));
        // Neovim in a Windows Terminal tab: the item runs wt.exe; detection finds nvim.exe.
        v.push_back(preset("nvim", "Neovim", "new-tab -d {dir} nvim {path}",
                           { "%ProgramFiles%\\Neovim\\bin\\nvim.exe", "%LOCALAPPDATA%\\Programs\\Neovim\\bin\\nvim.exe",
                             "%USERPROFILE%\\scoop\\apps\\neovim\\current\\bin\\nvim.exe" },
                           { { "nvim.exe", "" } }));
        v.push_back(preset("devenv", "Visual Studio", "{path}",
                           { "%ProgramFiles%\\Microsoft Visual Studio\\*\\*\\Common7\\IDE\\devenv.exe",
                             "%ProgramFiles(x86)%\\Microsoft Visual Studio\\*\\*\\Common7\\IDE\\devenv.exe" },
                           { { "devenv.exe", "" } }));
        // Terminals and Notepad are already in the Terminal and Develop menus: off until picked.
        v.push_back(preset("wt", "Windows Terminal", "-d {dir}", { "%LOCALAPPDATA%\\Microsoft\\WindowsApps\\wt.exe" },
                           { { "wt.exe", "" } }, Scope::All, false));
        v.back().icon = "@package.path(\"WindowsTerminal\")\\WindowsTerminal.exe";
        v.push_back(preset("pwsh", "PowerShell 7", "-NoExit -WorkingDirectory {dir}",
                           { "%ProgramFiles%\\PowerShell\\7\\pwsh.exe", "%ProgramFiles%\\PowerShell\\7-preview\\pwsh.exe",
                             "%LOCALAPPDATA%\\Microsoft\\WindowsApps\\pwsh.exe" },
                           { { "pwsh.exe", "" } }, Scope::All, false));
        v.push_back(preset("git-bash", "Git Bash", "--cd={dir}",
                           { "%ProgramFiles%\\Git\\git-bash.exe", "%LOCALAPPDATA%\\Programs\\Git\\git-bash.exe",
                             "%USERPROFILE%\\scoop\\apps\\git\\current\\git-bash.exe" },
                           { { "git.exe", "..\\git-bash.exe" } }, Scope::All, false));
        v.push_back(preset("notepad", "Notepad", "{path}", { "%WINDIR%\\System32\\notepad.exe" }, {}, Scope::Files,
                           false));

        // Their own menu entries, for when the program path doesn't say (see linkNatives()).
        auto own = [&](std::string_view id, std::vector<std::string> keys, std::vector<std::string> titles,
                       std::string handlerTitle = {}) {
            for (auto &p : v)
                if (p.id == id) {
                    p.nativeKeys = std::move(keys);
                    p.nativeTitles = std::move(titles);
                    p.handlerTitle = std::move(handlerTitle);
                }
        };
        own("vscode", { "vscode" }, { "Open with Code" });
        own("vscode-insiders", { "vscodeinsiders" }, { "Open with Code - Insiders" });
        own("zed", {}, { "Open with Zed" });
        own("cursor", { "cursor" }, { "Open with Cursor" });
        own("windsurf", { "windsurf" }, { "Open with Windsurf" });
        own("sublime", {}, { "Open with Sublime Text" });
        own("notepad++", { "notepad++", "anotepad++", "anotepad++64" }, { "Edit with Notepad++" }, "Edit with Notepad++");
        own("pwsh", { "powershell7x64", "powershell7x86", "powershell7-previewx64" },
            { "PowerShell 7", "PowerShell 7-preview" });
        own("git-bash", { "git_shell", "git_gui", "git_bash" }, { "Open Git Bash here", "Open Git GUI here" });
        return v;
    }();
    return list;
}

const Preset *findPreset(std::string_view id)
{
    for (const auto &p : presets())
        if (p.id == id)
            return &p;
    return nullptr;
}

std::string detect(const Preset &preset, const Env &env)
{
    for (const auto &c : preset.candidates) {
        auto pattern = expandTokens(c, env);
        if (pattern.empty())
            continue;
        auto found = resolveGlob(pattern);
        if (!found.empty())
            return nss::narrow(found);
    }
    for (const auto &probe : preset.probes) {
        auto found = findOnPath(nss::widen(probe.name), env.path);
        if (found.empty())
            continue;
        fs::path target = probe.relative.empty()
                              ? fs::path(found)
                              : (fs::path(found).parent_path() / fs::path(nss::widen(probe.relative))).lexically_normal();
        if (fileExists(target.wstring()))
            return nss::narrow(target.wstring());
    }
    return {};
}

std::vector<App> detectAll(const Env &env)
{
    std::vector<App> out;
    std::string wt;
    if (const auto *p = findPreset("wt"))
        wt = detect(*p, env);
    for (const auto &p : presets()) {
        App a;
        a.id = p.id;
        a.name = p.name;
        a.args = p.args;
        a.icon = p.icon;
        a.scope = p.scope;
        a.exe = detect(p, env);
        if (p.id == "nvim") {
            // nvim.exe runs in a Windows Terminal tab.
            if (!a.exe.empty() && !wt.empty()) {
                a.args = "new-tab -d {dir} \"" + a.exe + "\" {path}";
                a.icon = a.exe;
                a.exe = wt;
            } else {
                a.exe.clear();
            }
        }
        if (a.icon.empty() && isScript(a.exe))
            a.icon = scriptTarget(a.exe); // a Toolbox script: the IDE's own icon
        a.enabled = p.suggested && !a.exe.empty();
        out.push_back(std::move(a));
    }
    return out;
}

Model merge(const std::optional<Model> &saved, const std::vector<App> &detected)
{
    Model m;
    // Presets the file doesn't list: the ones found on this PC first.
    std::vector<App> rest = detected;
    std::stable_partition(rest.begin(), rest.end(), [](const App &a) { return !a.exe.empty(); });
    if (!saved) {
        m.apps = std::move(rest);
        return m;
    }
    m = *saved;
    for (auto &a : m.apps) {
        // A preset that moved (reinstalled elsewhere): the new place.
        if (a.id == "custom" || fileExists(nss::widen(a.exe)))
            continue;
        for (const auto &d : detected)
            if (d.id == a.id && !d.exe.empty() && d.id != "nvim")
                a.exe = d.exe;
    }
    for (const auto &d : rest) {
        bool listed = std::any_of(m.apps.begin(), m.apps.end(), [&](const App &a) { return a.id == d.id; });
        if (!listed) {
            App a = d;
            a.enabled = false;
            m.apps.push_back(std::move(a));
        }
    }
    return m;
}

// --- The apps' own menu entries ------------------------------------------------

namespace {

// A program or folder for comparing: lower case, backslashes, no quotes or
// ",index", no trailing backslash.
std::string pathKey(std::string_view raw)
{
    std::string s(nss::trim(raw));
    if (s.size() >= 2 && s.front() == '"') {
        auto end = s.find('"', 1);
        s = s.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    }
    auto comma = s.rfind(',');
    if (comma != std::string::npos && comma + 1 < s.size() &&
        s.find_first_not_of("-0123456789 ", comma + 1) == std::string::npos)
        s.resize(comma);
    std::replace(s.begin(), s.end(), '/', '\\');
    s = nss::toLower(nss::trim(s));
    while (s.size() > 3 && s.back() == '\\')
        s.pop_back();
    return s;
}

// The programs that stand for an app: its program, and its icon when that's
// a program too (Neovim runs in Windows Terminal; a Toolbox script has the
// IDE as its icon).
std::vector<std::string> appPrograms(const App &a)
{
    std::vector<std::string> out;
    if (!a.exe.empty())
        out.push_back(a.exe);
    auto icon = pathKey(a.icon);
    if (icon.size() > 4 && icon.compare(icon.size() - 4, 4, ".exe") == 0 && icon.find('@') == std::string::npos)
        out.push_back(a.icon);
    return out;
}

// The last part of each registry key an item comes from: "git_gui".
std::vector<std::string> keyNames(const otherapps::Item &item)
{
    std::vector<std::string> out{ nss::toLower(item.name) };
    for (const auto &k : item.keys) {
        auto slash = k.rfind('\\');
        out.push_back(nss::toLower(slash == std::string::npos ? k : k.substr(slash + 1)));
    }
    return out;
}

} // namespace

std::string installFolder(std::string_view program, const Env &env)
{
    auto p = pathKey(program);
    if (p.empty() || isScript(p))
        return {}; // a launcher script: its folder has other scripts
    auto dir = folderOf(p);
    auto leaf = nameOf(dir);
    if (leaf == "bin" || leaf == "cmd")
        dir = folderOf(dir); // "...\Git\cmd\git.exe", "...\IDEA\bin\idea64.exe"
    if (dir.size() < 3 || dir[1] != ':')
        return {};
    // At least two folders deep: "C:\Program Files\Git", not "C:\Tools".
    if (std::count(dir.begin(), dir.end(), '\\') < 2)
        return {};
    auto key = [](const std::wstring &w) { return w.empty() ? std::string{} : pathKey(nss::narrow(w)); };
    const std::string shared[] = {
        key(env.programFiles),
        key(env.programFilesX86),
        key(env.localAppData),
        key(env.localAppData) + "\\programs",
        key(env.localAppData) + "\\microsoft\\windowsapps",
        key(env.userProfile),
        key(env.userProfile) + "\\scoop\\shims",
        key(env.userProfile) + "\\appdata\\roaming",
    };
    for (const auto &s : shared)
        if (!s.empty() && dir == s)
            return {};
    auto win = key(env.windir);
    if (!win.empty() && (dir == win || dir.rfind(win + "\\", 0) == 0))
        return {};
    if (dir.find("\\windowsapps") != std::string::npos || dir.find("\\toolbox\\scripts") != std::string::npos)
        return {};
    return dir;
}

std::vector<int> nativeOwners(const std::vector<App> &apps, const std::vector<otherapps::Item> &items, const Env &env)
{
    struct Known
    {
        std::vector<std::string> programs; // pathKey()s
        std::vector<std::string> folders;  // install folders
        const Preset *preset = nullptr;
    };
    std::vector<Known> known(apps.size());
    for (size_t a = 0; a < apps.size(); ++a) {
        if (apps[a].exe.empty())
            continue;
        for (const auto &p : appPrograms(apps[a])) {
            known[a].programs.push_back(pathKey(p));
            auto dir = installFolder(p, env);
            if (!dir.empty())
                known[a].folders.push_back(dir);
        }
        known[a].preset = apps[a].id == "custom" ? nullptr : findPreset(apps[a].id);
    }

    std::vector<int> owners(items.size(), -1);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        auto program = pathKey(item.program);
        auto names = keyNames(item);
        int best = -1, bestScore = 0;
        size_t bestLength = 0;
        for (size_t a = 0; a < apps.size(); ++a) {
            const auto &k = known[a];
            int score = 0;
            size_t length = 0;
            if (!program.empty() && std::find(k.programs.begin(), k.programs.end(), program) != k.programs.end())
                score = 3;
            for (const auto &dir : k.folders)
                if (score < 3 && !program.empty() && program.rfind(dir + "\\", 0) == 0 && dir.size() > length) {
                    score = 2;
                    length = dir.size();
                }
            if (score == 0 && k.preset) {
                for (const auto &n : names)
                    if (std::find(k.preset->nativeKeys.begin(), k.preset->nativeKeys.end(), n) != k.preset->nativeKeys.end())
                        score = 1;
                for (const auto &t : k.preset->nativeTitles)
                    if (item.titleKnown && nss::iequals(t, item.title))
                        score = 1;
            }
            // The best kind of match wins; between folders, the deepest one.
            if (score > bestScore || (score == 2 && bestScore == 2 && length > bestLength)) {
                best = int(a);
                bestScore = score;
                bestLength = length;
            }
        }
        owners[i] = best;
    }
    return owners;
}

std::vector<int> linkNatives(Model &model, const std::vector<otherapps::Item> &items, const Env &env)
{
    auto owners = nativeOwners(model.apps, items, env);
    for (auto &a : model.apps)
        a.natives.clear();
    for (size_t i = 0; i < items.size(); ++i) {
        if (owners[i] < 0)
            continue;
        auto &a = model.apps[size_t(owners[i])];
        const auto &item = items[i];
        Hidden h{ item.kind, item.name, item.title };
        if (item.kind == "handler") {
            // A shell extension sets its own title: the preset knows it, else the name is the guess.
            const auto *p = a.id == "custom" ? nullptr : findPreset(a.id);
            if (p && !p->handlerTitle.empty())
                h.title = p->handlerTitle;
        }
        a.natives.push_back(std::move(h));
    }
    return owners;
}

std::vector<OtherRef> otherRows(const std::vector<otherapps::Item> &items, const std::vector<int> &owners,
                                const std::vector<Hidden> &hidden)
{
    std::vector<OtherRef> rows;
    std::vector<bool> used(hidden.size(), false);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        OtherRef r;
        r.item = int(i);
        for (size_t h = 0; h < hidden.size(); ++h) {
            const auto &hd = hidden[h];
            if (used[h] || hd.kind != item.kind)
                continue;
            // A verb by its title (the rule matches titles), a shell extension by its name.
            bool same = item.kind == "verb" ? nss::iequals(hd.title, item.title) : nss::iequals(hd.name, item.name);
            if (same) {
                r.hidden = int(h);
                used[h] = true;
                break;
            }
        }
        if (i < owners.size() && owners[i] >= 0 && r.hidden < 0)
            continue;
        rows.push_back(r);
    }
    // Leftovers of uninstalled programs first: they're the ones to hide.
    std::stable_partition(rows.begin(), rows.end(),
                          [&](const OtherRef &r) { return !items[size_t(r.item)].missing.empty(); });
    for (size_t h = 0; h < hidden.size(); ++h)
        if (!used[h])
            rows.push_back(OtherRef{ -1, int(h) });
    return rows;
}

// --- Paths in .nss ----------------------------------------------------------

namespace {

struct Root
{
    const char *expr;
    std::wstring dir;
};

// Longest folders first, so %ProgramFiles(x86)% wins over %ProgramFiles%.
std::vector<Root> roots(const Env &env)
{
    std::vector<Root> r;
    if (!env.windir.empty())
        r.push_back({ "@sys.bin", env.windir + L"\\System32" });
    r.push_back({ "@user.localappdata", env.localAppData });
    r.push_back({ "@sys.prog32", env.programFilesX86 });
    r.push_back({ "@sys.prog", env.programFiles });
    r.push_back({ "@user.home", env.userProfile });
    r.push_back({ "@sys.dir", env.windir });
    r.erase(std::remove_if(r.begin(), r.end(), [](const Root &x) { return x.dir.empty(); }), r.end());
    std::stable_sort(r.begin(), r.end(), [](const Root &a, const Root &b) { return a.dir.size() > b.dir.size(); });
    return r;
}

} // namespace

std::string toNssPath(std::string_view absolute, const Env &env)
{
    auto w = nss::widen(absolute);
    auto lw = lowerW(w);
    for (const auto &r : roots(env)) {
        auto ld = lowerW(r.dir);
        while (!ld.empty() && ld.back() == L'\\')
            ld.pop_back();
        if (lw.size() > ld.size() && lw.compare(0, ld.size(), ld) == 0 && lw[ld.size()] == L'\\')
            return std::string(r.expr) + nss::narrow(w.substr(ld.size()));
    }
    return std::string(absolute);
}

std::string fromNssPath(std::string_view nssPath, const Env &env)
{
    for (const auto &r : roots(env)) {
        std::string_view e = r.expr;
        if (nssPath.size() > e.size() && nssPath.substr(0, e.size()) == e && nssPath[e.size()] == '\\') {
            auto dir = r.dir;
            while (!dir.empty() && dir.back() == L'\\')
                dir.pop_back();
            return nss::narrow(dir) + std::string(nssPath.substr(e.size()));
        }
    }
    return std::string(nssPath);
}

// --- Argument templates -------------------------------------------------------

namespace {

struct Placeholder
{
    std::string_view name;
    std::string_view nss;
};
// Longest first where one is a prefix of another.
constexpr Placeholder kPlaceholders[] = {
    { "{paths}", "@sel(true)" },
    { "{path}", "@sel.path.quote" },
    { "{dir}", "\"@sel.dir\"" },
    { "{name}", "@sel.name" },
};

} // namespace

std::string argsToNss(std::string_view tmpl)
{
    std::string s(tmpl);
    for (const auto &p : kPlaceholders)
        s = replaceAll(s, p.name, p.nss);
    return s;
}

std::string nssToArgs(std::string_view text)
{
    std::string s(text);
    for (const auto &p : kPlaceholders)
        s = replaceAll(s, p.nss, p.name);
    return s;
}

bool usesAllSelected(std::string_view tmpl) { return tmpl.find("{paths}") != std::string_view::npos; }

std::string expandArgs(std::string_view tmpl, const Sample &sample)
{
    std::string paths;
    for (const auto &p : sample.paths)
        paths += (paths.empty() ? "" : " ") + quoteArg(p);
    std::string first = sample.paths.empty() ? std::string{} : sample.paths[0];
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        bool matched = false;
        for (const auto &p : kPlaceholders) {
            if (tmpl.substr(i, p.name.size()) != p.name)
                continue;
            if (p.name == "{paths}")
                out += paths;
            else if (p.name == "{path}")
                out += quoteArg(first);
            else if (p.name == "{dir}")
                out += quoteArg(sample.dir);
            else
                out += nameOf(first);
            i += p.name.size();
            matched = true;
            break;
        }
        if (!matched)
            out += tmpl[i++];
    }
    return out;
}

std::string commandLine(const App &app, const Sample &sample)
{
    auto args = expandArgs(app.args, sample);
    return quoteArg(app.exe) + (args.empty() ? "" : " " + args);
}

std::string validate(const App &app)
{
    if (nss::trim(app.name).empty())
        return nsp::tr("Give it a name; it's the item's title.");
    if (app.exe.empty())
        return nsp::tr("Pick the program it runs.");
    if (app.exe.find('@') != std::string::npos)
        return nsp::tr("The program path contains @, which Shell reads as an expression.");
    if (app.args.find('\'') != std::string::npos && app.args.find('`') != std::string::npos)
        return nsp::tr("The arguments can use ' or ` quotes, but not both.");
    if (app.scope == Scope::Extensions && extensionsToFind(app.extensions).empty())
        return nsp::tr("List at least one extension, like .py .js");
    return {};
}

// --- The generated file ------------------------------------------------------

std::string locationType(const Options &options) { return join(kindsOf(options), "|"); }

std::string itemTitle(const Options &options, const App &app)
{
    return options.submenu ? app.name : "Open in " + app.name;
}

bool showsFor(const Options &options, const App &app, std::string_view kind, std::string_view extension)
{
    auto kinds = itemKinds(options, app);
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
        return false;
    if (app.scope != Scope::Extensions)
        return true;
    auto list = menus::extensionsToFind(app.extensions);
    auto ext = nss::toLower(extension);
    size_t b = 0;
    while (b <= list.size()) {
        auto bar = list.find('|', b);
        if (list.substr(b, bar == std::string::npos ? std::string::npos : bar - b) == ext)
            return true;
        if (bar == std::string::npos)
            break;
        b = bar + 1;
    }
    return false;
}

std::string generate(const Model &model, const Env &env, nss::Eol eol)
{
    const std::string nl(nss::eolString(eol));
    const auto &o = model.options;
    std::string out;
    out += std::string(kHeader) + nl;
    out += "// Edit these items on the Open with page of Nilesoft Shell Pro." + nl;
    out += "// options: layout=" + std::string(o.submenu ? "submenu" : "top") + " pos=" + std::string(kPositionWord) +
           " show=" +
           (kindsOf(o).empty() ? std::string("none") : locationType(o)) +
           " title=" + menus::MenuWriter::plainString(o.title) + nl;

    auto menuType = locationType(o);
    auto shows = [&](const App &a) { return a.enabled && validate(a).empty() && !itemKinds(o, a).empty(); };
    bool anyShown = false, anyMultiple = false;
    for (const auto &a : model.apps) {
        if (a.exe.empty())
            continue;
        anyShown |= shows(a);
        anyMultiple |= shows(a) && usesAllSelected(a.args);
    }
    const bool inMenu = o.submenu && anyShown;

    out += nl;
    if (inMenu) {
        menus::Node m;
        m.kind = menus::Kind::Menu;
        m.title = o.title;
        m.image = "icon.open_with";
        m.type = menuType;
        if (anyMultiple)
            m.mode = "multiple";
        m.extra.push_back({ "pos", std::string(kPosition) });
        out += "menu(" + menus::MenuWriter::props(m) + ")" + nl + "{" + nl;
    }
    const std::string indent = inMenu ? "\t" : "";
    for (const auto &a : model.apps) {
        if (a.exe.empty())
            continue; // nothing to keep: a preset that wasn't found
        std::string meta = "// app=" + (a.id.empty() ? std::string("custom") : a.id);
        if (a.scope != Scope::All)
            meta += std::string(" scope=") + scopeWord(a.scope);
        if (!a.enabled)
            meta += " off";
        if (!a.enabled && a.hideOwn)
            meta += " hide-own"; // switched off on the page: its own entries go too
        if (a.keepOwn)
            meta += " keep-own";
        out += indent + meta + nl;

        menus::Node n;
        n.kind = menus::Kind::Item;
        n.title = itemTitle(o, a);
        auto exeLit = nssPathLiteral(a.exe, env);
        n.image = a.icon.empty() ? exeLit : nssPathLiteral(a.icon, env);
        if (o.submenu) {
            if (a.scope == Scope::Files || a.scope == Scope::Extensions)
                n.type = "file";
            else if (a.scope == Scope::Folders)
                n.type = "dir|back|desktop";
        } else {
            auto kinds = itemKinds(o, a);
            n.type = kinds.empty() ? std::string(a.scope == Scope::Folders ? "dir|back|desktop" : "file")
                                   : join(kinds, "|");
        }
        if (a.scope == Scope::Extensions)
            n.find = extensionsToFind(a.extensions);
        n.where = "path.exists(" + exeLit + ")";
        if (usesAllSelected(a.args))
            n.mode = "multiple";
        // cmd and args are written by props() with interpolated quotes; the
        // path is passed through as its .nss text.
        n.cmd = toNssPath(a.exe, env);
        n.args = argsToNss(a.args);
        n.admin = a.admin;
        if (isScript(a.exe))
            n.window = "hidden"; // no console flashing up for a .cmd launcher
        // Top-level items go under WinRAR or "Open in new window" (else the
        // top), in the order of the list; no separators between them.
        if (!o.submenu)
            n.extra.push_back({ "pos", std::string(kPosition) });
        n.extra.insert(n.extra.end(), a.extra.begin(), a.extra.end());
        auto line = "item(" + menus::MenuWriter::props(n) + ")";
        out += indent + (shows(a) ? "" : "// ") + line + nl;
    }
    // The user's own lines: in the submenu when they were in it and it's there.
    if (inMenu) {
        for (const auto &l : model.keptInMenu)
            out += l + nl;
        out += "}" + nl;
    }

    // The apps' own entries: "// own app=pwsh verb=PowerShell7x64", then its
    // remove() rule, commented out while the app is on (or keeps them). A
    // custom app is "custom-<n>", the n-th custom app in the file.
    bool anyOwn = false;
    int customs = 0;
    for (const auto &a : model.apps) {
        if (a.exe.empty())
            continue;
        std::string ref = a.id.empty() || a.id == "custom" ? "custom-" + std::to_string(++customs) : a.id;
        for (const auto &h : a.natives) {
            if (!anyOwn)
                out += nl + std::string(kOwnSection) + nl;
            anyOwn = true;
            out += "// own app=" + ref + " " + h.kind + "=" + h.name + nl;
            auto find = exactFind(h.title);
            if (!find.empty())
                out += std::string(hidesNatives(a) ? "" : "// ") + "remove(find=" + find + ")" + nl;
        }
    }

    // Other apps' items switched off on the page.
    if (!model.hidden.empty()) {
        out += nl + "// Other apps' items, hidden on the Open with page." + nl;
        for (const auto &h : model.hidden) {
            out += "// hide " + h.kind + "=" + h.name + nl;
            auto find = exactFind(h.title);
            if (!find.empty())
                out += "remove(find=" + find + ")" + nl;
        }
    }

    // Lines the user wrote by hand, as written.
    if (!model.kept.empty() || (!inMenu && !model.keptInMenu.empty())) {
        out += nl;
        for (const auto &l : model.kept)
            out += l + nl;
        if (!inMenu)
            for (const auto &l : model.keptInMenu)
                out += l + nl;
    }
    return out;
}

std::string exactFind(std::string_view title)
{
    // Shell compares with the title as it normalises it: trailing dots dropped.
    auto t = nss::trim(title);
    while (!t.empty() && t.back() == '.')
        t = nss::trim(t.substr(0, t.size() - 1));
    if (t.empty() || t.find_first_of("\"|\r\n\t") != std::string_view::npos)
        return {};
    // A plain string: no @ expressions; the inner quotes make Shell compare the whole title.
    return menus::MenuWriter::plainString("\"" + std::string(t) + "\"");
}

ParseResult parse(std::string_view text, const Env &env)
{
    ParseResult r;
    r.model.options = Options{};
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
        text.remove_prefix(3); // the file's bytes, read as they are
    auto lines = splitLines(text);
    struct Meta
    {
        std::string id;
        Scope scope = Scope::All;
        bool off = false;
        bool keepOwn = false;
        bool hideOwn = false;
    };
    std::optional<Meta> pending;
    bool sawOptions = false, sawPos = false, olderPos = false;
    std::string menuTitle; // the submenu's title as written on its line
    std::optional<size_t> pendingHide; // a "// hide" line waiting for its remove()
    struct Own
    {
        std::string ref; // "pwsh", "custom-2"
        Hidden hidden;
        bool active = false; // its remove() is in force (not commented out)
    };
    std::vector<Own> owns;
    bool pendingOwn = false; // an "// own" line waiting for its remove()

    // The "Open in" submenu (the page has always given it icon.open_with; a menu
    // the user added is read like any other lines of theirs):
    // its first lines up to the '{' and the '}' line are the page's; the
    // lines between are read like the others.
    size_t menuFirst = 0, menuOpen = 0, menuClose = 0; // 1-based lines; 0: none
    auto doc = nss::NssDocument::fromText(std::string(text));
    const nss::Node *menuNode = nullptr;
    for (const auto &n : doc.nodes()) {
        if (n.kind != nss::NodeKind::MenuDecl || n.menuKind != nss::MenuKind::Menu || n.bodySpan.size() < 2)
            continue;
        bool icon = std::any_of(n.props.begin(), n.props.end(), [&](const nss::MenuProp &p) {
            return (p.name == "image" || p.name == "icon") && nss::trim(doc.slice(p.valueSpan)) == "icon.open_with";
        });
        if (icon) {
            menuNode = &n;
            break;
        }
    }
    if (menuNode) {
        menuFirst = doc.lineOf(menuNode->span.begin);
        menuOpen = doc.lineOf(menuNode->bodySpan.begin);
        menuClose = doc.lineOf(menuNode->bodySpan.end - 1);
        // A title changed by hand on the menu line wins over the options line's.
        for (const auto &p : menuNode->props) {
            bool str = false;
            auto v = p.name == "title" && p.hasValue ? menus::unquote(doc.slice(p.valueSpan), &str) : std::string();
            if (str && !v.empty() && v.find('@') == std::string::npos)
                menuTitle = v;
        }
    }
    auto keep = [&](size_t i) {
        std::string_view raw = lines[i];
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t'))
            raw.remove_suffix(1);
        (i + 1 > menuOpen && i + 1 < menuClose ? r.model.keptInMenu : r.model.kept).emplace_back(raw);
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string_view line = nss::trim(lines[i]);
        if (line.empty())
            continue;
        if (menuFirst && ((i + 1 >= menuFirst && i + 1 <= menuOpen) || i + 1 == menuClose))
            continue; // the submenu's: its type comes from the options line
        auto body = commentBody(line);
        if (nss::OverridesWriter::isHeader(line))
            continue;
        if (body && body->rfind("Edit these items", 0) == 0)
            continue;
        if (body && body->rfind("options:", 0) == 0) {
            sawOptions = true;
            std::string_view rest = nss::trim(std::string_view(*body).substr(8));
            auto t = rest.find("title=");
            if (t != std::string_view::npos) {
                auto lit = nss::Literal::parse(nss::trim(rest.substr(t + 6)));
                if (lit.kind == nss::LiteralKind::String)
                    r.model.options.title = lit.string;
                rest = rest.substr(0, t);
            }
            size_t b = 0;
            std::string s(rest);
            while (b < s.size()) {
                auto sp = s.find(' ', b);
                auto word = s.substr(b, sp == std::string::npos ? std::string::npos : sp - b);
                if (word == "layout=top")
                    r.model.options.submenu = false;
                else if (word == "layout=submenu")
                    r.model.options.submenu = true;
                else if (word.rfind("pos=", 0) == 0) {
                    sawPos = true;
                    olderPos = word.substr(4) != kPositionWord;
                }
                else if (word.rfind("show=", 0) == 0) {
                    auto v = "|" + word.substr(5) + "|";
                    r.model.options.files = v.find("|file|") != std::string::npos;
                    r.model.options.folders = v.find("|dir|") != std::string::npos;
                    r.model.options.background = v.find("|back|") != std::string::npos;
                    r.model.options.desktop = v.find("|desktop|") != std::string::npos;
                }
                if (sp == std::string::npos)
                    break;
                b = sp + 1;
            }
            continue;
        }
        if (body && body->rfind("app=", 0) == 0) {
            Meta m;
            std::string s = *body;
            size_t b = 0;
            while (b < s.size()) {
                auto sp = s.find(' ', b);
                auto word = s.substr(b, sp == std::string::npos ? std::string::npos : sp - b);
                if (word.rfind("app=", 0) == 0)
                    m.id = word.substr(4);
                else if (word == "off")
                    m.off = true;
                else if (word == "keep-own")
                    m.keepOwn = true;
                else if (word == "hide-own")
                    m.hideOwn = true;
                else if (word == "scope=files")
                    m.scope = Scope::Files;
                else if (word == "scope=folders")
                    m.scope = Scope::Folders;
                else if (word == "scope=ext")
                    m.scope = Scope::Extensions;
                if (sp == std::string::npos)
                    break;
                b = sp + 1;
            }
            pending = m;
            continue;
        }
        if (body && *body == "Other apps' items, hidden on the Open with page.")
            continue;
        if (body && *body == kOwnSection.substr(3))
            continue;
        if (body && body->rfind("own app=", 0) == 0) {
            // "own app=git-bash verb=git_shell": the name may have spaces.
            std::string_view rest = std::string_view(*body).substr(8);
            auto sp = rest.find(' ');
            auto eq = sp == std::string_view::npos ? std::string_view::npos : rest.find('=', sp);
            if (eq != std::string_view::npos) {
                Own o;
                o.ref = std::string(rest.substr(0, sp));
                o.hidden.kind = std::string(rest.substr(sp + 1, eq - sp - 1));
                o.hidden.name = std::string(rest.substr(eq + 1));
                if ((o.hidden.kind == "verb" || o.hidden.kind == "handler") && !o.hidden.name.empty() && !o.ref.empty()) {
                    owns.push_back(std::move(o));
                    pendingOwn = true;
                    pendingHide.reset();
                    pending.reset();
                    continue;
                }
            }
        }
        if (pendingOwn) {
            // Its rule: active while the app is off, commented out while it's on.
            std::string_view rule = body ? std::string_view(*body) : line;
            pendingOwn = false;
            if (rule.rfind("remove(find=", 0) == 0 && rule.back() == ')') {
                auto lit = nss::Literal::parse(nss::trim(rule.substr(12, rule.size() - 13)));
                const auto &v = lit.string;
                if (lit.kind == nss::LiteralKind::String && v.size() > 2 && v.front() == '"' && v.back() == '"') {
                    owns.back().hidden.title = v.substr(1, v.size() - 2);
                    owns.back().active = !body;
                    continue;
                }
            }
        }
        if (body && body->rfind("hide ", 0) == 0) {
            // "hide verb=git_gui", "hide handler=7-Zip": the name may have spaces.
            std::string_view rest = nss::trim(std::string_view(*body).substr(5));
            auto eq = rest.find('=');
            Hidden h;
            h.kind = std::string(rest.substr(0, eq == std::string_view::npos ? 0 : eq));
            h.name = eq == std::string_view::npos ? std::string{} : std::string(rest.substr(eq + 1));
            if ((h.kind == "verb" || h.kind == "handler") && !h.name.empty()) {
                r.model.hidden.push_back(std::move(h));
                pendingHide = r.model.hidden.size() - 1;
                pending.reset();
                continue;
            }
        }
        if (pendingHide && !body && line.rfind("remove(find=", 0) == 0 && line.back() == ')') {
            auto lit = nss::Literal::parse(nss::trim(line.substr(12, line.size() - 13)));
            const auto &v = lit.string;
            if (lit.kind == nss::LiteralKind::String && v.size() > 2 && v.front() == '"' && v.back() == '"') {
                r.model.hidden[*pendingHide].title = v.substr(1, v.size() - 2);
                pendingHide.reset();
                continue;
            }
        }
        pendingHide.reset();
        std::string itemText = body ? *body : std::string(line);
        if (pending && nss::trim(itemText).substr(0, 5) == "item(") {
            auto parsed = menus::parse(itemText);
            if (parsed.tree.size() == 1 && parsed.tree[0].kind == menus::Kind::Item) {
                const auto &n = parsed.tree[0];
                App a;
                a.id = pending->id.empty() ? "custom" : pending->id;
                a.enabled = !pending->off;
                a.keepOwn = pending->keepOwn;
                a.hideOwn = pending->off && pending->hideOwn;
                a.scope = pending->scope;
                a.name = r.model.options.submenu
                             ? n.title
                             : std::string(stripPrefix(n.title, sawPos ? "Open in " : "Open with ")); // older pages: "Open with"
                a.exe = fromNssPath(n.cmd, env);
                a.args = nssToArgs(n.args);
                a.admin = n.admin;
                bool str = false;
                auto image = menus::unquote(n.image, &str);
                if (str && image != n.cmd)
                    a.icon = fromNssPath(image, env);
                if (a.scope == Scope::Extensions)
                    a.extensions = findToExtensions(n.find);
                // Properties added by hand; pos= is the page's own.
                for (const auto &p : n.extra)
                    if (p.name != "pos")
                        a.extra.push_back(p);
                if (!n.keys.empty())
                    a.extra.push_back({ "keys", menus::MenuWriter::plainString(n.keys) });
                if (!n.tip.empty())
                    a.extra.push_back({ "tip", menus::MenuWriter::plainString(n.tip) });
                if (!n.sep.empty())
                    a.extra.push_back({ "sep", n.sep });
                r.model.apps.push_back(std::move(a));
                pending.reset();
                continue;
            }
        }
        // A line the page didn't write: kept as it is.
        r.warnings.push_back(nsp::trf("line {}: {}", i + 1, line.substr(0, 60)));
        keep(i);
        pending.reset();
    }
    if (!menuTitle.empty())
        r.model.options.title = menuTitle;
    // The apps' own entries, to their apps ("custom-2": the second custom app).
    for (auto &o : owns) {
        App *owner = nullptr;
        int customs = 0;
        for (auto &a : r.model.apps) {
            bool custom = a.id == "custom";
            customs += custom;
            if (custom ? o.ref == "custom-" + std::to_string(customs) : o.ref == a.id) {
                owner = &a;
                break;
            }
        }
        if (owner) {
            // A rule in force: switched off by the user (files from before
            // "hide-own" have only this to tell).
            if (o.active && !owner->enabled)
                owner->hideOwn = true;
            owner->natives.push_back(std::move(o.hidden));
        } else
            r.warnings.push_back(nsp::trf("own app={}: no such app in the file", o.ref));
    }
    // An older page wrote no pos=: its "Open in" submenu (then the default)
    // landed in the middle of the menu. It reads as today's default layout,
    // top-level items, and is rewritten on the next save. One that wrote
    // pos=top keeps its layout and only gets today's position.
    if (sawOptions && !sawPos) {
        r.legacy = true;
        r.model.options.submenu = false;
    } else if (sawOptions && olderPos) {
        r.legacy = true;
    }
    return r;
}

// --- develop.nss ----------------------------------------------------------------

EditorsMenu findEditorsMenu(const nss::NssDocument &doc)
{
    EditorsMenu e;
    if (const auto *n = findEditorsNode(doc, doc.nodes())) {
        e.found = true;
        e.enabled = true;
        e.firstLine = doc.lineOf(n->span.begin);
        e.lastLine = doc.lineOf(n->span.end > n->span.begin ? n->span.end - 1 : n->span.begin);
        return e;
    }
    // Commented out: a run of lone line comments that reads as the menu.
    auto lines = splitLines(doc.text());
    for (size_t i = 0; i < lines.size(); ++i) {
        auto body = commentBody(lines[i]);
        if (!body || !isEditorsMenuLine(*body))
            continue;
        int depth = 0;
        bool opened = false;
        for (size_t j = i; j < lines.size() && j < i + 200; ++j) {
            auto b = commentBody(lines[j]);
            if (!b)
                break;
            depth += braceDelta(*b, &opened);
            if (opened && depth <= 0) {
                e.found = true;
                e.enabled = false;
                e.firstLine = i + 1;
                e.lastLine = j + 1;
                return e;
            }
        }
    }
    return e;
}

bool setEditorsMenuEnabled(nss::NssDocument &doc, bool enabled)
{
    auto e = findEditorsMenu(doc);
    if (!e.found)
        return false;
    if (e.enabled == enabled)
        return true;
    std::string_view text = doc.text();
    auto starts = lineStarts(text);
    if (e.lastLine > starts.size())
        return false;
    size_t b = starts[e.firstLine - 1];
    size_t end = lineEnd(text, starts, e.lastLine);
    if (enabled == false) {
        // Nothing else may share the first or last line.
        std::string_view first = text.substr(b, lineEnd(text, starts, e.firstLine) - b);
        if (!isEditorsMenuLine(first))
            return false;
        std::string_view lastLine = text.substr(starts[e.lastLine - 1], end - starts[e.lastLine - 1]);
        if (nss::trim(lastLine) != "}")
            return false;
    }
    size_t firstIndent = 0;
    while (b + firstIndent < text.size() && isBlank(text[b + firstIndent]))
        ++firstIndent;
    std::string out;
    for (size_t line = e.firstLine; line <= e.lastLine; ++line) {
        size_t ls = starts[line - 1];
        size_t le = lineEnd(text, starts, line);
        auto body = std::string(text.substr(ls, le - ls));
        if (enabled) {
            size_t p = 0;
            while (p < body.size() && isBlank(body[p]))
                ++p;
            size_t q = p + 2;
            if (q < body.size() && body[q] == ' ')
                ++q;
            out += body.substr(0, p) + body.substr(q);
        } else {
            size_t lead = 0;
            while (lead < body.size() && isBlank(body[lead]))
                ++lead;
            body.insert(std::min(lead, firstIndent), "// ");
            out += body;
        }
        if (line < e.lastLine)
            out += std::string(text.substr(le, starts[line] - le));
    }
    doc.replace({ b, end }, out);
    return true;
}

} // namespace openwith
