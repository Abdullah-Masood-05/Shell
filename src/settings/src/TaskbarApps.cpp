#include "TaskbarApps.h"

#include "ConfigRules.h"
#include "MenuModel.h"
#include "MenuWriter.h"
#include "OpenWithModel.h"
#include "Tr.h"

#include "nss/NssDocument.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <set>

namespace fs = std::filesystem;

namespace taskbarapps {

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t'; }

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

std::string joinLines(const std::vector<std::string> &lines, std::string_view eol)
{
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i)
            out += eol;
        out += lines[i];
    }
    return out;
}

bool blankLine(std::string_view l) { return nss::trim(l).empty(); }

// "  // x" -> "x" (the marker and one space after it); nullopt when the line
// isn't a lone line comment.
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

// "  item(...)" -> "  // item(...)"
std::vector<std::string> commentOut(const std::vector<std::string> &lines)
{
    std::vector<std::string> out;
    for (const auto &l : lines) {
        size_t p = 0;
        while (p < l.size() && isBlank(l[p]))
            ++p;
        out.push_back(l.substr(0, p) + "// " + l.substr(p));
    }
    return out;
}

std::vector<std::string> uncomment(const std::vector<std::string> &lines)
{
    std::vector<std::string> out;
    for (const auto &l : lines) {
        size_t p = 0;
        while (p < l.size() && isBlank(l[p]))
            ++p;
        auto body = commentBody(l);
        out.push_back(body ? l.substr(0, p) + *body : l);
    }
    return out;
}

nss::NssDocument parseMenu(std::string text)
{
    nss::ParseContext ctx;
    ctx.mode = nss::ParseMode::Menu;
    return nss::NssDocument::fromText(std::move(text), nss::defaultFormat(), ctx);
}

const nss::MenuProp *findProp(const nss::Node &n, std::initializer_list<std::string_view> names)
{
    for (const auto &p : n.props)
        for (auto name : names)
            if (p.name == name)
                return &p;
    return nullptr;
}

constexpr std::initializer_list<std::string_view> kTitle = { "title" };
constexpr std::initializer_list<std::string_view> kCmd = { "cmd", "command" };
constexpr std::initializer_list<std::string_view> kArgs = { "args", "arg", "arguments" };
constexpr std::initializer_list<std::string_view> kAdmin = { "admin" };

std::string expandPath(const Paths &paths, std::string_view s)
{
    return paths.expand ? paths.expand(s) : std::string(s);
}

// The .nss string for a program path: '...' with the known folders as
// @sys.prog32 and so on, else `...`, else "..." (the full path, escaped).
std::string pathLiteral(const Paths &paths, const std::string &program)
{
    auto p = paths.contract ? paths.contract(program) : program;
    if (p.find('\'') == std::string::npos)
        return "'" + p + "'";
    if (p.find('`') == std::string::npos)
        return "`" + p + "`";
    return menus::MenuWriter::plainString(program);
}

std::string shorten(std::string_view s, size_t max = 70)
{
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += c;
    }
    if (out.size() > max) {
        size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out = out.substr(0, cut) + "...";
    }
    return out;
}

// Reads one declaration into `e`.
void fill(Entry &e, const nss::NssDocument &doc, const nss::Node &n, const Paths &paths)
{
    e.kind = Kind::Other;
    e.summary = shorten(doc.slice(n.span));
    if (n.kind != nss::NodeKind::MenuDecl)
        return;
    if (n.menuKind == nss::MenuKind::Separator) {
        e.kind = Kind::Separator;
        return;
    }
    if (n.menuKind != nss::MenuKind::Item || !n.bodySpan.empty())
        return;

    const auto *cmd = findProp(n, kCmd);
    if (!cmd || !cmd->hasValue)
        return;
    bool str = false;
    auto program = menus::unquote(doc.slice(cmd->valueSpan), &str);
    if (!str)
        return;

    std::string args;
    if (const auto *a = findProp(n, kArgs)) {
        if (!a->hasValue)
            return;
        args = menus::unquote(doc.slice(a->valueSpan), &str);
        if (!str)
            return;
    }

    bool admin = false;
    if (const auto *a = findProp(n, kAdmin)) {
        if (a->hasValue) {
            auto v = nss::toLower(nss::trim(doc.slice(a->valueSpan)));
            if (v == "true" || v == "yes" || v == "1")
                admin = true;
            else if (v != "false" && v != "no" && v != "0")
                return; // an expression: kept as written
        } else {
            admin = true;
        }
    }

    std::string title, titleExpr;
    if (const auto *t = findProp(n, kTitle); t && t->hasValue) {
        auto raw = std::string(nss::trim(doc.slice(t->valueSpan)));
        auto value = menus::unquote(raw, &str);
        // '@' in '...' or `...` is an expression in the string.
        if (str && (raw.front() == '"' || value.find('@') == std::string::npos))
            title = value;
        else
            titleExpr = raw;
    }

    e.kind = Kind::App;
    e.title = title;
    e.titleExpr = titleExpr;
    e.program = expandPath(paths, program);
    e.args = args;
    e.admin = admin;
}

