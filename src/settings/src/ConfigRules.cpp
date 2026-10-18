#include "ConfigRules.h"

#include "MenuModel.h"
#include "Tr.h"

#include "nss/ConfigGraph.h"
#include "nss/NssLexer.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <regex>

namespace rules {

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t'; }

std::string stemOf(std::string_view path)
{
    auto p = std::string(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    auto slash = p.find_last_of('/');
    if (slash != std::string::npos)
        p = p.substr(slash + 1);
    auto dot = p.find_last_of('.');
    if (dot != std::string::npos)
        p = p.substr(0, dot);
    return nss::toLower(p);
}

struct KnownModule
{
    const char *stem;
    const char *title;       // English (it goes into snapshot reasons); shown with nsp::tr
    const char *description; // nsp::mark()ed, translated by listModules
    const char *lock;        // non-null: can't be turned off here
};

constexpr KnownModule kKnown[] = {
    { "theme", nsp::mark("Theme"), nsp::mark("Colors, fonts, spacing and effects of the menu. Off: Shell's built-in defaults."), nullptr },
    { "images", nsp::mark("Icons"), nsp::mark("The icon.* images the other files draw with. Always on: turning it off breaks them."),
      nsp::mark("Other files use its icons; turning it off breaks them") },
    { "modify", nsp::mark("Windows items"), nsp::mark("Hides, moves and groups Windows' own items. Each rule is listed below."), nullptr },
    { "terminal", nsp::mark("Terminal"), nsp::mark("Command Prompt, PowerShell and Windows Terminal, opened in the current folder."), nullptr },
    { "file-manage", nsp::mark("File manage"),
      nsp::mark("Copy paths, change extension, select, take ownership, attributes, new folder and file."), nullptr },
    { "develop", nsp::mark("Develop"), nsp::mark("Open in an editor, dotnet commands. Shown while Shift is held."), nullptr },
    { "goto", nsp::mark("Go to"), nsp::mark("Jump to common folders and Windows Settings pages."), nullptr },
    { "taskbar", nsp::mark("Taskbar"), nsp::mark("Apps, window arrangement and Task Manager on the taskbar's menu."), nullptr },
    { "my-tools", nsp::mark("My tools"), nsp::mark("New file and System menus from Abdullah Masood's config."), nullptr },
    { "settings-app-menus", nsp::mark("Your menus"), nsp::mark("The menus and items you built on the Menu Builder page."), nullptr },
    { "settings-app-map", nsp::mark("Menu map"), nsp::mark("The Windows items you hid, renamed or moved on the Menu map page."), nullptr },
    { "settings-app-openwith", nsp::mark("Open with"), nsp::mark("Editors and terminals you picked on the Open with page."), nullptr },
    { "settings-app-snippets", nsp::mark("Snippets"), nsp::mark("The snippets you turned on on the Snippets page."), nullptr },
};

std::string lineText(std::string_view text, size_t begin)
{
    auto e = text.find_first_of("\r\n", begin);
    return std::string(text.substr(begin, e == std::string_view::npos ? std::string_view::npos : e - begin));
}

std::vector<size_t> lineStarts(std::string_view text)
{
    std::vector<size_t> starts{ 0 };
    for (size_t k = 0; k < text.size(); ++k)
        if (text[k] == '\n' || (text[k] == '\r' && (k + 1 >= text.size() || text[k + 1] != '\n')))
            starts.push_back(k + 1);
    return starts;
}

// End of line `line` (1-based), before its line break.
size_t lineEnd(std::string_view text, const std::vector<size_t> &starts, size_t line)
{
    size_t b = starts[line - 1];
    auto e = text.find_first_of("\r\n", b);
    return e == std::string_view::npos ? text.size() : e;
}

// Nothing but blanks before `span` on its first line, and nothing but blanks
// or a line comment after it on its last line.
bool ownsLines(std::string_view text, nss::Span span)
{
    size_t ls = span.begin;
    while (ls > 0 && text[ls - 1] != '\n' && text[ls - 1] != '\r')
        --ls;
    for (size_t k = ls; k < span.begin; ++k)
        if (!isBlank(text[k]))
            return false;
    size_t k = span.end;
    while (k < text.size() && isBlank(text[k]))
        ++k;
    if (k + 1 < text.size() && text[k] == '/' && text[k + 1] == '/')
        return true;
    return k >= text.size() || text[k] == '\r' || text[k] == '\n';
}

// "// x" -> "x": the comment marker and one space after it.
std::string uncomment(std::string_view line)
{
    size_t p = 0;
    while (p < line.size() && isBlank(line[p]))
        ++p;
    if (line.substr(p, 2) != "//")
        return std::string(line);
    size_t q = p + 2;
    if (q < line.size() && line[q] == ' ')
        ++q;
    return std::string(line.substr(0, p)) + std::string(line.substr(q));
}

bool startsRuleKeyword(std::string_view s)
{
    s = nss::trim(s);
    for (std::string_view kw : { "modify", "remove", "set", "update", "change" }) {
        if (s.size() > kw.size() && nss::iequals(s.substr(0, kw.size()), kw)) {
            auto rest = nss::trim(s.substr(kw.size()));
            if (!rest.empty() && rest.front() == '(')
                return true;
        }
    }
    return false;
}

// Splits at top-level separators ("||"/"or" or "&&"/"and"), outside
// brackets and strings.
std::vector<std::string> splitTop(std::string_view s, bool orLevel)
{
    std::vector<std::string> parts;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    auto word = [&](size_t i, std::string_view w) {
        if (i + w.size() > s.size() || !nss::iequals(s.substr(i, w.size()), w))
            return false;
        bool leftOk = i == 0 || !nss::NssLexer::isIdentChar(s[i - 1]);
        bool rightOk = i + w.size() >= s.size() || !nss::NssLexer::isIdentChar(s[i + w.size()]);
        return leftOk && rightOk;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            quote = c;
            continue;
        }
        if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']')
            --depth;
        if (depth != 0)
            continue;
        size_t len = 0;
        if (orLevel && s.substr(i, 2) == "||")
            len = 2;
        else if (orLevel && word(i, "or"))
            len = 2;
        else if (!orLevel && s.substr(i, 2) == "&&")
            len = 2;
        else if (!orLevel && word(i, "and"))
            len = 3;
        if (len) {
            parts.emplace_back(nss::trim(s.substr(start, i - start)));
            start = i + len;
            i += len - 1;
        }
    }
    parts.emplace_back(nss::trim(s.substr(start)));
    return parts;
}

