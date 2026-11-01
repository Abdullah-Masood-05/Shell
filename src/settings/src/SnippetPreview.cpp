#include "SnippetPreview.h"

#include "Tr.h"

#include "nss/Text.h"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace snippetpreview {

namespace {

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }

std::string lower(std::string_view s) { return nss::toLower(s); }

std::string trimmed(std::string_view s) { return std::string(nss::trim(s)); }

// 'text', "text" or `text` without its quotes (nothing is decoded); else as it is.
std::string unquote(std::string_view s)
{
    s = nss::trim(s);
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"' || s.front() == '`') && s.back() == s.front())
        return std::string(s.substr(1, s.size() - 2));
    return std::string(s);
}

bool isQuoted(std::string_view s)
{
    s = nss::trim(s);
    return s.size() >= 2 && (s.front() == '\'' || s.front() == '"' || s.front() == '`') && s.back() == s.front();
}

std::string utf8(unsigned cp)
{
    std::string s;
    if (cp < 0x80)
        s += char(cp);
    else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
    return s;
}

// "" (quoted or not) -> the character; "" when it isn't one.
std::string glyphOf(std::string_view text)
{
    auto t = unquote(text);
    if (t.size() != 6 || t[0] != '\\' || (t[1] != 'u' && t[1] != 'U'))
        return {};
    unsigned cp = 0;
    for (size_t i = 2; i < 6; ++i) {
        const char c = t[i];
        cp <<= 4;
        if (c >= '0' && c <= '9')
            cp |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f')
            cp |= unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            cp |= unsigned(c - 'A' + 10);
        else
            return {};
    }
    return cp >= 0xE000 && cp <= 0xF8FF ? utf8(cp) : std::string{};
}

// Splits "a, f(b, c), 'd,e'" at its top-level commas.
std::vector<std::string> splitArgs(std::string_view s)
{
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`')
            quote = c;
        else if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']')
            --depth;
        else if (c == ',' && depth == 0) {
            out.push_back(trimmed(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start < s.size() || !out.empty())
        out.push_back(trimmed(s.substr(start)));
    return out;
}

// "image.mdl()" -> ("image.mdl", ""); nullopt when it isn't a call.
std::optional<std::pair<std::string, std::string>> call(std::string_view s)
{
    s = nss::trim(s);
    size_t open = 0;
    while (open < s.size() && isIdent(s[open]))
        ++open;
    if (open == 0 || open >= s.size() || s[open] != '(' || s.back() != ')')
        return std::nullopt;
    return std::make_pair(lower(s.substr(0, open)), std::string(s.substr(open + 1, s.size() - open - 2)));
}

// The text of a string or call that starts at `at` ('...' or name(...)), up to its end.
size_t valueEnd(std::string_view text, size_t at)
{
    char quote = 0;
    int depth = 0;
    for (size_t i = at; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == '@') {
                // '<svg fill="@if(theme.islight,'#fff','none')">': the quotes inside an
                // @call(...) don't end the string.
                size_t open = i + 1;
                while (open < text.size() && isIdent(text[open]))
                    ++open;
                if (open > i + 1 && open < text.size() && text[open] == '(') {
                    int nest = 0;
                    char inner = 0;
                    size_t j = open;
                    for (; j < text.size(); ++j) {
                        const char d = text[j];
                        if (inner) {
                            if (d == inner)
                                inner = 0;
                        } else if (d == '\'' || d == '"' || d == '`')
                            inner = d;
                        else if (d == '(')
                            ++nest;
                        else if (d == ')' && --nest == 0)
                            break;
                        else if (d == '\n' || d == '\r')
                            break; // not a call after all
                    }
                    if (j < text.size() && text[j] == ')') {
                        i = j;
                        continue;
                    }
                }
            }
            if (c == quote) {
                quote = 0;
                if (depth == 0)
                    return i + 1;
            }
            continue;
        }
        if (c == '\'' || c == '"' || c == '`')
            quote = c;
        else if (c == '(')
            ++depth;
        else if (c == ')') {
            if (--depth <= 0)
                return i + 1;
        } else if ((c == '\n' || c == '\r') && depth == 0)
            return i;
    }
    return text.size();
}

const nss::Node *findDecl(const nss::NssDocument &doc, int line, int column)
{
    std::vector<const nss::Node *> onLine;
    doc.visit([&](const nss::Node &n) {
        if (n.kind == nss::NodeKind::MenuDecl && int(n.line) == line)
            onLine.push_back(&n);
        return true;
    });
    if (onLine.empty())
        return nullptr;
    if (onLine.size() == 1 || column <= 0)
        return onLine.front();
    const std::string_view text = doc.text();
    for (const auto *n : onLine) {
        size_t start = text.rfind('\n', n->span.begin == 0 ? 0 : n->span.begin - 1);
        start = start == std::string_view::npos ? 0 : start + 1;
        int col = 1;
        for (size_t i = start; i < n->span.begin; ++i)
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                ++col;
        if (col == column)
            return n;
    }
    return onLine.front();
}

std::optional<std::string> prop(const nss::NssDocument &doc, const nss::Node &n, std::initializer_list<const char *> names)
{
    for (const auto &p : n.props)
        for (const char *name : names)
            if (p.name == name)
                return p.hasValue ? trimmed(doc.slice(p.valueSpan)) : std::string{};
    return std::nullopt;
}

bool truthy(const std::string &v)
{
    auto t = lower(unquote(v));
    return t.empty() || t == "true" || t == "yes" || t == "1";
}

bool hasThemeOrImages(std::string_view text)
{
    auto doc = nss::NssDocument::fromText(std::string(text));
    if (doc.findBlock("theme"))
        return true;
    ImageDefs defs;
    collectImageDefs(text, defs);
    return !defs.svgs.empty();
}

std::string hex(uint32_t argb)
{
    if ((argb >> 24) == 0)
        return "none";
    char buf[10];
    snprintf(buf, sizeof buf, "#%06X", argb & 0xFFFFFF);
    return buf;
}

// A $variable's value as SVG text: a string, image.colorN, or @if(theme.islight, a, b).
std::string varText(std::string_view value, const ImageDefs &defs, const uint32_t colors[3], bool light, int depth);

