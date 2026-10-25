#include "MenuModel.h"

#include "MenuWriter.h"
#include "Tr.h"

#include "nss/OverridesWriter.h"

#include "nss/Literal.h"
#include "nss/Text.h"

#include <algorithm>
#include <cctype>

namespace menus {

namespace {

std::string lowerTrim(std::string_view s) { return nss::toLower(nss::trim(s)); }

// mode.multiple / "multiple" / multiple -> multiple
std::string enumWord(std::string_view raw, std::string_view prefix)
{
    bool str = false;
    auto v = lowerTrim(unquote(raw, &str));
    std::string p = std::string(prefix) + ".";
    if (!str && v.rfind(p, 0) == 0)
        v = v.substr(p.size());
    return v;
}

bool isWord(std::string_view s)
{
    if (s.empty())
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return nss::NssLexer::isIdentChar(c); });
}

} // namespace

std::string unquote(std::string_view raw, bool *wasString)
{
    auto t = nss::trim(raw);
    if (wasString)
        *wasString = false;
    if (t.size() >= 2 && (t.front() == '\'' || t.front() == '`') && t.back() == t.front() &&
        t.substr(1, t.size() - 2).find(t.front()) == std::string_view::npos) {
        if (wasString)
            *wasString = true;
        return std::string(t.substr(1, t.size() - 2));
    }
    if (t.size() >= 2 && t.front() == '"') {
        auto lit = nss::Literal::parse(t);
        if (lit.kind == nss::LiteralKind::String) {
            if (wasString)
                *wasString = true;
            return lit.string;
        }
    }
    return std::string(t);
}

// --- Locations ------------------------------------------------------------

std::string extensionsToFind(std::string_view text)
{
    std::string out;
    std::string cur;
    auto flush = [&] {
        if (cur.empty())
            return;
        if (cur[0] != '.')
            cur.insert(cur.begin(), '.');
        if (cur.size() > 1) {
            if (!out.empty())
                out += '|';
            out += nss::toLower(cur);
        }
        cur.clear();
    };
    for (char c : text) {
        if (c == ' ' || c == ',' || c == ';' || c == '|' || c == '\t')
            flush();
        else if (c != '*' && c != '\'' && c != '"' && c != '`')
            cur += c;
    }
    flush();
    return out;
}

std::string findToExtensions(std::string_view find)
{
    std::string out(find);
    std::replace(out.begin(), out.end(), '|', ' ');
    return out;
}

static bool isExtensionList(std::string_view find)
{
    if (find.empty())
        return false;
    size_t i = 0;
    while (i < find.size()) {
        if (find[i] != '.')
            return false;
        size_t j = i + 1;
        while (j < find.size() && find[j] != '|') {
            char c = find[j];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
                return false;
            ++j;
        }
        if (j == i + 1)
            return false;
        i = j + 1;
        if (j < find.size() && i >= find.size())
            return false; // trailing '|'
    }
    return true;
}

Location locationOf(const Node &n)
{
    if (!n.where.empty())
        return Location::Custom;
    auto type = nss::toLower(n.type);
    if (!n.find.empty())
        return type == "file" && isExtensionList(n.find) ? Location::Extensions : Location::Custom;
    if (type.empty())
        return Location::Everywhere;
    if (type == "file")
        return Location::Files;
    if (type == "dir")
        return Location::Folders;
    if (type == "back")
        return Location::Background;
    if (type == "desktop")
        return Location::Desktop;
    if (type == "drive")
        return Location::Drives;
    return Location::Custom;
}

void setLocation(Node &n, Location loc, std::string_view extensions)
{
    switch (loc) {
    case Location::Everywhere: n.type.clear(); n.find.clear(); n.where.clear(); break;
    case Location::Files: n.type = "file"; n.find.clear(); n.where.clear(); break;
    case Location::Folders: n.type = "dir"; n.find.clear(); n.where.clear(); break;
    case Location::Background: n.type = "back"; n.find.clear(); n.where.clear(); break;
    case Location::Desktop: n.type = "desktop"; n.find.clear(); n.where.clear(); break;
    case Location::Drives: n.type = "drive"; n.find.clear(); n.where.clear(); break;
    case Location::Extensions:
        n.type = "file";
        n.find = extensionsToFind(extensions);
        n.where.clear();
        break;
    case Location::Custom: break;
    }
}

