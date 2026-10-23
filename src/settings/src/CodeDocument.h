#pragma once

// The text model behind the code editors (the Config Editor's edit view and
// the Menu map's "Edit code"): the file text, the text the TextInput shows
// (each tab as blank glyphs, see Highlight.h), and undo/redo. Free of UI code,
// so it can be tested on its own.
//
// Edits flow one way. The TextInput edits its own text while the user types
// and reports the result (input()); the text is written back into the
// TextInput only when what it shows must change: a tab re-padded or removed
// whole, a paste, Enter with indentation, Tab/Shift+Tab, undo and redo.
// Writing the text resets the TextInput's own undo stack, so history lives
// here, in file-text offsets (it survives tab re-padding), one per file.
//
// Selections in and out are byte offsets into display().

#include "Highlight.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace code {

struct Selection
{
    size_t anchor = 0;
    size_t cursor = 0;
    size_t begin() const { return anchor < cursor ? anchor : cursor; }
    size_t end() const { return anchor < cursor ? cursor : anchor; }
    bool empty() const { return anchor == cursor; }
    bool operator==(const Selection &) const = default;
};

// What an operation did, for the TextInput.
struct Result
{
    bool edited = false;   // the file text changed
    bool display = false;  // display() is not what the TextInput holds: write it back
    bool select = false;   // set `selection` (display offsets)
    Selection selection;
};

// '\r\n' and lone '\r' become '\n'.
std::string normalizeNewlines(std::string_view text);

class Document
{
public:
    explicit Document(std::string text = {}, int tabWidth = 4);

    // New content ('\n' line ends); history cleared.
    void reset(std::string text);

    const std::string &text() const { return text_; }
    const std::string &display() const { return display_; }
    const hl::TabGlyphs &glyphs() const { return glyphs_; }
    int tabWidth() const { return tabWidth_; }
    // What Tab inserts: "\t", or spaces when the file indents with spaces.
    const std::string &indentUnit() const { return unit_; }

    size_t toDisplay(size_t textOffset) const;
    // An offset inside a tab's glyphs maps to after the tab.
    size_t toText(size_t displayOffset) const;
    Selection toDisplay(Selection textSel) const { return { toDisplay(textSel.anchor), toDisplay(textSel.cursor) }; }
    Selection toText(Selection displaySel) const { return { toText(displaySel.anchor), toText(displaySel.cursor) }; }

    // The TextInput changed its own text (typing, Backspace, Delete, Ctrl+Backspace,
    // an IME commit) to `shown`, with the caret at `cursor`. `before` is the
    // selection when the key was pressed (for undo). Typed characters and runs
    // of Backspace/Delete are merged into one undo step; `nowMs` is a clock in
    // milliseconds (0: the steady clock).
    Result input(std::string_view shown, size_t cursor, Selection before, int64_t nowMs = 0);

    // Commands. Each one is its own undo step.
    Result paste(Selection sel, std::string_view clipboard); // newlines normalised, tabs kept
    Result cut(Selection sel);                                // the caller copies selectedText() first
    Result newline(Selection sel);                            // keeps the line's indentation
    Result tab(Selection sel);     // indents the lines when the selection spans lines, else inserts the unit
    Result outdent(Selection sel); // Shift+Tab
    Result replace(Selection sel, std::string_view text);
    Result undo();
    Result redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // The next typed character starts a new undo step.
    void closeGroup();

    // The selection's text with real tabs and '\n'.
    std::string selectedText(Selection sel) const;

    // Case-insensitive search for `query` (plain text, as typed). From the
    // selection: forwards from its end (or its start when `incremental`),
    // backwards from its start; wraps around.
    struct Match
    {
        Selection selection; // display offsets
        int index = 0;       // 1-based
        int count = 0;
    };
    std::optional<Match> find(std::string_view query, Selection from, bool backwards, bool incremental = false) const;

    // Where the view was when the user left this file (kept per file).
    struct View
    {
        bool saved = false;
        Selection selection; // file-text offsets
        float x = 0, y = 0;  // scroll offsets (logical px, <= 0)
    };
    View view;

private:
    enum class Kind { Typing, Delete, Other };
    struct Change
    {
        size_t pos = 0;
        std::string removed;
        std::string inserted;
        Selection before; // file-text offsets
        Selection after;
        Kind kind = Kind::Other;
        int64_t time = 0;
        bool open = false; // later typing may still merge into it
    };

    void refreshDisplay();
    void detectIndent();
    Result commit(Change c, int64_t nowMs, const std::string *shown);
    void applyRaw(size_t pos, size_t removeLen, std::string_view insert);
    Result selectText(Selection textSel) const;
    // The [begin, end) of the lines a selection covers (file-text offsets).
    std::pair<size_t, size_t> lineSpan(Selection textSel) const;

    std::string text_;
    std::string display_;
    hl::TabGlyphs glyphs_;
    int tabWidth_ = 4;
    std::string unit_ = "\t";
    std::vector<Change> undo_;
    std::vector<Change> redo_;
};

} // namespace code
