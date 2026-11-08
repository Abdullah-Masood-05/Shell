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
        n.seq = getInt(j, "seq");
        n.shows = getString(j, "shows");
        if (n.shows != "yes" && n.shows != "no" && n.shows != "maybe")
            n.shows.clear();
        n.showsSample = getBool(j, "shows_sample");
        n.showsWhy = getString(j, "shows_why");
        n.titleSample = stripAccelerators(getString(j, "title_sample"));
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
        n.pos = getString(j, "pos");
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

bool notInMenu(const Node &n) { return n.origin == Origin::Shell && n.shows == "no"; }

std::optional<std::vector<MenuRef>> mergedMenu(const Tree &t)
{
    if (!t.hasOrder)
        return std::nullopt;
    std::vector<MenuRef> out;
    std::vector<bool> shellUsed(t.shell.size()), systemUsed(t.system.size());
    for (const auto &ref : t.order) {
        const auto &list = ref.shell ? t.shell : t.system;
        auto &used = ref.shell ? shellUsed : systemUsed;
        if (ref.index < 0 || size_t(ref.index) >= list.size() || used[size_t(ref.index)])
            continue;
        used[size_t(ref.index)] = true;
        if (!notInMenu(list[size_t(ref.index)]))
            out.push_back(MenuRef{ &list[size_t(ref.index)], size_t(ref.index) });
    }
    // Not in the order: added here (addUnlisted, at the end of Windows' items), or
    // one the order missed. At the end, so nothing is lost.
    for (size_t i = 0; i < t.shell.size(); ++i)
        if (!shellUsed[i] && !notInMenu(t.shell[i]))
            out.push_back(MenuRef{ &t.shell[i], i });
    for (size_t i = 0; i < t.system.size(); ++i)
        if (!systemUsed[i])
            out.push_back(MenuRef{ &t.system[i], i });
    return out;
}

Placing placingOf(const Node &n)
{
    if (n.origin == Origin::Shell) {
        if (notInMenu(n))
            return Placing::Out;
        if (n.vis == "hidden")
            return Placing::Hidden;
        // menu= names a menu this tab doesn't have: nothing puts it in this menu.
        if (!n.menu.empty() && !n.relocated && !isRootPath(n.menu))
            return Placing::Out;
        return Placing::Menu;
    }
    if (n.unlisted)
        return Placing::Out;
    if (n.hidden)
        return Placing::Hidden;
    if (!n.moved.empty() && !isRootPath(n.moved))
        return Placing::Moved;
    return Placing::Menu;
}

std::string menuPathKey(std::string_view path)
{
    std::string out;
    for (char c : stripAccelerators(path))
        out += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    // " / " around a slash and the '/' at both ends mean nothing.
    std::string clean;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/') {
            while (!clean.empty() && clean.back() == ' ')
                clean.pop_back();
            clean += '/';
            while (i + 1 < out.size() && out[i + 1] == ' ')
                ++i;
            continue;
        }
        clean += out[i];
    }
    size_t b = 0, e = clean.size();
    while (b < e && (clean[b] == '/' || clean[b] == ' '))
        ++b;
    while (e > b && (clean[e - 1] == '/' || clean[e - 1] == ' '))
        --e;
    return clean.substr(b, e - b);
}

std::vector<size_t> collapseSeparators(const std::vector<bool> &separator)
{
    std::vector<size_t> kept;
    bool lastSeparator = true; // nothing yet: a separator first is left out
    for (size_t i = 0; i < separator.size(); ++i) {
        if (separator[i] && lastSeparator)
            continue;
        kept.push_back(i);
        lastSeparator = separator[i];
    }
    while (!kept.empty() && separator[kept.back()])
        kept.pop_back();
    return kept;
}