const char *locationLabel(Location loc)
{
    switch (loc) {
    case Location::Everywhere: return nsp::tr("Everywhere");
    case Location::Files: return nsp::tr("Files");
    case Location::Folders: return nsp::tr("Folders");
    case Location::Background: return nsp::tr("Folder background");
    case Location::Desktop: return nsp::tr("Desktop");
    case Location::Drives: return nsp::tr("Drives");
    case Location::Extensions: return nsp::tr("Files with extensions");
    case Location::Custom: return nsp::tr("Custom");
    }
    return "";
}

// --- Tree navigation --------------------------------------------------------

std::vector<Row> flatten(const Tree &tree)
{
    std::vector<Row> rows;
    Path path;
    auto walk = [&](auto &&self, const Tree &level, int depth) -> void {
        for (size_t i = 0; i < level.size(); ++i) {
            path.push_back(i);
            rows.push_back(Row{ path, depth, &level[i] });
            if (level[i].kind == Kind::Menu)
                self(self, level[i].children, depth + 1);
            path.pop_back();
        }
    };
    walk(walk, tree, 0);
    return rows;
}

Node *nodeAt(Tree &tree, const Path &path)
{
    Tree *level = &tree;
    Node *n = nullptr;
    for (size_t k = 0; k < path.size(); ++k) {
        if (path[k] >= level->size())
            return nullptr;
        n = &(*level)[path[k]];
        level = &n->children;
    }
    return n;
}

const Node *nodeAt(const Tree &tree, const Path &path) { return nodeAt(const_cast<Tree &>(tree), path); }

int rowOf(const Tree &tree, const Path &path)
{
    auto rows = flatten(tree);
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].path == path)
            return int(i);
    return -1;
}

static Tree *levelOf(Tree &tree, const Path &path)
{
    if (path.empty())
        return nullptr;
    if (path.size() == 1)
        return &tree;
    auto parent = nodeAt(tree, Path(path.begin(), path.end() - 1));
    return parent && parent->kind == Kind::Menu ? &parent->children : nullptr;
}

// --- Edits ------------------------------------------------------------------

Node makeNode(Kind kind)
{
    Node n;
    n.kind = kind;
    // A starting title in the UI language; titles already in the file are never changed.
    if (kind == Kind::Menu)
        n.title = nsp::trc("Default title of a new menu", "New menu");
    else if (kind == Kind::Item)
        n.title = nsp::trc("Default title of a new item", "New item");
    return n;
}

std::optional<Path> add(Tree &tree, const Path &at, Node node)
{
    if (at.empty()) {
        tree.push_back(std::move(node));
        return Path{ tree.size() - 1 };
    }
    auto target = nodeAt(tree, at);
    if (!target)
        return std::nullopt;
    if (target->kind == Kind::Menu) {
        target->children.push_back(std::move(node));
        Path p = at;
        p.push_back(target->children.size() - 1);
        return p;
    }
    auto level = levelOf(tree, at);
    if (!level)
        return std::nullopt;
    level->insert(level->begin() + std::ptrdiff_t(at.back() + 1), std::move(node));
    Path p = at;
    p.back() += 1;
    return p;
}

std::optional<Path> remove(Tree &tree, const Path &at)
{
    auto level = levelOf(tree, at);
    if (!level || at.back() >= level->size())
        return std::nullopt;
    auto kept = std::move((*level)[at.back()].before);
    level->erase(level->begin() + std::ptrdiff_t(at.back()));
    if (!kept.empty()) {
        if (at.back() < level->size())
            (*level)[at.back()].before = joinKept(kept, (*level)[at.back()].before);
        else if (auto parent = at.size() > 1 ? nodeAt(tree, Path(at.begin(), at.end() - 1)) : nullptr)
            parent->tail = joinKept(kept, parent->tail);
    }
    Path p = at;
    if (p.back() < level->size())
        return p;
    if (p.back() > 0) {
        p.back() -= 1;
        return p;
    }
    p.pop_back();
    return p; // the parent, or empty (nothing left at the root)
}

