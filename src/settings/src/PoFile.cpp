#include "PoFile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nsp::po {

std::string Entry::key() const { return hasContext ? context + '\x04' + id : id; }

std::string File::headerField(std::string_view name) const
{
    size_t pos = 0;
    while (pos < header.size()) {
        size_t end = header.find('\n', pos);
        if (end == std::string::npos)
            end = header.size();
        std::string_view line(header.data() + pos, end - pos);
        pos = end + 1;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        std::string_view key = line.substr(0, colon);
        while (!key.empty() && key.back() == ' ')
            key.remove_suffix(1);
        if (key.size() != name.size() ||
            !std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b));
            }))
            continue;
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
            value.remove_suffix(1);
        return std::string(value);
    }
    return {};
}

namespace {

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// A "..." string with C escapes; nothing but spaces may follow it.
bool quoted(std::string_view s, std::string &out, std::string &error)
{
    s = trim(s);
    if (s.size() < 2 || s.front() != '"') {
        error = "expected a string in quotes";
        return false;
    }
    size_t i = 1;
    for (; i < s.size() && s[i] != '"'; ++i) {
        char c = s[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= s.size())
            break;
        c = s[i];
        switch (c) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'a': out.push_back('\a'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'v': out.push_back('\v'); break;
        case '"':
        case '\\':
        case '\'':
        case '?': out.push_back(c); break;
        case 'x': {
            int v = 0, digits = 0;
            while (i + 1 < s.size() && std::isxdigit(uint8_t(s[i + 1])) && digits < 2) {
                const char d = s[++i];
                v = v * 16 + (d <= '9' ? d - '0' : (std::tolower(uint8_t(d)) - 'a' + 10));
                ++digits;
            }
            if (!digits) {
                error = "\\x without hex digits";
                return false;
            }
            out.push_back(char(v));
            break;
        }
        default:
            if (c >= '0' && c <= '7') {
                int v = c - '0', digits = 1;
                while (i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7' && digits < 3) {
                    v = v * 8 + (s[++i] - '0');
                    ++digits;
                }
                out.push_back(char(v));
            } else {
                error = std::string("unknown escape \\") + c;
                return false;
            }
        }
    }
    if (i >= s.size()) {
        error = "the closing quote is missing";
        return false;
    }
    if (!trim(s.substr(i + 1)).empty()) {
        error = "text after the closing quote";
        return false;
    }
    return true;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

} // namespace

bool parse(std::string_view text, File &out, std::string &error)
{
    out = File{};
    if (startsWith(text, "\xEF\xBB\xBF"))
        text.remove_prefix(3);

    Entry cur;
    bool any = false, pendingFuzzy = false, headerSeen = false;
    std::string *target = nullptr;
    int lineNo = 0, pendingLine = 0;
    auto flush = [&] {
        if (any) {
            if (cur.id.empty() && !cur.hasContext && !headerSeen) {
                headerSeen = true;
                out.header = cur.str.empty() ? std::string() : cur.str[0];
            } else {
                out.entries.push_back(std::move(cur));
            }
        }
        cur = Entry{};
        any = false;
        target = nullptr;
    };
    auto fail = [&](const std::string &what) {
        error = "line " + std::to_string(lineNo) + ": " + what;
        return false;
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++lineNo;

        if (line.empty()) {
            flush();
            continue;
        }
        if (line[0] == '#') {
            if (startsWith(line, "#~")) {
                // An obsolete entry: it ends the one before and is skipped.
                flush();
                continue;
            }
            if (startsWith(line, "#,")) {
                // Flags, comma separated: "#, fuzzy, c-format".
                std::string_view flags = line.substr(2);
                while (!flags.empty()) {
                    const size_t comma = flags.find(',');
                    if (trim(flags.substr(0, comma)) == "fuzzy")
                        pendingFuzzy = true;
                    if (comma == std::string_view::npos)
                        break;
                    flags.remove_prefix(comma + 1);
                }
            }
            if (!pendingLine)
                pendingLine = lineNo;
            continue;
        }
        if (line[0] == '"') {
            if (!target)
                return fail("a string with no msgid or msgstr before it");
            std::string why;
            if (!quoted(line, *target, why))
                return fail(why);
            continue;
        }

        // A keyword. msgctxt, or a msgid not after its msgctxt, starts a new entry.
        auto begin = [&] {
            if (any && !cur.str.empty())
                flush();
            if (!any) {
                cur.fuzzy = pendingFuzzy;
                cur.line = pendingLine ? pendingLine : lineNo;
                pendingFuzzy = false;
                pendingLine = 0;
                any = true;
            }
        };
        const size_t space = line.find_first_of(" \t\"");
        const std::string_view keyword = line.substr(0, space);
        const std::string_view rest = space == std::string_view::npos ? std::string_view() : line.substr(space);
        std::string value, why;
        if (!quoted(rest, value, why))
            return fail(std::string(keyword) + ": " + why);

        if (keyword == "msgctxt") {
            begin();
            if (cur.hasContext || !cur.id.empty())
                return fail("msgctxt in the middle of an entry");
            cur.hasContext = true;
            cur.context = std::move(value);
            target = &cur.context;
        } else if (keyword == "msgid") {
            if (!cur.hasContext || !cur.str.empty() || !cur.id.empty())
                begin();
            if (!cur.str.empty() || !cur.id.empty())
                return fail("msgid in the middle of an entry");
            cur.id = std::move(value);
            target = &cur.id;
        } else if (keyword == "msgid_plural") {
            if (!any || !cur.str.empty())
                return fail("msgid_plural without a msgid before it");
            cur.idPlural = std::move(value);
            target = &cur.idPlural;
        } else if (startsWith(keyword, "msgstr")) {
            if (!any)
                return fail("msgstr without a msgid before it");
            size_t index = 0;
            if (keyword.size() > 6) {
                if (keyword[6] != '[' || keyword.back() != ']' || keyword.size() < 9)
                    return fail("unknown keyword " + std::string(keyword));
                const std::string digits(keyword.substr(7, keyword.size() - 8));
                if (!std::all_of(digits.begin(), digits.end(), [](char d) { return d >= '0' && d <= '9'; }) ||
                    digits.size() > 2)
                    return fail("bad plural index in " + std::string(keyword));
                index = size_t(std::atoi(digits.c_str()));
            }
            if (cur.str.size() <= index)
                cur.str.resize(index + 1);
            cur.str[index] = std::move(value);
            target = &cur.str[index];
        } else {
            return fail("unknown keyword " + std::string(keyword));
        }
    }
    flush();
    for (const auto &e : out.entries)
        if (e.str.empty()) {
            error = "line " + std::to_string(e.line) + ": an entry without msgstr";
            return false;
        }
    return true;
}

// --- plural rules -------------------------------------------------------------------

struct PluralRule::Node
{
    enum Kind { Number, N, Not, Binary, Ternary } kind = Number;
    std::string op; // Binary: * / % + - < > <= >= == != && ||
    unsigned long long value = 0;
    std::shared_ptr<const Node> a, b, c;

