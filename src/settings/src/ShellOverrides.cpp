#include "ShellOverrides.h"

#include "MenuWriter.h"
#include "Tr.h"

#include "nss/Text.h"

#include "../../shared/ConfigUpgrade.h"

#include <algorithm>
#include <cstdlib>

namespace overrides {

namespace {

std::string slashes(std::string_view path)
{
    std::string out(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

std::string unquote(std::string_view s)
{
    s = nss::trim(s);
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return std::string(s);
}

std::string quotePath(std::string_view path)
{
    if (path.find('\'') == std::string_view::npos)
        return "'" + std::string(path) + "'";
    return "\"" + std::string(path) + "\"";
}

} // namespace

std::string sourceKey(std::string_view text)
{
    std::string out;
    bool space = false;
    auto put = [&](char c) {
        if (space && !out.empty())
            out += ' ';
        space = false;
        out += c;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\0')
            break;
        if (c == '\'' || c == '"' || c == '`') {
            // A string, as it is. In "..." a backslash escapes the next character.
            put(c);
            for (++i; i < text.size() && text[i]; ++i) {
                out += text[i];
                if (c == '"' && text[i] == '\\' && i + 1 < text.size() && text[i + 1])
                    out += text[++i];
                else if (text[i] == c)
                    break;
            }
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] && text[i] != '\n')
                ++i;
            space = true;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            for (i += 2; i < text.size() && text[i]; ++i)
                if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/') {
                    ++i;
                    break;
                }
            space = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = true;
            continue;
        }
        put(c);
    }
    return out;
}

bool Key::operator==(const Key &other) const
{
    return index == other.index && item == other.item && nss::iequals(slashes(file), slashes(other.file));
}

const std::string *Override::find(std::string_view name) const
{
    const std::string *found = nullptr;
    for (const auto &[n, v] : props)
        if (n == name)
            found = &v;
    return found;
}

std::string canonical(std::string_view name)
{
    auto n = nss::toLower(name);
    if (n == "vis" || n == "visibility")
        return "vis";
    if (n == "title" || n == "text")
        return "title";
    if (n == "image" || n == "icon")
        return "image";
    if (n == "image.sel" || n == "image.select" || n == "icon.sel" || n == "icon.select")
        return "image.sel";
    if (n == "menu" || n == "parent" || n == "sub" || n == "move")
        return "menu";
    if (n == "pos" || n == "position")
        return "pos";
    if (n == "sep" || n == "separator")
        return "sep";
    if (n == "tip" || n == "keys")
        return n;
    return {};
}

std::optional<Override> parse(std::string_view line)
{
    auto t = nss::trim(line);
    if (t.size() < 9 || !nss::iequals(t.substr(0, 8), "override"))
        return std::nullopt;
    auto doc = nss::NssDocument::fromText(std::string(t));
    if (!doc.diagnostics().empty() || doc.nodes().size() != 1)
        return std::nullopt;
    const auto &n = doc.nodes().front();
    if (n.kind != nss::NodeKind::MenuDecl || n.menuKind != nss::MenuKind::Override)
        return std::nullopt;
    Override o;
    for (const auto &p : n.props) {
        std::string value = p.hasValue ? std::string(nss::trim(doc.slice(p.valueSpan))) : std::string();
        if (p.name == "file")
            o.key.file = slashes(unquote(value));
        else if (p.name == "item")
            o.key.item = sourceKey(value);
        else if (p.name == "index")
            o.key.index = std::atoi(value.c_str());
        else {
            auto name = canonical(p.name);
            o.props.emplace_back(name.empty() ? p.name : name, value);
        }
    }
    if (o.key.file.empty() || o.key.item.empty() || o.key.index < 1)
        return std::nullopt;
    return o;
}

std::string format(const Override &o)
{
    std::string out = "override(file=" + quotePath(slashes(o.key.file)) + " item=" + o.key.item;
    if (o.key.index > 1)
        out += " index=" + std::to_string(o.key.index);
    for (const auto &[name, value] : o.props) {
        std::string v = value;
        // One line: a line break would end the "// " that turns it off.
        std::replace(v.begin(), v.end(), '\r', ' ');
        std::replace(v.begin(), v.end(), '\n', ' ');
        out += " " + name + "=" + v;
    }
    return out + ")";
}

std::vector<const nss::Node *> declarations(const nss::NssDocument &doc)
{
    std::vector<const nss::Node *> out;
    doc.visit([&](const nss::Node &n) {
        if (n.kind == nss::NodeKind::MenuDecl && (n.menuKind == nss::MenuKind::Menu || n.menuKind == nss::MenuKind::Item))
            out.push_back(&n);
        return true;
    });
    return out;
}

