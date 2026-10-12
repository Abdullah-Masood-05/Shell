#include "Highlight.h"

#include "nss/NssLexer.h"
#include "nss/Text.h"

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

std::vector<Line> highlight(std::string_view s, int tabWidth)
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
            put(std::string(size_t(n), ' '), kinds[i]);
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

} // namespace hl
