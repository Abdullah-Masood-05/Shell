#include "ConfigRules.h"

#include "MenuModel.h"

#include "nss/ConfigGraph.h"
#include "nss/NssLexer.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"

#include <algorithm>
#include <cctype>
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
    const char *title;
    const char *description;
    const char *lock; // non-null: can't be turned off here
};

constexpr KnownModule kKnown[] = {
    { "theme", "Theme", "Colors, fonts, spacing and effects of the menu. Off: Shell's built-in defaults.", nullptr },
    { "images", "Icons", "The icon.* images the other files draw with. Always on: turning it off breaks them.",
      "Other files use its icons; turning it off breaks them" },
    { "modify", "Windows items", "Hides, moves and groups Windows' own items. Each rule is listed below.", nullptr },
    { "terminal", "Terminal", "Command Prompt, PowerShell and Windows Terminal, opened in the current folder.", nullptr },
    { "file-manage", "File manage",
      "Copy paths, change extension, select, take ownership, attributes, new folder and file.", nullptr },
    { "develop", "Develop", "Open in an editor, dotnet commands. Shown while Shift is held.", nullptr },
    { "goto", "Go to", "Jump to common folders and Windows Settings pages.", nullptr },
    { "taskbar", "Taskbar", "Apps, window arrangement and Task Manager on the taskbar's menu.", nullptr },
    { "my-tools", "My tools", "New file and System menus from Abdullah Masood's config.", nullptr },
    { "settings-app-menus", "Your menus", "The menus and items you built on the Menu Builder page.", nullptr },
    { "settings-app-openwith", "Open with", "Editors and terminals you picked on the Open with page.", nullptr },
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

std::string curly(std::string_view s) { return "\xE2\x80\x9C" + std::string(s) + "\xE2\x80\x9D"; }

// A string literal or a name (title.more_options) as display text.
std::string displayValue(std::string_view raw)
{
    bool str = false;
    auto v = menus::unquote(raw, &str);
    if (str)
        return v;
    return humanize(v);
}

// Item names a where expression selects, plus qualifiers ("on the desktop").
// False when it says anything else.
bool summarizeWhere(std::string_view where, std::vector<std::string> &names, std::vector<std::string> &qualifiers)
{
    static const std::regex idEq(R"(^this\.id\s*==\s*id\.(\w+)$)", std::regex::icase);
    static const std::regex idEq2(R"(^id\.(\w+)\s*==\s*this\.id$)", std::regex::icase);
    static const std::regex idList(R"(^this\.id\s*\(([\s\S]*)\)$)", std::regex::icase);
    static const std::regex idItem(R"(^id\.(\w+)$)", std::regex::icase);
    static const std::regex nameEq(R"(^this\.name\s*==\s*(["'][^"']*["'])$)", std::regex::icase);
    static const std::regex strEquals(R"(^str\.equals\s*\(\s*this\.name\s*,\s*([\s\S]*)\)$)", std::regex::icase);
    static const std::regex windowIs(R"(^(window|wnd)\.is_(desktop|taskbar|explorer|edit|start)$)", std::regex::icase);

    for (const auto &alt : splitTop(where, true)) {
        bool any = false;
        for (const auto &c : splitTop(alt, false)) {
            std::smatch m;
            if (std::regex_match(c, m, idEq) || std::regex_match(c, m, idEq2)) {
                names.push_back(humanize(m[1].str()));
                any = true;
            } else if (std::regex_match(c, m, idList)) {
                for (const auto &a : splitArgs(m[1].str())) {
                    std::smatch im;
                    if (!std::regex_match(a, im, idItem))
                        return false;
                    names.push_back(humanize(im[1].str()));
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
                auto q = w == "explorer" ? std::string("in File Explorer") : "on the " + w;
                if (std::find(qualifiers.begin(), qualifiers.end(), q) == qualifiers.end())
                    qualifiers.push_back(q);
            } else {
                return false;
            }
        }
        if (!any)
            return false;
    }
    return !names.empty();
}

std::string joinNames(const std::vector<std::string> &names, size_t max = 4)
{
    std::string out;
    for (size_t i = 0; i < names.size() && i < max; ++i)
        out += (i ? ", " : "") + names[i];
    if (names.size() > max)
        out += " and " + std::to_string(names.size() - max) + " more";
    return out;
}

void describe(const nss::NssDocument &doc, const nss::Node &d, Rule &r)
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

    // What it does
    std::vector<std::string> actions;
    if (r.kind == RuleKind::Remove)
        actions.push_back("Remove");
    if (has("vis")) {
        auto v = word("vis", "vis");
        if (v == "remove" || v == "hidden" || v == "hide")
            actions.push_back("Hide");
        else if (v == "disable" || v == "disabled")
            actions.push_back("Disable");
        else if (v != "normal" && v != "enable") {
            actions.push_back("Change visibility");
            r.complex = true;
        }
    }
    if (has("menu") || has("pos")) {
        std::string move = "Move";
        if (has("menu"))
            move += " to " + curly(displayValue(props["menu"]));
        if (has("pos")) {
            auto v = word("pos", "pos");
            if (v == "top" || v == "bottom" || v == "middle")
                move += has("menu") ? ", at the " + v : " to the " + v;
            else if (!v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
                move += has("menu") ? ", position " + v : " to position " + v;
            else if (v != "auto")
                r.complex = true;
        }
        actions.push_back(move);
    }
    if (has("title"))
        actions.push_back("Rename to " + curly(displayValue(props["title"])));
    if (has("image"))
        actions.push_back("Change icon");
    if (has("sep")) {
        auto v = props["sep"].empty() ? std::string("before") : word("sep", "sep");
        if (v == "top")
            v = "before";
        if (v == "bottom")
            v = "after";
        actions.push_back(v == "both" ? "Add separators" : "Add a separator " + v);
    }
    if (has("keys"))
        actions.push_back("Show a shortcut");
    if (has("tip"))
        actions.push_back("Add a tip");
    if (has("checked") || has("default"))
        actions.push_back("Change state");
    if (has("cmd") || has("command") || has("args") || has("arg"))
        actions.push_back("Change command");
    if (actions.empty())
        actions.push_back(r.kind == RuleKind::Remove ? "Remove" : "No visible change");
    std::string action;
    for (size_t i = 0; i < actions.size(); ++i) {
        auto a = actions[i];
        if (i > 0 && !a.empty())
            a[0] = char(std::tolower(static_cast<unsigned char>(a[0])));
        action += (i ? ", " : "") + a;
    }
    r.action = action;

    // What it applies to
    std::vector<std::string> names, qualifiers;
    if (has("find")) {
        bool str = false;
        auto v = menus::unquote(props["find"], &str);
        if (str) {
            size_t start = 0;
            for (size_t k = 0; k <= v.size(); ++k)
                if (k == v.size() || v[k] == '|') {
                    names.push_back(curly(v.substr(start, k - start)));
                    start = k + 1;
                }
        } else {
            r.complex = true;
        }
    }
    if (has("where")) {
        r.where = props["where"];
        if (!summarizeWhere(props["where"], names, qualifiers)) {
            r.complex = true;
            names.clear();
        }
    }
    if (r.complex && names.empty())
        r.target = "Items matching an expression";
    else if (names.empty())
        r.target = "Every item";
    else
        r.target = joinNames(names);

    std::vector<std::string> scope;
    if (has("type")) {
        auto v = menus::unquote(props["type"]);
        std::string t;
        size_t start = 0;
        for (size_t k = 0; k <= v.size(); ++k)
            if (k == v.size() || v[k] == '|') {
                auto part = humanize(v.substr(start, k - start));
                if (!part.empty())
                    t += (t.empty() ? "" : " or ") + part;
                start = k + 1;
            }
        scope.push_back(t);
    }
    if (has("mode")) {
        auto v = word("mode", "mode");
        if (v == "multiple")
            scope.push_back("any selection");
        else if (v == "multi_unique" || v == "multi_single")
            scope.push_back("multiple selection");
        else if (v == "none")
            scope.push_back("no selection");
        else if (v != "single")
            r.complex = true;
    }
    if (has("in"))
        scope.push_back("in " + curly(displayValue(props["in"])));
    for (auto &q : qualifiers)
        scope.push_back(q);
    for (size_t i = 0; i < scope.size(); ++i)
        r.scope += (i ? " \xC2\xB7 " : "") + scope[i];
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
            auto h = humanize(other == "dir" || other == "directory" ? std::string("folder") : other);
            if (h.size() > 1 && h.back() == 's' && h != "Folders")
                h.pop_back();
            return (h == "Folders" ? std::string("Folder") : h) + " background";
        }
    }
    if (low == "dir" || low == "directory")
        return "Folders";
    if (low == "back")
        return "Background";
    if (low == "file")
        return "Files";
    if (low == "recyclebin")
        return "Recycle bin";
    if (low == "*")
        return "Anything";
    std::replace(s.begin(), s.end(), '_', ' ');
    std::replace(s.begin(), s.end(), '-', ' ');
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!s.empty())
        s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
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
        m.description = "Imported from " + m.path + ".";
        for (const auto &k : kKnown)
            if (m.name == k.stem) {
                m.title = k.title;
                m.description = k.description;
                if (k.lock) {
                    m.locked = true;
                    m.lockReason = k.lock;
                }
            }
        if (!m.locked && m.enabled && root.slice(n->span).find_first_of("\r\n") != std::string_view::npos) {
            m.locked = true;
            m.lockReason = "The import spans several lines";
            m.description += " Locked: the import spans several lines.";
        }
        if (!m.locked && m.enabled && !ownsLines(root.text(), n->span)) {
            m.locked = true;
            m.lockReason = "Something else shares its line";
            m.description += " Locked: something else shares its line.";
        }
        out.push_back(std::move(m));
    }
    return out;
}

bool setModuleEnabled(nss::NssDocument &root, size_t importIndex, bool enabled)
{
    return root.toggleImport(importIndex, enabled);
}

std::vector<Rule> listRules(const nss::NssDocument &doc)
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
        describe(doc, n, r);
        bool topLevel = std::find(top.begin(), top.end(), &n) != top.end();
        if (!topLevel) {
            r.toggleable = false;
            r.note = "Inside a menu";
        } else if (!ownsLines(text, n.span)) {
            r.toggleable = false;
            r.note = "Shares a line with other code";
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
        describe(sub, n, r);
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