std::optional<Path> duplicate(Tree &tree, const Path &at)
{
    auto level = levelOf(tree, at);
    if (!level || at.back() >= level->size())
        return std::nullopt;
    Node copy = (*level)[at.back()];
    // Statements kept from the file would run twice: the copy goes without them.
    auto clearKept = [](auto &&self, Node &n) -> void {
        n.before.clear();
        n.tail.clear();
        for (auto &c : n.children)
            self(self, c);
    };
    clearKept(clearKept, copy);
    level->insert(level->begin() + std::ptrdiff_t(at.back() + 1), std::move(copy));
    Path p = at;
    p.back() += 1;
    return p;
}

std::optional<Path> moveUp(Tree &tree, const Path &at)
{
    auto level = levelOf(tree, at);
    if (!level || at.back() == 0 || at.back() >= level->size())
        return std::nullopt;
    std::swap((*level)[at.back()], (*level)[at.back() - 1]);
    Path p = at;
    p.back() -= 1;
    return p;
}

std::optional<Path> moveDown(Tree &tree, const Path &at)
{
    auto level = levelOf(tree, at);
    if (!level || at.back() + 1 >= level->size())
        return std::nullopt;
    std::swap((*level)[at.back()], (*level)[at.back() + 1]);
    Path p = at;
    p.back() += 1;
    return p;
}

std::optional<Path> indent(Tree &tree, const Path &at)
{
    auto level = levelOf(tree, at);
    if (!level || at.back() == 0 || at.back() >= level->size())
        return std::nullopt;
    auto &prev = (*level)[at.back() - 1];
    if (prev.kind != Kind::Menu)
        return std::nullopt;
    Node moving = std::move((*level)[at.back()]);
    level->erase(level->begin() + std::ptrdiff_t(at.back()));
    auto &menu = (*level)[at.back() - 1];
    menu.children.push_back(std::move(moving));
    Path p = at;
    p.back() -= 1;
    p.push_back(menu.children.size() - 1);
    return p;
}

std::optional<Path> outdent(Tree &tree, const Path &at)
{
    if (at.size() < 2)
        return std::nullopt;
    Path parentPath(at.begin(), at.end() - 1);
    auto parent = nodeAt(tree, parentPath);
    auto outer = levelOf(tree, parentPath);
    if (!parent || !outer || at.back() >= parent->children.size())
        return std::nullopt;
    Node moving = std::move(parent->children[at.back()]);
    parent->children.erase(parent->children.begin() + std::ptrdiff_t(at.back()));
    outer->insert(outer->begin() + std::ptrdiff_t(parentPath.back() + 1), std::move(moving));
    Path p = parentPath;
    p.back() += 1;
    return p;
}

// --- Reading ------------------------------------------------------------------

