#include "PendingChanges.h"

#include "MenuWriter.h"
#include "Tr.h"

#include "nss/Literal.h"
#include "nss/NssDocument.h"
#include "nss/Text.h"
#include "nss/ThemeSchema.h"

#include <algorithm>
#include <set>

namespace fs = std::filesystem;

std::vector<std::string> PendingChanges::set(std::string_view key, std::string_view literal, std::string_view page)
{
    auto canonical = nss::ThemeSchema::canonical(key);
    auto lit = nss::Literal::parse(literal);
    const auto *info = nss::ThemeSchema::find(canonical);
    std::vector<std::string> errors;
    if (!info)
        errors.push_back(nsp::trf("{}: unknown key", canonical));
    else if (lit.text.empty())
        errors.push_back(nsp::trf("{}: a value is needed", canonical));
    else if (lit.text.find_first_of("\r\n") != std::string::npos)
        errors.push_back(nsp::trf("{}: the value must fit on one line", canonical));
    else if (!lit.isLiteral() && info->type != nss::ValueType::Any)
        errors.push_back(nsp::trf("{}: only plain values can be set here", canonical));
    else
        errors = nss::ThemeSchema::validate(canonical, lit);
    if (!errors.empty())
        return errors;
    changes_[canonical] = Change{ lit.text, std::string(page) };
    return {};
}

void PendingChanges::reset(std::string_view key, std::string_view page)
{
    changes_[nss::ThemeSchema::canonical(key)] = Change{ std::nullopt, std::string(page) };
}

void PendingChanges::forget(std::string_view key) { changes_.erase(nss::ThemeSchema::canonical(key)); }

const PendingChanges::Change *PendingChanges::find(std::string_view key) const
{
    auto it = changes_.find(nss::ThemeSchema::canonical(key));
    return it == changes_.end() ? nullptr : &it->second;
}

nss::Overrides PendingChanges::apply(const nss::Overrides &existing) const
{
    nss::Overrides out;
    for (const auto &[key, value] : existing)
        out[nss::ThemeSchema::canonical(key)] = value;
    for (const auto &[key, change] : changes_) {
        if (change.literal)
            out[key] = *change.literal;
        else
            out.erase(key);
    }
    return out;
}

using maprules::uncommented;

void PendingChanges::addRule(std::string_view text, std::string_view label, std::string_view page)
{
    std::string key(nss::trim(text));
    auto it = rules_.find(key);
    if (it != rules_.end() && !it->second.add) {
        rules_.erase(it);
        return;
    }
    rules_[key] = RuleChange{ true, std::string(label), std::string(page) };
}

void PendingChanges::removeRule(std::string_view text, std::string_view label, std::string_view page)
{
    std::string key(uncommented(text));
    auto it = rules_.find(key);
    if (it != rules_.end() && it->second.add) {
        rules_.erase(it);
        return;
    }
    rules_[key] = RuleChange{ false, std::string(label), std::string(page) };
}

const PendingChanges::RuleChange *PendingChanges::findRule(std::string_view text) const
{
    auto it = rules_.find(std::string(uncommented(text)));
    return it == rules_.end() ? nullptr : &it->second;
}

maprules::Rules PendingChanges::applyRules(const maprules::Rules &existing) const
{
    maprules::Rules out;
    for (const auto &line : existing) {
        auto it = rules_.find(std::string(uncommented(line)));
        if (it == rules_.end())
            out.push_back(line);
        else if (it->second.add)
            out.push_back(it->first); // turned off: on again, in place
        // removed: dropped, on or off
    }
    for (const auto &[text, change] : rules_) {
        if (!change.add)
            continue;
        if (std::find(out.begin(), out.end(), text) == out.end())
            out.push_back(text);
    }
    return out;
}

std::string PendingChanges::describe() const
{
    std::map<std::string, std::vector<std::string>> byPage;
    for (const auto &[text, change] : rules_)
        byPage[change.page.empty() ? std::string("Menu map") : change.page].push_back(
            change.label.empty() ? std::string(change.add ? "add a rule" : "remove a rule") : change.label);
    for (const auto &[key, change] : changes_) {
        std::string name = key;
        if (name.rfind("theme.", 0) == 0)
            name = name.substr(6);
        else if (name.rfind("settings.", 0) == 0)
            name = name.substr(9);
        if (!change.literal)
            name += " (reset)";
        byPage[change.page.empty() ? std::string("Settings") : change.page].push_back(name);
    }
    std::string out;
    for (const auto &[page, keys] : byPage) {
        if (!out.empty())
            out += "; ";
        out += page + ": ";
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i == 4) {
                out += " and " + std::to_string(keys.size() - 4) + " more";
                break;
            }
            out += (i ? ", " : "") + keys[i];
        }
    }
    return out;
}

