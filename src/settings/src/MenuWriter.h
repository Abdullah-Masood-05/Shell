#pragma once

// MenuWriter: the app-owned imports/settings-app-menus.nss.
//
// generate() turns a Menu Builder tree into .nss text deterministically: the
// same tree always gives the same bytes, and parse(generate(t)) == t for any
// tree the builder can produce. ensureImported() adds
// `import 'imports/settings-app-menus.nss'` to the end of the root shell.nss
// once; an existing import of it (on or off) is left alone, so turning the
// file off on the Context Menus page sticks.
//
// The user may edit the file by hand: what the builder doesn't edit (comments,
// other statements, unknown properties) is read into the tree (Node::before,
// comment, tail, extra; ParseResult::tail) and written back by generate().

#include "MenuModel.h"

#include "nss/NssDocument.h"
#include "nss/OverridesWriter.h"
#include "nss/Text.h"

#include <string>
#include <string_view>

namespace menus {

struct MenuWriter
{
    static constexpr std::string_view header = nss::OverridesWriter::header;
    static constexpr std::string_view intro = "// Edit these menus on the Menu Builder page of Nilesoft Shell Pro.";
    static constexpr std::string_view relativePath = "imports/settings-app-menus.nss";

    // `tail`: ParseResult::tail, the kept text after the last entry.
    static std::string generate(const Tree &tree, nss::Eol eol = nss::Eol::CRLF, std::string_view tail = {});

    // One property list "(title=... image=...)" without the keyword; "" when empty.
    static std::string props(const Node &n);

    // Returns true when the root changed.
    static bool ensureImported(nss::NssDocument &root, std::string_view relPath = relativePath);

    // The index (into root.imports()) of the import of relPath, or -1.
    static int findImport(const nss::NssDocument &root, std::string_view relPath = relativePath);

    // Quoting used by generate().
    static std::string plainString(std::string_view value);       // "..." with escapes
    static std::string interpolatedString(std::string_view value); // '...', `...`, else "..."
};

// A plain message when `doc` (an app-owned file the user edited by hand)
// doesn't parse, so the page won't overwrite it; "" when it parses.
// `name`: the file name shown to the user.
std::string handEditProblem(const nss::NssDocument &doc, std::string_view name);

} // namespace menus