std::vector<std::string> childTitles(const Node &menu, size_t max, bool *more)
{
    std::vector<std::string> out;
    if (more)
        *more = false;
    for (const auto &c : menu.children) {
        if (c.kind == "separator")
            continue;
        const std::string &t = !c.title.empty() ? c.title : c.titleSample;
        if (t.empty())
            continue;
        if (out.size() == max) {
            if (more)
                *more = true;
            break;
        }
        out.push_back(t);
    }
    return out;
}

std::string shellKey(const fs::path &file, int line, int column)
{
    std::string path = nss::narrow(file.wstring());
    for (auto &ch : path)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return "s|" + path + "|" + std::to_string(line) + "|" + std::to_string(column);
}

std::optional<MenuList> buildMenuList(const Tree &tree, const std::map<std::string, int> &pinned)
{
    auto order = mergedMenu(tree);
    if (!order)
        return std::nullopt;
    MenuList list;
    auto &entries = list.entries;
    auto addGroup = [&](int group, const char *key) {
        ListEntry h;
        h.group = group;
        h.key = key;
        entries.push_back(h);
        return int(entries.size()) - 1;
    };
    const int mainIdx = addGroup(3, "m");
    const int hiddenIdx = addGroup(4, "h");
    const int outIdx = addGroup(2, "x");

    auto uniqueShellKey = [&](const Node &n) {
        const auto base = shellKey(n.file, n.line, n.column);
        auto key = base;
        auto taken = [&](const std::string &k) {
            return std::any_of(entries.begin(), entries.end(), [&](const ListEntry &x) { return x.key == k; });
        };
        for (int k = 2; taken(key); ++k)
            key = base + "#" + std::to_string(k);
        return key;
    };
    auto systemKey = [](const std::string &prefix, size_t i, const Node &n) {
        std::string name = n.name;
        for (auto &ch : name)
            if (ch >= 'A' && ch <= 'Z')
                ch = char(ch - 'A' + 'a');
        return prefix + "/" + std::to_string(i) + ":" + name;
    };

    // One of Windows' items that a rule sends to a menu: listed in that menu once it is built.
    struct MovedItem
    {
        const Node *node = nullptr;
        size_t index = 0;
        std::string prefix;
        std::string from;
    };
    std::vector<MovedItem> moved;

    using Steps = std::vector<MenuStep>;
    std::function<void(const Node &, size_t, int, int, const std::string &, const std::string &, const Steps &,
                       const std::string &)>
        addNode;
    // Makes the entry (and, below it, its items) in `parent`; `group` is the group it is in.
    auto make = [&](const Node &n, size_t i, int parent, int depth, const std::string &prefix, const std::string &menuPath,
                    const Steps &steps, const std::string &from, int group, bool movedIn) {
        ListEntry e;
        e.node = &n;
        e.group = group;
        e.depth = depth;
        e.parent = parent;
        e.from = from;
        e.movedIn = movedIn;
        e.key = n.origin == Origin::System ? systemKey(prefix, i, n) : uniqueShellKey(n);
        if (n.kind == "menu") {
            if (n.origin == Origin::System) {
                std::string name = n.name;
                for (auto &ch : name)
                    if (ch >= 'A' && ch <= 'Z')
                        ch = char(ch - 'A' + 'a');
                e.menuPath = n.path.empty() ? name : n.path + "/" + name;
            } else if (n.state == "text") {
                e.menuPath = menuPath.empty() ? n.rawTitle : menuPath + "/" + n.rawTitle;
                e.steps = steps;
                e.steps.push_back(MenuStep{ n.rawTitle, n.titleSource });
            }
        }
        const int idx = int(entries.size());
        entries.push_back(e);
        entries[size_t(parent)].kids.push_back(idx);
        const std::string myTitle = n.title.empty() ? n.titleSample : n.title;
        const std::string myMenuPath = e.menuPath;
        const Steps mySteps = e.steps;
        const std::string myKey = e.key;
        for (size_t k = 0; k < n.children.size(); ++k)
            addNode(n.children[k], k, idx, depth + 1, myKey, myMenuPath, mySteps, myTitle);
        return idx;
    };
    // Where `n` goes, by what shell.exe says about it (placingOf) and, after a switch, where
    // it was (pinned).
    addNode = [&](const Node &n, size_t i, int parent, int depth, const std::string &prefix, const std::string &menuPath,
                  const Steps &steps, const std::string &parentTitle) {
        const int parentGroup = entries[size_t(parent)].group;
        if (parentGroup == 2 || parentGroup == 4) {
            // Inside a menu that isn't in the menu: everything in it comes along.
            make(n, i, parent, depth, prefix, menuPath, steps, {}, parentGroup, false);
            return;
        }
        const auto placing = placingOf(n);
        const int own = n.origin == Origin::Shell ? 0 : 1;
        if (n.kind == "separator") {
            if (placing == Placing::Menu)
                make(n, i, parent, depth, prefix, menuPath, steps, {}, own, false);
            return;
        }
        if (placing == Placing::Moved) {
            moved.push_back(MovedItem{ &n, i, prefix, parentTitle });
            return;
        }
        const std::string base = n.origin == Origin::System ? systemKey(prefix, i, n) : shellKey(n.file, n.line, n.column);
        int disp = placing == Placing::Menu ? 0 : placing == Placing::Hidden ? 1 : 2;
        if (auto it = pinned.find(base); it != pinned.end())
            disp = it->second;
        list.placed[base] = disp;
        if (disp == 0)
            make(n, i, parent, depth, prefix, menuPath, steps, {}, own, false);
        else
            make(n, i, disp == 1 ? hiddenIdx : outIdx, 1, prefix, menuPath, steps, parentTitle, disp == 1 ? 4 : 2, false);
    };

    for (const auto &ref : *order)
        addNode(*ref.node, ref.index, mainIdx, 1, ref.node->origin == Origin::Shell ? "s" : "w", {}, {}, {});
    // Your items the order leaves out (where=, find=, a menu that isn't here...): not in this menu.
    for (size_t i = 0; i < tree.shell.size(); ++i)
        if (notInMenu(tree.shell[i]) && tree.shell[i].kind != "separator")
            make(tree.shell[i], i, outIdx, 1, "s", {}, {}, {}, 2, false);

    // Windows' items sent to a menu go in that menu, after its own items. A menu this tab
    // doesn't have leaves them out of the menu.
    for (size_t m = 0; m < moved.size(); ++m) {
        const MovedItem item = moved[m];
        const auto want = menuPathKey(item.node->moved);
        int target = -1;
        for (size_t t = 0; t < entries.size() && target < 0; ++t) {
            const auto &te = entries[t];
            if (te.node && te.node->kind == "menu" && (te.group == 0 || te.group == 1) && !te.menuPath.empty() &&
                menuPathKey(te.menuPath) == want)
                target = int(t);
        }
        if (target >= 0)
            make(*item.node, item.index, target, entries[size_t(target)].depth + 1, item.prefix, {}, {}, item.from, 1, true);
        else
            make(*item.node, item.index, outIdx, 1, item.prefix, {}, {}, item.from, 2, false);
    }

    // The separators the real menu keeps: none first or last, never two together.
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto &e = entries[i];
        if (!(e.group == 3 || ((e.group == 0 || e.group == 1) && e.node && e.node->kind == "menu")))
            continue;
        std::vector<bool> separator;
        for (int k : e.kids)
            separator.push_back(entries[size_t(k)].node && entries[size_t(k)].node->kind == "separator");
        auto kept = collapseSeparators(separator);
        std::vector<int> kids;
        for (size_t k : kept)
            kids.push_back(e.kids[k]);
        for (int k : e.kids)
            if (std::find(kids.begin(), kids.end(), k) == kids.end())
                entries[size_t(k)].dropped = true;
        entries[i].kids = std::move(kids);
    }
    return list;
}

