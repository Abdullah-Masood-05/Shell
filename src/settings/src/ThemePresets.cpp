#include "ThemePresets.h"

#include "nss/Literal.h"
#include "nss/NssDocument.h"
#include "nss/Text.h"
#include "nss/ThemeSchema.h"

#include "Tr.h"

#include <algorithm>
#include <set>

// The presets, generated at configure time from presets/themes/*.nss:
//   struct EmbeddedPreset { const char *id; const char *text; };
//   constexpr EmbeddedPreset kEmbeddedPresets[] = { ... };
#include "ThemePresetsData.inc"

namespace presets {

namespace {

// The bundled presets' names, about lines and tags (presets/themes/*.nss headers),
// marked for translation; displayName() and friends look them up. Keep in step with
// the files: a header line missing here is shown in English.
[[maybe_unused]] constexpr const char *kPresetText[] = {
    nsp::mark("Default"),
    nsp::mark("The look Shell ships with: modern, following Windows."),
    nsp::mark("Shell 1.8"),
    nsp::mark("The 2023 default: modern, without the blur effect."),
    nsp::mark("Acrylic"),
    nsp::mark("Acrylic tinted with the accent colour (README screenshot)."),
    nsp::mark("Gradient"),
    nsp::mark("An accent gradient from dark to light and back (README screenshot)."),
    nsp::mark("Clear acrylic"),
    nsp::mark("Modern with plain acrylic and no background colour."),
    nsp::mark("Accent outline"),
    nsp::mark("A 1 px accent-coloured border and accent icons."),
    nsp::mark("Flat"),
    nsp::mark("No border, no shadow, square item highlights."),
    nsp::mark("Blue"),
    nsp::mark("Shell 1.x colours: white on deep blue, square corners."),
    nsp::mark("Gray"),
    nsp::mark("Shell 1.x colours: white on grey, light highlight."),
    nsp::mark("Green"),
    nsp::mark("Shell 1.x colours: black on green, square corners."),
    nsp::mark("Yellow"),
    nsp::mark("Shell 1.x colours: white on olive, yellow highlight."),
    // Tags
    nsp::mark("Modern"),
    nsp::mark("Auto"),
    nsp::mark("Opaque"),
    nsp::mark("Accent tint"),
    nsp::mark("Accent"),
    nsp::mark("No tint"),
    nsp::mark("Border"),
    nsp::mark("Square"),
    nsp::mark("No shadow"),
    nsp::mark("Shell 1.x"),
};

// "// Preset: Dark acrylic" -> {"preset", "Dark acrylic"}; only the comment lines
// before the first statement are read.
void readHeader(std::string_view text, Preset &p)
{
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        auto line = nss::trim(text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        pos = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty())
            continue;
        if (line.substr(0, 2) != "//")
            break;
        line = nss::trim(line.substr(2));
        auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        auto key = nss::toLower(nss::trim(line.substr(0, colon)));
        auto value = std::string(nss::trim(line.substr(colon + 1)));
        if (key == "preset")
            p.name = value;
        else if (key == "about")
            p.about = value;
        else if (key == "tags") {
            std::string_view rest = value;
            while (!rest.empty()) {
                auto comma = rest.find(',');
                auto tag = nss::trim(rest.substr(0, comma));
                if (!tag.empty())
                    p.tags.emplace_back(tag);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
        }
        else if (key == "source")
            p.source = value;
    }
}

// A literal written over several lines (gradient stops) on one, as settings-app.nss
// stores values: each line break and the indentation around it become one space,
// none right inside brackets. Quoted strings never span lines, so they are untouched.
std::string oneLine(std::string_view text)
{
    if (text.find('\n') == std::string_view::npos)
        return std::string(text);
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        auto line = nss::trim(text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        pos = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty())
            continue;
        if (!out.empty() && out.back() != '[' && line.front() != ']')
            out += ' ';
        out += line;
    }
    return out;
}

std::vector<Preset> parseEmbedded()
{
    std::vector<Preset> out;
    for (const auto &e : kEmbeddedPresets)
        out.push_back(parse(e.id, e.text));
    std::sort(out.begin(), out.end(), [](const Preset &a, const Preset &b) { return a.id < b.id; });
    return out;
}

} // namespace

Preset parse(std::string id, std::string_view text)
{
    Preset p;
    p.id = std::move(id);
    p.text = std::string(text);
    readHeader(text, p);
    if (p.name.empty())
        p.errors.push_back("no `// Preset:` name line");
    if (p.tags.size() > 3)
        p.errors.push_back("more than three `// Tags:`");

    auto doc = nss::NssDocument::fromText(std::string(text));
    for (const auto &d : doc.diagnostics())
        p.errors.push_back("line " + std::to_string(d.line) + ": " + d.message);

    // Only theme{} (and theme.x = y lines): a preset is a look, nothing else.
    for (const auto &node : doc.nodes()) {
        const bool theme = (node.kind == nss::NodeKind::Block && node.path == "theme") ||
                           (node.kind == nss::NodeKind::Assign && node.path.rfind("theme.", 0) == 0);
        if (!theme)
            p.errors.push_back("line " + std::to_string(node.line) + ": only theme{} belongs in a preset");
    }

    for (const auto *n : doc.assigns()) {
        const auto key = nss::ThemeSchema::canonical(n->path);
        const auto value = std::string(nss::trim(doc.slice(n->valueSpan)));
        const auto where = "line " + std::to_string(n->line) + ": ";
        const auto *info = nss::ThemeSchema::find(key);
        auto lit = nss::Literal::parse(value);
        if (!info) {
            p.errors.push_back(where + key + ": unknown key");
            continue;
        }
        if (!lit.isLiteral()) {
            p.errors.push_back(where + key + ": an expression, not a plain value");
            continue;
        }
        for (const auto &e : nss::ThemeSchema::validate(key, lit))
            p.errors.push_back(where + e);
        p.values[key] = oneLine(lit.text); // later lines win, like the core
    }
    if (p.values.empty())
        p.errors.push_back("no theme values");
    return p;
}

