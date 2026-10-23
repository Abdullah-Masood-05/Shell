#include "Highlight.h"

#include "nss/NssLexer.h"
#include "nss/Text.h"

#include <algorithm>
#include <array>
#include <string>

namespace hl {

namespace {

bool isKeyword(std::string_view word)
{
    static constexpr std::string_view words[] = {
        "menu",  "item",   "separator", "sep",     "modify", "remove", "import", "theme",
        "settings", "dynamic", "static", "if",     "else",   "for",    "while",  "break",
        "continue", "return", "true",   "false",   "null",   "yes",    "no",     "lang",
        "loc",   "auto",   "default",   "none",    "var",
    };
    for (auto w : words)
        if (nss::iequals(word, w))
            return true;
    return false;
}

// Marks the comments inside a trivia range; whitespace stays Plain.
void markTrivia(std::string_view s, size_t begin, size_t end, std::vector<unsigned char> &kinds)
{
    size_t pos = begin;
    while (pos < end) {
        if (s[pos] == '/' && pos + 1 < end && s[pos + 1] == '/') {
            size_t e = pos;
            while (e < end && s[e] != '\n' && s[e] != '\r')
                ++e;
            for (size_t k = pos; k < e; ++k)
                kinds[k] = Comment;
            pos = e;
        } else if (s[pos] == '/' && pos + 1 < end && s[pos + 1] == '*') {
            size_t e = pos + 2;
            while (e < end && !(s[e] == '*' && e + 1 < end && s[e + 1] == '/'))
                ++e;
            e = e < end ? e + 2 : end;
            for (size_t k = pos; k < e; ++k)
                kinds[k] = Comment;
            pos = e;
        } else {
            ++pos;
        }
    }
}

} // namespace

std::vector<unsigned char> classify(std::string_view s)
{
    std::vector<unsigned char> kinds(s.size(), Plain);
    auto tokens = nss::NssLexer::tokenize(s);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto &t = tokens[i];
        markTrivia(s, t.triviaBegin, t.begin, kinds);
        int kind = Plain;
        switch (t.kind) {
        case nss::TokenKind::Ident: {
            auto word = s.substr(t.begin, t.size());
            const auto *next = i + 1 < tokens.size() ? &tokens[i + 1] : nullptr;
            if (isKeyword(word))
                kind = Keyword;
            else if (next && next->kind == nss::TokenKind::Assign)
                kind = Property;
            else if (next && next->kind == nss::TokenKind::LParen)
                kind = Function;
            else
                kind = Ident;
            break;
        }
        case nss::TokenKind::Number: kind = Number; break;
        case nss::TokenKind::Color: kind = Color; break;
        case nss::TokenKind::String:
        case nss::TokenKind::IString:
        case nss::TokenKind::Char: kind = String; break;
        case nss::TokenKind::Variable:
        case nss::TokenKind::At: kind = Variable; break;
        case nss::TokenKind::Unknown: kind = Error; break;
        default: kind = Plain; break;
        }
        if (t.error)
            kind = Error;
        for (size_t k = t.begin; k < t.end && k < s.size(); ++k)
            kinds[k] = static_cast<unsigned char>(kind);
    }
    return kinds;
}

// ---------------------------------------------------------------------------
// Tabs in the edit view

namespace {

bool continuationByte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

std::string utf8(char32_t c)
{
    std::string out;
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
    return out;
}

// The replacement for a tab at `column`.
std::string tabGlyphs(int column, int tabWidth, const TabGlyphs &g)
{
    int n = tabWidth - column % tabWidth;
    std::string out = utf8(g.lead);
    auto pad = utf8(g.pad);
    for (int k = 1; k < n; ++k)
        out += pad;
    return out;
}

bool at(std::string_view s, size_t pos, std::string_view what)
{
    return pos <= s.size() && s.substr(pos, what.size()) == what;
}

} // namespace

TabGlyphs pickTabGlyphs(std::string_view text)
{
    static constexpr TabGlyphs candidates[] = {
        { U' ', U' ' }, // figure space, punctuation space
        { U' ', U' ' }, // en space, em space
        { U' ', U' ' }, // thin space, hair space
    };
    for (const auto &c : candidates)
        if (text.find(utf8(c.lead)) == std::string_view::npos && text.find(utf8(c.pad)) == std::string_view::npos)
            return c;
    return {};
}

std::string tabsToDisplay(std::string_view text, TabGlyphs glyphs, int tabWidth)
{
    if (!glyphs.valid())
        return std::string(text);
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    int column = 0;
    for (char c : text) {
        if (c == '\t') {
            auto g = tabGlyphs(column, tabWidth, glyphs);
            column += tabWidth - column % tabWidth;
            out += g;
            continue;
        }
        out += c;
        if (c == '\n' || c == '\r')
            column = 0;
        else if (!continuationByte(c))
            ++column;
    }
    return out;
}