bool canMoveInto(const std::vector<ListEntry> &entries, int target, int selected)
{
    if (target < 0 || size_t(target) >= entries.size())
        return false;
    const auto &t = entries[size_t(target)];
    if (!t.node || t.node->kind != "menu" || (t.group != 0 && t.group != 1))
        return false;
    for (int p = target; p >= 0; p = entries[size_t(p)].parent) {
        if (p == selected)
            return false;
        const int g = entries[size_t(p)].group;
        if (g == 2 || g == 4)
            return false;
    }
    return true;
}

bool showsSometimes(const Node &n)
{
    if (n.origin != Origin::Shell)
        return false;
    if (!n.shows.empty())
        return n.shows == "maybe";
    // An older shell.exe: any where=, find= or vis= that isn't a constant.
    bool sometimes = n.vis == "dynamic";
    for (const auto &c : n.conditions)
        sometimes |= c == "where" || c == "find";
    return sometimes;
}

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
            if (n.is_object()) {
                list.push_back(readNode(n, std::string_view(key) == "shell" ? Origin::Shell : Origin::System));
                list.back().index = int(list.size()) - 1;
            }
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
            rule.hasPos = getBool(r, "pos");
            t.rules.push_back(std::move(rule));
        }
    t.systemError = getString(*tree, "system_error");
    if (auto it = tree->find("order"); it != tree->end() && it->is_array()) {
        t.hasOrder = true;
        for (const auto &o : *it)
            if (o.is_object())
                t.order.push_back(OrderRef{ getString(o, "from") == "shell", getInt(o, "index") });
    }
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