std::string fill(std::string_view svg, const ImageDefs &defs, const uint32_t colors[3], bool light, int depth)
{
    std::string out;
    out.reserve(svg.size());
    for (size_t i = 0; i < svg.size(); ++i) {
        if (svg[i] != '@') {
            out += svg[i];
            continue;
        }
        size_t end = i + 1;
        while (end < svg.size() && (std::isalnum(static_cast<unsigned char>(svg[end])) || svg[end] == '_' || svg[end] == '.'))
            ++end;
        std::string name = lower(svg.substr(i + 1, end - i - 1));
        while (!name.empty() && name.back() == '.') {
            name.pop_back();
            --end;
        }
        if (name.empty()) {
            out += '@';
            continue;
        }
        if (name == "if" && end < svg.size() && svg[end] == '(') {
            // fill="@if(theme.islight,'#fff','none')": the call, up to its closing bracket.
            size_t close = end;
            int nest = 0;
            char quote = 0;
            for (; close < svg.size(); ++close) {
                const char c = svg[close];
                if (quote) {
                    if (c == quote)
                        quote = 0;
                } else if (c == '\'' || c == '"' || c == '`')
                    quote = c;
                else if (c == '(')
                    ++nest;
                else if (c == ')' && --nest == 0)
                    break;
            }
            if (close < svg.size()) {
                out += varText(svg.substr(i + 1, close - i), defs, colors, light, depth + 1);
                i = close;
                continue;
            }
        }
        if (name == "image.color1" || name == "image.color2" || name == "image.color3")
            out += hex(colors[name.back() - '1']);
        else if (auto v = defs.vars.find(name); v != defs.vars.end() && depth < 4)
            out += varText(v->second, defs, colors, light, depth + 1);
        else
            out += "none";
        i = end - 1;
    }
    return out;
}

std::string varText(std::string_view value, const ImageDefs &defs, const uint32_t colors[3], bool light, int depth)
{
    auto v = nss::trim(value);
    if (isQuoted(v))
        return fill(unquote(v), defs, colors, light, depth);
    if (!v.empty() && v.front() == '@')
        v.remove_prefix(1);
    const auto name = lower(v);
    if (name == "image.color1" || name == "image.color2" || name == "image.color3")
        return hex(colors[name.back() - '1']);
    if (auto c = call(v); c && c->first == "if") {
        auto args = splitArgs(c->second);
        if (args.size() == 3) {
            const auto cond = lower(args[0]);
            const bool pick = cond == "theme.islight" ? light : cond == "theme.isdark" ? !light : true;
            return varText(pick ? args[1] : args[2], defs, colors, light, depth + 1);
        }
    }
    if (auto it = defs.vars.find(name); it != defs.vars.end() && depth < 4)
        return varText(it->second, defs, colors, light, depth + 1);
    return "none";
}

} // namespace

// --- contexts ---------------------------------------------------------------------------

const char *contextName(Context c)
{
    switch (c) {
    case Context::File: return "file";
    case Context::Folder: return "folder";
    case Context::Back: return "back";
    case Context::Desktop: return "desktop";
    case Context::Drive: return "drive";
    case Context::Taskbar: return "taskbar";
    }
    return "file";
}

const char *contextLabel(Context c)
{
    switch (c) {
    case Context::File: return nsp::mark("File");
    case Context::Folder: return nsp::mark("Folder");
    case Context::Back: return nsp::mark("Background");
    case Context::Desktop: return nsp::mark("Desktop");
    case Context::Drive: return nsp::mark("Drive");
    case Context::Taskbar: return nsp::mark("Taskbar");
    }
    return "";
}

// --- the scratch root -------------------------------------------------------------------

ThemeSource themeSource(const nss::NssDocument &root, const fs::path &rootDir, const ReadFile &read)
{
    ThemeSource out;
    std::vector<std::string> seen;
    for (const auto *imp : root.imports()) {
        if (!imp->enabled || imp->importKind != nss::ImportKind::Config || !imp->context.empty())
            continue;
        const auto raw = nss::trim(root.slice(imp->valueSpan));
        if (!isQuoted(raw) || raw.find('@') != std::string_view::npos)
            continue; // an expression: not followed
        fs::path p(nss::widen(unquote(raw)));
        if (p.is_relative())
            p = rootDir / p;
        p = p.lexically_normal();
        const auto key = nss::pathKey(p);
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;
        seen.push_back(key);
        if (auto text = read(p); text && hasThemeOrImages(*text))
            out.imports.push_back(p);
    }
    root.visit([&](const nss::Node &n) {
        if (n.kind == nss::NodeKind::Block && n.path == "theme" && !n.bodySpan.empty()) {
            if (!out.inlineTheme.empty())
                out.inlineTheme += "\n";
            out.inlineTheme += std::string(root.slice(n.span));
        }
        return false; // top level only
    });
    const auto lang = (rootDir / L"imports" / L"lang" / L"en.nss").lexically_normal();
    if (read(lang))
        out.lang = lang;
    return out;
}

std::string scratchRoot(const ThemeSource &source, std::string_view snippetFile)
{
    std::string out = "// Nilesoft Shell Pro: the Snippets page's preview of one snippet. Made again each time.\n";
    if (source.lang)
        out += "import lang " + nss::quoteImportPath(nss::narrow(source.lang->wstring())) + "\n";
    for (const auto &p : source.imports)
        out += "import " + nss::quoteImportPath(nss::narrow(p.wstring())) + "\n";
    if (!source.inlineTheme.empty())
        out += source.inlineTheme + "\n";
    out += "import " + nss::quoteImportPath(snippetFile) + "\n";
    return out;
}

// --- the sample -------------------------------------------------------------------------