static void readProps(const nss::NssDocument &doc, const nss::Node &decl, Node &n)
{
    for (const auto &p : decl.props) {
        std::string raw = p.hasValue ? std::string(nss::trim(doc.slice(p.valueSpan))) : std::string{};
        const auto &name = p.name;
        auto keep = [&] { n.extra.push_back(Prop{ name, raw }); };
        bool str = false;
        if (name == "title") {
            auto v = unquote(raw, &str);
            if (str && (raw.front() == '"' || v.find('@') == std::string::npos))
                n.title = v;
            else
                keep();
        } else if (name == "image" || name == "icon") {
            if (p.hasValue && n.image.empty())
                n.image = raw;
            else
                keep();
        } else if (name == "type") {
            auto v = unquote(raw, &str);
            if (str)
                n.type = v;
            else if (isWord(raw) || raw == "*")
                n.type = raw;
            else
                keep();
        } else if (name == "find") {
            auto v = unquote(raw, &str);
            if (str && v.find('@') == std::string::npos)
                n.find = v;
            else
                keep();
        } else if (name == "where") {
            if (p.hasValue)
                n.where = raw;
            else
                keep();
        } else if (name == "mode") {
            auto v = enumWord(raw, "mode");
            if (v == "single" || v == "multiple" || v == "multi_unique" || v == "multi_single" || v == "none")
                n.mode = v;
            else
                keep();
        } else if (name == "cmd" || name == "command") {
            auto v = unquote(raw, &str);
            if (str && p.hasValue)
                n.cmd = v;
            else
                keep();
        } else if (name == "args" || name == "arg" || name == "arguments") {
            auto v = unquote(raw, &str);
            if (str)
                n.args = v;
            else
                keep();
        } else if (name == "admin") {
            auto v = lowerTrim(raw);
            if (!p.hasValue || v == "true" || v == "1" || v == "yes")
                n.admin = true;
            else if (v != "false" && v != "0" && v != "no")
                keep();
        } else if (name == "window") {
            auto v = enumWord(raw, "window");
            if (v == "hidden" || v == "minimized" || v == "maximized")
                n.window = v;
            else if (v != "show" && v != "visible" && v != "normal")
                keep();
        } else if (name == "keys") {
            auto v = unquote(raw, &str);
            if (str)
                n.keys = v;
            else
                keep();
        } else if (name == "tip") {
            auto v = unquote(raw, &str);
            if (str && (raw.front() == '"' || v.find('@') == std::string::npos))
                n.tip = v;
            else
                keep();
        } else if (name == "sep" || name == "separator") {
            auto v = enumWord(raw, "sep");
            if (v == "top")
                v = "before";
            if (v == "bottom")
                v = "after";
            if (v == "before" || v == "after" || v == "both")
                n.sep = v;
            else if (v != "none" && p.hasValue)
                keep();
            else if (!p.hasValue)
                keep();
        } else {
            keep();
        }
    }
}

std::string joinKept(std::string_view first, std::string_view second)
{
    if (first.empty())
        return std::string(second);
    if (second.empty())
        return std::string(first);
    return std::string(first) + "\n" + std::string(second);
}

