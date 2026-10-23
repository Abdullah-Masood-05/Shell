#pragma once

// Glue between code::Document (src/CodeDocument.h) and the CodeEditor
// component (ui/components/code.slint), shared by the Config Editor and the
// Menu map's "Edit code".

#include "app.h"

#include "CodeDocument.h"
#include "Highlight.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codeui {

code::Selection selection(int anchor, int cursor);
EditResult toSlint(const code::Result &r);

// A CodeEditor command: undo, redo, copy, cut, paste, tab, outdent, newline.
// Copy and cut put the text on the Windows clipboard with real tabs and CRLF
// line ends; paste takes it with its line ends normalised. Read-only
// documents only copy.
code::Result command(code::Document &doc, std::string_view name, int anchor, int cursor, bool readOnly);

FindResult find(const code::Document &doc, std::string_view query, int anchor, int cursor, bool backwards,
                bool incremental);

// The selection by rows, for drawing it behind the highlighted text
// (CodeText.selection in ui/editor-types.slint). Offsets into `text`.
CodeSelection selectionRows(std::string_view text, int anchor, int cursor);

// The selection's text for the find box: "" unless it is on one line.
std::string findSeed(const code::Document &doc, int anchor, int cursor);

std::optional<std::string> readClipboard();
bool writeClipboard(std::string_view text); // '\n' line ends become CRLF

// The edit view's rows. update() replaces only the rows that changed, so
// typing doesn't rebuild the rows on screen.
class Rows
{
public:
    Rows();
    std::shared_ptr<slint::VectorModel<CodeLine>> model() const { return model_; }
    void update(std::vector<hl::Line> lines, std::vector<int> marks = {});
    // Only the rows of a hl::highlightPatch(); new rows have no problem marker.
    void patch(hl::LinePatch p);
    void clear();

private:
    CodeLine row(size_t i) const;
    std::shared_ptr<slint::VectorModel<CodeLine>> model_;
    std::vector<hl::Line> lines_;
    std::vector<int> marks_;
};

} // namespace codeui