// A commented-out item or separator starting at lines[at]: how many lines it takes.
size_t commentedDecl(const std::vector<std::string> &lines, size_t at, nss::NssDocument &doc)
{
    auto first = commentBody(lines[at]);
    if (!first)
        return 0;
    auto t = nss::trim(*first);
    auto keyword = [&](std::string_view kw, bool parenOnly) {
        if (t.size() < kw.size() || !nss::iequals(t.substr(0, kw.size()), kw))
            return false;
        auto rest = nss::trim(t.substr(kw.size()));
        return rest.empty() ? !parenOnly : rest.front() == '(';
    };
    if (!keyword("item", true) && !keyword("separator", false) && !keyword("sep", false))
        return 0;
    std::string joined;
    for (size_t k = 0; k < 40 && at + k < lines.size(); ++k) {
        auto body = commentBody(lines[at + k]);
        if (!body)
            break;
        if (k)
            joined += '\n';
        joined += *body;
        auto d = parseMenu(joined);
        if (d.diagnostics().empty() && d.nodes().size() == 1 && d.nodes()[0].kind == nss::NodeKind::MenuDecl &&
            (d.nodes()[0].menuKind == nss::MenuKind::Item || d.nodes()[0].menuKind == nss::MenuKind::Separator)) {
            doc = std::move(d);
            return k + 1;
        }
    }
    return 0;
}

// Sets (or adds) a property of the document's one declaration.
void setProp(nss::NssDocument &doc, std::initializer_list<std::string_view> names, std::string_view value)
{
    if (doc.nodes().empty())
        return;
    const auto &n = doc.nodes()[0];
    if (const auto *p = findProp(n, names)) {
        if (p->hasValue)
            doc.replace(p->valueSpan, value);
        else
            doc.replace(p->nameSpan, std::string(doc.slice(p->nameSpan)) + "=" + std::string(value));
        return;
    }
    if (n.propsSpan.empty())
        return;
    size_t at = n.propsSpan.end - 1; // the ')'
    bool open = at > 0 && doc.text()[at - 1] == '(';
    std::string add = (open ? "" : " ") + std::string(*names.begin());
    if (!value.empty())
        add += "=" + std::string(value);
    doc.replace(nss::Span{ at, at }, add);
}

void removeProp(nss::NssDocument &doc, std::initializer_list<std::string_view> names)
{
    if (doc.nodes().empty())
        return;
    const auto *p = findProp(doc.nodes()[0], names);
    if (!p)
        return;
    size_t b = p->nameSpan.begin, e = p->hasValue ? p->valueSpan.end : p->nameSpan.end;
    while (b > 0 && isBlank(doc.text()[b - 1]))
        --b;
    doc.replace(nss::Span{ b, e }, "");
}