std::string tabsFromDisplay(std::string_view display, TabGlyphs glyphs)
{
    if (!glyphs.valid())
        return std::string(display);
    auto lead = utf8(glyphs.lead), pad = utf8(glyphs.pad);
    std::string out;
    out.reserve(display.size());
    for (size_t i = 0; i < display.size();) {
        if (at(display, i, lead)) {
            out += '\t';
            i += lead.size();
        } else if (at(display, i, pad)) {
            i += pad.size();
        } else {
            out += display[i++];
        }
    }
    return out;
}

DisplayEdit applyDisplayEdit(std::string_view oldDisplay, std::string_view newDisplay, TabGlyphs glyphs, int tabWidth)
{
    const auto &o = oldDisplay;
    const auto &n = newDisplay;
    // The changed range: old [a, b) became new [a, n.size() - (o.size() - b)).
    size_t p = 0;
    while (p < o.size() && p < n.size() && o[p] == n[p])
        ++p;
    while (p > 0 && ((p < o.size() && continuationByte(o[p])) || (p < n.size() && continuationByte(n[p]))))
        --p;
    size_t s = 0, maxS = std::min(o.size(), n.size()) - p;
    while (s < maxS && o[o.size() - 1 - s] == n[n.size() - 1 - s])
        ++s;
    while (s > 0 && continuationByte(o[o.size() - s]))
        --s;
    size_t a = p, b = o.size() - s;
    auto inserted = n.substr(p, n.size() - s - p);

    if (glyphs.valid() && b > a) {
        // Removing part of a tab's glyphs removes the whole tab.
        auto lead = utf8(glyphs.lead), pad = utf8(glyphs.pad);
        if (at(o, a, pad)) {
            while (a >= pad.size() && at(o, a - pad.size(), pad))
                a -= pad.size();
            if (a >= lead.size() && at(o, a - lead.size(), lead))
                a -= lead.size();
        }
        if ((b >= lead.size() && at(o, b - lead.size(), lead)) || (b >= pad.size() && at(o, b - pad.size(), pad)))
            while (at(o, b, pad) && b < o.size())
                b += pad.size();
    }

    DisplayEdit r;
    auto head = tabsFromDisplay(o.substr(0, a), glyphs) + tabsFromDisplay(inserted, glyphs);
    r.cursor = glyphs.valid() ? tabsToDisplay(head, glyphs, tabWidth).size() : head.size();
    r.text = head + tabsFromDisplay(o.substr(b), glyphs);
    r.display = tabsToDisplay(r.text, glyphs, tabWidth);
    r.changed = r.display != n;
    return r;
}

// ---------------------------------------------------------------------------
// Lines

std::vector<Line> highlight(std::string_view s, int tabWidth, const TabGlyphs *glyphs)
{
    auto kinds = classify(s);
    std::vector<Line> lines(1);
    int column = 0;
    auto put = [&](std::string_view piece, int kind) {
        auto &spans = lines.back().spans;
        if (spans.empty() || spans.back().kind != kind)
            spans.push_back(Span{ {}, kind });
        spans.back().text.append(piece);
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
                ++i;
            lines.emplace_back();
            column = 0;
            continue;
        }
        if (c == '\t') {
            int n = tabWidth - column % tabWidth;
            if (!glyphs)
                put(std::string(size_t(n), ' '), kinds[i]);
            else if (glyphs->valid())
                put(tabGlyphs(column, tabWidth, *glyphs), kinds[i]);
            else
                put("\t", kinds[i]);
            column += n;
            continue;
        }
        // A UTF-8 continuation byte always stays in the span of its lead byte.
        bool continuation = (static_cast<unsigned char>(c) & 0xC0) == 0x80;
        const auto &spans = lines.back().spans;
        put(std::string_view(&s[i], 1), continuation && !spans.empty() ? spans.back().kind : kinds[i]);
        if (!continuation)
            ++column;
    }
    return lines;
}

LinePatch highlightPatch(std::string_view o, std::string_view n, int tabWidth, const TabGlyphs *glyphs)
{
    size_t p = 0;
    const size_t maxP = std::min(o.size(), n.size());
    while (p < maxP && o[p] == n[p])
        ++p;
    size_t s = 0;
    const size_t maxS = std::min(o.size(), n.size()) - p;
    while (s < maxS && o[o.size() - 1 - s] == n[n.size() - 1 - s])
        ++s;
    // Whole lines around the change: [lineStart, end of the line holding the change's end).
    constexpr char nl = '\n';
    size_t lineStart = 0;
    if (p > 0) {
        const size_t k = n.rfind(nl, p - 1);
        lineStart = k == std::string_view::npos ? 0 : k + 1;
    }
    size_t newEnd = n.find(nl, n.size() - s);
    if (newEnd == std::string_view::npos)
        newEnd = n.size();
    size_t oldEnd = o.find(nl, o.size() - s);
    if (oldEnd == std::string_view::npos)
        oldEnd = o.size();
    LinePatch out;
    out.first = size_t(std::count(n.begin(), n.begin() + ptrdiff_t(lineStart), nl));
    out.oldCount = size_t(std::count(o.begin() + ptrdiff_t(lineStart), o.begin() + ptrdiff_t(oldEnd), nl)) + 1;
    out.lines = highlight(n.substr(lineStart, newEnd - lineStart), tabWidth, glyphs);
    return out;
}

} // namespace hl
