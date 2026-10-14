#pragma once

// Syntax highlighting for the Config Editor, built from NssLexer tokens. The
// text is split into lines; each line is a list of spans of one kind.
//
// Tabs: the read view expands them to spaces. The edit view can't, because its
// TextInput must hold one character per tab-stop column without changing the
// file: Slint draws U+0009 as a missing-glyph box. There each tab is shown as a
// lead character followed by pad characters up to the next tab stop, both
// blank one-column spaces (TabGlyphs) that don't occur in the file, and mapped
// back to a single '\t' on every edit.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hl {

// Keep in sync with CodeColors.color() in ui/components/code.slint.
enum Kind : int
{
    Plain = 0,    // punctuation, operators, whitespace
    Keyword = 1,  // menu, item, import, theme, if, true, ...
    String = 2,
    Number = 3,
    Color = 4,
    Comment = 5,
    Ident = 6,
    Variable = 7, // $name, @
    Property = 8, // an identifier followed by '='
    Function = 9, // an identifier followed by '('
    Error = 10,   // unterminated strings and comments, unknown characters
};

struct Span
{
    std::string text;
    int kind = Plain;
    bool operator==(const Span &) const = default;
};

struct Line
{
    std::vector<Span> spans;
    bool operator==(const Line &) const = default;
};

// How the edit view shows a tab: `lead` then pads up to the next tab stop.
// With lead == 0 (no usable pair) tabs stay '\t'.
struct TabGlyphs
{
    char32_t lead = 0;
    char32_t pad = 0;
    bool valid() const { return lead != 0; }
};

// A pair of blank one-column spaces neither of which occurs in `text`
// (U+2007/U+2008, else U+2002/U+2003, else U+2009/U+200A); {} if none is free.
TabGlyphs pickTabGlyphs(std::string_view text);

// `text` with each tab replaced by its glyphs (columns count code points).
std::string tabsToDisplay(std::string_view text, TabGlyphs glyphs, int tabWidth = 4);

// The reverse: lead -> '\t', pads dropped, everything else kept.
std::string tabsFromDisplay(std::string_view display, TabGlyphs glyphs);

// The edit view's TextInput changed from `oldDisplay` (canonical, from
// tabsToDisplay) to `newDisplay`. Deleting any part of a tab's glyphs deletes
// the tab. Returns the file text, its canonical display, and the cursor (a
// byte offset into `display`) at the end of the change.
struct DisplayEdit
{
    std::string text;
    std::string display;
    size_t cursor = 0;
    bool changed = false; // display != newDisplay: the TextInput must be updated
};
DisplayEdit applyDisplayEdit(std::string_view oldDisplay, std::string_view newDisplay, TabGlyphs glyphs,
                             int tabWidth = 4);

// `text` is UTF-8 with any line endings. Without `glyphs` tabs become spaces;
// with them, the line text matches tabsToDisplay(text, *glyphs) exactly.
std::vector<Line> highlight(std::string_view text, int tabWidth = 4, const TabGlyphs *glyphs = nullptr);

// The per-byte kind of `text` (exposed for tests).
std::vector<unsigned char> classify(std::string_view text);

} // namespace hl