std::vector<std::string> render(const Entry &e, const Paths &paths)
{
    if (!e.edited() || e.kind == Kind::Other)
        return e.text;
    std::vector<std::string> base;
    if (e.kind == Kind::Separator) {
        base = e.text.empty() ? std::vector<std::string>{ "sep" } : e.read.enabled ? e.text : uncomment(e.text);
        return e.enabled ? base : commentOut(base);
    }
    std::string text;
    if (e.text.empty()) {
        text = "item(title=" + (e.titleExpr.empty() ? menus::MenuWriter::plainString(e.title) : e.titleExpr);
        text += e.image.empty() ? " image" : " image=" + e.image;
        text += " cmd=" + pathLiteral(paths, e.program);
        if (!e.args.empty())
            text += " args=" + menus::MenuWriter::interpolatedString(e.args);
        if (e.admin)
            text += " admin";
        text += ")";
    } else {
        auto lines = e.read.enabled ? e.text : uncomment(e.text);
        // Edit with '\n' only; the lines are split again below.
        auto doc = parseMenu(joinLines(lines, "\n"));
        if (e.title != e.read.title || e.titleExpr != e.read.titleExpr)
            setProp(doc, kTitle, e.titleExpr.empty() ? menus::MenuWriter::plainString(e.title) : e.titleExpr);
        if (e.program != e.read.program)
            setProp(doc, kCmd, pathLiteral(paths, e.program));
        if (e.args != e.read.args) {
            if (e.args.empty())
                removeProp(doc, kArgs);
            else
                setProp(doc, kArgs, menus::MenuWriter::interpolatedString(e.args));
        }
        if (e.admin != e.read.admin) {
            if (e.admin)
                setProp(doc, kAdmin, "");
            else
                removeProp(doc, kAdmin);
        }
        text = doc.text();
    }
    base = splitLines(text);
    return e.enabled ? base : commentOut(base);
}

// taskbar.nss's four apps, as that file has them (without its "no list" condition).
constexpr std::string_view kDefaults =
    "item(title=loc.paint image=\\uE116 cmd='mspaint')\n"
    "item(where=path.exists(sys.prog32 + '\\Microsoft\\Edge\\Application\\msedge.exe') title=loc.edge image cmd='@sys.prog32\\Microsoft\\Edge\\Application\\msedge.exe')\n"
    "item(title=loc.calculator image=\\ue1e7 cmd='calc.exe')\n"
    "item(title=str.res('regedit.exe,-16') image cmd='regedit.exe')\n";

std::string fileName(std::string_view path)
{
    auto p = std::string(path);
    auto slash = p.find_last_of("\\/");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string stem(std::string_view path)
{
    auto n = fileName(path);
    auto dot = n.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
}

std::string extension(std::string_view path)
{
    auto n = fileName(path);
    auto dot = n.find_last_of('.');
    return dot == std::string::npos ? std::string() : nss::toLower(n.substr(dot));
}

} // namespace

Paths realPaths()
{
    auto env = std::make_shared<openwith::Env>(openwith::Env::current());
    Paths p;
    p.expand = [env](std::string_view s) { return openwith::fromNssPath(s, *env); };
    p.contract = [env](std::string_view s) { return openwith::toNssPath(s, *env); };
    return p;
}

List parse(std::string_view text, const Paths &paths)
{
    List out;
    out.exists = true;
    out.eol = nss::detectEol(text);
    auto doc = parseMenu(std::string(text));
    if (!doc.diagnostics().empty()) {
        out.error = doc.diagnostics().front().message;
        out.errorLine = doc.diagnostics().front().line;
        return out;
    }
    auto lines = splitLines(doc.text());

    // The lines each top-level declaration takes; two on one line are one entry.
    struct Seg
    {
        size_t first, last;
        size_t node;
        bool merged = false;
    };
    std::vector<Seg> segs;
    for (size_t i = 0; i < doc.nodes().size(); ++i) {
        const auto &n = doc.nodes()[i];
        if (n.span.empty())
            continue;
        size_t first = doc.lineOf(n.span.begin) - 1;
        size_t last = doc.lineOf(n.span.end - 1) - 1;
        if (!segs.empty() && first <= segs.back().last) {
            segs.back().last = std::max(segs.back().last, last);
            segs.back().merged = true;
            continue;
        }
        segs.push_back({ first, last, i });
    }

    std::vector<std::string> loose;
    size_t si = 0;
    for (size_t li = 0; li < lines.size();) {
        while (si < segs.size() && segs[si].first < li)
            ++si; // can't happen; never loop on it
        if (si < segs.size() && segs[si].first == li) {
            const auto &s = segs[si++];
            Entry e;
            fill(e, doc, doc.nodes()[s.node], paths);
            if (s.merged) {
                e.kind = Kind::Other;
                e.summary = shorten(lines[s.first]);
            }
            e.text.assign(lines.begin() + ptrdiff_t(s.first), lines.begin() + ptrdiff_t(s.last + 1));
            e.lead = std::move(loose);
            loose.clear();
            e.line = s.first + 1;
            e.read = e.values();
            out.entries.push_back(std::move(e));
            li = s.last + 1;
            continue;
        }
        nss::NssDocument d;
        if (size_t k = commentedDecl(lines, li, d)) {
            Entry e;
            fill(e, d, d.nodes()[0], paths);
            e.enabled = false;
            if (e.kind == Kind::Other) {
                // A commented-out item the page can't edit: a comment like any other.
                for (size_t j = 0; j < k; ++j)
                    loose.push_back(lines[li + j]);
                li += k;
                continue;
            }
            e.text.assign(lines.begin() + ptrdiff_t(li), lines.begin() + ptrdiff_t(li + k));
            e.lead = std::move(loose);
            loose.clear();
            e.line = li + 1;
            e.read = e.values();
            out.entries.push_back(std::move(e));
            li += k;
            continue;
        }
        loose.push_back(lines[li++]);
    }

    if (out.entries.empty()) {
        out.head = std::move(loose);
        return out;
    }
    out.tail = std::move(loose);
    // The first entry's lead up to its last blank line is the file's head (the
    // header): it stays at the top when that entry moves.
    auto &lead = out.entries.front().lead;
    for (size_t k = lead.size(); k-- > 0;) {
        if (blankLine(lead[k])) {
            out.head.assign(lead.begin(), lead.begin() + ptrdiff_t(k + 1));
            lead.erase(lead.begin(), lead.begin() + ptrdiff_t(k + 1));
            break;
        }
    }
    return out;
}