std::optional<std::string> ruleProp(const Node &n, std::string_view rule, std::string_view prop)
{
    auto m = matchProps(n);
    if (m.empty())
        return std::nullopt;
    const std::string prefix = "modify(" + m + " " + std::string(prop) + "=";
    if (rule.size() <= prefix.size() + 1 || rule.substr(0, prefix.size()) != prefix || rule.back() != ')')
        return std::nullopt;
    auto lit = nss::Literal::parse(rule.substr(prefix.size(), rule.size() - prefix.size() - 1));
    if (lit.kind != nss::LiteralKind::String)
        return std::nullopt;
    return lit.string;
}

std::string posRule(const Node &n, std::string_view value)
{
    auto m = matchProps(n);
    auto v = nss::trim(value);
    if (m.empty() || v.empty() || v.find_first_of("\r\n") != std::string_view::npos)
        return {};
    return "modify(" + m + " pos=" + std::string(v) + ")";
}

std::string imageRule(const Node &n, std::string_view value)
{
    auto m = matchProps(n);
    auto v = nss::trim(value);
    if (m.empty() || v.empty() || v.find_first_of("\r\n") != std::string_view::npos)
        return {};
    return "modify(" + m + " image=" + std::string(v) + ")";
}

namespace {

bool endsWithNoCase(std::string_view s, std::string_view end)
{
    if (s.size() < end.size())
        return false;
    for (size_t i = 0; i < end.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - end.size() + i])) != end[i])
            return false;
    return true;
}

// "C:\a.dll,3" -> "C:\a.dll": the icon index after the file.
std::string_view withoutIndex(std::string_view path)
{
    auto comma = path.rfind(',');
    if (comma == std::string_view::npos)
        return path;
    auto index = path.substr(comma + 1);
    if (!index.empty() && index.front() == '-')
        index.remove_prefix(1);
    if (index.empty() || index.find_first_not_of("0123456789") != std::string_view::npos)
        return path;
    return path.substr(0, comma);
}

// A file, typed as it is: C:\..., \\server\..., %SystemRoot%\..., @sys.dir\..., or
// a name such as shell32.dll,4 or a.ico.
bool pathLike(std::string_view t)
{
    if (t.empty() || t.front() == '"' || t.front() == '\'' || t.front() == '[' || t.find('(') != std::string_view::npos)
        return false;
    if (t.size() >= 3 && std::isalpha(static_cast<unsigned char>(t[0])) && t[1] == ':' && (t[2] == '\\' || t[2] == '/'))
        return true;
    if (t.rfind("\\\\", 0) == 0 || t.front() == '%')
        return true;
    if (t.front() == '@' && t.find_first_of("\\/") != std::string_view::npos)
        return true;
    auto file = withoutIndex(t);
    for (auto ext : { ".ico", ".png", ".bmp", ".jpg", ".jpeg", ".gif", ".tif", ".tiff", ".exe", ".dll", ".mui", ".lnk", ".svg" })
        if (endsWithNoCase(file, ext))
            return true;
    return false;
}

