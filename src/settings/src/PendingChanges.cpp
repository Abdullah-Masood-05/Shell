#include "PendingChanges.h"

#include "nss/NssDocument.h"
#include "nss/ThemeSchema.h"

#include <set>

namespace fs = std::filesystem;

std::vector<std::string> PendingChanges::set(std::string_view key, std::string_view literal, std::string_view page)
{
    auto canonical = nss::ThemeSchema::canonical(key);
    auto lit = nss::Literal::parse(literal);
    const auto *info = nss::ThemeSchema::find(canonical);
    std::vector<std::string> errors;
    if (!info)
        errors.push_back(canonical + ": unknown key");
    else if (lit.text.empty())
        errors.push_back(canonical + ": a value is needed");
    else if (lit.text.find_first_of("\r\n") != std::string::npos)
        errors.push_back(canonical + ": the value must fit on one line");
    else if (!lit.isLiteral() && info->type != nss::ValueType::Any)
        errors.push_back(canonical + ": only plain values can be set here");
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

std::string PendingChanges::describe() const
{
    std::map<std::string, std::vector<std::string>> byPage;
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

SavePlan planSave(const fs::path &root, const PendingChanges &pending)
{
    SavePlan plan;
    std::string error;
    auto rootDoc = nss::NssDocument::load(root, {}, &error);
    if (!rootDoc) {
        plan.error = "Couldn't read " + nss::narrow(root.wstring()) + ": " + error;
        return plan;
    }
    plan.overridesPath = (root.parent_path() / fs::path(nss::widen(nss::OverridesWriter::relativePath))).lexically_normal();

    std::optional<nss::NssDocument> existing;
    std::error_code ec;
    if (fs::is_regular_file(plan.overridesPath, ec)) {
        nss::ParseContext ctx;
        existing = nss::NssDocument::load(plan.overridesPath, ctx, &error);
        if (!existing) {
            plan.error = "Couldn't read " + nss::narrow(plan.overridesPath.wstring()) + ": " + error;
            return plan;
        }
        plan.before = nss::OverridesWriter::read(*existing);
    }
    plan.after = pending.apply(plan.before);

    auto doc = nss::OverridesWriter::generateDocument(plan.after, existing ? &*existing : nullptr);
    if (!doc.diagnostics().empty()) {
        plan.error = "The generated settings-app.nss doesn't parse: " + doc.diagnostics().front().message;
        return plan;
    }
    if (!existing || existing->text() != doc.text())
        plan.writes.push_back(nss::FileWrite{ plan.overridesPath, doc.text(), doc.format() });

    plan.rootChanged = nss::OverridesWriter::ensureImportedLast(*rootDoc);
    if (plan.rootChanged)
        plan.writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
    plan.ok = true;
    return plan;
}