Sample sampleFor(std::string_view snippetText)
{
    Sample out;
    auto doc = nss::NssDocument::fromText(std::string(snippetText));
    ImageDefs vars;
    collectImageDefs(snippetText, vars);
    std::string found;
    auto consider = [&](std::string_view value) {
        std::string v = unquote(value);
        size_t start = 0;
        while (found.empty() && start <= v.size()) {
            size_t bar = v.find('|', start);
            auto token = trimmed(std::string_view(v).substr(start, bar == std::string::npos ? std::string::npos : bar - start));
            if (!token.empty() && token.front() == '*')
                token.erase(0, 1);
            // '."zip"': exactly that extension.
            if (token.size() >= 4 && token[0] == '.' && token[1] == '"' && token.back() == '"')
                token = "." + token.substr(2, token.size() - 3);
            if (token.size() >= 2 && token.front() == '.' &&
                std::all_of(token.begin() + 1, token.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }))
                found = lower(token);
            if (bar == std::string::npos)
                break;
            start = bar + 1;
        }
    };
    doc.visit([&](const nss::Node &n) {
        if (!found.empty())
            return false;
        if (n.kind != nss::NodeKind::MenuDecl)
            return true;
        if (auto find = prop(doc, n, { "find" })) {
            // find=ext_list: a variable holding the list.
            std::string value = *find;
            for (int depth = 0; depth < 4 && !isQuoted(value); ++depth) {
                auto it = vars.vars.find(lower(value));
                if (it == vars.vars.end())
                    break;
                value = it->second;
            }
            if (isQuoted(value))
                consider(value);
        }
        if (found.empty())
            if (auto where = prop(doc, n, { "where" })) {
                // sel.file.ext == '.zip' / sel.ext=='.zip'
                const auto w = lower(*where);
                for (const char *key : { "sel.file.ext", "sel.ext" }) {
                    auto at = w.find(key);
                    if (at == std::string::npos)
                        continue;
                    auto eq = w.find("==", at);
                    if (eq == std::string::npos)
                        continue;
                    auto q = w.find_first_of("'\"", eq);
                    if (q == std::string::npos)
                        continue;
                    auto end = w.find(w[q], q + 1);
                    if (end != std::string::npos)
                        consider(w.substr(q, end - q + 1));
                    if (!found.empty())
                        break;
                }
            }
        return true;
    });
    if (!found.empty()) {
        out.extension = found;
        out.fromSnippet = true;
    }
    return out;
}

// --- icons ------------------------------------------------------------------------------

void collectImageDefs(std::string_view text, ImageDefs &defs)
{
    size_t at = 0;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string_view::npos)
            eol = text.size();
        size_t i = at;
        while (i < eol && (text[i] == ' ' || text[i] == '\t'))
            ++i;
        const char lead = i < eol ? text[i] : 0;
        if (lead != '@' && lead != '$') {
            at = eol + 1;
            continue;
        }
        const size_t eq = text.find('=', i);
        if (eq == std::string_view::npos || eq > eol || (eq + 1 < text.size() && text[eq + 1] == '=')) {
            at = eol + 1;
            continue;
        }
        size_t v = eq + 1;
        while (v < text.size() && (text[v] == ' ' || text[v] == '\t'))
            ++v;
        const size_t end = valueEnd(text, v);
        const std::string value = trimmed(text.substr(v, end - v));
        const std::string names = lower(text.substr(i + 1, eq - i - 1));
        if (lead == '$') {
            auto name = trimmed(names);
            if (!name.empty() && std::all_of(name.begin(), name.end(), isIdent))
                defs.vars[name] = value;
        } else {
            std::string svg;
            if (isQuoted(value))
                svg = unquote(value);
            else if (auto c = call(value); c && (c->first == "image.svg" || c->first == "icon.svg")) {
                auto args = splitArgs(c->second);
                if (!args.empty() && isQuoted(args[0]))
                    svg = unquote(args[0]);
            }
            if (svg.find("<svg") != std::string::npos)
                for (const auto &n : splitArgs(names))
                    if (!n.empty())
                        defs.svgs[n] = svg;
        }
        at = std::max(end, eol) + 1;
    }
}

