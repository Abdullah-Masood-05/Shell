#include "MapRules.h"

#include "MenuWriter.h"

#include "nss/OverridesWriter.h"
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
        std::string line(rule);
        // One statement per line: a line break would end the "// " that turns it off.
        std::replace(line.begin(), line.end(), '\r', ' ');
        std::replace(line.begin(), line.end(), '\n', ' ');
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        out += line + nl; // blank lines too: the user's own
    }
    return out;
}

Rules read(const nss::NssDocument &doc)
{
    Rules out;
    std::string_view text = doc.text();
    size_t at = 0;
    bool top = true; // the header and blank lines before the first line of the file's own
    while (at < text.size()) {
        auto end = text.find_first_of("\r\n", at);
        auto line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        auto trimmed = nss::trim(line);
        if (!(top && (trimmed.empty() || nss::OverridesWriter::isHeader(trimmed)))) {
            top = false;
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.remove_suffix(1);
            out.emplace_back(line);
        }
        if (end == std::string_view::npos)
            break;
        at = end + (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n' ? 2 : 1);
    }
    while (!out.empty() && nss::trim(out.back()).empty())
        out.pop_back();
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
