#include "MenuMap.h"

#include "MapRules.h"

#include "nss/Literal.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <functional>

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
        n.menu = getString(j, "menu");
        n.menuDynamic = getBool(j, "menu_dynamic");
        n.relocated = getBool(j, "moved");
        n.movedFrom = getString(j, "moved_from");
        n.pos = getString(j, "pos");
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
    if (auto it = tree->find("quiet"); it != tree->end() && it->is_array())
        for (const auto &q : *it) {
            if (!q.is_object() || getBool(q, "windows"))
                continue;
            // "Windows Desktop Gadgets (Windows Sidebar\sbdrop.dll)", or the folder and
            // file alone ("Windows Defender\shellext.dll"): Microsoft's DLLs outside
            // the Windows folder all have the same product name.
            auto module = toPath(getString(q, "module"));
            std::string file = module.filename().string();
            if (auto folder = module.parent_path().filename().string(); !folder.empty() && !file.empty())
                file = folder + "\\" + file;
            auto name = getString(q, "name");
            if (name.empty())
                name = file;
            else if (!file.empty())
                name += " (" + file + ")";
            if (!name.empty() && std::find(t.quietPrograms.begin(), t.quietPrograms.end(), name) == t.quietPrograms.end())
                t.quietPrograms.push_back(name);
        }
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

std::string itemName(std::string_view title)
{
    auto name = stripAccelerators(nss::trim(title));
    while (!name.empty() && (name.back() == '.' || std::isspace(static_cast<unsigned char>(name.back()))))
        name.pop_back();
    return std::string(nss::trim(name));
}

