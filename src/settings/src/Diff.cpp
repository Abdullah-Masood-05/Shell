#include "Diff.h"

#include <algorithm>
#include <unordered_map>

namespace diff {

std::vector<std::string> splitLines(std::string_view text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c != '\r' && c != '\n')
            continue;
        lines.emplace_back(text.substr(start, i - start));
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            ++i;
        start = i + 1;
    }
    if (start < text.size())
        lines.emplace_back(text.substr(start));
    return lines;
}

namespace {

// Myers' greedy algorithm on interned lines; `out` gets the script for
// a[0..n) -> b[0..m), with line numbers offset by `base` (0-based).
bool myers(const int *a, int n, const int *b, int m, int baseA, int baseB, int maxEdits, std::vector<Edit> &out)
{
    const int max = n + m;
    if (max == 0)
        return true;
    std::vector<int> v(size_t(2 * max + 3), 0);
    const int off = max + 1;
    std::vector<std::vector<int>> trace; // trace[d][k + d + 1] = v[k] before step d, k in [-d-1, d+1]
    int found = -1;
    for (int d = 0; d <= max; ++d) {
        if (d > maxEdits)
            return false;
        std::vector<int> slice(size_t(2 * d + 3));
        for (int k = -d - 1; k <= d + 1; ++k)
            slice[size_t(k + d + 1)] = v[size_t(k + off)];
        trace.push_back(std::move(slice));
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[size_t(k - 1 + off)] < v[size_t(k + 1 + off)]))
                x = v[size_t(k + 1 + off)];
            else
                x = v[size_t(k - 1 + off)] + 1;
            int y = x - k;
            while (x < n && y < m && a[x] == b[y]) {
                ++x;
                ++y;
            }
            v[size_t(k + off)] = x;
            if (x >= n && y >= m) {
                found = d;
                break;
            }
        }
        if (found >= 0)
            break;
    }

    std::vector<Edit> rev;
    int x = n, y = m;
    for (int d = found; d >= 0; --d) {
        const auto &tv = trace[size_t(d)];
        auto at = [&](int k) { return tv[size_t(k + d + 1)]; };
        int k = x - y;
        int prevK = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
        int prevX = d == 0 ? 0 : at(prevK);
        int prevY = prevX - prevK;
        if (d == 0) {
            prevX = 0;
            prevY = 0;
        }
        while (x > prevX && y > prevY) {
            rev.push_back(Edit{ Op::Same, baseA + x, baseB + y });
            --x;
            --y;
        }
        if (d > 0) {
            if (x == prevX)
                rev.push_back(Edit{ Op::Added, 0, baseB + y });
            else
                rev.push_back(Edit{ Op::Removed, baseA + x, 0 });
        }
        x = prevX;
        y = prevY;
    }
    out.insert(out.end(), rev.rbegin(), rev.rend());
    return true;
}

} // namespace

std::vector<Edit> diffLines(const std::vector<std::string> &a, const std::vector<std::string> &b, int maxEdits)
{
    std::unordered_map<std::string_view, int> ids;
    std::vector<int> ia, ib;
    ia.reserve(a.size());
    ib.reserve(b.size());
    for (const auto &s : a)
        ia.push_back(ids.emplace(s, int(ids.size())).first->second);
    for (const auto &s : b)
        ib.push_back(ids.emplace(s, int(ids.size())).first->second);

    const int n = int(ia.size()), m = int(ib.size());
    int prefix = 0;
    while (prefix < n && prefix < m && ia[size_t(prefix)] == ib[size_t(prefix)])
        ++prefix;
    int suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix &&
           ia[size_t(n - 1 - suffix)] == ib[size_t(m - 1 - suffix)])
        ++suffix;

    std::vector<Edit> out;
    out.reserve(size_t(std::max(n, m)) + 16);
    for (int i = 0; i < prefix; ++i)
        out.push_back(Edit{ Op::Same, i + 1, i + 1 });

    const int midA = n - prefix - suffix, midB = m - prefix - suffix;
    if (!myers(ia.data() + prefix, midA, ib.data() + prefix, midB, prefix, prefix, maxEdits, out)) {
        for (int i = 0; i < midA; ++i)
            out.push_back(Edit{ Op::Removed, prefix + i + 1, 0 });
        for (int i = 0; i < midB; ++i)
            out.push_back(Edit{ Op::Added, 0, prefix + i + 1 });
    }

    for (int i = 0; i < suffix; ++i)
        out.push_back(Edit{ Op::Same, n - suffix + i + 1, m - suffix + i + 1 });
    return out;
}

Unified unified(std::string_view oldText, std::string_view newText, int context)
{
    auto a = splitLines(oldText);
    auto b = splitLines(newText);
    auto edits = diffLines(a, b);

    Unified u;
    const int count = int(edits.size());
    // Mark which edits are shown: every change plus `context` lines around it.
    std::vector<char> shown(size_t(count), 0);
    for (int i = 0; i < count; ++i) {
        if (edits[size_t(i)].op == Op::Same)
            continue;
        if (edits[size_t(i)].op == Op::Added)
            ++u.added;
        else
            ++u.removed;
        for (int j = std::max(0, i - context); j <= std::min(count - 1, i + context); ++j)
            shown[size_t(j)] = 1;
    }

    for (int i = 0; i < count;) {
        if (!shown[size_t(i)]) {
            ++i;
            continue;
        }
        int end = i;
        while (end < count && shown[size_t(end)])
            ++end;
        // Hunk header: the old and new ranges of edits [i, end).
        int oldStart = 0, oldCount = 0, newStart = 0, newCount = 0;
        for (int j = i; j < end; ++j) {
            const auto &e = edits[size_t(j)];
            if (e.oldLine) {
                if (!oldStart)
                    oldStart = e.oldLine;
                ++oldCount;
            }
            if (e.newLine) {
                if (!newStart)
                    newStart = e.newLine;
                ++newCount;
            }
        }
        Row header;
        header.kind = Row::Hunk;
        header.text = "@@ -" + std::to_string(oldStart) + "," + std::to_string(oldCount) + " +" +
                      std::to_string(newStart) + "," + std::to_string(newCount) + " @@";
        u.rows.push_back(std::move(header));
        for (int j = i; j < end; ++j) {
            const auto &e = edits[size_t(j)];
            Row r;
            r.oldLine = e.oldLine;
            r.newLine = e.newLine;
            switch (e.op) {
            case Op::Same:
                r.kind = Row::Context;
                r.text = b[size_t(e.newLine - 1)];
                break;
            case Op::Added:
                r.kind = Row::Add;
                r.text = b[size_t(e.newLine - 1)];
                break;
            case Op::Removed:
                r.kind = Row::Del;
                r.text = a[size_t(e.oldLine - 1)];
                break;
            }
            u.rows.push_back(std::move(r));
        }
        i = end;
    }
    return u;
}

} // namespace diff
