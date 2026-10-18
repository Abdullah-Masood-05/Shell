#include "MenuMap.h"

#include "MapRules.h"

#include "nss/Literal.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace menumap {

namespace {

using json = nlohmann::json;

std::string getString(const json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int getInt(const json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int>() : 0;
}

bool getBool(const json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::vector<int> getInts(const json &j, const char *key)
{
    std::vector<int> out;
    auto it = j.find(key);
    if (it != j.end() && it->is_array())
        for (const auto &v : *it)
            if (v.is_number())
                out.push_back(v.get<int>());
    return out;
}

fs::path toPath(const std::string &utf8) { return utf8.empty() ? fs::path() : fs::path(nss::widen(utf8)); }

Node readNode(const json &j, Origin origin)
{
    Node n;
    n.origin = origin;
    n.kind = getString(j, "kind");
    n.rawTitle = getString(j, "title");
    n.title = stripAccelerators(n.rawTitle);
    if (origin == Origin::Shell) {
        n.state = getString(j, "state");
        n.titleSource = getString(j, "source");
        n.file = toPath(getString(j, "file"));
        n.line = getInt(j, "line");
        n.column = getInt(j, "column");
        n.vis = getString(j, "vis");
        if (auto it = j.find("conditions"); it != j.end() && it->is_array())
            for (const auto &c : *it)
                if (c.is_string())
                    n.conditions.push_back(c.get<std::string>());
        n.dynamic = getBool(j, "dynamic");
    } else {
        n.name = getString(j, "name");
        n.path = getString(j, "path");
        n.id = getString(j, "id");
        n.verb = getString(j, "verb");
        n.source = getString(j, "source");
        n.program = getString(j, "program");
        n.module = getString(j, "module");
        n.clsid = getString(j, "clsid");
        n.handler = getString(j, "handler");
        n.disabled = getBool(j, "disabled");
        n.ownerDrawn = getBool(j, "owner_drawn");
        n.rules = getInts(j, "rules");
        n.maybe = getInts(j, "maybe");
        n.hidden = getBool(j, "hidden");
        n.maybeHidden = getBool(j, "maybe_hidden");
        n.moved = getString(j, "moved");
        n.renamed = getString(j, "renamed");
    }
    if (auto it = j.find("items"); it != j.end() && it->is_array())
        for (const auto &c : *it)
            if (c.is_object())
                n.children.push_back(readNode(c, origin));
    return n;
}

// The 1-based character column of byte `offset` on its line.
size_t columnOf(std::string_view text, size_t offset)
{
    size_t start = offset;
    while (start > 0 && text[start - 1] != '\n' && text[start - 1] != '\r')
        --start;
    size_t col = 1;
    for (size_t k = start; k < offset; ++k)
        if ((static_cast<unsigned char>(text[k]) & 0xC0) != 0x80)
            ++col;
    return col;
}

size_t lineStart(std::string_view text, size_t offset)
{
    while (offset > 0 && text[offset - 1] != '\n' && text[offset - 1] != '\r')
        --offset;
    return offset;
}

const nss::Node *findDecl(const nss::NssDocument &doc, size_t line, size_t column)
{
    std::vector<const nss::Node *> onLine;
    doc.visit([&](const nss::Node &n) {
        if (n.kind == nss::NodeKind::MenuDecl && n.line == line)
            onLine.push_back(&n);
        return true;
    });
    if (onLine.empty())
        return nullptr;
    if (onLine.size() == 1 || column == 0)
        return onLine.front();
    for (const auto *n : onLine)
        if (columnOf(doc.text(), n->span.begin) == column)
            return n;
    return onLine.front();
}

const nss::MenuProp *findProp(const nss::Node &n, std::string_view name)
{
    for (const auto &p : n.props)
        if (p.name == name)
            return &p;
    return nullptr;
}

// The quoted text find= needs for an exact match: "\"Open with\"" (a plain string
// holding the title in double quotes). "" when a title can't be matched that way.
std::string exactFind(std::string_view name)
{
    if (name.empty() || name.find_first_of("\"|\r\n") != std::string_view::npos)
        return {};
    return nss::quoteString("\"" + std::string(name) + "\"");
}

} // namespace

std::string stripAccelerators(std::string_view title)
{
    std::string out;
    out.reserve(title.size());
    for (size_t i = 0; i < title.size(); ++i) {
        if (title[i] == '&') {
            if (i + 1 < title.size() && title[i + 1] == '&') {
                out += '&';
                ++i;
            }
            continue;
        }
        out += title[i];
    }
    // "Open with\tCtrl+O": the shortcut is not part of the title.
    if (auto tab = out.find('\t'); tab != std::string::npos)
        out.resize(tab);
    return out;
}

std::optional<Tree> parseTree(std::string_view text, std::string *error)
{
    auto j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        if (error)
            *error = "not JSON";
        return std::nullopt;
    }
    Tree t;
    t.ok = getBool(j, "ok");
    t.context = getString(j, "context");
    t.sample = getString(j, "sample");
    if (auto it = j.find("errors"); it != j.end() && it->is_array())
        for (const auto &e : *it) {
            if (!e.is_object())
                continue;
            std::string where = toPath(getString(e, "file")).filename().string();
            if (int line = getInt(e, "line"); line > 0)
                where += (where.empty() ? "line " : ":") + std::to_string(line);
            auto msg = getString(e, "message");
            t.errors.push_back(where.empty() ? msg : where + ": " + msg);
        }
    auto tree = j.find("tree");
    if (tree == j.end() || tree->is_null()) {
        if (t.ok) {
            if (error)
                *error = "no tree in the output";
            return std::nullopt;
        }
        return t;
    }
    if (!tree->is_object()) {
        if (error)
            *error = "the tree is not an object";
        return std::nullopt;
    }
    for (const char *key : { "shell", "system" }) {
        auto it = tree->find(key);
        if (it == tree->end() || !it->is_array())
            continue;
        auto &list = std::string_view(key) == "shell" ? t.shell : t.system;
        for (const auto &n : *it)
            if (n.is_object())
                list.push_back(readNode(n, std::string_view(key) == "shell" ? Origin::Shell : Origin::System));
    }
    if (auto it = tree->find("rules"); it != tree->end() && it->is_array())
        for (const auto &r : *it) {
            if (!r.is_object())
                continue;
            Rule rule;
            rule.index = getInt(r, "index");
            rule.file = toPath(getString(r, "file"));
            rule.line = getInt(r, "line");
            rule.column = getInt(r, "column");
            rule.applies = getBool(r, "applies");
            rule.clsid = getBool(r, "clsid");
            rule.find = getString(r, "find");
            rule.findDynamic = getBool(r, "find_dynamic");
            rule.in = getString(r, "in");
            rule.where = getString(r, "where");
            rule.vis = getString(r, "vis");
            rule.menu = getString(r, "menu");
            rule.menuDynamic = getBool(r, "menu_dynamic");
            rule.title = getString(r, "title");
            rule.titleDynamic = getBool(r, "title_dynamic");
            t.rules.push_back(std::move(rule));
        }
    t.systemError = getString(*tree, "system_error");
    return t;
}

std::string matchProps(const Node &n)
{
    if (n.origin != Origin::System || n.kind == "separator")
        return {};
    std::string out;
    if (!n.id.empty())
        out = "where=this.id==id." + n.id;
    else {
        auto find = exactFind(n.name);
        if (find.empty())
            return {};
        out = "find=" + find;
    }
    if (!n.path.empty()) {
        if (n.path.find_first_of("\"\r\n") != std::string::npos)
            return {};
        out += " in=" + nss::quoteString(n.path);
    }
    return out;
}

std::string hideRule(const Node &n)
{
    auto m = matchProps(n);
    return m.empty() ? std::string{} : "remove(" + m + ")";
}

std::string renameRule(const Node &n, std::string_view title)
{
    auto m = matchProps(n);
    auto t = nss::trim(title);
    if (m.empty() || t.empty() || t.find_first_of("\r\n") != std::string_view::npos)
        return {};
    return "modify(" + m + " title=" + nss::quoteString(t) + ")";
}

std::string moveRule(const Node &n, std::string_view menuPath)
{
    auto m = matchProps(n);
    auto p = nss::trim(menuPath);
    if (m.empty() || p.empty() || p.find_first_of("\r\n") != std::string_view::npos)
        return {};
    return "modify(" + m + " menu=" + nss::quoteString(p) + ")";
}

bool isMapFile(const fs::path &file)
{
    if (file.empty())
        return false;
    auto value = nss::narrow(file.generic_wstring());
    return nss::OverridesWriter::samePath(value, maprules::relativePath);
}

std::optional<Block> findBlock(const nss::NssDocument &doc, size_t line, size_t column)
{
    const auto *n = findDecl(doc, line, column);
    if (!n)
        return std::nullopt;
    std::string_view text = doc.text();
    size_t begin = n->span.begin;
    size_t start = lineStart(text, begin);
    if (nss::trim(text.substr(start, begin - start)).empty())
        begin = start;
    Block b;
    b.span = { begin, n->span.end };
    b.line = n->line;
    b.text = std::string(text.substr(begin, n->span.end - begin));
    return b;
}

bool replaceBlock(nss::NssDocument &doc, const Block &block, std::string_view text, std::string *error)
{
    auto fail = [&](std::string why) {
        if (error)
            *error = std::move(why);
        return false;
    };
    if (block.span.end > doc.text().size() || doc.slice(block.span) != block.text)
        return fail("the file changed since the code was read");

    // The new text on its own: one declaration, nothing else, no syntax errors.
    auto alone = nss::NssDocument::fromText(std::string(text), doc.format(), doc.context());
    if (!alone.diagnostics().empty())
        return fail("line " + std::to_string(alone.diagnostics().front().line) + ": " +
                    alone.diagnostics().front().message);
    size_t decls = 0, other = 0;
    for (const auto &n : alone.nodes())
        (n.kind == nss::NodeKind::MenuDecl ? decls : other)++;
    if (decls != 1 || other != 0)
        return fail("the code must be exactly one menu, item, separator, modify or remove");

    auto before = doc.diagnostics().size();
    auto saved = doc.text();
    doc.replace(block.span, text);
    if (doc.diagnostics().size() > before) {
        doc.setText(saved);
        return fail("the file wouldn't parse with this code");
    }
    return true;
}

bool shellItemHidden(const nss::NssDocument &doc, size_t line, size_t column)
{
    const auto *n = findDecl(doc, line, column);
    if (!n)
        return false;
    const auto *vis = findProp(*n, "vis");
    if (!vis)
        vis = findProp(*n, "visibility");
    return vis && vis->hasValue && nss::trim(doc.slice(vis->valueSpan)) == "vis.remove";
}

bool setShellItemHidden(nss::NssDocument &doc, size_t line, size_t column, bool hidden, std::string *error)
{
    auto fail = [&](std::string why) {
        if (error)
            *error = std::move(why);
        return false;
    };
    const auto *n = findDecl(doc, line, column);
    if (!n || n->propsSpan.empty())
        return fail("the declaration wasn't found");
    const auto *vis = findProp(*n, "vis");
    if (!vis)
        vis = findProp(*n, "visibility");
    const bool ours = vis && vis->hasValue && nss::trim(doc.slice(vis->valueSpan)) == "vis.remove";
    if (hidden) {
        if (ours)
            return true;
        if (vis)
            return fail("it has its own vis=; change that with Edit code");
        doc.replace({ n->propsSpan.begin + 1, n->propsSpan.begin + 1 }, "vis=vis.remove ");
        return true;
    }
    if (!vis)
        return true;
    if (!ours)
        return fail("it has its own vis=; change that with Edit code");
    // The property and one space next to it.
    std::string_view text = doc.text();
    size_t begin = vis->nameSpan.begin, end = vis->valueSpan.end;
    if (end < text.size() && text[end] == ' ')
        ++end;
    else if (begin > 0 && text[begin - 1] == ' ')
        --begin;
    doc.replace({ begin, end }, "");
    return true;
}

bool setShellItemTitle(nss::NssDocument &doc, size_t line, size_t column, std::string_view title, std::string *error)
{
    auto fail = [&](std::string why) {
        if (error)
            *error = std::move(why);
        return false;
    };
    auto t = nss::trim(title);
    if (t.empty() || t.find_first_of("\r\n") != std::string_view::npos)
        return fail("the title must be one line of text");
    const auto *n = findDecl(doc, line, column);
    if (!n || n->propsSpan.empty())
        return fail("the declaration wasn't found");
    const auto *prop = findProp(*n, "title");
    if (!prop)
        prop = findProp(*n, "text");
    auto quoted = nss::quoteString(t);
    if (prop && prop->hasValue)
        doc.replace(prop->valueSpan, quoted);
    else
        doc.replace({ n->propsSpan.begin + 1, n->propsSpan.begin + 1 }, "title=" + quoted + " ");
    return true;
}

std::string conditionText(const nss::NssDocument &doc, size_t line, size_t column)
{
    const auto *n = findDecl(doc, line, column);
    if (!n)
        return {};
    std::string out;
    for (const auto &p : n->props) {
        if (p.name != "where" && p.name != "find" && p.name != "mode" && p.name != "type" && p.name != "vis" &&
            p.name != "visibility" && p.name != "keys")
            continue;
        if (!out.empty())
            out += "  ";
        out += p.name;
        if (p.hasValue) {
            std::string value(nss::trim(doc.slice(p.valueSpan)));
            for (auto &c : value)
                if (c == '\r' || c == '\n' || c == '\t')
                    c = ' ';
            out += "=" + value;
        }
    }
    return out;
}

} // namespace menumap