namespace {

struct NamedColor
{
    const char *name;
    uint32_t argb;
};

// color.<name>: Shell's table (src/dll/src/Expression/Constants.h).
constexpr NamedColor kColors[] = {
    { "black", 0xFF000000 }, { "white", 0xFFFFFFFF }, { "aliceblue", 0xFFF0F8FF },
    { "antiquewhite", 0xFFFAEBD7 }, { "aqua", 0xFF00FFFF }, { "aquamarine", 0xFF7FFFD4 },
    { "azure", 0xFFF0FFFF }, { "beige", 0xFFF5F5DC }, { "bisque", 0xFFFFE4C4 },
    { "blanchedalmond", 0xFFFFEBCD }, { "blue", 0xFF0000FF }, { "blueviolet", 0xFF8A2BE2 },
    { "brown", 0xFFA52A2A }, { "burlywood", 0xFFDEB887 }, { "cadetblue", 0xFF5F9EA0 },
    { "chartreuse", 0xFF7FFF00 }, { "chocolate", 0xFFD2691E }, { "coral", 0xFFFF7F50 },
    { "cornflowerblue", 0xFF6495ED }, { "cornsilk", 0xFFFFF8DC }, { "crimson", 0xFFDC143C },
    { "cyan", 0xFF00FFFF }, { "darkblue", 0xFF00008B }, { "darkcyan", 0xFF008B8B },
    { "darkgoldenrod", 0xFFB8860B }, { "darkgray", 0xFFA9A9A9 }, { "darkgreen", 0xFF006400 },
    { "darkkhaki", 0xFFBDB76B }, { "darkmagenta", 0xFF8B008B }, { "darkolivegreen", 0xFF556B2F },
    { "darkorange", 0xFFFF8C00 }, { "darkorchid", 0xFF9932CC }, { "darkred", 0xFF8B0000 },
    { "darksalmon", 0xFFE9967A }, { "darkseagreen", 0xFF8FBC8F }, { "darkslateblue", 0xFF483D8B },
    { "darkslategray", 0xFF2F4F4F }, { "darkturquoise", 0xFF00CED1 }, { "darkviolet", 0xFF9400D3 },
    { "deeppink", 0xFFFF1493 }, { "deepskyblue", 0xFF00BFFF }, { "dimgray", 0xFF696969 },
    { "dodgerblue", 0xFF1E90FF }, { "firebrick", 0xFFB22222 }, { "floralwhite", 0xFFFFFAF0 },
    { "forestgreen", 0xFF228B22 }, { "fuchsia", 0xFFFF00FF }, { "gainsboro", 0xFFDCDCDC },
    { "ghostwhite", 0xFFF8F8FF }, { "gold", 0xFFFFD700 }, { "goldenrod", 0xFFDAA520 }, { "gray", 0xFF808080 },
    { "green", 0xFF008000 }, { "greenyellow", 0xFFADFF2F }, { "honeydew", 0xFFF0FFF0 },
    { "hotpink", 0xFFFF69B4 }, { "indianred", 0xFFCD5C5C }, { "indigo", 0xFF4B0082 }, { "ivory", 0xFFFFFFF0 },
    { "khaki", 0xFFF0E68C }, { "lavender", 0xFFE6E6FA }, { "lavenderblush", 0xFFFFF0F5 },
    { "lawngreen", 0xFF7CFC00 }, { "lemonchiffon", 0xFFFFFACD }, { "lightblue", 0xFFADD8E6 },
    { "lightcoral", 0xFFF08080 }, { "lightcyan", 0xFFE0FFFF }, { "lightgoldenrodyellow", 0xFFFAFAD2 },
    { "lightgray", 0xFFD3D3D3 }, { "lightgreen", 0xFF90EE90 }, { "lightpink", 0xFFFFB6C1 },
    { "lightsalmon", 0xFFFFA07A }, { "lightseagreen", 0xFF20B2AA }, { "lightskyblue", 0xFF87CEFA },
    { "lightslategray", 0xFF778899 }, { "lightsteelblue", 0xFFB0C4DE }, { "lightyellow", 0xFFFFFFE0 },
    { "lime", 0xFF00FF00 }, { "limegreen", 0xFF32CD32 }, { "linen", 0xFFFAF0E6 }, { "magenta", 0xFFFF00FF },
    { "maroon", 0xFF800000 }, { "mediumaquamarine", 0xFF66CDAA }, { "mediumblue", 0xFF0000CD },
    { "mediumorchid", 0xFFBA55D3 }, { "mediumpurple", 0xFF9370DB }, { "mediumseagreen", 0xFF3CB371 },
    { "mediumslateblue", 0xFF7B68EE }, { "mediumspringgreen", 0xFF00FA9A }, { "mediumturquoise", 0xFF48D1CC },
    { "mediumvioletred", 0xFFC71585 }, { "midnightblue", 0xFF191970 }, { "mintcream", 0xFFF5FFFA },
    { "mistyrose", 0xFFFFE4E1 }, { "moccasin", 0xFFFFE4B5 }, { "navajowhite", 0xFFFFDEAD },
    { "navy", 0xFF000080 }, { "oldlace", 0xFFFDF5E6 }, { "olive", 0xFF808000 }, { "olivedrab", 0xFF6B8E23 },
    { "orange", 0xFFFFA500 }, { "orangered", 0xFFFF4500 }, { "orchid", 0xFFDA70D6 },
    { "palegoldenrod", 0xFFEEE8AA }, { "palegreen", 0xFF98FB98 }, { "paleturquoise", 0xFFAFEEEE },
    { "palevioletred", 0xFFDB7093 }, { "papayawhip", 0xFFFFEFD5 }, { "peachpuff", 0xFFFFDAB9 },
    { "peru", 0xFFCD853F }, { "pink", 0xFFFFC0CB }, { "plum", 0xFFDDA0DD }, { "powderblue", 0xFFB0E0E6 },
    { "purple", 0xFF800080 }, { "red", 0xFFFF0000 }, { "rosybrown", 0xFFBC8F8F }, { "royalblue", 0xFF4169E1 },
    { "saddlebrown", 0xFF8B4513 }, { "salmon", 0xFFFA8072 }, { "sandybrown", 0xFFF4A460 },
    { "seagreen", 0xFF2E8B57 }, { "seashell", 0xFFFFF5EE }, { "sienna", 0xFFA0522D }, { "silver", 0xFFC0C0C0 },
    { "skyblue", 0xFF87CEEB }, { "slateblue", 0xFF6A5ACD }, { "slategray", 0xFF708090 },
    { "snow", 0xFFFFFAFA }, { "springgreen", 0xFF00FF7F }, { "steelblue", 0xFF4682B4 }, { "tan", 0xFFD2B48C },
    { "teal", 0xFF008080 }, { "thistle", 0xFFD8BFD8 }, { "tomato", 0xFFFF6347 }, { "transparent", 0x00FFFFFF },
    { "turquoise", 0xFF40E0D0 }, { "violet", 0xFFEE82EE }, { "wheat", 0xFFF5DEB3 },
    { "whitesmoke", 0xFFF5F5F5 }, { "yellow", 0xFFFFFF00 }, { "yellowgreen", 0xFF9ACD32 },
};

// images.nss names a config may not define (no images.nss): a Segoe glyph like each,
// so icon.copy still shows a copy icon.
struct NamedGlyph
{
    const char *name;
    unsigned glyph;
};
constexpr NamedGlyph kBuiltinGlyphs[] = {
    { "copy", 0xE8C8 },          { "copy_to_clipboard", 0xE8C8 }, { "cut", 0xE8C6 },
    { "paste", 0xE77F },         { "command_prompt", 0xE756 },    { "windows_powershell", 0xE756 },
    { "run_with_powershell", 0xE756 }, { "run_as_administrator", 0xE7EF }, { "new_file", 0xE8A5 },
    { "new_folder", 0xE8F4 },    { "open_new_tab", 0xE8A7 },      { "open_new_window", 0xE8A7 },
    { "extract_all", 0xF012 },   { "compressed", 0xF012 },        { "settings", 0xE713 },
    { "options", 0xE713 },       { "folder_options", 0xE713 },    { "refresh", 0xE72C },
    { "delete", 0xE74D },        { "rename", 0xE8AC },            { "select_all", 0xE8B3 },
    { "copy_path", 0xE8C8 },     { "copy_as_path", 0xE8C8 },      { "share", 0xE72D },
    { "send_to", 0xE724 },       { "open_with", 0xE7AC },         { "open_folder", 0xE838 },
    { "create_shortcut", 0xE71B }, { "details", 0xE8FD },         { "expand", 0xE740 },
    { "properties", 0xE946 },    { "view", 0xE8A9 },              { "sort_by", 0xE8CB },
    { "undo", 0xE7A7 },          { "redo", 0xE7A6 },              { "print", 0xE749 },
};

constexpr int kMaxDepth = 8;

// A colour from its hex digits, as Shell's lexer reads #RGB, #RGBA, #RGBAA, #RRGGBB, #RRGGBBAA.
std::optional<uint32_t> hexColor(std::string_view digits)
{
    const auto n = digits.size();
    if (n < 3 || n > 8 || n == 7 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
        return std::nullopt;
    const uint32_t v = uint32_t(std::stoul(std::string(digits), nullptr, 16));
    uint32_t r = 0, g = 0, b = 0, a = 0xFF;
    if (n <= 5) {
        const int shift = n == 3 ? 0 : n == 4 ? 4 : 8;
        r = (v >> (8 + shift)) & 0xF;
        g = (v >> (4 + shift)) & 0xF;
        b = (v >> shift) & 0xF;
        r |= r << 4;
        g |= g << 4;
        b |= b << 4;
        if (n == 4)
            a = (v & 0xF) | ((v & 0xF) << 4);
        else if (n == 5)
            a = v & 0xFF;
    } else if (n == 6) {
        r = (v >> 16) & 0xFF;
        g = (v >> 8) & 0xFF;
        b = v & 0xFF;
    } else {
        r = (v >> 24) & 0xFF;
        g = (v >> 16) & 0xFF;
        b = (v >> 8) & 0xFF;
        a = v & 0xFF;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

Icon iconAt(std::string_view value, const ImageDefs &defs, const Icon *parent, int depth);

// A file icon, then `next` when the file isn't there.
Icon orElse(Icon first, const Icon &next)
{
    if (first.kind == IconKind::None || (first.kind == IconKind::File && first.path.empty()))
        return next;
    if (first.kind != IconKind::File || next.kind == IconKind::None)
        return first;
    first.fallback = std::make_shared<const Icon>(first.fallback ? orElse(*first.fallback, next) : next);
    return first;
}

// The text of a path expression: 'C:\x.exe', a variable holding one, path.combine(sys.bin,
// 'x.dll'), eval(...); sys.* and user.* become @sys.* for expandPath(). nullopt when it
// can't be told without running it.
std::optional<std::string> pathText(std::string_view value, const ImageDefs &defs, int depth)
{
    auto v = nss::trim(value);
    if (depth > kMaxDepth || v.empty())
        return std::nullopt;
    if (isQuoted(v))
        return unquote(v);
    if (auto c = call(v)) {
        auto args = splitArgs(c->second);
        if (c->first == "eval" && args.size() == 1)
            return pathText(args[0], defs, depth + 1);
        if ((c->first == "path.combine" || c->first == "path.join") && !args.empty()) {
            std::string out;
            for (const auto &a : args) {
                auto part = pathText(a, defs, depth + 1);
                if (!part)
                    return std::nullopt;
                if (!out.empty() && out.back() != '\\' && !part->empty() && part->front() != '\\')
                    out += '\\';
                out += *part;
            }
            return out;
        }
        return std::nullopt;
    }
    const auto low = lower(v);
    for (const char *scope : { "sys.", "user.", "app." })
        if (low.rfind(scope, 0) == 0 && std::all_of(low.begin(), low.end(), isIdent))
            return "@" + low;
    std::string var = low;
    if (!var.empty() && var.front() == '$')
        var.erase(0, 1);
    if (auto it = defs.vars.find(var); it != defs.vars.end())
        return pathText(it->second, defs, depth + 1);
    return std::nullopt;
}

Icon fileIcon(std::string path, int index)
{
    Icon icon{ IconKind::File };
    icon.path = std::move(path);
    icon.index = index;
    return icon;
}

// 'C:\x\app.dll,3' -> the path and the index.
Icon fileIconOf(const std::string &s)
{
    auto comma = s.rfind(',');
    if (comma != std::string::npos && comma + 1 < s.size() &&
        std::all_of(s.begin() + long(comma) + 1, s.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) || ch == '-'; }))
        return fileIcon(trimmed(std::string_view(s).substr(0, comma)), std::atoi(s.c_str() + comma + 1));
    return fileIcon(s, 0);
}

IconColor colorAt(std::string_view value, const ImageDefs &defs, int depth)
{
    if (auto c = colorOf(value))
        return *c;
    std::string var = lower(nss::trim(value));
    if (!var.empty() && var.front() == '$')
        var.erase(0, 1);
    if (auto it = defs.vars.find(var); it != defs.vars.end() && depth < kMaxDepth)
        return colorAt(it->second, defs, depth + 1);
    return {};
}

// The colour among a glyph's other arguments (the rest are its size).
IconColor glyphArgColor(const std::vector<std::string> &args, const ImageDefs &defs)
{
    for (size_t i = 1; i < args.size(); ++i)
        if (auto c = colorAt(args[i], defs, 0); c.given())
            return c;
    return {};
}

Icon glyphIcon(const std::string &glyph, GlyphFont font, IconColor color = {})
{
    Icon icon{ IconKind::Glyph, glyph, font };
    icon.colors[0] = color;
    return icon;
}

// icon.copy, image.copy: the images.nss definition, else a Segoe glyph like it.
std::optional<Icon> namedImage(const std::string &name, const ImageDefs &defs)
{
    if (auto it = defs.svgs.find(name); it != defs.svgs.end())
        return Icon{ IconKind::Svg, {}, GlyphFont::Shell, it->second };
    for (const auto &g : kBuiltinGlyphs)
        if (name == g.name)
            return glyphIcon(utf8(g.glyph), GlyphFont::Segoe);
    return std::nullopt;
}

Icon iconAt(std::string_view value, const ImageDefs &defs, const Icon *parent, int depth)
{
    auto v = nss::trim(value);
    const auto low = lower(v);
    if (v.empty() || low == "inherit" || low == "image.inherit" || low == "icon.inherit")
        return parent ? *parent : Icon{};
    if (depth > kMaxDepth || low == "none" || low == "null" || low == "nil" || low == "image.none" ||
        low == "icon.none" || low == "0" || low == "false")
        return {};
    // [glyph, colour]: a glyph in that colour; otherwise the first is the image.
    if (v.front() == '[' && v.back() == ']') {
        auto args = splitArgs(v.substr(1, v.size() - 2));
        if (args.empty())
            return {};
        auto icon = iconAt(args[0], defs, parent, depth + 1);
        if (icon.kind == IconKind::Glyph)
            if (auto c = glyphArgColor(args, defs); c.given())
                icon.colors[0] = c;
        return icon;
    }
    if (auto g = glyphOf(v); !g.empty())
        return glyphIcon(g, GlyphFont::Shell);
    if (auto c = call(v)) {
        auto args = splitArgs(c->second);
        const std::string first = args.empty() ? std::string{} : args[0];
        const auto &fn = c->first;
        if (fn == "eval" && args.size() == 1)
            return iconAt(first, defs, parent, depth + 1);
        // if(path.exists(x), x, [\uE187, image.color2]): the first when it can be shown,
        // else the second (a file that isn't there falls back to it).
        if (fn == "if" && args.size() >= 2)
            return orElse(iconAt(args[1], defs, parent, depth + 1),
                          args.size() > 2 ? iconAt(args[2], defs, parent, depth + 1) : Icon{});
        auto dot = fn.find('.');
        const std::string scope = dot == std::string::npos ? fn : fn.substr(0, dot);
        const std::string name = dot == std::string::npos ? std::string{} : fn.substr(dot + 1);
        if (scope != "image" && scope != "icon")
            return {};
        if (name == "mdl" || name == "fluent" || name == "segoe") {
            if (auto g = glyphOf(first); !g.empty())
                return glyphIcon(g, GlyphFont::Segoe, glyphArgColor(args, defs));
            return {};
        }
        if (name == "glyph") {
            if (auto g = glyphOf(first); !g.empty())
                return glyphIcon(g, GlyphFont::Shell, glyphArgColor(args, defs));
            return {};
        }
        if (name == "svg") {
            if (isQuoted(first))
                return Icon{ IconKind::Svg, {}, GlyphFont::Shell, unquote(first) };
            if (auto it = defs.vars.find(lower(first)); it != defs.vars.end())
                return iconAt(it->second, defs, parent, depth + 1);
            return {};
        }
        if (name.empty() || name == "res" || name == "icon") {
            auto path = pathText(first, defs, depth + 1);
            if (!path || path->empty())
                return {};
            if (args.size() > 1)
                return fileIcon(*path, std::atoi(args[1].c_str()));
            return fileIconOf(*path);
        }
        // icon.rename(auto, color.greenyellow): the image in these colours.
        if (auto icon = namedImage(name, defs)) {
            for (size_t i = 0; i < args.size() && i < 3; ++i)
                icon->colors[i] = colorAt(args[i], defs, depth + 1);
            if (icon->kind == IconKind::Glyph && !icon->colors[0].given())
                icon->colors[0] = icon->colors[1];
            return *icon;
        }
        return {};
    }
    if (isQuoted(v)) {
        auto s = unquote(v);
        if (s.find("<svg") != std::string::npos)
            return Icon{ IconKind::Svg, {}, GlyphFont::Shell, s };
        if (s.empty())
            return {};
        return fileIconOf(s);
    }
    // icon.copy / image.copy: an image definition; a variable.
    std::string name = low;
    if (name.rfind("icon.", 0) == 0)
        name.erase(0, 5);
    else if (name.rfind("image.", 0) == 0)
        name.erase(0, 6);
    if (name != low)
        if (auto icon = namedImage(name, defs))
            return *icon;
    std::string var = low;
    if (!var.empty() && var.front() == '$')
        var.erase(0, 1);
    if (auto it = defs.vars.find(var); it != defs.vars.end() && it->second != value)
        return iconAt(it->second, defs, parent, depth + 1);
    return {};
}

uint32_t resolve(const IconColor &c, const uint32_t theme[3], uint32_t otherwise)
{
    if (c.theme >= 1 && c.theme <= 3)
        return theme[c.theme - 1];
    return c.argb ? c.argb : otherwise;
}

} // namespace

std::optional<IconColor> colorOf(std::string_view value)
{
    auto v = unquote(value);
    const auto low = lower(v);
    if (low == "auto" || low == "default" || low == "color.default" || low == "color.auto" || low == "null")
        return IconColor{};
    if (low.size() > 1 && low.front() == '#') {
        if (auto argb = hexColor(std::string_view(low).substr(1)))
            return IconColor{ 0, *argb };
        return std::nullopt;
    }
    if (low == "image.color1" || low == "image.color2" || low == "image.color3")
        return IconColor{ low.back() - '0', 0 };
    if (low.rfind("color.", 0) == 0) {
        const auto name = std::string_view(low).substr(6);
        for (const auto &c : kColors)
            if (name == c.name)
                return IconColor{ 0, c.argb };
        return IconColor{}; // color.accent and the like: the theme's
    }
    if (auto c = call(v); c && (c->first == "rgb" || c->first == "rgba")) {
        auto args = splitArgs(c->second);
        if (args.size() < 3)
            return std::nullopt;
        uint32_t ch[4] = { 0, 0, 0, 255 };
        for (size_t i = 0; i < args.size() && i < 4; ++i)
            ch[i] = uint32_t(std::clamp(std::atoi(args[i].c_str()), 0, 255));
        return IconColor{ 0, (ch[3] << 24) | (ch[0] << 16) | (ch[1] << 8) | ch[2] };
    }
    return std::nullopt;
}

void iconColors(const Icon &icon, const uint32_t theme[3], uint32_t out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = resolve(icon.colors[i], theme, theme[i]);
}

uint32_t glyphColor(const Icon &icon, const uint32_t theme[3]) { return resolve(icon.colors[0], theme, 0); }

Icon iconOf(std::string_view value, const ImageDefs &defs, const Icon *parent)
{
    return iconAt(value, defs, parent, 0);
}

std::string svgFor(std::string_view svg, const ImageDefs &defs, const uint32_t colors[3], bool light)
{
    return fill(svg, defs, colors, light, 0);
}

std::string expandPath(std::string_view path, const std::map<std::string, std::string> &vars)
{
    std::string s = unquote(path);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '@') {
            out += s[i];
            continue;
        }
        size_t end = i + 1;
        while (end < s.size() && (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_' || s[end] == '.'))
            ++end;
        auto name = lower(std::string_view(s).substr(i + 1, end - i - 1));
        // The longest known name: @sys.prog32 before @sys.prog.
        while (!name.empty() && !vars.count(name)) {
            auto dot = name.rfind('.');
            if (dot == std::string::npos) {
                name.clear();
                break;
            }
            name.resize(dot);
        }
        if (name.empty()) {
            out += '@';
            continue;
        }
        out += vars.at(name);
        i += name.size();
    }
    return out;
}

// --- the snippet's items ----------------------------------------------------------------

Title splitTitle(std::string_view raw)
{
    Title out;
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\t') {
            out.keys = trimmed(raw.substr(i + 1));
            break;
        }
        if (c == '&') {
            if (i + 1 < raw.size() && raw[i + 1] == '&') {
                out.label += '&';
                ++i;
            } else if (out.accel < 0 && i + 1 < raw.size() && raw[i + 1] != ' ' && raw[i + 1] != '\t')
                out.accel = int(out.label.size());
            continue;
        }
        out.label += c;
    }
    return out;
}