List defaults(const Paths &paths)
{
    auto list = parse(kDefaults, paths);
    list.exists = false;
    list.eol = nss::Eol::CRLF;
    for (auto &e : list.entries)
        e.line = 0;
    return list;
}

std::optional<Entry> builtinEntry(std::string_view id, const Paths &paths)
{
    for (auto &e : defaults(paths).entries)
        if (builtinOf(e) == id) {
            e.lead.clear();
            return e;
        }
    return std::nullopt;
}

std::string generate(const List &list, const Paths &paths)
{
    std::vector<std::string> lines = list.head;
    if (!list.exists && lines.empty()) {
        for (auto h : kHeader)
            lines.emplace_back(h);
        lines.emplace_back();
    }
    for (const auto &e : list.entries) {
        lines.insert(lines.end(), e.lead.begin(), e.lead.end());
        auto r = render(e, paths);
        lines.insert(lines.end(), r.begin(), r.end());
    }
    lines.insert(lines.end(), list.tail.begin(), list.tail.end());
    auto eol = nss::eolString(list.eol);
    auto out = joinLines(lines, eol);
    if (!lines.empty())
        out += eol;
    return out;
}

bool dirty(const List &list, const List &original, const Paths &paths)
{
    return generate(list, paths) != generate(original, paths);
}

Entry makeApp(std::string title, std::string program, std::string args, std::string image)
{
    Entry e;
    e.kind = Kind::App;
    e.title = std::move(title);
    e.program = std::move(program);
    e.args = std::move(args);
    e.image = std::move(image);
    return e;
}

Entry makeSeparator()
{
    Entry e;
    e.kind = Kind::Separator;
    return e;
}

std::string builtinOf(const Entry &e)
{
    if (e.kind != Kind::App)
        return {};
    auto p = nss::toLower(e.program);
    auto name = fileName(p);
    if (p == "mspaint" || name == "mspaint.exe")
        return "paint";
    if (name == "msedge.exe")
        return "edge";
    if (p == "calc" || name == "calc.exe")
        return "calculator";
    if (p == "regedit" || name == "regedit.exe")
        return "regedit";
    return {};
}

std::string displayTitle(const Entry &e, const std::map<std::string, std::string> &loc)
{
    if (e.kind == Kind::Separator)
        return nsp::tr("Separator");
    if (e.kind == Kind::Other)
        return e.summary;
    if (e.titleExpr.empty())
        return e.title.empty() ? stem(e.program) : e.title;
    auto expr = std::string(nss::trim(e.titleExpr));
    if (expr.size() > 4 && nss::iequals(std::string_view(expr).substr(0, 4), "loc.")) {
        auto key = nss::toLower(expr.substr(4));
        if (auto it = loc.find(key); it != loc.end() && !it->second.empty())
            return it->second;
        return rules::humanize(expr);
    }
    auto b = builtinOf(e);
    if (b == "regedit")
        return nsp::tr("Registry Editor");
    if (b == "paint")
        return nsp::tr("Paint");
    if (b == "calculator")
        return nsp::tr("Calculator");
    if (b == "edge")
        return "Microsoft Edge";
    return expr;
}

