#include "MenuWriter.h"

#include "Tr.h"

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

std::string MenuWriter::generate(const Tree &tree, nss::Eol eol, std::string_view tail)
{
    const std::string nl(nss::eolString(eol));
    std::string out;
    out += header;
    out += nl;
    out += intro;
    out += nl;

    // Kept text, one line at a time at the entry's indentation.
    auto kept = [&](std::string_view text, const std::string &indent) {
        size_t at = 0;
        while (!text.empty() && at <= text.size()) {
            auto end = text.find('\n', at);
            auto line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
            out += (line.empty() ? std::string() : indent + std::string(line)) + nl;
            if (end == std::string_view::npos)
                break;
            at = end + 1;
        }
    };
    auto comment = [](const Node &n) { return n.comment.empty() ? std::string() : " " + n.comment; };
    auto write = [&](auto &&self, const Node &n, int depth) -> void {
        std::string indent(size_t(depth), '\t');
        kept(n.before, indent);
        auto p = props(n);
        out += indent;
        out += n.kind == Kind::Menu ? "menu" : n.kind == Kind::Item ? "item" : "separator";
        if (!p.empty() || n.kind != Kind::Separator)
            out += "(" + p + ")";
        if (n.kind != Kind::Menu)
            out += comment(n);
        out += nl;
        if (n.kind == Kind::Menu) {
            out += indent + "{" + nl;
            for (const auto &c : n.children)
                self(self, c, depth + 1);
            kept(n.tail, indent + "\t");
            out += indent + "}" + comment(n) + nl;
        }
    };
    for (const auto &n : tree) {
        out += nl;
        write(write, n, 0);
    }
    if (!tail.empty()) {
        out += nl;
        kept(tail, {});
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

std::string handEditProblem(const nss::NssDocument &doc, std::string_view name)
{
    if (doc.diagnostics().empty())
        return {};
    const auto &d = doc.diagnostics().front();
    // clang-format off
    return nsp::trf("{} was edited by hand, and line {} can't be read ({}). The app didn't change the file, so your edit is safe. Fix it in the Config Editor, then try again.",
                    std::string(name), d.line, d.message);
    // clang-format on
}

} // namespace menus
