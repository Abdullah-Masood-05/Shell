#include "ExcludeRules.h"

#include "nss/Literal.h"
#include "nss/Text.h"

#include <algorithm>

namespace rules {

std::optional<std::vector<std::string>> parseNames(std::string_view literal)
{
    auto lit = nss::Literal::parse(literal);
    std::vector<std::string> out;
    auto take = [&](const nss::Literal &l) {
        if (l.kind != nss::LiteralKind::String)
            return false;
        auto name = std::string(nss::trim(l.string));
        if (!name.empty())
            addName(out, name);
        return true;
    };
    if (lit.kind == nss::LiteralKind::Array) {
        for (const auto &item : lit.items)
            if (!take(item))
                return std::nullopt;
        return out;
    }
    if (!take(lit))
        return std::nullopt;
    return out;
}

std::string formatNames(const std::vector<std::string> &names)
{
    if (names.empty())
        return "\"\"";
    if (names.size() == 1)
        return nss::quoteString(names.front());
    std::string out = "[";
    for (size_t i = 0; i < names.size(); ++i)
        out += (i ? ", " : "") + nss::quoteString(names[i]);
    return out + "]";
}

std::string processName(std::string_view input)
{
    auto s = std::string(nss::trim(input));
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    if (auto slash = s.find_last_of("\\/"); slash != std::string::npos)
        s = s.substr(slash + 1);
    if (s.size() > 4 && nss::iequals(std::string_view(s).substr(s.size() - 4), ".exe"))
        s.resize(s.size() - 4);
    return std::string(nss::trim(s));
}

std::string windowClass(std::string_view input) { return std::string(nss::trim(input)); }

bool addName(std::vector<std::string> &names, const std::string &name)
{
    if (name.empty())
        return false;
    if (std::any_of(names.begin(), names.end(), [&](const std::string &n) { return nss::iequals(n, name); }))
        return false;
    names.push_back(name);
    return true;
}

Where classifyWhere(std::string_view text)
{
    std::string compact;
    for (char c : nss::trim(text))
        if (c != ' ' && c != '\t')
            compact += c;
    if (compact.empty())
        return Where::Off;
    auto lit = nss::Literal::parse(compact);
    if (lit.kind == nss::LiteralKind::Number)
        return lit.number == 0 ? Where::Off : Where::Custom;
    if (lit.isBool())
        return lit.boolValue() ? Where::Custom : Where::Off;
    if (nss::iequals(compact, kExplorerOnly))
        return Where::ExplorerOnly;
    return Where::Custom;
}

} // namespace rules
