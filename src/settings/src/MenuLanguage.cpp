#include "MenuLanguage.h"

#include "MenuWriter.h"
#include "Text.h"
#include "Tr.h"

#include "nss/ConfigStore.h"
#include "nss/NssDocument.h"
#include "nss/OverridesWriter.h"

#include <algorithm>

namespace fs = std::filesystem;

namespace menulang {

namespace {

std::string lower(std::string_view s) { return nss::toLower(s); }

} // namespace

std::vector<std::string> menuFiles(const fs::path &langDir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(langDir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto &p = it->path();
        if (lower(text::narrow(p.extension().wstring())) == ".nss" && it->is_regular_file(ec))
            out.push_back(text::narrow(p.stem().wstring()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string pick(std::string_view setting, const std::vector<std::string> &files)
{
    if (setting.empty() || lower(setting) == "auto")
        return {};
    const auto want = lower(setting);
    for (const auto &f : files)
        if (lower(f) == want)
            return f;
    // The base language: "es" gets es-ES, "ar" gets ar.
    const auto base = want.substr(0, want.find('-'));
    for (const auto &f : files)
        if (lower(f) == base)
            return f;
    for (const auto &f : files) // sorted, so the same file every time
        if (lower(f).rfind(base + "-", 0) == 0)
            return f;
    return {};
}

std::string generate(std::string_view file, nss::Eol eol)
{
    const std::string nl(nss::eolString(eol));
    std::string out(nss::OverridesWriter::header);
    out += nl;
    out += "// The language of the right-click menu, from Preferences > Language." + nl;
    out += "// Choose Auto there and the app removes this file." + nl;
    if (lower(file) != "en") // English first: a string the language lacks shows in English
        out += "import lang " + nss::quoteImportPath("lang/en.nss") + nl;
    out += "import lang " + nss::quoteImportPath("lang/" + std::string(file) + ".nss") + nl;
    return out;
}

std::string read(std::string_view text)
{
    std::string found;
    size_t at = 0;
    while (at < text.size()) {
        auto end = text.find_first_of("\r\n", at);
        auto line = nss::trim(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? text.size() : end + 1;
        if (line.rfind("import", 0) != 0)
            continue;
        line = nss::trim(line.substr(6));
        if (line.rfind("lang", 0) != 0)
            continue;
        line = nss::trim(line.substr(4));
        if (line.size() < 2 || (line[0] != '\'' && line[0] != '"'))
            continue;
        auto close = line.find(line[0], 1);
        if (close == std::string_view::npos)
            continue;
        std::string path(line.substr(1, close - 1));
        std::replace(path.begin(), path.end(), '\\', '/');
        auto name = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
        if (lower(name).size() > 4 && lower(name).ends_with(".nss"))
            name.resize(name.size() - 4);
        found = name;
    }
    return found;
}

std::string englishName(std::string_view file)
{
    static const struct
    {
        const char *file, *name;
    } names[] = {
        { "ar", "Arabic" },        { "de-DE", "German" },       { "en", "English" },
        { "es-ES", "Spanish" },    { "it", "Italian" },         { "ja", "Japanese" },
        { "ko", "Korean" },        { "nl", "Dutch" },           { "no", "Norwegian" },
        { "pt-BR", "Portuguese" }, { "ro", "Romanian" },        { "ru", "Russian" },
        { "sl", "Slovenian" },     { "tr", "Turkish" },         { "uk", "Ukrainian" },
        { "zh-CN", "Simplified Chinese" }, { "zh-TW", "Traditional Chinese" },
    };
    for (const auto &n : names)
        if (lower(n.file) == lower(file))
            return n.name;
    return std::string(file);
}

Result apply(const fs::path &root, std::string_view setting)
{
    Result r;
    const fs::path dir = root.parent_path();
    const fs::path file = dir / fs::path(text::widen(std::string(kRelativePath))).make_preferred();
    const std::string want = pick(setting, menuFiles(dir / L"imports" / L"lang"));

    std::string error;
    auto rootDoc = nss::NssDocument::load(root, {}, &error);
    if (!rootDoc) {
        r.ok = false;
        r.error = nsp::trf("Couldn't read {}: {}", text::narrow(root.wstring()), error);
        return r;
    }
    const int index = menus::MenuWriter::findImport(*rootDoc, kRelativePath);
    std::error_code ec;
    const bool fileExists = fs::is_regular_file(file, ec);

    nss::ConfigStore store(root);
    nss::SaveOptions options;
    options.reason = want.empty() ? "Language: menu follows Windows" : "Language: menu in " + englishName(want);

    if (want.empty()) {
        if (index < 0 && !fileExists)
            return r; // nothing there, nothing to take away
        std::vector<nss::FileWrite> writes;
        if (index >= 0) {
            if (!rootDoc->removeImport(size_t(index))) {
                const auto *n = rootDoc->imports()[size_t(index)];
                rootDoc->replace(n->span, ""); // sharing its line with something: only the import goes
            }
            writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
        }
        if (!writes.empty()) {
            auto saved = store.save(std::move(writes), options);
            if (!saved.ok) {
                r.ok = false;
                r.error = saved.error;
                return r;
            }
            r.snapshotId = saved.snapshotId;
        } else {
            auto snap = store.snapshot(options.reason, &error);
            if (!snap) {
                r.ok = false;
                r.error = error;
                return r;
            }
            r.snapshotId = snap->id;
        }
        // The import is gone, so nothing reads the file any more.
        fs::remove(file, ec);
        r.changed = true;
        return r;
    }

    if (index >= 0 && fileExists) {
        // Already that language: a file the user edited by hand stays as it is.
        std::string existing;
        if (auto doc = nss::NssDocument::load(file, {}, &error))
            existing = read(doc->text());
        if (lower(existing) == lower(want))
            return r;
    }
    std::vector<nss::FileWrite> writes;
    writes.push_back(nss::FileWrite{ file, generate(want, rootDoc->format().eol), rootDoc->format() });
    if (menus::MenuWriter::ensureImported(*rootDoc, kRelativePath))
        writes.push_back(nss::FileWrite{ root, rootDoc->text(), rootDoc->format() });
    auto saved = store.save(std::move(writes), options);
    if (!saved.ok) {
        r.ok = false;
        r.error = saved.error;
        return r;
    }
    r.snapshotId = saved.snapshotId;
    r.changed = true;
    return r;
}

} // namespace menulang
