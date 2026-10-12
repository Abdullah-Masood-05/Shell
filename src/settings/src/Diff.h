#pragma once

// A small line diff (Myers' O(ND) algorithm) for the Backups page.

#include <string>
#include <string_view>
#include <vector>

namespace diff {

enum class Op
{
    Same,
    Added,   // only in the new text
    Removed, // only in the old text
};

struct Edit
{
    Op op = Op::Same;
    int oldLine = 0; // 1-based; 0 when the line is not in the old text
    int newLine = 0; // 1-based; 0 when the line is not in the new text
};

// Splits on \r\n, \n or \r. A final line break does not start an extra line.
std::vector<std::string> splitLines(std::string_view text);

// The shortest edit script from `a` to `b`, in order. When the texts differ in
// more than `maxEdits` lines the result is still correct but not minimal (every
// line of the differing middle is removed and then added).
std::vector<Edit> diffLines(const std::vector<std::string> &a, const std::vector<std::string> &b,
                            int maxEdits = 4000);

// One row of a unified view.
struct Row
{
    enum Kind
    {
        Context = 0,
        Add = 1,
        Del = 2,
        Hunk = 3, // "@@ -a,b +c,d @@" separator between hunks
    };
    Kind kind = Context;
    int oldLine = 0;
    int newLine = 0;
    std::string text;
};

struct Unified
{
    std::vector<Row> rows;
    int added = 0;
    int removed = 0;
};

// Hunks with `context` unchanged lines around each change.
Unified unified(std::string_view oldText, std::string_view newText, int context = 3);

} // namespace diff