std::vector<std::string> splitArgs(std::string_view s)
{
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`')
            quote = c;
        else if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']')
            --depth;
        else if (c == ',' && depth == 0) {
            out.emplace_back(nss::trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    auto last = nss::trim(s.substr(start));
    if (!last.empty())
        out.emplace_back(last);
    return out;
}

// A name in a rule summary, in the language's quotation marks.
std::string curly(std::string_view s)
{
    const std::string arg[] = { std::string(s) };
    return nsp::format(nsp::trc("quote", "“{}”"), arg);
}

std::string fmt(const char *translated, std::initializer_list<std::string> args)
{
    std::vector<std::string> list(args);
    return nsp::format(translated, list);
}

// "&Develop" -> "Develop", "Go To..." -> "Go To": the form a title is compared in.
std::string plainTitle(std::string_view s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (i + 1 < s.size() && s[i + 1] == '&')
                out.push_back(s[++i]);
            continue;
        }
        out.push_back(s[i]);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    return out;
}

// A Windows item by its id.* / title.* name: the title Windows shows, or readable
// English. "" when the item doesn't exist on this Windows.
std::string windowsName(std::string_view name, const Names &names)
{
    if (names.windows)
        if (auto t = names.windows(nss::toLower(name)))
            return *t;
    return humanize(name);
}

// Built-in titles that old configs name with a literal ("Terminal", "More options").
constexpr const char *kLiteralTitles[] = { "terminal", "more_options", "go_to" };

// The menu a rule moves items into, as the menu itself is titled.
std::string menuName(std::string_view raw, const Names &names)
{
    bool str = false;
    auto v = std::string(nss::trim(menus::unquote(raw, &str)));
    if (str) {
        // A literal: a title the bundled imports now take from a language file
        // ("File manage" is loc.file_manage), else one of Windows' own.
        auto key = nss::toLower(plainTitle(v));
        for (const auto &[k, english] : names.locEnglish)
            if (nss::toLower(plainTitle(english)) == key) {
                auto it = names.loc.find(k);
                return plainTitle(it != names.loc.end() ? it->second : english);
            }
        for (const char *t : kLiteralTitles)
            if (nss::toLower(humanize(t)) == key) {
                auto w = windowsName(t, names);
                return w.empty() ? v : w;
            }
        return v;
    }
    auto low = nss::toLower(v);
    if (low.rfind("loc.", 0) == 0) {
        auto k = low.substr(4);
        if (auto it = names.loc.find(k); it != names.loc.end())
            return plainTitle(it->second);
        if (auto it = names.locEnglish.find(k); it != names.locEnglish.end())
            return plainTitle(it->second);
        return humanize(k);
    }
    if (low.rfind("title.", 0) == 0) {
        auto w = windowsName(low.substr(6), names);
        return w.empty() ? humanize(low.substr(6)) : w;
    }
    return v;
}

std::string renameValue(std::string_view raw, const Names &names)
{
    bool str = false;
    auto v = menus::unquote(raw, &str);
    if (str)
        return v;
    return menuName(raw, names);
}

// Item names a where expression selects, plus window qualifiers ("on the desktop").
// False when it says anything else.
bool summarizeWhere(std::string_view where, const Names &n, std::vector<std::string> &names, std::vector<std::string> &qualifiers)
{
    static const std::regex idEq(R"(^this\.id\s*==\s*id\.(\w+)$)", std::regex::icase);
    static const std::regex idEq2(R"(^id\.(\w+)\s*==\s*this\.id$)", std::regex::icase);
    static const std::regex idList(R"(^this\.id\s*\(([\s\S]*)\)$)", std::regex::icase);
    static const std::regex idItem(R"(^id\.(\w+)$)", std::regex::icase);
    static const std::regex nameEq(R"(^this\.name\s*==\s*(["'][^"']*["'])$)", std::regex::icase);
    static const std::regex strEquals(R"(^str\.equals\s*\(\s*this\.name\s*,\s*([\s\S]*)\)$)", std::regex::icase);
    static const std::regex windowIs(R"(^(window|wnd)\.is_(desktop|taskbar|explorer|edit|start)$)", std::regex::icase);

    std::vector<std::string> missing; // ids with no such item on this Windows
    auto addId = [&](const std::string &id) {
        auto t = windowsName(id, n);
        if (t.empty())
            missing.push_back(curly(humanize(id)));
        else
            names.push_back(curly(t));
    };
    for (const auto &alt : splitTop(where, true)) {
        bool any = false;
        for (const auto &c : splitTop(alt, false)) {
            std::smatch m;
            if (std::regex_match(c, m, idEq) || std::regex_match(c, m, idEq2)) {
                addId(m[1].str());
                any = true;
            } else if (std::regex_match(c, m, idList)) {
                for (const auto &a : splitArgs(m[1].str())) {
                    std::smatch im;
                    if (!std::regex_match(a, im, idItem))
                        return false;
                    addId(im[1].str());
                }
                any = true;
            } else if (std::regex_match(c, m, nameEq)) {
                names.push_back(curly(menus::unquote(m[1].str())));
                any = true;
            } else if (std::regex_match(c, m, strEquals)) {
                auto arg = std::string(nss::trim(m[1].str()));
                if (arg.size() >= 2 && arg.front() == '[' && arg.back() == ']')
                    arg = arg.substr(1, arg.size() - 2);
                for (const auto &a : splitArgs(arg)) {
                    bool str = false;
                    auto v = menus::unquote(a, &str);
                    if (!str)
                        return false;
                    names.push_back(curly(v));
                }
                any = true;
            } else if (std::regex_match(c, m, windowIs)) {
                auto w = nss::toLower(m[2].str());
                std::string q = w == "explorer"  ? nsp::tr("in File Explorer")
                              : w == "desktop" ? nsp::tr("on the desktop")
                              : w == "taskbar" ? nsp::tr("on the taskbar")
                              : w == "edit"    ? nsp::tr("in a text box")
                                               : nsp::tr("on the Start menu");
                if (std::find(qualifiers.begin(), qualifiers.end(), q) == qualifiers.end())
                    qualifiers.push_back(q);
            } else {
                return false;
            }
        }
        if (!any)
            return false;
    }
    // Only Windows 7 items (pin_to_start_menu): name them anyway.
    if (names.empty())
        names = missing;
    return !names.empty();
}

// "“A”, “B” and “C”"; past `max`, "“A”, “B” and 3 more".
std::string joinNames(const std::vector<std::string> &names, size_t max = 5)
{
    if (names.empty())
        return {};
    if (names.size() > max) {
        std::string head = names[0];
        for (size_t i = 1; i < max - 1; ++i)
            head = fmt(nsp::trc("names", "{0}, {1}"), { head, names[i] });
        return nsp::trn("{0} and {n} more", "{0} and {n} more", (long long)(names.size() - (max - 1)), head);
    }
    std::string out = names[0];
    for (size_t i = 1; i < names.size(); ++i)
        out = fmt(i + 1 == names.size() ? nsp::trc("names", "{0} and {1}") : nsp::trc("names", "{0}, {1}"), { out, names[i] });
    return out;
}

// What a `type=` value names, as a phrase: "folder backgrounds", "the Recycle Bin".
std::string typeLabel(std::string_view part)
{
    auto t = nss::toLower(nss::trim(part));
    if (t.rfind("type.", 0) == 0)
        t = t.substr(5);
    std::string bg;
    if (t.size() > 5 && t.compare(t.size() - 5, 5, ".back") == 0)
        bg = t.substr(0, t.size() - 5);
    else if (t.size() > 5 && t.rfind("back.", 0) == 0)
        bg = t.substr(5);
    if (!bg.empty()) {
        if (bg == "dir" || bg == "directory" || bg == "folder")
            return nsp::trc("type", "folder backgrounds");
        if (bg == "drive")
            return nsp::trc("type", "drive backgrounds");
        if (bg == "namespace")
            return nsp::trc("type", "virtual folder backgrounds");
        if (bg == "computer")
            return nsp::trc("type", "the This PC background");
        if (bg == "recyclebin")
            return nsp::trc("type", "the Recycle Bin background");
        if (bg == "library")
            return nsp::trc("type", "library backgrounds");
        return nsp::trf("{} backgrounds", typeLabel(bg));
    }
    if (t == "file" || t == "files")
        return nsp::trc("type", "files");
    if (t == "dir" || t == "directory" || t == "folder")
        return nsp::trc("type", "folders");
    if (t == "drive")
        return nsp::trc("type", "drives");
    if (t == "usb")
        return nsp::trc("type", "USB drives");
    if (t == "dvd")
        return nsp::trc("type", "DVD drives");
    if (t == "fixed")
        return nsp::trc("type", "fixed drives");
    if (t == "vhd")
        return nsp::trc("type", "virtual hard disks");
    if (t == "removable")
        return nsp::trc("type", "removable drives");
    if (t == "remote")
        return nsp::trc("type", "network drives");
    if (t == "back")
        return nsp::trc("type", "backgrounds");
    if (t == "desktop")
        return nsp::trc("type", "the desktop");
    if (t == "recyclebin")
        return nsp::trc("type", "the Recycle Bin");
    if (t == "computer")
        return nsp::trc("type", "This PC");
    if (t == "namespace")
        return nsp::trc("type", "virtual folders");
    if (t == "taskbar")
        return nsp::trc("type", "the taskbar");
    if (t == "edit")
        return nsp::trc("type", "text boxes");
    if (t == "start")
        return nsp::trc("type", "the Start button");
    if (t == "titlebar")
        return nsp::trc("type", "title bars");
    if (t == "compressed")
        return nsp::trc("type", "compressed folders");
    if (t == "library")
        return nsp::trc("type", "libraries");
    return std::string(nss::trim(part));
}

// One action of a rule. `lead` takes the item names ("Hides {0}"); `follow` comes
// after another action and refers back to them ("hides it" / "hides them").
struct Action
{
    std::string lead;
    std::string follow;
};

void describe(const nss::NssDocument &doc, const nss::Node &d, Rule &r, const Names &n)
{
    std::map<std::string, std::string> props;
    for (const auto &p : d.props) {
        auto raw = std::string(nss::trim(doc.slice(p.valueSpan)));
        auto name = p.name;
        if (name == "parent")
            name = "menu";
        if (name == "icon")
            name = "image";
        if (name == "separator")
            name = "sep";
        if (name == "visibility")
            name = "vis";
        if (name == "position")
            name = "pos";
        if (!props.count(name))
            props[name] = p.hasValue ? raw : std::string{};
    }
    auto has = [&](const char *k) { return props.count(k) > 0; };
    auto word = [&](const char *k, const char *prefix) {
        bool str = false;
        auto v = nss::toLower(nss::trim(menus::unquote(props[k], &str)));
        std::string p = std::string(prefix) + ".";
        if (!str && v.rfind(p, 0) == 0)
            v = v.substr(p.size());
        return v;
    };

    // What it applies to
    std::vector<std::string> names, qualifiers;
    std::vector<std::string> patterns, exact;
    if (has("find")) {
        bool str = false;
        auto v = menus::unquote(props["find"], &str);
        if (str) {
            size_t start = 0;
            for (size_t k = 0; k <= v.size(); ++k)
                if (k == v.size() || v[k] == '|') {
                    auto part = std::string(nss::trim(v.substr(start, k - start)));
                    // find='"Open Git Bash here"': that exact title (as the Open with page writes it)
                    if (part.size() > 2 && part.front() == '"' && part.back() == '"' &&
                        part.find_first_of("*\"", 1) == part.size() - 1)
                        exact.push_back(curly(part.substr(1, part.size() - 2)));
                    else if (!part.empty())
                        patterns.push_back(curly(part));
                    start = k + 1;
                }
        } else {
            r.complex = true;
        }
    }
    if (has("where")) {
        r.where = props["where"];
        if (!summarizeWhere(props["where"], n, names, qualifiers)) {
            r.complex = true;
            names.clear();
            qualifiers.clear();
        }
    }
    names.insert(names.end(), exact.begin(), exact.end());
    if (!patterns.empty()) {
        std::string list = patterns[0];
        for (size_t i = 1; i < patterns.size(); ++i)
            list = nsp::trf("{} or {}", list, patterns[i]);
        names.push_back(nsp::trf("items whose name matches {}", list));
    }
    // "it" or "them" in the follow-up actions
    const size_t count = names.empty() || !patterns.empty() ? 2 : names.size();
    std::string target;
    if (r.complex && names.empty())
        target = nsp::tr("the items an expression picks");
    else if (names.empty())
        target = nsp::tr("every item");
    else
        target = joinNames(names);

    // What it does, most important first.
    std::vector<Action> actions;
    if (r.kind == RuleKind::Remove)
        actions.push_back({ nsp::trf("Removes {0}", target), nsp::trn("removes it", "removes them", (long long)count) });
    if (has("vis")) {
        auto v = word("vis", "vis");
        if (v == "remove" || v == "hidden" || v == "hide")
            actions.push_back({ nsp::trf("Hides {0}", target), nsp::trn("hides it", "hides them", (long long)count) });
        else if (v == "disable" || v == "disabled")
            actions.push_back({ nsp::trf("Disables {0}", target), nsp::trn("disables it", "disables them", (long long)count) });
        else if (v != "normal" && v != "enable") {
            actions.push_back({ nsp::trf("Changes when {0} is shown", target),
                                nsp::trn("changes when it is shown", "changes when they are shown", (long long)count) });
            r.complex = true;
        }
    }
    if (has("menu") || has("pos")) {
        const bool toMenu = has("menu");
        const std::string menu = toMenu ? curly(menuName(props["menu"], n)) : std::string{};
        auto v = has("pos") ? word("pos", "pos") : std::string("auto");
        const bool number = !v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        // pos is an index from 0; people count from 1.
        const std::string place = number ? std::to_string(std::stoll(v.substr(0, 9)) + 1) : std::string{};
        Action a;
        if (toMenu) {
            if (v == "top")
                a = { nsp::trf("Moves {0} to the top of {1}", target, menu), nsp::trn("moves it to the top of {0}", "moves them to the top of {0}", (long long)count, menu) };
            else if (v == "bottom")
                a = { nsp::trf("Moves {0} to the bottom of {1}", target, menu),
                      nsp::trn("moves it to the bottom of {0}", "moves them to the bottom of {0}", (long long)count, menu) };
            else if (v == "middle")
                a = { nsp::trf("Moves {0} to the middle of {1}", target, menu),
                      nsp::trn("moves it to the middle of {0}", "moves them to the middle of {0}", (long long)count, menu) };
            else if (number)
                a = { nsp::trf("Moves {0} into {1} at position {2}", target, menu, place),
                      nsp::trn("moves it into {0} at position {1}", "moves them into {0} at position {1}", (long long)count, menu, place) };
            else
                a = { nsp::trf("Moves {0} into {1}", target, menu), nsp::trn("moves it into {0}", "moves them into {0}", (long long)count, menu) };
        } else {
            if (v == "top")
                a = { nsp::trf("Moves {0} to the top of the menu", target),
                      nsp::trn("moves it to the top of the menu", "moves them to the top of the menu", (long long)count) };
            else if (v == "bottom")
                a = { nsp::trf("Moves {0} to the bottom of the menu", target),
                      nsp::trn("moves it to the bottom of the menu", "moves them to the bottom of the menu", (long long)count) };
            else if (v == "middle")
                a = { nsp::trf("Moves {0} to the middle of the menu", target),
                      nsp::trn("moves it to the middle of the menu", "moves them to the middle of the menu", (long long)count) };
            else if (number)
                a = { nsp::trf("Moves {0} to position {1} in the menu", target, place),
                      nsp::trn("moves it to position {0} in the menu", "moves them to position {0} in the menu", (long long)count, place) };
            else
                a = { nsp::trf("Moves {0}", target), nsp::trn("moves it", "moves them", (long long)count) };
        }
        if (!number && v != "top" && v != "bottom" && v != "middle" && v != "auto")
            r.complex = true;
        actions.push_back(a);
    }
    if (has("title")) {
        auto title = curly(renameValue(props["title"], n));
        actions.push_back({ nsp::trf("Renames {0} to {1}", target, title), nsp::trn("renames it to {0}", "renames them to {0}", (long long)count, title) });
    }
    if (has("sep")) {
        auto v = props["sep"].empty() ? std::string("before") : word("sep", "sep");
        if (v == "top")
            v = "before";
        if (v == "bottom")
            v = "after";
        if (v == "before")
            actions.push_back({ nsp::trf("Adds a separator above {0}", target), nsp::trn("adds a separator above it", "adds a separator above them", (long long)count) });
        else if (v == "after")
            actions.push_back({ nsp::trf("Adds a separator below {0}", target), nsp::trn("adds a separator below it", "adds a separator below them", (long long)count) });
        else if (v == "both")
            actions.push_back({ nsp::trf("Adds separators above and below {0}", target),
                                nsp::trn("adds separators above and below it", "adds separators above and below them", (long long)count) });
        else if (v != "none")
            actions.push_back({ nsp::trf("Changes the separators around {0}", target),
                                nsp::trn("changes the separators around it", "changes the separators around them", (long long)count) });
    }
    if (has("image"))
        actions.push_back({ nsp::trf("Changes the icon of {0}", target), nsp::trn("changes its icon", "changes their icons", (long long)count) });
    if (has("keys"))
        actions.push_back({ nsp::trf("Shows a shortcut key on {0}", target), nsp::trn("shows a shortcut key", "shows shortcut keys", (long long)count) });
    if (has("tip"))
        actions.push_back({ nsp::trf("Adds a tip to {0}", target), nsp::trn("adds a tip", "adds tips", (long long)count) });
    if (has("checked") || has("default"))
        actions.push_back({ nsp::trf("Changes the state of {0}", target), nsp::trn("changes its state", "changes their state", (long long)count) });
    if (has("cmd") || has("command") || has("args") || has("arg"))
        actions.push_back({ nsp::trf("Changes the command of {0}", target), nsp::trn("changes its command", "changes their commands", (long long)count) });
    if (actions.empty())
        actions.push_back({ nsp::trf("Matches {0} but changes nothing", target), {} });

    std::string sentence = actions[0].lead;
    for (size_t i = 1; i < actions.size(); ++i)
        sentence = fmt(i + 1 == actions.size() ? nsp::trc("actions", "{0} and {1}") : nsp::trc("actions", "{0}, {1}"),
                       { sentence, actions[i].follow });

    // When: the selection, then where in Windows.
    std::string selection;
    if (has("mode")) {
        auto v = word("mode", "mode");
        if (v == "multiple")
            selection = nsp::tr("when one or more items are selected");
        else if (v == "multi_unique" || v == "multi_single")
            selection = nsp::tr("when several items are selected");
        else if (v == "none")
            selection = nsp::tr("when nothing is selected");
        else if (v != "single")
            r.complex = true;
    }
    if (!selection.empty())
        sentence = fmt(nsp::trc("when", "{0} {1}"), { sentence, selection });

    std::string place;
    if (has("type")) {
        auto v = menus::unquote(props["type"]);
        std::string t;
        size_t start = 0;
        for (size_t k = 0; k <= v.size(); ++k)
            if (k == v.size() || v[k] == '|') {
                auto part = std::string(nss::trim(v.substr(start, k - start)));
                if (!part.empty() && part != "*")
                    t = t.empty() ? typeLabel(part) : nsp::trf("{} or {}", t, typeLabel(part));
                start = k + 1;
            }
        if (!t.empty())
            place = nsp::trf("for {0}", t);
    }
    std::string windows;
    for (const auto &q : qualifiers)
        windows = windows.empty() ? q : nsp::trf("{} or {}", windows, q);
    if (!windows.empty())
        place = place.empty() ? windows : fmt(nsp::trc("where", "{0} {1}"), { place, windows });
    if (has("in")) {
        auto in = nsp::trf("in {}", curly(menuName(props["in"], n)));
        place = place.empty() ? in : fmt(nsp::trc("where", "{0} {1}"), { place, in });
    }
    if (!place.empty())
        sentence = fmt(nsp::trc("where", "{0}, {1}"), { sentence, place });
    r.summary = sentence;
}

// Line comments that are alone on their line, in order: (line, start of "//").
std::vector<std::pair<size_t, size_t>> loneLineComments(const nss::NssDocument &doc)
{
    std::vector<std::pair<size_t, size_t>> out;
    std::string_view src = doc.text();
    for (const auto &tok : doc.tokens()) {
        size_t pos = tok.triviaBegin;
        while (pos < tok.begin) {
            if (src[pos] == '/' && pos + 1 < tok.begin && src[pos + 1] == '*') {
                auto e = src.find("*/", pos + 2);
                pos = e == std::string_view::npos ? tok.begin : e + 2;
                continue;
            }
            if (!(src[pos] == '/' && pos + 1 < tok.begin && src[pos + 1] == '/')) {
                ++pos;
                continue;
            }
            size_t cb = pos;
            size_t ce = src.find_first_of("\r\n", cb);
            if (ce == std::string_view::npos || ce > tok.begin)
                ce = tok.begin;
            pos = ce;
            size_t ls = cb;
            while (ls > 0 && src[ls - 1] != '\n' && src[ls - 1] != '\r')
                --ls;
            bool alone = true;
            for (size_t k = ls; k < cb; ++k)
                if (!isBlank(src[k]))
                    alone = false;
            if (alone)
                out.emplace_back(doc.lineOf(cb), cb);
        }
    }
    return out;
}

} // namespace

std::string humanize(std::string_view name)
{
    auto s = std::string(nss::trim(name));
    for (std::string_view prefix : { "id.", "title.", "loc.", "type." })
        if (s.size() > prefix.size() && nss::iequals(std::string_view(s).substr(0, prefix.size()), prefix))
            s = s.substr(prefix.size());
    auto low = nss::toLower(s);
    // dir.back / back.dir: the background of a folder
    for (std::string_view sfx : { ".back", "back." }) {
        auto at = low.find(sfx);
        if (at != std::string::npos && low.size() > sfx.size()) {
            auto other = sfx == ".back" ? low.substr(0, at) : low.substr(at + sfx.size());
            std::string h;
            if (other == "dir" || other == "directory" || other == "folder")
                h = nsp::tr("Folder");
            else if (other == "file" || other == "files")
                h = nsp::tr("File");
            else {
                h = humanize(other);
                const bool word = other != "back" && other != "recyclebin" && other != "*";
                if (word && h.size() > 1 && h.back() == 's')
                    h.pop_back();
            }
            return nsp::trf("{} background", h);
        }
    }
    if (low == "dir" || low == "directory")
        return nsp::tr("Folders");
    if (low == "back")
        return nsp::tr("Background");
    if (low == "file")
        return nsp::tr("Files");
    if (low == "recyclebin")
        return nsp::tr("Recycle bin");
    if (low == "*")
        return nsp::tr("Anything");
    std::replace(s.begin(), s.end(), '_', ' ');
    std::replace(s.begin(), s.end(), '-', ' ');
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!s.empty())
        s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::map<std::string, std::string> readLangFile(const std::filesystem::path &path)
{
    std::map<std::string, std::string> out;
    auto doc = nss::NssDocument::load(path, nss::ParseContext{ nss::ParseMode::Lang, {} });
    if (!doc)
        return out;
    for (const auto &node : doc->nodes()) {
        if (node.kind != nss::NodeKind::Opaque || node.path.empty() || node.path[0] == '$' || node.valueSpan.empty())
            continue;
        bool str = false;
        auto v = menus::unquote(doc->slice(node.valueSpan), &str);
        if (str)
            out[node.path] = v;
    }
    return out;
}

Names langNames(const std::vector<std::filesystem::path> &langDirs, std::string_view language)
{
    namespace fs = std::filesystem;
    auto find = [&](const std::string &stem) -> fs::path {
        for (const auto &dir : langDirs) {
            std::error_code ec;
            auto p = dir / (stem + ".nss");
            if (!stem.empty() && fs::is_regular_file(p, ec))
                return p;
        }
        return {};
    };
    Names names;
    if (auto en = find("en"); !en.empty())
        names.locEnglish = readLangFile(en);
    names.loc = names.locEnglish;
    std::string code(language);
    if (!code.empty() && code != "en") {
        auto file = find(code);
        if (file.empty() && code.find('-') != std::string::npos)
            file = find(code.substr(0, code.find('-')));
        if (!file.empty())
            for (auto &[k, v] : readLangFile(file))
                names.loc[k] = v;
    }
    return names;
}

std::vector<Module> listModules(const nss::NssDocument &root)
{
    std::vector<Module> out;
    auto list = root.imports();
    for (size_t i = 0; i < list.size(); ++i) {
        const auto *n = list[i];
        if (n->importKind != nss::ImportKind::Config || !n->context.empty())
            continue;
        Module m;
        m.importIndex = i;
        m.line = n->line;
        m.enabled = n->enabled;
        auto expr = std::string(root.slice(n->valueSpan));
        auto value = nss::ConfigGraph::evaluate(expr, {});
        m.path = value ? *value : expr;
        if (value && nss::OverridesWriter::samePath(*value, nss::OverridesWriter::relativePath))
            continue; // the Appearance page's file
        m.name = stemOf(m.path);
        m.title = humanize(m.name);
        m.description = nsp::trf("Imported from {}.", m.path);
        for (const auto &k : kKnown)
            if (m.name == k.stem) {
                m.title = k.title;
                m.description = nsp::tr(k.description);
                if (k.lock) {
                    m.locked = true;
                    m.lockReason = nsp::tr(k.lock);
                }
            }
        if (!m.locked && m.enabled && root.slice(n->span).find_first_of("\r\n") != std::string_view::npos) {
            m.locked = true;
            m.lockReason = nsp::tr("The import spans several lines");
            m.description += std::string(" ") + nsp::tr("Locked: the import spans several lines.");
        }
        if (!m.locked && m.enabled && !ownsLines(root.text(), n->span)) {
            m.locked = true;
            m.lockReason = nsp::tr("Something else shares its line");
            m.description += std::string(" ") + nsp::tr("Locked: something else shares its line.");
        }
        out.push_back(std::move(m));
    }
    return out;
}

bool setModuleEnabled(nss::NssDocument &root, size_t importIndex, bool enabled)
{
    return root.toggleImport(importIndex, enabled);
}

std::vector<Rule> listRules(const nss::NssDocument &doc, const Names &names)
{
    std::vector<Rule> out;
    std::string_view text = doc.text();

    // Enabled: modify/remove declarations anywhere in the tree.
    std::vector<const nss::Node *> top;
    for (const auto &n : doc.nodes())
        top.push_back(&n);
    doc.visit([&](const nss::Node &n) {
        if (n.kind != nss::NodeKind::MenuDecl ||
            (n.menuKind != nss::MenuKind::Modify && n.menuKind != nss::MenuKind::Remove))
            return true;
        Rule r;
        r.kind = n.menuKind == nss::MenuKind::Remove ? RuleKind::Remove : RuleKind::Modify;
        r.enabled = true;
        r.line = n.line;
        r.lineCount = doc.lineOf(n.span.end > n.span.begin ? n.span.end - 1 : n.span.begin) - n.line + 1;
        r.text = std::string(doc.slice(n.span));
        describe(doc, n, r, names);
        bool topLevel = std::find(top.begin(), top.end(), &n) != top.end();
        if (!topLevel) {
            r.toggleable = false;
            r.note = nsp::tr("Inside a menu");
        } else if (!ownsLines(text, n.span)) {
            r.toggleable = false;
            r.note = nsp::tr("Shares a line with other code");
        }
        out.push_back(std::move(r));
        return true;
    });

    // Disabled: a run of lone line comments that reads as one declaration.
    auto comments = loneLineComments(doc);
    auto starts = lineStarts(text);
    for (size_t i = 0; i < comments.size(); ++i) {
        auto first = uncomment(lineText(text, starts[comments[i].first - 1]));
        if (!startsRuleKeyword(first))
            continue;
        std::string joined;
        size_t j = i;
        bool complete = false;
        for (; j < comments.size() && j < i + 80; ++j) {
            if (j > i && comments[j].first != comments[j - 1].first + 1)
                break;
            if (j > i)
                joined += "\n";
            joined += uncomment(lineText(text, starts[comments[j].first - 1]));
            int depth = 0;
            bool opened = false;
            for (const auto &t : nss::NssLexer::tokenize(joined)) {
                if (t.kind == nss::TokenKind::LParen) {
                    ++depth;
                    opened = true;
                } else if (t.kind == nss::TokenKind::RParen)
                    --depth;
            }
            if (opened && depth <= 0) {
                complete = true;
                break;
            }
        }
        if (!complete)
            continue;
        auto sub = nss::NssDocument::fromText(joined, nss::defaultFormat());
        if (!sub.diagnostics().empty() || sub.nodes().size() != 1)
            continue;
        const auto &n = sub.nodes()[0];
        if (n.kind != nss::NodeKind::MenuDecl ||
            (n.menuKind != nss::MenuKind::Modify && n.menuKind != nss::MenuKind::Remove))
            continue;
        Rule r;
        r.kind = n.menuKind == nss::MenuKind::Remove ? RuleKind::Remove : RuleKind::Modify;
        r.enabled = false;
        r.line = comments[i].first;
        r.lineCount = j - i + 1;
        r.text = std::string(sub.slice(n.span));
        describe(sub, n, r, names);
        out.push_back(std::move(r));
        i = j;
    }
    std::stable_sort(out.begin(), out.end(), [](const Rule &a, const Rule &b) { return a.line < b.line; });
    return out;
}

bool setRuleEnabled(nss::NssDocument &doc, const Rule &rule, bool enabled)
{
    auto all = listRules(doc);
    const Rule *found = nullptr;
    for (const auto &r : all)
        if (r.line == rule.line && r.text == rule.text && r.kind == rule.kind)
            found = &r;
    if (!found)
        for (const auto &r : all)
            if (r.text == rule.text && r.kind == rule.kind && r.enabled == rule.enabled) {
                found = &r;
                break;
            }
    if (!found)
        return false;
    if (found->enabled == enabled)
        return true;
    if (!found->toggleable)
        return false;

    std::string_view text = doc.text();
    auto starts = lineStarts(text);
    size_t firstLine = found->line, lastLine = found->line + found->lineCount - 1;
    if (lastLine > starts.size())
        return false;
    size_t b = starts[firstLine - 1];
    size_t e = lineEnd(text, starts, lastLine);

    // Every line of the declaration, with its own line break (none on the last).
    std::string out;
    size_t firstIndent = 0;
    while (b + firstIndent < text.size() && isBlank(text[b + firstIndent]))
        ++firstIndent;
    for (size_t line = firstLine; line <= lastLine; ++line) {
        size_t ls = starts[line - 1];
        size_t le = lineEnd(text, starts, line);
        auto body = std::string(text.substr(ls, le - ls));
        if (enabled) {
            out += uncomment(body);
        } else {
            size_t lead = 0;
            while (lead < body.size() && isBlank(body[lead]))
                ++lead;
            size_t at = std::min(lead, firstIndent);
            body.insert(at, "// ");
            out += body;
        }
        if (line < lastLine)
            out += std::string(text.substr(le, starts[line] - le));
    }
    doc.replace({ b, e }, out);
    return true;
}

} // namespace rules