namespace {

// where=key.shift() (or key.shift() and ...): the item shows only while Shift is held.
bool shiftOnly(std::string_view where)
{
    std::string w = lower(unquote(where));
    for (char &c : w)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    if (w.find(" or ") != std::string::npos || w.find("||") != std::string::npos)
        return false;
    for (size_t at = w.find("&&"); at != std::string::npos; at = w.find("&&", at))
        w.replace(at, 2, " and ");
    size_t start = 0;
    while (start <= w.size()) {
        size_t next = w.find(" and ", start);
        std::string term;
        for (char c : w.substr(start, next == std::string::npos ? std::string::npos : next - start))
            if (c != ' ' && c != '(' && c != ')')
                term += c;
        if (term == "key.shift" || term == "keys.shift")
            return true;
        if (next == std::string::npos)
            break;
        start = next + 5;
    }
    return false;
}

std::optional<Item> convert(const menumap::Node &n, const nss::NssDocument &doc, const ImageDefs &defs,
                            const Icon *parentIcon)
{
    if (n.vis == "hidden" || n.vis == "remove")
        return std::nullopt;
    Item it;
    it.kind = n.kind.empty() ? "item" : n.kind;
    it.disabled = n.vis == "disabled";
    const auto *decl = findDecl(doc, n.line, n.column);
    if (it.kind == "separator") {
        return it;
    }
    // The title as the menu shows it: no '&', the accelerator underlined, a tab's tail on the right.
    auto title = splitTitle(n.rawTitle.empty() ? n.title : n.rawTitle);
    it.title = title.label;
    it.accel = title.accel;
    it.keys = title.keys;
    if (it.title.empty() && (n.state == "dynamic" || n.dynamic))
        it.dynamicTitle = true;
    if (decl) {
        if (auto where = prop(doc, *decl, { "where" }); where && shiftOnly(*where))
            return std::nullopt;
        // keys= written as text; one that is worked out (keys=@x) is left out.
        if (auto keys = prop(doc, *decl, { "keys" }); keys && isQuoted(*keys)) {
            auto k = unquote(*keys);
            if (k.find('@') == std::string::npos)
                it.keys = k;
        }
        if (auto image = prop(doc, *decl, { "image", "icon" }))
            it.icon = iconOf(*image, defs, parentIcon);
        if (auto pos = prop(doc, *decl, { "pos", "position" })) {
            auto p = lower(unquote(*pos));
            if (p == "top" || p == "pos.top" || p == "0")
                it.pos = Pos::Top;
            else if (p == "bottom" || p == "pos.bottom")
                it.pos = Pos::Bottom;
        }
        if (auto sep = prop(doc, *decl, { "sep", "separator" })) {
            auto s = lower(unquote(*sep));
            it.sepBefore = s.empty() || s == "before" || s == "top" || s == "both" || s == "sep.top" || s == "sep.before" || s == "sep.both";
            it.sepAfter = s == "after" || s == "bottom" || s == "both" || s == "sep.bottom" || s == "sep.after" || s == "sep.both";
        }
        if (it.kind == "menu")
            if (auto e = prop(doc, *decl, { "expanded" }))
                it.expanded = truthy(*e);
    }
    if (it.kind == "menu" && it.title.empty() && !it.dynamicTitle)
        it.expanded = true; // no title: Shell shows its items in its place
    for (const auto &c : n.children)
        if (auto child = convert(c, doc, defs, it.icon.kind == IconKind::None ? parentIcon : &it.icon))
            it.children.push_back(std::move(*child));
    if (it.kind == "item" && it.title.empty() && !it.dynamicTitle)
        return std::nullopt;
    if (it.kind == "menu" && it.children.empty() && it.expanded)
        return std::nullopt;
    return it;
}

} // namespace

