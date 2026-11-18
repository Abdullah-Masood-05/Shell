#include "CarryOver.h"

#include "Diff.h"

#include "nss/NssDocument.h"

#include "../../shared/ConfigUpgrade.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>

namespace carryover {

namespace {

struct BaseText
{
    const char *rel;
    const char *text;
};

constexpr BaseText kBases[] = {
#include "ShippedBases.inc"
};

// The properties the Menu map changed on an item's own line.
constexpr std::string_view kGroups[] = { "vis", "title", "image", "menu", "pos" };

bool carryable(std::string_view canonicalName)
{
    return std::find(std::begin(kGroups), std::end(kGroups), canonicalName) != std::end(kGroups);
}

std::string slashLower(std::string_view s)
{
    auto out = nss::toLower(s);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

bool isRoot(std::string_view rel) { return slashLower(rel) == "shell.nss"; }

std::optional<nss::NssDocument> parse(const std::string &text)
{
    auto doc = nss::NssDocument::fromText(text);
    if (doc.diagnostics().empty())
        return doc;
    // A file imported inside menu{}: its declarations parse as a menu body.
    nss::ParseContext menu;
    menu.mode = nss::ParseMode::Menu;
    auto alt = nss::NssDocument::fromText(text, nss::defaultFormat(), menu);
    if (alt.diagnostics().empty())
        return alt;
    return std::nullopt;
}

// "// import 'imports/goto.nss'": an import the user turned off.
bool disabledImport(std::string_view comment)
{
    if (comment.substr(0, 2) != "//")
        return false;
    auto c = nss::trim(comment.substr(2));
    if (c.size() < 8 || c.substr(0, 6) != "import" || (c[6] != ' ' && c[6] != '\t'))
        return false;
    c = nss::trim(c.substr(6));
    return !c.empty() && (c.front() == '\'' || c.front() == '"');
}

// The comments in a token's leading trivia, as written (trailing blanks left out).
void commentsIn(std::string_view trivia, bool root, std::vector<std::string> &out)
{
    for (size_t i = 0; i + 1 < trivia.size(); ++i) {
        std::string_view c;
        if (trivia[i] == '/' && trivia[i + 1] == '/') {
            auto e = trivia.find_first_of("\r\n", i);
            c = trivia.substr(i, e == std::string_view::npos ? std::string_view::npos : e - i);
            i = e == std::string_view::npos ? trivia.size() : e;
        } else if (trivia[i] == '/' && trivia[i + 1] == '*') {
            auto e = trivia.find("*/", i + 2);
            c = trivia.substr(i, e == std::string_view::npos ? std::string_view::npos : e + 2 - i);
            i = e == std::string_view::npos ? trivia.size() : e + 1;
        } else
            continue;
        while (!c.empty() && (c.back() == ' ' || c.back() == '\t'))
            c.remove_suffix(1);
        if (root && disabledImport(c))
            continue;
        out.emplace_back(c);
    }
}

// The file as tokens and comments, without the properties the Menu map changes (and,
// in shell.nss, without its import lines): equal for two texts that differ only there.
std::vector<std::string> skeleton(const nss::NssDocument &doc, bool root)
{
    std::vector<nss::Span> cut;
    doc.visit([&](const nss::Node &n) {
        if (n.kind == nss::NodeKind::MenuDecl && (n.menuKind == nss::MenuKind::Menu || n.menuKind == nss::MenuKind::Item))
            for (const auto &p : n.props)
                if (carryable(overrides::canonical(p.name)))
                    cut.push_back({ p.nameSpan.begin, p.hasValue ? p.valueSpan.end : p.nameSpan.end });
        return true;
    });
    if (root)
        for (const auto *n : doc.imports())
            if (n->enabled && n->context.empty() && n->importKind == nss::ImportKind::Config)
                cut.push_back(n->span);

    std::vector<std::string> out;
    std::string_view text = doc.text();
    for (const auto &t : doc.tokens()) {
        commentsIn(text.substr(t.triviaBegin, t.begin - t.triviaBegin), root, out);
        if (t.kind == nss::TokenKind::Eof)
            break;
        bool inCut = std::any_of(cut.begin(), cut.end(), [&](const nss::Span &s) { return t.begin >= s.begin && t.end <= s.end; });
        if (!inCut)
            out.emplace_back(text.substr(t.begin, t.end - t.begin));
    }
    return out;
}

struct PropVal
{
    bool present = false;
    bool hasValue = false;
    std::string text;
};

PropVal propOf(const nss::NssDocument &doc, const nss::Node &decl, std::string_view group)
{
    PropVal v;
    for (const auto &p : decl.props)
        if (overrides::canonical(p.name) == group) {
            v.present = true;
            v.hasValue = p.hasValue;
            v.text = p.hasValue ? std::string(nss::trim(doc.slice(p.valueSpan))) : std::string();
        }
    return v;
}

bool same(const PropVal &a, const PropVal &b)
{
    if (a.present != b.present)
        return false;
    return !a.present || (a.hasValue == b.hasValue && overrides::sourceKey(a.text) == overrides::sourceKey(b.text));
}

// A declaration's properties other than the ones the Menu map changes: what pairs an
// item of the user's copy with one of the base when they differ elsewhere (`partial`).
std::string signature(const nss::NssDocument &doc, const nss::Node &decl)
{
    std::string out = decl.path;
    for (const auto &p : decl.props) {
        if (carryable(overrides::canonical(p.name)))
            continue;
        out += " " + p.name;
        if (p.hasValue)
            out += "=" + overrides::sourceKey(doc.slice(p.valueSpan));
    }
    std::replace(out.begin(), out.end(), '\n', ' ');
    std::replace(out.begin(), out.end(), '\r', ' ');
    return out;
}

// The top-level `import '...'` lines, on or turned off.
struct Import
{
    size_t index = 0;     // in doc.imports()
    std::string key;      // the path, lower case, '/' separated
    bool app = false;     // imports/settings-app*.nss: the app's own (with_app_imports())
    bool enabled = true;
    size_t lineBegin = 0; // the line it is on
    size_t lineEnd = 0;   // after its line break
    std::string line;     // the line as written, without the line break
};

std::vector<Import> importsOf(const nss::NssDocument &doc)
{
    std::vector<Import> out;
    std::string_view text = doc.text();
    auto list = doc.imports();
    for (size_t i = 0; i < list.size(); ++i) {
        const auto *n = list[i];
        if (!n->context.empty() || n->importKind != nss::ImportKind::Config)
            continue;
        auto path = overrides::sourceKey(doc.slice(n->valueSpan));
        if (path.size() >= 2 && (path.front() == '\'' || path.front() == '"') && path.back() == path.front())
            path = path.substr(1, path.size() - 2);
        Import imp;
        imp.index = i;
        imp.key = slashLower(path);
        imp.app = imp.key.rfind("imports/settings-app", 0) == 0;
        imp.enabled = n->enabled;
        size_t at = n->enabled ? n->span.begin : n->keywordSpan.begin;
        imp.lineBegin = at;
        while (imp.lineBegin > 0 && text[imp.lineBegin - 1] != '\n' && text[imp.lineBegin - 1] != '\r')
            --imp.lineBegin;
        auto e = text.find_first_of("\r\n", at);
        auto lineStop = e == std::string_view::npos ? text.size() : e;
        imp.line = std::string(text.substr(imp.lineBegin, lineStop - imp.lineBegin));
        while (!imp.line.empty() && (imp.line.back() == ' ' || imp.line.back() == '\t'))
            imp.line.pop_back();
        imp.lineEnd = lineStop;
        if (imp.lineEnd < text.size() && text[imp.lineEnd] == '\r')
            ++imp.lineEnd;
        if (imp.lineEnd < text.size() && text[imp.lineEnd] == '\n')
            ++imp.lineEnd;
        out.push_back(std::move(imp));
    }
    return out;
}

const Import *findImport(const std::vector<Import> &list, const std::string &key)
{
    for (const auto &i : list)
        if (i.key == key)
            return &i;
    return nullptr;
}

// The user's import changes in shell.nss, made to the new version's text.
size_t carryImports(const nss::NssDocument &user, const nss::NssDocument &base, nss::NssDocument &next)
{
    auto mine = importsOf(user), theirs = importsOf(base);
    std::set<std::string> onMine, onBase;
    for (const auto &i : mine)
        if (i.enabled && !i.app)
            onMine.insert(i.key);
    for (const auto &i : theirs)
        if (i.enabled && !i.app)
            onBase.insert(i.key);
    size_t carried = 0;
    const std::string eol(next.eol());

    // Turned off (or taken out) by the user: turned off in the new version too.
    for (const auto &key : onBase) {
        if (onMine.count(key))
            continue;
        auto now = importsOf(next);
        if (const auto *i = findImport(now, key); i && i->enabled && next.toggleImport(i->index, false))
            ++carried;
    }
    // Added or turned on by the user: right before the import after it in the user's
    // copy (the app's ones too), else right after the one before it, else at the end.
    for (size_t k = 0; k < mine.size(); ++k) {
        const auto &imp = mine[k];
        if (!imp.enabled || imp.app || onBase.count(imp.key))
            continue;
        auto now = importsOf(next);
        if (const auto *i = findImport(now, imp.key)) {
            if (!i->enabled && next.toggleImport(i->index, true))
                ++carried;
            continue;
        }
        std::optional<size_t> at;
        for (size_t j = k + 1; j < mine.size() && !at; ++j)
            if (const auto *anchor = findImport(now, mine[j].key))
                at = anchor->lineBegin;
        // Nothing but imports after it in the user's copy: imported last, at the end.
        bool last = !at && std::none_of(user.nodes().begin(), user.nodes().end(), [&](const nss::Node &n) {
            return n.kind != nss::NodeKind::Import && n.span.begin >= imp.lineEnd;
        });
        for (size_t j = k; j-- > 0 && !at && !last;)
            if (const auto *anchor = findImport(now, mine[j].key))
                at = anchor->lineEnd;
        std::string line = std::string(nss::trim(imp.line));
        const auto &text = next.text();
        if (at) {
            // At the end of a file without a final line break: one before it.
            std::string before = *at == text.size() && !text.empty() && text.back() != '\n' ? eol : std::string();
            next.replace({ *at, *at }, before + line + eol);
        } else {
            std::string before = !text.empty() && text.back() != '\n' ? eol : std::string();
            next.replace({ text.size(), text.size() }, before + line + eol);
        }
        ++carried;
    }
    return carried;
}

// The overrides that give the new version the user's properties, for the pairs of
// declarations (the user's, the base's).
void carryProps(std::string_view rel, const nss::NssDocument &user, const nss::NssDocument &base, const nss::NssDocument &next,
                const std::vector<std::pair<const nss::Node *, const nss::Node *>> &pairs, Result &r)
{
    auto fail = [&](std::string why) {
        r.complete = false;
        ++r.failed;
        if (r.why.empty())
            r.why = std::move(why);
    };
    for (const auto &[mine, theirs] : pairs) {
        for (auto group : kGroups) {
            auto vu = propOf(user, *mine, group), vb = propOf(base, *theirs, group);
            if (same(vu, vb))
                continue;
            std::string value;
            if (vu.present && !vu.hasValue) {
                fail(std::string(group) + " without a value");
                continue;
            }
            if (!vu.present) {
                if (group != "image") {
                    fail(std::string(group) + "= taken out");
                    continue;
                }
                value = "null"; // the icon taken away
            } else
                value = vu.text;
            if (value.find_first_of("\r\n") != std::string::npos) {
                // The override line is one line; flattening the break would change the string.
                fail(std::string(group) + " over several lines");
                continue;
            }
            auto key = overrides::keyOf(base, rel, *theirs);
            if (!key) {
                fail("an item without a title");
                continue;
            }
            const auto *now = overrides::find(next, *key);
            if (!now || now->menuKind != theirs->menuKind) {
                fail("an item the new version doesn't have: " + key->item);
                continue;
            }
            // The new version has it so already: nothing to keep.
            if (auto vn = propOf(next, *now, group); vn.present && vn.hasValue && overrides::sourceKey(vn.text) == overrides::sourceKey(value))
                continue;
            auto it = std::find_if(r.overrides.begin(), r.overrides.end(), [&](const overrides::Override &o) { return o.key == *key; });
            if (it == r.overrides.end()) {
                r.overrides.push_back(overrides::Override{ *key, {} });
                it = r.overrides.end() - 1;
            }
            it->props.emplace_back(std::string(group), value);
            ++r.carried;
        }
    }
}

} // namespace

std::vector<std::string_view> shippedBases(std::string_view rel)
{
    std::vector<std::string_view> out;
    auto key = slashLower(rel);
    for (const auto &b : kBases)
        if (key == b.rel)
            out.emplace_back(b.text);
    return out;
}

Result carry(std::string_view rel, std::string_view user, std::string_view shipped, const std::vector<std::string_view> &bases,
             bool partial)
{
    Result fail;
    auto decU = nss::decode(user), decN = nss::decode(shipped);
    if (!decU.ok || !decN.ok) {
        fail.why = "not text";
        return fail;
    }
    const bool root = isRoot(rel);
    const std::string u = config_upgrade::normalize(decU.text);
    auto docU = parse(u);
    auto docN = parse(decN.text);
    // shell.nss as written, the app's import lines in it: where the user's own go.
    auto docRaw = root ? parse(decU.text) : std::nullopt;
    if (!docU || !docN) {
        fail.why = docU ? "the new version doesn't parse" : "your copy doesn't parse";
        return fail;
    }

    std::vector<std::string> candidates{ config_upgrade::normalize(decN.text) };
    for (auto b : bases) {
        auto c = config_upgrade::normalize(b);
        if (std::find(candidates.begin(), candidates.end(), c) == candidates.end())
            candidates.push_back(std::move(c));
    }

    // The new version as written for this user: the app's import lines kept (as
    // `shell.exe -config:upgrade` does), then the user's own.
    auto build = [&](const nss::NssDocument &base, const std::vector<std::pair<const nss::Node *, const nss::Node *>> &pairs) {
        Result r;
        r.complete = true;
        r.format = decN.format;
        carryProps(rel, *docU, base, *docN, pairs, r);
        if (root) {
            auto next = nss::NssDocument::fromText(
                config_upgrade::with_app_imports(decN.text, config_upgrade::app_import_lines(decU.text)), decN.format);
            r.carried += carryImports(docRaw ? *docRaw : *docU, base, next);
            r.text = next.text();
        } else
            r.text = decN.text;
        return r;
    };

    if (!partial) {
        const auto mine = skeleton(*docU, root);
        std::optional<Result> best;
        std::string why;
        for (const auto &c : candidates) {
            auto docB = parse(c);
            if (!docB)
                continue;
            if (skeleton(*docB, root) != mine) {
                if (why.empty())
                    why = "changes other than the Menu map's";
                continue;
            }
            auto du = overrides::declarations(*docU), db = overrides::declarations(*docB);
            if (du.size() != db.size())
                continue;
            std::vector<std::pair<const nss::Node *, const nss::Node *>> pairs;
            for (size_t i = 0; i < du.size(); ++i)
                pairs.emplace_back(du[i], db[i]);
            auto r = build(*docB, pairs);
            // The base that explains the copy with the fewest changes is the one it came
            // from, whether they can all be kept or not: another one that can would
            // take the release's own changes for the user's.
            if (!best || r.carried + r.failed < best->carried + best->failed)
                best = std::move(r);
        }
        if (best && best->complete)
            return *best;
        if (best)
            why = best->why;
        fail.why = why.empty() ? std::string("no version it came from") : why;
        return fail;
    }

    // Partial: the closest base, items paired as lines of a diff.
    const auto userLines = diff::splitLines(u);
    size_t bestIndex = 0, bestEdits = SIZE_MAX;
    for (size_t i = 0; i < candidates.size(); ++i) {
        size_t edits = 0;
        for (const auto &e : diff::diffLines(diff::splitLines(candidates[i]), userLines))
            edits += e.op != diff::Op::Same;
        if (edits < bestEdits) {
            bestEdits = edits;
            bestIndex = i;
        }
    }
    auto docB = parse(candidates[bestIndex]);
    if (!docB) {
        fail.why = "the version it came from doesn't parse";
        return fail;
    }
    auto du = overrides::declarations(*docU), db = overrides::declarations(*docB);
    std::vector<std::string> su, sb;
    for (const auto *d : du)
        su.push_back(signature(*docU, *d));
    for (const auto *d : db)
        sb.push_back(signature(*docB, *d));
    std::vector<std::pair<const nss::Node *, const nss::Node *>> pairs;
    for (const auto &e : diff::diffLines(sb, su))
        if (e.op == diff::Op::Same && e.oldLine > 0 && e.newLine > 0)
            pairs.emplace_back(du[size_t(e.newLine - 1)], db[size_t(e.oldLine - 1)]);
    auto r = build(*docB, pairs);
    r.complete = r.complete && skeleton(*docB, root) == skeleton(*docU, root);
    return r;
}

void addOverrides(maprules::Rules &lines, const std::vector<overrides::Override> &list)
{
    for (const auto &o : list)
        for (const auto &[name, value] : o.props)
            overrides::set(lines, o.key, name, value);
}

} // namespace carryover