// --- Finding apps --------------------------------------------------------------

namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p)
        out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring expandEnv(const std::wstring &s)
{
    if (s.find(L'%') == std::wstring::npos)
        return s;
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (!n)
        return s;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    while (!out.empty() && out.back() == L'\0')
        out.pop_back();
    return out;
}

bool isFile(const std::wstring &p)
{
    auto a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// A program by name, as Windows finds it (System32, PATH).
std::wstring search(const wchar_t *name)
{
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = SearchPathW(nullptr, name, nullptr, DWORD(std::size(buf)), buf, nullptr);
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

// A version resource string ("FileDescription", "ProductName"), "" when none.
std::string versionString(const std::wstring &path, const wchar_t *name)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Lang
    {
        WORD language, codepage;
    } *langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&langs), &len) || !len)
        return {};
    wchar_t key[128];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].language, langs[0].codepage, name);
    wchar_t *value = nullptr;
    UINT vlen = 0;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&value), &vlen) || !value || vlen <= 1)
        return {};
    return std::string(nss::trim(nss::narrow(std::wstring(value, vlen - 1))));
}

// A short name from a program's version resource, else its file name.
std::string programName(const std::wstring &exe)
{
    for (auto key : { L"FileDescription", L"ProductName" }) {
        auto v = versionString(exe, key);
        if (!v.empty() && v.size() <= 32 && v.find(':') == std::string::npos && v.find(" - ") == std::string::npos)
            return v;
    }
    return stem(nss::narrow(exe));
}

} // namespace

std::string shortcutTarget(const std::string &lnk, std::string *iconLocation)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return {};
    std::string target;
    IPersistFile *file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        if (SUCCEEDED(file->Load(nss::widen(lnk).c_str(), STGM_READ))) {
            wchar_t buf[MAX_PATH * 2]{};
            if (SUCCEEDED(link->GetPath(buf, int(std::size(buf)), nullptr, SLGP_RAWPATH)) && buf[0])
                target = nss::narrow(expandEnv(buf));
            if (iconLocation) {
                wchar_t icon[MAX_PATH * 2]{};
                int index = 0;
                if (SUCCEEDED(link->GetIconLocation(icon, int(std::size(icon)), &index)) && icon[0]) {
                    auto path = expandEnv(icon);
                    if (isFile(path))
                        *iconLocation = nss::narrow(path) + (index ? "," + std::to_string(index) : std::string());
                }
            }
        }
        file->Release();
    }
    link->Release();
    return target;
}

bool isNoise(std::string_view name, std::string_view target)
{
    auto n = nss::toLower(name);
    for (std::string_view w : { "uninstall", "deinstall", "remove ", "help", "readme", "read me", "documentation", "website",
                                "web site", "release notes", "license", "manual", "changelog", "what's new", "support",
                                "user guide", "faq" })
        if (n.find(w) != std::string::npos)
            return true;
    auto t = nss::toLower(fileName(target));
    return t.rfind("unins", 0) == 0 || t.rfind("uninst", 0) == 0;
}

Installed fromFile(const std::string &path)
{
    Installed a;
    a.program = path;
    a.icon = path;
    a.detail = path;
    a.group = "installed";
    auto ext = extension(path);
    if (ext == ".lnk") {
        std::string icon;
        auto target = shortcutTarget(path, &icon);
        a.name = stem(path);
        // Shell draws a shortcut's icon only from its icon location or target.
        if (!icon.empty())
            a.image = menus::MenuWriter::plainString(icon);
        else if (!target.empty() && isFile(nss::widen(target)))
            a.image = menus::MenuWriter::plainString(target);
        if (!target.empty())
            a.detail = target;
    } else if (ext == ".exe") {
        a.name = programName(nss::widen(path));
    } else {
        a.name = stem(path);
    }
    return a;
}