// The text of a plain string literal: "..." with only \\ and \" escaped, or '...'
// without @ or escapes. nullopt for anything else.
std::optional<std::string> plainString(std::string_view v)
{
    if (v.size() < 2 || v.front() != v.back() || (v.front() != '"' && v.front() != '\''))
        return std::nullopt;
    const char q = v.front();
    auto inner = v.substr(1, v.size() - 2);
    std::string out;
    for (size_t i = 0; i < inner.size(); ++i) {
        char c = inner[i];
        if (c == q || (q == '\'' && c == '@'))
            return std::nullopt;
        if (c == '\\' && q == '"') {
            if (i + 1 >= inner.size() || (inner[i + 1] != '\\' && inner[i + 1] != '"'))
                return std::nullopt;
            c = inner[++i];
        } else if (c == '\\' && i + 1 < inner.size() && inner[i + 1] == '\'') {
            return std::nullopt;
        }
        out += c;
    }
    return out;
}

} // namespace

std::string imageValue(std::string_view typed)
{
    auto t = nss::trim(typed);
    if (t.empty() || t.find_first_of("\r\n") != std::string_view::npos)
        return {};
    if (!pathLike(t))
        return std::string(t);
    // '@' starts an expression only in single quotes (@sys.dir\shell32.dll,4).
    std::string quoted = t.front() == '@' && t.find('\'') == std::string_view::npos ? "'" + std::string(t) + "'"
                                                                                   : nss::quoteString(t);
    if (endsWithNoCase(withoutIndex(t), ".svg"))
        return "image.svgf(" + quoted + ")";
    return quoted;
}