// The text between two entries as kept text: its lines without the blank ones
// at either end, the common indentation and trailing spaces.
static std::string keptText(std::string_view gap)
{
    std::vector<std::string_view> lines;
    size_t at = 0;
    while (at <= gap.size()) {
        auto end = gap.find_first_of("\r\n", at);
        lines.push_back(gap.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (end == std::string_view::npos)
            break;
        at = end + (gap[end] == '\r' && end + 1 < gap.size() && gap[end + 1] == '\n' ? 2 : 1);
    }
    auto blank = [](std::string_view l) { return nss::trim(l).empty(); };
    size_t b = 0, e = lines.size();
    while (b < e && blank(lines[b]))
        ++b;
    while (e > b && blank(lines[e - 1]))
        --e;
    size_t indent = std::string_view::npos;
    for (size_t i = b; i < e; ++i) {
        if (blank(lines[i]))
            continue;
        size_t k = 0;
        while (k < lines[i].size() && (lines[i][k] == ' ' || lines[i][k] == '\t'))
            ++k;
        indent = std::min(indent, k);
    }
    std::string out;
    for (size_t i = b; i < e; ++i) {
        auto l = lines[i];
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t'))
            l.remove_suffix(1);
        if (!l.empty())
            l.remove_prefix(std::min(indent, l.size()));
        if (i > b)
            out += '\n';
        out += l;
    }
    return out;
}

// The menu entries of `nodes`, which lie in doc.text()[begin, end). Everything
// between them (comments, other statements) is kept on the entry after it, or
// in `tail` after the last one.
static void readLevel(const nss::NssDocument &doc, const std::vector<nss::Node> &nodes, size_t begin, size_t end,
                      Tree &out, std::string &tail, std::vector<std::string> &warnings)
{
    std::string_view text = doc.text();
    size_t cursor = begin;
    for (const auto &d : nodes) {
        if (d.span.begin < cursor)
            continue; // before the region (a header line)
        if (d.kind != nss::NodeKind::MenuDecl ||
            (d.menuKind != nss::MenuKind::Menu && d.menuKind != nss::MenuKind::Item &&
             d.menuKind != nss::MenuKind::Separator)) {
            if (d.kind == nss::NodeKind::Import && !d.enabled)
                continue;
            auto line = std::string(doc.slice(d.span));
            if (line.size() > 60)
                line = line.substr(0, 57) + "...";
            warnings.push_back(nsp::trf("line {}: {}", d.line, line));
            continue; // kept: part of the text before the next entry
        }
        Node n;
        n.kind = d.menuKind == nss::MenuKind::Menu ? Kind::Menu
               : d.menuKind == nss::MenuKind::Item ? Kind::Item
                                                    : Kind::Separator;
        n.before = keptText(text.substr(cursor, d.span.begin - cursor));
        readProps(doc, d, n);
        if (n.kind == Kind::Menu && d.bodySpan.size() >= 2)
            readLevel(doc, d.children, d.bodySpan.begin + 1, d.bodySpan.end - 1, n.children, n.tail, warnings);
        cursor = d.span.end;
        // A comment at the end of its line goes with it.
        size_t k = cursor;
        while (k < end && (text[k] == ' ' || text[k] == '\t'))
            ++k;
        if (k + 1 < end && text[k] == '/' && text[k + 1] == '/') {
            auto eol = text.find_first_of("\r\n", k);
            eol = eol == std::string_view::npos ? end : std::min(eol, end);
            n.comment = std::string(nss::trim(text.substr(k, eol - k)));
            cursor = eol;
        }
        out.push_back(std::move(n));
    }
    tail = keptText(text.substr(cursor, end > cursor ? end - cursor : 0));
}

ParseResult parse(const nss::NssDocument &doc)
{
    ParseResult r;
    // The header lines the writer puts first are its own, not kept text.
    std::string_view text = doc.text();
    size_t begin = 0;
    for (;;) {
        size_t k = begin;
        while (k < text.size() && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r' || text[k] == '\n'))
            ++k;
        auto eol = text.find_first_of("\r\n", k);
        auto line = nss::trim(text.substr(k, eol == std::string_view::npos ? std::string_view::npos : eol - k));
        if (line.empty() || !(nss::OverridesWriter::isHeader(line) || line == MenuWriter::intro))
            break;
        begin = eol == std::string_view::npos ? text.size() : eol;
    }
    readLevel(doc, doc.nodes(), begin, text.size(), r.tree, r.tail, r.warnings);
    return r;
}

ParseResult parse(std::string_view text)
{
    auto doc = nss::NssDocument::fromText(std::string(text));
    return parse(doc);
}

std::string summary(const Node &n)
{
    if (n.kind == Kind::Separator)
        return {};
    std::string out;
    auto loc = locationOf(n);
    if (loc == Location::Extensions)
        out = findToExtensions(n.find);
    else if (loc == Location::Custom)
        out = !n.where.empty() ? "where " + n.where : "type " + n.type + (n.find.empty() ? "" : " · find " + n.find);
    else if (loc != Location::Everywhere)
        out = locationLabel(loc);
    if (!n.mode.empty() && n.mode != "single")
        out += (out.empty() ? "" : " · ") + n.mode;
    if (n.kind == Kind::Item && !n.cmd.empty()) {
        auto c = n.cmd;
        auto slash = c.find_last_of("\\/");
        if (slash != std::string::npos && slash + 1 < c.size())
            c = c.substr(slash + 1);
        out += (out.empty() ? "" : " · ") + c;
    }
    if (out.size() > 80)
        out = out.substr(0, 77) + "...";
    return out;
}

} // namespace menus