    unsigned long long eval(unsigned long long n) const
    {
        switch (kind) {
        case Number: return value;
        case N: return n;
        case Not: return !a->eval(n);
        case Ternary: return a->eval(n) ? b->eval(n) : c->eval(n);
        case Binary: break;
        }
        if (op == "&&")
            return a->eval(n) && b->eval(n);
        if (op == "||")
            return a->eval(n) || b->eval(n);
        const unsigned long long x = a->eval(n), y = b->eval(n);
        if (op == "*") return x * y;
        if (op == "/") return y ? x / y : 0;
        if (op == "%") return y ? x % y : 0;
        if (op == "+") return x + y;
        if (op == "-") return x - y;
        if (op == "<") return x < y;
        if (op == ">") return x > y;
        if (op == "<=") return x <= y;
        if (op == ">=") return x >= y;
        if (op == "==") return x == y;
        if (op == "!=") return x != y;
        return 0;
    }
};

namespace {

using NodeP = std::shared_ptr<const PluralRule::Node>;

class ExprParser
{
public:
    explicit ExprParser(std::string_view s) : s_(s) {}

    NodeP parseAll()
    {
        NodeP e = ternary();
        skip();
        if (!e || i_ != s_.size())
            return nullptr;
        return e;
    }

private:
    std::string_view s_;
    size_t i_ = 0;

    void skip()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }
    bool eat(std::string_view tok)
    {
        skip();
        if (s_.substr(i_, tok.size()) != tok)
            return false;
        // "<" is not "<=", "!" is not "!=", "&"/"|" alone are not operators here.
        if (tok.size() == 1 && i_ + 1 < s_.size() && s_[i_ + 1] == '=' && (tok == "<" || tok == ">" || tok == "!"))
            return false;
        i_ += tok.size();
        return true;
    }
    static NodeP binary(const std::string &op, NodeP a, NodeP b)
    {
        if (!a || !b)
            return nullptr;
        auto n = std::make_shared<PluralRule::Node>();
        n->kind = PluralRule::Node::Binary;
        n->op = op;
        n->a = std::move(a);
        n->b = std::move(b);
        return n;
    }

