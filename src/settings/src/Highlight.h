#pragma once

// Syntax highlighting for the Config Editor's read view, built from NssLexer
// tokens. The text is split into lines; each line is a list of spans of one
// kind. Tabs are expanded to the next multiple of four columns.

#include <string>
#include <string_view>
#include <vector>

namespace hl {

// Keep in sync with CodeKind in ui/editor-types.slint.
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
};

struct Line
{
    std::vector<Span> spans;
};

// `text` is UTF-8 with any line endings.
std::vector<Line> highlight(std::string_view text, int tabWidth = 4);

// The per-byte kind of `text` (exposed for tests).
std::vector<unsigned char> classify(std::string_view text);

} // namespace hl