namespace {

bool sameLiteral(const nss::Literal &a, const nss::Literal &b)
{
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
    case nss::LiteralKind::Number: return a.number == b.number;
    case nss::LiteralKind::String:
    case nss::LiteralKind::Keyword:
    case nss::LiteralKind::Name: return a.string == b.string;
    case nss::LiteralKind::Color: return a.rgba == b.rgba;
    case nss::LiteralKind::Array:
        if (a.items.size() != b.items.size())
            return false;
        for (size_t i = 0; i < a.items.size(); ++i)
            if (!sameLiteral(a.items[i], b.items[i]))
                return false;
        return true;
    default: return a.text == b.text;
    }
}

} // namespace

bool sameValue(std::string_view a, std::string_view b)
{
    return sameLiteral(nss::Literal::parse(a), nss::Literal::parse(b));
}

std::vector<std::string> PendingChanges::stage(std::string_view key, std::string_view literal, std::string_view page,
                                               const std::optional<std::string> &saved)
{
    if (saved && sameValue(*saved, literal)) {
        forget(key);
        return {};
    }
    return set(key, literal, page);
}

SavePlan planSave(const fs::path &root, const PendingChanges &pending)
{
    SavePlan plan;
    std::string error;
    auto rootDoc = nss::NssDocument::load(root, {}, &error);
    if (!rootDoc) {
        plan.error = nsp::trf("Couldn't read {}: {}", nss::narrow(root.wstring()), error);
        return plan;
    }
    plan.overridesPath = (root.parent_path() / fs::path(nss::widen(nss::OverridesWriter::relativePath))).lexically_normal();
    plan.mapPath = (root.parent_path() / fs::path(nss::widen(maprules::relativePath))).lexically_normal();

    // The file as it is now. One the user edited by hand so that it doesn't
    // parse is left alone: the save stops with a message saying so.
    auto loadExisting = [&](const fs::path &path, std::optional<nss::NssDocument> &out) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            return true;
        out = nss::NssDocument::load(path, nss::ParseContext{}, &error);
        if (!out) {
            plan.error = nsp::trf("Couldn't read {}: {}", nss::narrow(path.wstring()), error);
            return false;
        }
        plan.error = menus::handEditProblem(*out, nss::narrow(path.filename().wstring()));
        return plan.error.empty();
    };

    // Theme and settings values: imports/settings-app.nss, imported last. Only
    // written when one of them changed (the Menu map's rules alone don't need it).
    // Values edited by hand are read back (`before`), and only the changed ones
    // are replaced: the rest of the file stays as the user wrote it.
    if (!pending.items().empty() || pending.rules().empty()) {
        std::optional<nss::NssDocument> existing;
        if (!loadExisting(plan.overridesPath, existing))
            return plan;
        if (existing)
            plan.before = nss::OverridesWriter::read(*existing);
        plan.after = pending.apply(plan.before);

        auto doc = existing ? nss::OverridesWriter::merge(*existing, plan.after)
                            : nss::OverridesWriter::generateDocument(plan.after);
        if (!doc.diagnostics().empty()) {
            plan.error = nsp::trf("The generated settings-app.nss doesn't parse: {}", doc.diagnostics().front().message);
            return plan;
        }
        if (!existing || existing->text() != doc.text())
            plan.writes.push_back(nss::FileWrite{ plan.overridesPath, doc.text(), doc.format() });
        plan.rootChanged = nss::OverridesWriter::ensureImportedLast(*rootDoc);
    }

    // The Menu map's rules: imports/settings-app-map.nss, imported before the
    // other config files (see MapRules.h).
    if (!pending.rules().empty()) {
        std::optional<nss::NssDocument> existing;
        if (!loadExisting(plan.mapPath, existing))
            return plan;
        if (existing)
            plan.rulesBefore = maprules::read(*existing);
        plan.rulesAfter = pending.applyRules(plan.rulesBefore);
        auto format = existing ? existing->format() : nss::defaultFormat();
        auto doc = nss::NssDocument::fromText(maprules::generate(plan.rulesAfter, format.eol), format);
        if (!doc.diagnostics().empty()) {
            plan.error = nsp::trf("The generated settings-app-map.nss doesn't parse: {}", doc.diagnostics().front().message);
            return plan;
        }
        if (!existing || existing->text() != doc.text())
            plan.writes.push_back(nss::FileWrite{ plan.mapPath, doc.text(), doc.format() });
        plan.rootChanged |= maprules::ensureImported(*rootDoc);
    }

    if (plan.rootChanged)
        plan.writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
    plan.ok = true;
    return plan;
}