    NodeP ternary()
    {
        NodeP cond = logicalOr();
        if (!cond || !eat("?"))
            return cond;
        NodeP yes = ternary();
        if (!yes || !eat(":"))
            return nullptr;
        NodeP no = ternary();
        if (!no)
            return nullptr;
        auto n = std::make_shared<PluralRule::Node>();
        n->kind = PluralRule::Node::Ternary;
        n->a = cond;
        n->b = yes;
        n->c = no;
        return n;
    }
    NodeP logicalOr()
    {
        NodeP l = logicalAnd();
        while (l && eat("||"))
            l = binary("||", l, logicalAnd());
        return l;
    }
    NodeP logicalAnd()
    {
        NodeP l = equality();
        while (l && eat("&&"))
            l = binary("&&", l, equality());
        return l;
    }
    NodeP equality()
    {
        NodeP l = relational();
        while (l) {
            if (eat("=="))
                l = binary("==", l, relational());
            else if (eat("!="))
                l = binary("!=", l, relational());
            else
                break;
        }
        return l;
    }
    NodeP relational()
    {
        NodeP l = additive();
        while (l) {
            if (eat("<="))
                l = binary("<=", l, additive());
            else if (eat(">="))
                l = binary(">=", l, additive());
            else if (eat("<"))
                l = binary("<", l, additive());
            else if (eat(">"))
                l = binary(">", l, additive());
            else
                break;
        }
        return l;
    }
    NodeP additive()
    {
        NodeP l = multiplicative();
        while (l) {
            if (eat("+"))
                l = binary("+", l, multiplicative());
            else if (eat("-"))
                l = binary("-", l, multiplicative());
            else
                break;
        }
        return l;
    }
    NodeP multiplicative()
    {
        NodeP l = unary();
        while (l) {
            if (eat("*"))
                l = binary("*", l, unary());
            else if (eat("/"))
                l = binary("/", l, unary());
            else if (eat("%"))
                l = binary("%", l, unary());
            else
                break;
        }
        return l;
    }
    NodeP unary()
    {
        if (eat("!")) {
            NodeP a = unary();
            if (!a)
                return nullptr;
            auto n = std::make_shared<PluralRule::Node>();
            n->kind = PluralRule::Node::Not;
            n->a = a;
            return n;
        }
        return primary();
    }
    NodeP primary()
    {
        skip();
        if (eat("(")) {
            NodeP e = ternary();
            if (!e || !eat(")"))
                return nullptr;
            return e;
        }
        if (i_ < s_.size() && s_[i_] == 'n') {
            ++i_;
            auto n = std::make_shared<PluralRule::Node>();
            n->kind = PluralRule::Node::N;
            return n;
        }
        if (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
            unsigned long long v = 0;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                v = v * 10 + unsigned(s_[i_++] - '0');
            auto n = std::make_shared<PluralRule::Node>();
            n->kind = PluralRule::Node::Number;
            n->value = v;
            return n;
        }
        return nullptr;
    }
};

} // namespace

PluralRule::PluralRule()
{
    expr_ = ExprParser("n != 1").parseAll();
}

bool PluralRule::parse(std::string_view pluralForms)
{
    const size_t np = pluralForms.find("nplurals");
    const size_t pl = pluralForms.find("plural", np == std::string_view::npos ? 0 : np + 8);
    if (np == std::string_view::npos || pl == std::string_view::npos)
        return false;
    size_t eq = pluralForms.find('=', np);
    if (eq == std::string_view::npos || eq > pl)
        return false;
    const int count = std::atoi(std::string(pluralForms.substr(eq + 1, pl - eq - 1)).c_str());
    if (count < 1 || count > 10)
        return false;
    eq = pluralForms.find('=', pl);
    if (eq == std::string_view::npos)
        return false;
    std::string_view expr = pluralForms.substr(eq + 1);
    const size_t semi = expr.find(';');
    if (semi != std::string_view::npos)
        expr = expr.substr(0, semi);
    NodeP e = ExprParser(expr).parseAll();
    if (!e)
        return false;
    nplurals_ = count;
    expr_ = std::move(e);
    return true;
}

size_t PluralRule::index(unsigned long long n) const
{
    const unsigned long long i = expr_ ? expr_->eval(n) : (n != 1);
    return size_t(std::min<unsigned long long>(i, unsigned(nplurals_ - 1)));
}

} // namespace nsp::po