std::vector<Item> itemsOf(const std::vector<menumap::Node> &shell, const fs::path &snippetFile,
                          const nss::NssDocument &snippet, const ImageDefs &defs)
{
    std::vector<Item> out;
    const auto key = nss::pathKey(snippetFile);
    for (const auto &n : shell) {
        if (nss::pathKey(n.file) != key)
            continue;
        if (auto it = convert(n, snippet, defs, nullptr))
            out.push_back(std::move(*it));
    }
    // A snippet of separators only adds nothing.
    if (std::all_of(out.begin(), out.end(), [](const Item &i) { return i.kind == "separator"; }))
        out.clear();
    return out;
}

// --- the menu shown ---------------------------------------------------------------------

namespace {

struct FrameItem
{
    const char *label;
    unsigned glyph; // Segoe Fluent Icons
};
constexpr FrameItem kSep{ nullptr, 0 };

struct Frame
{
    std::vector<FrameItem> before, after;
};

// A few of Windows' own items around the snippet's, as the Appearance preview has them.
Frame frameOf(Context c)
{
    switch (c) {
    case Context::File:
        return { { { nsp::mark("Open"), 0xE8E5 }, { nsp::mark("Copy"), 0xE8C8 } }, { { nsp::mark("Properties"), 0xE946 } } };
    case Context::Folder:
        return { { { nsp::mark("Open"), 0xE8E5 }, { nsp::mark("Copy"), 0xE8C8 } }, { { nsp::mark("Properties"), 0xE946 } } };
    case Context::Back:
        return { { { nsp::mark("View"), 0xE8A9 }, { nsp::mark("Sort by"), 0xE8CB }, { nsp::mark("Refresh"), 0xE72C } },
                 { { nsp::mark("Properties"), 0xE946 } } };
    case Context::Desktop:
        return { { { nsp::mark("View"), 0xE8A9 }, { nsp::mark("Sort by"), 0xE8CB }, { nsp::mark("Refresh"), 0xE72C } },
                 { { nsp::mark("Display settings"), 0xE7F4 }, { nsp::mark("Personalize"), 0xE771 } } };
    case Context::Drive:
        return { { { nsp::mark("Open"), 0xE8E5 } }, { { nsp::mark("Properties"), 0xE946 } } };
    case Context::Taskbar:
        return { {}, { { nsp::mark("Task Manager"), 0xE9D9 }, { nsp::mark("Taskbar settings"), 0xE713 } } };
    }
    return {};
}

Row separatorRow(bool own)
{
    Row r;
    r.separator = true;
    r.own = own;
    return r;
}

// Separators never lead, trail or come twice.
void tidy(std::vector<Row> &rows)
{
    std::vector<Row> out;
    for (auto &r : rows) {
        if (r.separator && (out.empty() || out.back().separator))
            continue;
        out.push_back(std::move(r));
    }
    while (!out.empty() && out.back().separator)
        out.pop_back();
    rows = std::move(out);
}

void flatten(const std::vector<Item> &items, std::vector<Row> &rows, std::vector<Menu> &subs, const Translate &tr);

Menu subMenu(const std::vector<Item> &items, std::vector<Menu> &subs, const Translate &tr)
{
    Menu m;
    flatten(items, m.rows, subs, tr);
    tidy(m.rows);
    return m;
}

void flatten(const std::vector<Item> &items, std::vector<Row> &rows, std::vector<Menu> &subs, const Translate &tr)
{
    for (const auto &it : items) {
        if (it.kind == "separator") {
            rows.push_back(separatorRow(true));
            continue;
        }
        if (it.sepBefore)
            rows.push_back(separatorRow(true));
        if (it.kind == "menu" && it.expanded) {
            flatten(it.children, rows, subs, tr);
        } else {
            Row r;
            r.own = true;
            r.icon = it.icon;
            r.disabled = it.disabled;
            r.placeholder = it.dynamicTitle;
            r.label = it.dynamicTitle ? tr(nsp::mark("Name from the selection")) : it.title;
            r.accel = it.dynamicTitle ? -1 : it.accel;
            r.keys = it.keys;
            if (it.kind == "menu") {
                // The submenu's own submenus get an index too, so their rows show a chevron.
                const size_t index = subs.size();
                subs.emplace_back();
                auto m = subMenu(it.children, subs, tr);
                subs[index] = std::move(m);
                r.sub = int(index);
            }
            rows.push_back(std::move(r));
        }
        if (it.sepAfter)
            rows.push_back(separatorRow(true));
    }
}

} // namespace

