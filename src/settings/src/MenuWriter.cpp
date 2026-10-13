#include "MenuWriter.h"

#include "nss/ConfigGraph.h"
#include "nss/Literal.h"
#include "nss/OverridesWriter.h"

#include <initializer_list>

namespace menus {

std::string MenuWriter::plainString(std::string_view value) { return nss::quoteString(value); }

std::string MenuWriter::interpolatedString(std::string_view value)
{
    if (value.find('\'') == std::string_view::npos)
        return "'" + std::string(value) + "'";
    if (value.find('`') == std::string_view::npos)
        return "`" + std::string(value) + "`";
    return nss::quoteString(value);
}

std::string MenuWriter::props(const Node &n)
{
    std::string out;
    auto add = [&](std::string_view name, std::string_view value) {
        if (!out.empty())
            out += ' ';
        out += name;
        if (!value.empty()) {
            out += '=';
            out += value;
        }
    };
    // A property kept verbatim in `extra` wins over the (then empty) field.
    auto kept = [&](std::initializer_list<std::string_view> names) {
        for (const auto &p : n.extra)
            for (auto name : names)
                if (p.name == name)
                    return true;
        return false;
    };
    if (n.kind != Kind::Separator && !kept({ "title" }))
        add("title", plainString(n.title));
    if (!n.image.empty() && !kept({ "image", "icon" }))
        add("image", n.image);
    if (!n.type.empty())
        add("type", interpolatedString(n.type));
    if (!n.find.empty())
        add("find", interpolatedString(n.find));
    if (!n.where.empty())
        add("where", n.where);
    if (!n.mode.empty())
        add("mode", plainString(n.mode));
    if (!n.cmd.empty())
        add("cmd", interpolatedString(n.cmd));
    if (!n.args.empty())
        add("args", interpolatedString(n.args));
    if (n.admin)
        add("admin", {});
    if (!n.window.empty())
        add("window", n.window);
    if (!n.keys.empty())
        add("keys", plainString(n.keys));
    if (!n.tip.empty())
        add("tip", plainString(n.tip));
    if (!n.sep.empty())
        add("sep", plainString(n.sep));
    for (const auto &p : n.extra)
        add(p.name, p.value);
    return out;
}

std::string MenuWriter::generate(const Tree &tree, nss::Eol eol)
{
    const std::string nl(nss::eolString(eol));
    std::string out;
    out += header;
    out += nl;
    out += "// Edit these menus on the Menu Builder page of Nilesoft Shell Pro.";
    out += nl;

    auto write = [&](auto &&self, const Node &n, int depth) -> void {
        std::string indent(size_t(depth), '\t');
        auto p = props(n);
        out += indent;
        out += n.kind == Kind::Menu ? "menu" : n.kind == Kind::Item ? "item" : "separator";
        if (!p.empty() || n.kind != Kind::Separator)
            out += "(" + p + ")";
        out += nl;
        if (n.kind == Kind::Menu) {
            out += indent + "{" + nl;
            for (const auto &c : n.children)
                self(self, c, depth + 1);
            out += indent + "}" + nl;
        }
    };
    for (const auto &n : tree) {
        out += nl;
        write(write, n, 0);
    }
    return out;
}

int MenuWriter::findImport(const nss::NssDocument &root, std::string_view relPath)
{
    auto list = root.imports();
    for (size_t i = 0; i < list.size(); ++i) {
        const auto *n = list[i];
        if (n->importKind != nss::ImportKind::Config)
            continue;
        auto value = nss::ConfigGraph::evaluate(root.slice(n->valueSpan), {});
        if (value && nss::OverridesWriter::samePath(*value, relPath))
            return int(i);
    }
    return -1;
}

bool MenuWriter::ensureImported(nss::NssDocument &root, std::string_view relPath)
{
    if (findImport(root, relPath) >= 0)
        return false;
    return root.appendImport(relPath);
}

} // namespace menus