namespace {

std::string asciiLower(std::string s)
{
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// The name a rule's find= names exactly ("\"Title\"" as evaluated), or "".
std::string exactName(const Rule &r)
{
    if (r.findDynamic || !r.in.empty() || !r.where.empty() || r.find.size() < 3 || r.find.front() != '"' ||
        r.find.back() != '"')
        return {};
    auto name = r.find.substr(1, r.find.size() - 2);
    if (name.find_first_of("\"|*") != std::string::npos)
        return {};
    return name;
}

} // namespace

void addUnlisted(Tree &tree, const std::vector<std::string> &typed)
{
    std::vector<bool> used(tree.rules.size(), false);
    std::function<void(const std::vector<Node> &)> mark = [&](const std::vector<Node> &list) {
        for (const auto &n : list) {
            for (int i : n.rules)
                if (i >= 0 && size_t(i) < used.size())
                    used[size_t(i)] = true;
            for (int i : n.maybe)
                if (i >= 0 && size_t(i) < used.size())
                    used[size_t(i)] = true;
            mark(n.children);
        }
    };
    mark(tree.system);

    std::vector<std::string> names;
    auto add = [&](const std::string &name) {
        if (name.empty())
            return;
        auto key = asciiLower(name);
        for (const auto &n : names)
            if (asciiLower(n) == key)
                return;
        for (const auto &n : tree.system)
            if (n.kind != "separator" && asciiLower(n.name) == key)
                return;
        names.push_back(name);
    };
    for (const auto &t : typed)
        add(itemName(t));
    for (size_t i = 0; i < tree.rules.size(); ++i)
        if (!used[i] && tree.rules[i].applies && isMapFile(tree.rules[i].file))
            add(exactName(tree.rules[i]));

    for (const auto &name : names) {
        Node n;
        n.origin = Origin::System;
        n.kind = "item";
        n.title = n.rawTitle = n.name = name;
        n.source = "unknown";
        n.unlisted = true;
        const auto key = asciiLower(name);
        for (size_t i = 0; i < tree.rules.size(); ++i) {
            const auto &r = tree.rules[i];
            if (!r.applies || asciiLower(exactName(r)) != key)
                continue;
            n.rules.push_back(int(i));
            if (r.vis == "hidden")
                n.hidden = true;
            if (!r.menu.empty() && !r.menuDynamic && n.moved.empty())
                n.moved = r.menu;
            if (!r.title.empty() && !r.titleDynamic && n.renamed.empty())
                n.renamed = r.title;
            if (r.vis == "hidden" || (!r.menu.empty() && !isRootPath(r.menu)))
                break;
        }
        tree.system.push_back(std::move(n));
    }
}

bool isRootPath(std::string_view menuPath)
{
    bool slash = false;
    for (char c : menuPath) {
        if (c == '/')
            slash = true;
        else if (!std::isspace(static_cast<unsigned char>(c)))
            return false;
    }
    return slash;
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

namespace {

// The names Shell reads for the same property (Properties.cpp).
std::vector<std::string_view> aliasesOf(std::string_view name)
{
    if (name == "menu")
        return { "menu", "parent", "sub", "move" };
    if (name == "pos")
        return { "pos", "position" };
    return { name };
}

const nss::MenuProp *findAnyProp(const nss::Node &n, std::string_view name)
{
    for (auto alias : aliasesOf(name))
        if (const auto *p = findProp(n, alias))
            return p;
    return nullptr;
}

bool isTitleSource(std::string_view s)
{
    if (s.rfind("loc.", 0) != 0 && s.rfind("title.", 0) != 0)
        return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'))
            return false;
    return true;
}

// The list of declarations that holds `target`: a menu's, or the file's top level.
const std::vector<nss::Node> *siblingsOf(const std::vector<nss::Node> &list, const nss::Node *target)
{
    for (const auto &n : list)
        if (&n == target)
            return &list;
    for (const auto &n : list)
        if (const auto *found = siblingsOf(n.children, target))
            return found;
    return nullptr;
}

} // namespace

std::optional<std::string> shellItemProp(const nss::NssDocument &doc, size_t line, size_t column, std::string_view name)
{
    const auto *n = findDecl(doc, line, column);
    if (!n)
        return std::nullopt;
    const auto *p = findAnyProp(*n, name);
    if (!p)
        return std::nullopt;
    return p->hasValue ? std::string(nss::trim(doc.slice(p->valueSpan))) : std::string();
}

bool setShellItemProp(nss::NssDocument &doc, size_t line, size_t column, std::string_view name, std::string_view value,
                      std::string *error)
{
    auto fail = [&](std::string why) {
        if (error)
            *error = std::move(why);
        return false;
    };
    auto v = nss::trim(value);
    if (v.find_first_of("\r\n") != std::string_view::npos)
        return fail("the value must be one line");
    const auto *n = findDecl(doc, line, column);
    if (!n || n->propsSpan.empty())
        return fail("the declaration wasn't found");
    const auto *prop = findAnyProp(*n, name);
    std::string_view text = doc.text();
    const std::string saved = doc.text();
    const auto before = doc.diagnostics().size();
    if (v.empty()) {
        if (!prop)
            return true;
        // The property and one space next to it.
        size_t begin = prop->nameSpan.begin, end = prop->hasValue ? prop->valueSpan.end : prop->nameSpan.end;
        if (begin > 0 && text[begin - 1] == ' ')
            --begin;
        else if (end < text.size() && text[end] == ' ')
            ++end;
        doc.replace({ begin, end }, "");
    } else if (prop && prop->hasValue) {
        doc.replace(prop->valueSpan, v);
    } else if (prop) {
        doc.replace({ prop->nameSpan.end, prop->nameSpan.end }, "=" + std::string(v));
    } else {
        // After the last property, on its line; right after '(' when there is none.
        size_t at = n->propsSpan.begin + 1;
        for (const auto &p : n->props)
            at = std::max(at, p.hasValue ? p.valueSpan.end : p.nameSpan.end);
        std::string add = std::string(name) + "=" + std::string(v);
        if (at > n->propsSpan.begin + 1)
            add = " " + add;
        else if (at < n->propsSpan.end - 1)
            add += " ";
        doc.replace({ at, at }, add);
    }
    if (doc.diagnostics().size() > before) {
        doc.setText(saved);
        return fail("the file wouldn't parse with this change");
    }
    return true;
}

std::string menuValue(const std::vector<MenuStep> &path)
{
    if (path.empty())
        return nss::quoteString("/");
    std::string out;
    std::string literal; // plain titles next to each other go in one string
    auto join = [&](const std::string &part) { out += (out.empty() ? "" : " + ") + part; };
    for (size_t i = 0; i < path.size(); ++i) {
        const auto &s = path[i];
        if (i > 0)
            literal += "/";
        if (isTitleSource(s.titleSource)) {
            if (!literal.empty())
                join(nss::quoteString(literal));
            literal.clear();
            join(s.titleSource);
        } else {
            literal += s.rawTitle;
        }
    }
    if (!literal.empty())
        join(nss::quoteString(literal));
    return out;
}

std::string posValue(const MenuStep &sibling, bool after)
{
    const char *offset = after ? "1" : "0";
    if (isTitleSource(sibling.titleSource))
        return "indexof(quote(" + sibling.titleSource + "), " + offset + ")";
    auto title = stripAccelerators(sibling.rawTitle);
    while (!title.empty() && title.back() == '.')
        title.pop_back(); // Shell matches titles without the dots at the end
    if (title.empty() || title.find_first_of("\"|\r\n\t") != std::string::npos)
        return {};
    return "indexof(" + nss::quoteString("\"" + title + "\"") + ", " + offset + ")";
}

Reorder planReorder(const std::vector<Sibling> &siblings, size_t index, bool up)
{
    Reorder r;
    if (index >= siblings.size())
        return r;
    const auto &self = siblings[index];
    if (!self.own || self.separator)
        return r;
    if (up ? index == 0 : index + 1 >= siblings.size())
        return r;
    const size_t next = up ? index - 1 : index + 1;
    const auto &other = siblings[next];

    if (other.own && !self.fixed && !other.fixed && !self.file.empty() && self.file == other.file && self.line > 0 &&
        other.line > 0) {
        r.possible = r.text = true;
        r.anchor = next;
        r.after = !up;
    }

    // pos=: right before the item above (right after the one below); past a
    // separator, right after the item above it (right before the one below it).
    std::string pos;
    if (!other.separator) {
        if (other.titled)
            pos = posValue(other.step, !up);
    } else {
        const bool beyond = up ? next > 0 : next + 1 < siblings.size();
        if (!beyond)
            pos = nss::quoteString(up ? "top" : "bottom");
        else if (const auto &b = siblings[up ? next - 1 : next + 1]; b.titled && !b.separator)
            pos = posValue(b.step, up);
    }
    if (!pos.empty()) {
        r.possible = true;
        r.pos = pos;
    }
    return r;
}

bool moveDeclaration(nss::NssDocument &doc, size_t line, size_t column, size_t anchorLine, size_t anchorColumn,
                     bool after, std::string *error, size_t *newLine)
{
    auto fail = [&](std::string why) {
        if (error)
            *error = std::move(why);
        return false;
    };
    const auto *n = findDecl(doc, line, column);
    const auto *a = findDecl(doc, anchorLine, anchorColumn);
    if (!n || !a || n == a)
        return fail("the declarations weren't found");
    const auto *list = siblingsOf(doc.nodes(), n);
    if (!list || list != siblingsOf(doc.nodes(), a))
        return fail("they aren't in the same menu of the file");

    // Work on the text with a line end at the end (the file's kind), so every chunk
    // ends with one.
    std::string text = doc.text();
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const bool added = text.empty() || text.back() != '\n';
    if (added)
        text += eol;

    // The lines of a declaration, with the comment lines right above it.
    auto chunk = [&](const nss::Node &d, nss::Span &out) {
        size_t begin = lineStart(text, d.span.begin);
        if (!nss::trim(std::string_view(text).substr(begin, d.span.begin - begin)).empty())
            return false;
        size_t end = text.find('\n', d.span.end);
        if (end == std::string::npos)
            return false;
        // Nothing after it on its last line but spaces or a // comment.
        auto rest = nss::trim(std::string_view(text).substr(d.span.end, end - d.span.end));
        if (!rest.empty() && rest.rfind("//", 0) != 0)
            return false;
        ++end;
        while (begin > 0) {
            // The line before: past its line end ("\r\n" or "\n") first.
            size_t end = begin;
            if (end > 0 && text[end - 1] == '\n')
                --end;
            if (end > 0 && text[end - 1] == '\r')
                --end;
            size_t prev = lineStart(text, end);
            auto l = nss::trim(std::string_view(text).substr(prev, begin - prev));
            if (l.rfind("//", 0) != 0)
                break;
            begin = prev;
        }
        out = { begin, end };
        return true;
    };
    nss::Span cn, ca;
    if (!chunk(*n, cn) || !chunk(*a, ca))
        return fail("another declaration or code is on the same line");
    if (cn.begin < ca.end && ca.begin < cn.end)
        return fail("the declarations overlap");

    const std::string moved = text.substr(cn.begin, cn.size());
    const size_t at = after ? ca.end : ca.begin;
    const size_t start = (at > cn.begin ? at - cn.size() : at) + (n->span.begin - cn.begin);
    std::string out = text;
    if (at > cn.begin) {
        out.insert(at, moved);
        out.erase(cn.begin, cn.size());
    } else {
        out.erase(cn.begin, cn.size());
        out.insert(at, moved);
    }
    if (added && out.size() >= eol.size() && out.compare(out.size() - eol.size(), eol.size(), eol) == 0)
        out.erase(out.size() - eol.size());

    const std::string saved = doc.text();
    const auto before = doc.diagnostics().size();
    doc.setText(out);
    if (doc.diagnostics().size() > before) {
        doc.setText(saved);
        return fail("the file wouldn't parse with this change");
    }
    if (newLine)
        *newLine = size_t(std::count(out.begin(), out.begin() + std::ptrdiff_t(std::min(start, out.size())), '\n')) + 1;
    return true;
}

} // namespace menumap