std::string titleText(const nss::NssDocument &doc, const nss::Node &decl)
{
    std::string out;
    for (const auto &p : decl.props)
        if ((p.name == "title" || p.name == "text") && p.hasValue)
            out = sourceKey(doc.slice(p.valueSpan)); // the last one: the one Shell keeps
    return out;
}

std::optional<Key> keyOf(const nss::NssDocument &doc, std::string_view rel, const nss::Node &decl)
{
    if (decl.kind != nss::NodeKind::MenuDecl || (decl.menuKind != nss::MenuKind::Menu && decl.menuKind != nss::MenuKind::Item))
        return std::nullopt;
    auto title = titleText(doc, decl);
    if (title.empty())
        return std::nullopt;
    Key key{ slashes(rel), title, 0 };
    for (const auto *d : declarations(doc)) {
        if (titleText(doc, *d) == title)
            ++key.index;
        if (d == &decl)
            return key;
    }
    return std::nullopt;
}

const nss::Node *find(const nss::NssDocument &doc, const Key &key)
{
    int n = 0;
    for (const auto *d : declarations(doc))
        if (titleText(doc, *d) == key.item && ++n == key.index)
            return d;
    return nullptr;
}

std::optional<std::string> get(const maprules::Rules &lines, const Key &key, std::string_view name)
{
    std::optional<std::string> out;
    for (const auto &line : lines)
        if (auto o = parse(line); o && o->key == key)
            if (const auto *v = o->find(name))
                out = *v;
    return out;
}

bool has(const maprules::Rules &lines, const Key &key)
{
    for (const auto &line : lines)
        if (auto o = parse(line); o && o->key == key)
            return true;
    return false;
}

bool set(maprules::Rules &lines, const Key &key, std::string_view name, const std::optional<std::string> &value)
{
    const auto before = lines;
    bool placed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto o = parse(lines[i]);
        if (!o || !(o->key == key))
            continue;
        std::vector<std::pair<std::string, std::string>> props;
        bool here = false;
        for (auto &p : o->props) {
            if (p.first != name) {
                props.push_back(std::move(p));
                continue;
            }
            // In its place on the first line for the item.
            if (value && !placed && !here) {
                props.emplace_back(std::string(name), *value);
                here = true;
            }
        }
        if (value && !placed && !here) {
            props.emplace_back(std::string(name), *value);
            here = true;
        }
        placed |= here;
        o->props = std::move(props);
        if (o->props.empty()) {
            lines.erase(lines.begin() + std::ptrdiff_t(i));
            --i;
            continue;
        }
        // Rewritten only when it changes, so a line written by hand stays as it is.
        auto again = parse(lines[i]);
        if (!again || again->props != o->props)
            lines[i] = format(*o);
    }
    if (value && !placed) {
        Override o;
        o.key = key;
        o.props.emplace_back(std::string(name), *value);
        lines.push_back(format(o));
    }
    return lines != before;
}

MapWrite planMapWrite(const std::filesystem::path &root,
                      const std::function<bool(maprules::Rules &lines, std::string &error)> &edit)
{
    namespace fs = std::filesystem;
    MapWrite out;
    std::string error;
    auto rootDoc = nss::NssDocument::load(root, {}, &error);
    if (!rootDoc) {
        out.error = nsp::trf("Couldn't read {}: {}", nss::narrow(root.wstring()), error);
        return out;
    }
    const auto path = (root.parent_path() / fs::path(nss::widen(maprules::relativePath))).lexically_normal();
    std::optional<nss::NssDocument> existing;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        existing = nss::NssDocument::load(path, {}, &error);
        if (!existing) {
            out.error = nsp::trf("Couldn't read {}: {}", nss::narrow(path.wstring()), error);
            return out;
        }
        out.error = menus::handEditProblem(*existing, nss::narrow(path.filename().wstring()));
        if (!out.error.empty())
            return out;
    }
    maprules::Rules lines = existing ? maprules::read(*existing) : maprules::Rules{};
    const auto before = lines;
    if (!edit(lines, out.error))
        return out;
    out.ok = true;
    if (lines == before)
        return out;
    auto format = existing ? existing->format() : rootDoc->format();
    auto doc = nss::NssDocument::fromText(maprules::generate(lines, format.eol), format);
    if (!doc.diagnostics().empty()) {
        out.ok = false;
        out.error = nsp::trf("The generated settings-app-map.nss doesn't parse: {}", doc.diagnostics().front().message);
        return out;
    }
    out.writes.push_back(nss::FileWrite{ path, doc.text(), doc.format() });
    if (maprules::ensureImported(*rootDoc))
        out.writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
    return out;
}

bool isShipped(std::string_view rel)
{
    auto key = nss::toLower(slashes(rel));
    if (config_upgrade::is_app_owned(key))
        return false;
    for (const auto &k : config_upgrade::known_hashes)
        if (key == k.path)
            return true;
    return false;
}

} // namespace overrides