std::string imageText(std::string_view value)
{
    auto v = nss::trim(value);
    std::string_view inner = v;
    const bool svg = v.rfind("image.svgf(", 0) == 0 && v.size() > 12 && v.back() == ')';
    if (svg)
        inner = nss::trim(v.substr(11, v.size() - 12));
    if (auto text = plainString(inner); text && pathLike(*text) && endsWithNoCase(withoutIndex(*text), ".svg") == svg)
        return *text;
    return std::string(v);
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

const nss::Node *findDeclaration(const nss::NssDocument &doc, size_t line, size_t column)
{
    return findDecl(doc, line, column);
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
    if (name == "image")
        return { "image", "icon" };
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

std::optional<std::string> ruleImage(std::string_view rule)
{
    auto doc = nss::NssDocument::fromText(std::string(nss::trim(rule)));
    return shellItemProp(doc, 1, 0, "image");
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

std::vector<Place> placesNextTo(const std::vector<Sibling> &siblings, size_t index)
{
    std::vector<Place> out;
    for (size_t i = 0; i < siblings.size(); ++i) {
        const auto &sib = siblings[i];
        if (i == index || sib.separator || !sib.titled || !sib.shown)
            continue;
        auto title = stripAccelerators(sib.step.rawTitle);
        for (bool after : { false, true })
            if (auto v = posValue(sib.step, after); !v.empty())
                out.push_back(Place{ title, after, std::move(v), i });
    }
    return out;
}

Sibling siblingOf(const Node &n, const Node *self, bool shown, std::string_view moved, std::string_view renamed)
{
    Sibling s;
    s.own = n.origin == Origin::Shell && n.line > 0;
    s.separator = n.kind == "separator";
    s.titled = n.origin == Origin::Shell ? n.state == "text" : !n.title.empty();
    s.step = { n.rawTitle, n.titleSource };
    s.file = n.file;
    s.line = n.line;
    s.column = n.column;
    s.fixed = !n.menu.empty() || (!n.pos.empty() && n.pos != "auto") || n.relocated;
    s.shown = shown;
    if (n.origin == Origin::Shell) {
        // Sent to another menu by its menu= (and listed here where it is written). menu='/'
        // is the main menu: in this one it stays.
        if (!n.menu.empty() && !n.relocated && !isRootPath(n.menu))
            s.shown = false;
        // Not in this menu for the sample (where=, find= or vis= leave it out).
        if (notInMenu(n))
            s.shown = false;
        const bool placedInOrder = !n.pos.empty() && n.pos != "auto" && n.pos != "top" && n.pos != "middle" && n.pos != "bottom";
        // Yours go in before Windows' items with a pos= are, so a Windows item is no obstacle,
        // and a Windows item being placed finds all of yours.
        if (placedInOrder && self && self->origin == Origin::Shell && &n != self) {
            // Declared before it, in whichever file: the config's order decides (seq). An older
            // shell.exe doesn't give it: the same file and an earlier line.
            const bool before = n.seq > 0 && self->seq > 0
                                    ? n.seq < self->seq
                                    : n.file == self->file && n.line > 0 &&
                                          (n.line < self->line || (n.line == self->line && n.column < self->column));
            if (!before) {
                s.titled = false;
                s.laterInOrder = n.state == "text" && n.pos.find_first_not_of("0123456789") != std::string::npos;
            }
        }
        return s;
    }
    if (!moved.empty()) {
        // Where it is listed: n.path ("" the main menu). Moved anywhere else, it is
        // taken out of this menu.
        auto key = [](std::string_view path) {
            std::string out;
            for (char c : stripAccelerators(path))
                out += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
            auto t = nss::trim(out);
            while (!t.empty() && (t.front() == '/' || t.back() == '/'))
                t = nss::trim(t.front() == '/' ? t.substr(1) : t.substr(0, t.size() - 1));
            return std::string(t);
        };
        const bool here = isRootPath(moved) ? n.path.empty() : key(moved) == key(n.path);
        if (!here)
            s.shown = false;
    }
    if (!renamed.empty())
        s.step = { std::string(renamed), "" };
    s.rulable = n.kind != "separator" && !matchProps(n).empty();
    // A Windows item with a pos=N or pos=indexof() of its own is placed after yours, and after
    // the ones of Windows' before it: not in the menu yet when the one being placed is.
    const bool custom = !n.pos.empty() && n.pos != "auto" && n.pos != "top" && n.pos != "middle" && n.pos != "bottom";
    if (custom && self && &n != self) {
        const bool before = self->origin == Origin::System && n.index < self->index;
        if (!before)
            s.titled = false;
    }
    return s;
}

Reorder planReorder(const std::vector<Sibling> &siblings, size_t index, bool up)
{
    Reorder r;
    if (index >= siblings.size())
        return r;
    const auto &self = siblings[index];
    if ((!self.own && !self.rulable) || self.separator)
        return r;
    // The next sibling that shows, from `from` on (`from` itself first), or none.
    // Hidden ones are left out of the menu, so stepping over them is what the move
    // looks like; naming one in pos=indexof() wouldn't find it.
    constexpr size_t none = size_t(-1);
    auto shownFrom = [&](size_t from) {
        for (size_t i = from; i < siblings.size(); i = up ? i - 1 : i + 1)
            if (siblings[i].shown)
                return i;
        return none;
    };
    if (up ? index == 0 : index + 1 >= siblings.size())
        return r;
    const size_t next = shownFrom(up ? index - 1 : index + 1);
    if (next == none)
        return r;
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
        const size_t beyond = (up ? next > 0 : next + 1 < siblings.size()) ? shownFrom(up ? next - 1 : next + 1) : none;
        if (beyond == none)
            pos = nss::quoteString(up ? "top" : "bottom");
        else if (const auto &b = siblings[beyond]; b.titled && !b.separator)
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
