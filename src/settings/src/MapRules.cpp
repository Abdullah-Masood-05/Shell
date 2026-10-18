#include "MapRules.h"

#include "MenuWriter.h"

#include "nss/Text.h"

#include <algorithm>

namespace maprules {

std::string_view uncommented(std::string_view line)
{
    line = nss::trim(line);
    if (line.rfind("//", 0) == 0)
        line = nss::trim(line.substr(2));
    return line;
}

std::string generate(const Rules &rules, nss::Eol eol)
{
    const std::string nl(nss::eolString(eol));
    std::string out(header);
    out += nl;
    for (const auto &rule : rules) {
        std::string line(nss::trim(rule));
        // One statement per line: a line break would end the "// " that turns it off.
        std::replace(line.begin(), line.end(), '\r', ' ');
        std::replace(line.begin(), line.end(), '\n', ' ');
        if (!line.empty())
            out += line + nl;
    }
    return out;
}

Rules read(const nss::NssDocument &doc)
{
    Rules out;
    std::string_view text = doc.text();
    size_t at = 0;
    while (at <= text.size()) {
        auto end = text.find('\n', at);
        auto line = nss::trim(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (!line.empty() && line != header)
            out.emplace_back(line);
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return out;
}

bool ensureImported(nss::NssDocument &root, std::string_view relPath)
{
    if (menus::MenuWriter::findImport(root, relPath) >= 0)
        return false;
    for (const auto *n : root.imports()) {
        if (n->importKind != nss::ImportKind::Config || !n->context.empty())
            continue;
        // The start of the import's line, so the new one gets a line of its own.
        size_t pos = n->span.begin;
        const auto &text = root.text();
        while (pos > 0 && text[pos - 1] != '\n')
            --pos;
        std::string stmt = "import " + nss::quoteImportPath(relPath) + std::string(root.eol());
        root.replace({ pos, pos }, stmt);
        return true;
    }
    return root.appendImport(relPath);
}

} // namespace maprules