std::vector<Installed> windowsApps()
{
    struct Known
    {
        const char *builtin;
        const char *name; // nsp::mark()ed
        const wchar_t *file;
        const char *program; // what cmd= gets
        const char *image;   // "" = the program's own icon
    };
    static const Known kApps[] = {
        { "paint", nsp::mark("Paint"), L"mspaint.exe", "mspaint", "" },
        { "edge", "Microsoft Edge", nullptr, "", "" },
        { "calculator", nsp::mark("Calculator"), L"calc.exe", "calc.exe", "" },
        { "regedit", nsp::mark("Registry Editor"), L"regedit.exe", "regedit.exe", "" },
        { "", nsp::mark("Notepad"), L"notepad.exe", "notepad.exe", "" },
        { "", nsp::mark("File Explorer"), L"explorer.exe", "explorer.exe", "" },
        { "", nsp::mark("Command Prompt"), L"cmd.exe", "cmd.exe", "" },
        { "", nsp::mark("Windows PowerShell"), L"powershell.exe", "powershell.exe", "" },
        { "", nsp::mark("Control Panel"), L"control.exe", "control.exe", "" },
        { "", nsp::mark("Character Map"), L"charmap.exe", "charmap.exe", "" },
        { "", nsp::mark("Remote Desktop Connection"), L"mstsc.exe", "mstsc.exe", "" },
        { "", nsp::mark("Device Manager"), L"devmgmt.msc", "devmgmt.msc", "'@sys.bin\\mmc.exe'" },
        { "", nsp::mark("Services"), L"services.msc", "services.msc", "'@sys.bin\\mmc.exe'" },
        { "", nsp::mark("Disk Management"), L"diskmgmt.msc", "diskmgmt.msc", "'@sys.bin\\mmc.exe'" },
        { "", nsp::mark("Event Viewer"), L"eventvwr.msc", "eventvwr.msc", "'@sys.bin\\mmc.exe'" },
    };
    std::vector<Installed> out;
    auto sys = knownFolder(FOLDERID_System);
    for (const auto &k : kApps) {
        Installed a;
        a.group = "windows";
        a.name = nsp::tr(k.name);
        a.program = k.program;
        a.image = k.image;
        a.builtin = k.builtin;
        std::wstring path;
        if (std::string_view(k.builtin) == "edge") {
            auto x86 = knownFolder(FOLDERID_ProgramFilesX86);
            path = x86 + L"\\Microsoft\\Edge\\Application\\msedge.exe";
            a.program = nss::narrow(path);
        } else if (k.file) {
            path = search(k.file);
            if (path.empty() && !sys.empty() && isFile(sys + L"\\" + k.file))
                path = sys + L"\\" + k.file;
            if (path.empty() && std::wstring_view(k.file) == L"powershell.exe" && !sys.empty())
                path = sys + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        }
        if (path.empty() || !isFile(path))
            continue;
        a.detail = nss::narrow(path);
        a.icon = extension(a.detail) == ".msc" ? nss::narrow(sys + L"\\mmc.exe") : a.detail;
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<Installed> installedApps()
{
    std::vector<Installed> out;
    std::set<std::string> seen; // lower-case targets
    for (auto id : { FOLDERID_Programs, FOLDERID_CommonPrograms }) {
        auto dir = knownFolder(id);
        if (dir.empty())
            continue;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || nss::toLower(it->path().extension().string()) != ".lnk")
                continue;
            auto lnk = nss::narrow(it->path().wstring());
            std::string icon;
            auto target = shortcutTarget(lnk, &icon);
            auto name = nss::narrow(it->path().stem().wstring());
            if (target.empty() || extension(target) != ".exe" || !isFile(nss::widen(target)) || isNoise(name, target))
                continue;
            if (!seen.insert(nss::toLower(target)).second)
                continue;
            Installed a;
            a.group = "installed";
            a.name = name;
            a.program = lnk;
            a.image = menus::MenuWriter::plainString(icon.empty() ? target : icon);
            a.icon = lnk;
            a.detail = target;
            out.push_back(std::move(a));
        }
    }
    // The editors and terminals the Open with page knows, where the Start menu has none.
    auto env = openwith::Env::current();
    for (const auto &app : openwith::detectAll(env)) {
        if (app.exe.empty() || extension(app.exe) != ".exe" || !isFile(nss::widen(app.exe)))
            continue;
        if (!seen.insert(nss::toLower(app.exe)).second)
            continue;
        Installed a;
        a.group = "installed";
        a.name = app.name;
        a.program = app.exe;
        a.icon = app.exe;
        a.detail = app.exe;
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const Installed &a, const Installed &b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, nss::widen(a.name).c_str(), -1,
                               nss::widen(b.name).c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return out;
}

} // namespace taskbarapps