Composed compose(Context context, const std::vector<Item> &items, const Translate &tr)
{
    Composed out;
    std::vector<Item> top, middle, bottom;
    for (const auto &it : items)
        (it.pos == Pos::Top ? top : it.pos == Pos::Bottom ? bottom : middle).push_back(it);

    // `keep` of Windows' rows: the last ones before the snippet's, the first ones after.
    auto frameRows = [&](std::vector<FrameItem> list, size_t keep, bool last) {
        if (list.size() > keep)
            list.erase(last ? list.begin() : list.begin() + long(keep), last ? list.end() - long(keep) : list.end());
        for (const auto &f : list) {
            if (!f.label) {
                out.main.rows.push_back(separatorRow(false));
                continue;
            }
            Row r;
            r.label = tr(f.label);
            r.icon = Icon{ IconKind::Glyph, utf8(f.glyph), GlyphFont::Segoe };
            r.ghost = true;
            out.main.rows.push_back(std::move(r));
        }
    };
    auto ownRows = [&](const std::vector<Item> &list) {
        if (list.empty())
            return;
        out.main.rows.push_back(separatorRow(false));
        flatten(list, out.main.rows, out.subs, tr);
        out.main.rows.push_back(separatorRow(false));
    };

    const auto frame = frameOf(context);
    ownRows(top);
    frameRows(frame.before, 2, true);
    ownRows(middle);
    frameRows(frame.after, 1, false);
    ownRows(bottom);
    tidy(out.main.rows);
    for (auto &m : out.subs)
        tidy(m.rows);

    // The runs of the snippet's rows, for the outline around them.
    auto &rows = out.main.rows;
    for (size_t i = 0; i < rows.size();) {
        if (!rows[i].own || rows[i].separator) {
            ++i;
            continue;
        }
        size_t last = i;
        size_t j = i;
        while (j < rows.size() && rows[j].own) {
            if (!rows[j].separator)
                last = j;
            ++j;
        }
        rows[i].groupFirst = true;
        rows[last].groupLast = true;
        i = j;
    }

    for (size_t i = 0; i < out.main.rows.size(); ++i) {
        const auto &r = out.main.rows[i];
        if (r.own && !r.separator)
            ++out.own;
        if (out.open < 0 && r.own && r.sub >= 0 && !out.subs[size_t(r.sub)].rows.empty())
            out.open = int(i);
    }
    return out;
}

} // namespace snippetpreview