const std::vector<Preset> &embedded()
{
    static const std::vector<Preset> list = parseEmbedded();
    return list;
}

const std::vector<Preset> &all()
{
    static const std::vector<Preset> list = [] {
        std::vector<Preset> ok;
        for (const auto &p : embedded())
            if (p.errors.empty())
                ok.push_back(p);
        return ok;
    }();
    return list;
}

bool isThemeKey(std::string_view key) { return key.rfind("theme.", 0) == 0; }

std::vector<std::string> stage(PendingChanges &pending, const Preset &preset, const std::vector<std::string> &appKeys,
                               const ValueFn &saved, std::string_view page)
{
    // Validate everything first, on a scratch copy, so a bad value changes nothing.
    {
        PendingChanges probe;
        for (const auto &[key, literal] : preset.values)
            if (auto errors = probe.set(key, literal, page); !errors.empty())
                return errors;
    }

    // Other pending theme changes go; this app's saved theme overrides are reset.
    std::vector<std::string> drop;
    for (const auto &[key, change] : pending.items())
        if (isThemeKey(key) && !preset.values.count(key))
            drop.push_back(key);
    for (const auto &key : drop)
        pending.forget(key);
    for (const auto &raw : appKeys) {
        auto key = nss::ThemeSchema::canonical(raw);
        if (isThemeKey(key) && !preset.values.count(key))
            pending.reset(key, page);
    }
    for (const auto &[key, literal] : preset.values)
        pending.stage(key, literal, page, saved(key));
    return {};
}

bool inUse(const Preset &preset, const ValueFn &current)
{
    for (const auto &[key, literal] : preset.values) {
        auto now = current(key);
        if (!now || !sameValue(*now, literal))
            return false;
    }
    return !preset.values.empty();
}

int activeIndex(const std::vector<Preset> &list, const ValueFn &current, const nss::Overrides &overrides)
{
    auto rank = [&](const Preset &p) {
        size_t own = 0;
        for (const auto &[key, literal] : p.values)
            own += overrides.count(key);
        return std::make_pair(own, p.values.size());
    };
    int best = -1;
    for (size_t i = 0; i < list.size(); ++i)
        if ((best < 0 || rank(list[i]) > rank(list[size_t(best)])) && inUse(list[i], current))
            best = int(i);
    return best;
}

int replacedCount(const nss::Overrides &overrides, const Preset *active)
{
    int n = 0;
    for (const auto &[raw, literal] : overrides) {
        const auto key = nss::ThemeSchema::canonical(raw);
        if (!isThemeKey(key))
            continue;
        if (active) {
            auto it = active->values.find(key);
            if (it != active->values.end() && sameValue(it->second, literal))
                continue;
        }
        ++n;
    }
    return n;
}

std::string exportBlock(const nss::Overrides &values, std::string_view eol)
{
    std::vector<std::pair<std::string, std::string>> sorted;
    for (const auto &[key, value] : values) {
        auto canonical = nss::ThemeSchema::canonical(key);
        if (isThemeKey(canonical))
            sorted.emplace_back(canonical, std::string(nss::trim(value)));
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        auto oa = nss::ThemeSchema::order(a.first), ob = nss::ThemeSchema::order(b.first);
        return oa != ob ? oa < ob : a.first < b.first;
    });
    const std::string nl(eol);
    std::string out = "theme" + nl + "{" + nl;
    for (const auto &[key, value] : sorted)
        out += "\t" + key.substr(6) + " = " + value + nl;
    out += "}" + nl;
    return out;
}

std::string exportText(const Preset &preset, std::string_view eol)
{
    std::string out;
    out.reserve(preset.text.size() + 64);
    for (char c : preset.text) {
        if (c == '\r')
            continue;
        if (c == '\n')
            out += eol;
        else
            out += c;
    }
    return out;
}

std::string displayName(const Preset &preset)
{
    return preset.name.empty() ? std::string{} : std::string(nsp::tr(preset.name.c_str()));
}

std::string displayAbout(const Preset &preset)
{
    return preset.about.empty() ? std::string{} : std::string(nsp::tr(preset.about.c_str()));
}

std::vector<std::string> displayTags(const Preset &preset)
{
    std::vector<std::string> out;
    out.reserve(preset.tags.size());
    for (const auto &tag : preset.tags)
        out.emplace_back(nsp::tr(tag.c_str()));
    return out;
}

} // namespace presets
