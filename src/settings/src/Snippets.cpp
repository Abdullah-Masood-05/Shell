#include "Snippets.h"

#include "MenuWriter.h"
#include "Tr.h"
#include "Updater.h"

#include "nss/ConfigStore.h"
#include "nss/Text.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <set>

// kEmbeddedSnippetFiles: src/bin/snippets/* (cmake/Snippets.cmake).
#include "SnippetsData.inc"

namespace snippets {

namespace {

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::string trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return std::string(s);
}

std::vector<std::string_view> lines(std::string_view text)
{
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        auto end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            if (pos < text.size())
                out.push_back(text.substr(pos));
            break;
        }
        out.push_back(text.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

std::string str(const nlohmann::json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool isHex(std::string_view s, size_t size)
{
    return s.size() == size && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}
bool isHex64(std::string_view s) { return isHex(s, 64); }

// `import 'snippets/<id>.nss'` (either quote or slash, any case) -> id, else "".
std::string importedId(std::string_view line)
{
    auto l = nss::toLower(trim(line));
    std::string_view s(l);
    if (!startsWith(s, "import"))
        return {};
    s.remove_prefix(6);
    if (s.empty() || (s.front() != ' ' && s.front() != '\t'))
        return {};
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    if (s.size() < 2 || (s.front() != '\'' && s.front() != '"') || s.back() != s.front())
        return {};
    auto path = s.substr(1, s.size() - 2);
    if (!startsWith(path, "snippets/") && !startsWith(path, "snippets\\"))
        return {};
    path.remove_prefix(9);
    if (path.size() < 5 || path.substr(path.size() - 4) != ".nss")
        return {};
    std::string id(path.substr(0, path.size() - 4));
    return validId(id) ? id : std::string();
}

struct KnownApp
{
    const char *id;
    const char *name;
    std::vector<const wchar_t *> paths; // %VARIABLES% expanded
    const wchar_t *exe;                 // also found on PATH; nullptr: paths only
};

const std::vector<KnownApp> &knownApps()
{
    static const std::vector<KnownApp> apps = {
        // The snippets look in sys.prog (the 64-bit Program Files): the same place.
        { "7-zip", "7-Zip", { L"%ProgramFiles%\\7-Zip\\7z.exe" }, nullptr },
        // The app execution alias Windows Terminal installs.
        { "windows-terminal", "Windows Terminal", { L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\wt.exe" }, nullptr },
        { "git", "Git", { L"%ProgramFiles%\\Git\\cmd\\git.exe" }, L"git.exe" },
        { "vscode", "Visual Studio Code",
          { L"%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe", L"%ProgramFiles%\\Microsoft VS Code\\Code.exe" },
          L"code.cmd" },
        { "everything", "Everything",
          { L"%ProgramFiles%\\Everything\\Everything.exe", L"%ProgramFiles(x86)%\\Everything\\Everything.exe" }, nullptr },
    };
    return apps;
}

} // namespace

// --- catalog ----------------------------------------------------------------------------

const std::string &Snippet::titleIn(std::string_view lang) const
{
    auto it = translations.find(std::string(lang));
    return it != translations.end() && !it->second.title.empty() ? it->second.title : title;
}

const std::string &Snippet::descriptionIn(std::string_view lang) const
{
    auto it = translations.find(std::string(lang));
    return it != translations.end() && !it->second.description.empty() ? it->second.description : description;
}

const Snippet *Catalog::find(std::string_view id) const
{
    for (const auto &s : snippets)
        if (s.id == id)
            return &s;
    return nullptr;
}

std::string normalize(std::string_view bytes)
{
    if (startsWith(bytes, "\xEF\xBB\xBF"))
        bytes.remove_prefix(3);
    std::string out;
    out.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r') {
            out += '\n';
            if (i + 1 < bytes.size() && bytes[i + 1] == '\n')
                ++i;
        } else {
            out += bytes[i];
        }
    }
    return out;
}

std::string hashText(std::string_view bytes) { return nss::ConfigStore::sha256(normalize(bytes)); }

std::string toCrlf(std::string_view text)
{
    auto n = normalize(text);
    std::string out;
    out.reserve(n.size() + n.size() / 16);
    for (char c : n) {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return out;
}

bool validId(std::string_view id)
{
    if (id.empty() || id.size() > 64 || id.front() == '-' || id.back() == '-')
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

namespace {

// The fields every catalog has. False (and why, in `errors`) for an entry to leave out.
bool readEntry(const nlohmann::json &e, Snippet &s, std::set<std::string> &seen, std::vector<std::string> &errors)
{
    if (!e.is_object()) {
        errors.push_back("catalog: an entry is not an object");
        return false;
    }
    s.id = str(e, "id");
    if (!validId(s.id)) {
        errors.push_back("catalog: invalid id '" + s.id + "'");
        return false;
    }
    if (!seen.insert(s.id).second) {
        errors.push_back(s.id + ": listed twice");
        return false;
    }
    s.title = str(e, "title");
    s.description = str(e, "description");
    s.category = nss::toLower(str(e, "category"));
    s.minVersion = str(e, "min_version");
    s.sha256 = nss::toLower(str(e, "sha256"));
    if (s.title.empty()) {
        errors.push_back(s.id + ": no title");
        return false;
    }
    if (!isHex64(s.sha256)) {
        errors.push_back(s.id + ": no valid sha256");
        return false;
    }
    auto risk = nss::toLower(str(e, "risk"));
    s.risk = risk == "admin" ? Risk::Admin : risk == "network" ? Risk::Network : Risk::None;
    if (auto r = e.find("requires"); r != e.end() && r->is_array())
        for (const auto &x : *r)
            if (x.is_string() && !x.get<std::string>().empty())
                s.needs.push_back(x.get<std::string>());
    if (auto t = e.find("translations"); t != e.end() && t->is_object())
        for (auto it = t->begin(); it != t->end(); ++it)
            if (it->is_object())
                s.translations[it.key()] = Text{ str(*it, "title"), str(*it, "description") };
    return true;
}

std::optional<std::string> embedded(std::string_view name)
{
    for (const auto &f : kEmbeddedSnippetFiles)
        if (name == f.name)
            return std::string(reinterpret_cast<const char *>(f.data), f.size);
    return std::nullopt;
}

} // namespace

Catalog parseCatalog(std::string_view json, const std::function<std::optional<std::string>(const std::string &)> &file,
                     std::string_view origin)
{
    Catalog out;
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object() || !j.contains("snippets") || !j["snippets"].is_array()) {
        out.errors.push_back("catalog.json: not a catalog (no \"snippets\" list)");
        return out;
    }
    const auto author = str(j, "author");
    const auto authorUrl = str(j, "author_url");
    const auto license = str(j, "license");
    const auto repository = str(j, "repository");
    const auto commit = nss::toLower(str(j, "commit"));
    std::set<std::string> seen;
    for (const auto &e : j["snippets"]) {
        Snippet s;
        if (!readEntry(e, s, seen, out.errors))
            continue;
        auto either = [&](const char *key, const std::string &fallback) {
            auto v = str(e, key);
            return v.empty() ? fallback : v;
        };
        s.author = either("author", author);
        s.authorUrl = either("author_url", authorUrl);
        s.license = either("license", license);
        s.source = str(e, "source");
        if (s.source.empty() && !repository.empty() && isHex(commit, 40)) {
            const auto path = str(e, "path");
            if (validRepoPath(path))
                s.source = "https://github.com/" + repository + "/blob/" + commit + "/" + path;
        }
        s.origin = std::string(origin);
        auto bytes = file ? file(s.id) : std::nullopt;
        if (!bytes) {
            out.errors.push_back(s.id + ": " + s.id + ".nss is missing");
            continue;
        }
        if (hashText(*bytes) != s.sha256) {
            out.errors.push_back(s.id + ": " + s.id + ".nss doesn't match its sha256");
            continue;
        }
        s.text = normalize(*bytes);
        out.snippets.push_back(std::move(s));
    }
    return out;
}

bool validRepoPath(std::string_view path)
{
    if (path.size() < 5 || path.size() > 200 || path.substr(path.size() - 4) != ".nss")
        return false;
    size_t begin = 0;
    for (;;) {
        auto end = path.find('/', begin);
        auto part = path.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (char c : part)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-')
                return false;
        if (end == std::string_view::npos)
            return true;
        begin = end + 1;
    }
}

Catalog BundledSource::load() const
{
    auto json = embedded("catalog.json");
    if (!json) {
        Catalog c;
        c.errors.push_back("catalog.json is not embedded");
        return c;
    }
    return parseCatalog(*json, [&](const std::string &id) { return embedded(id + ".nss"); }, id());
}

Catalog RubicSource::load() const
{
    auto json = embedded("rubicbg/catalog.json");
    if (!json) {
        Catalog c;
        c.errors.push_back("rubicbg/catalog.json is not embedded");
        return c;
    }
    return parseCatalog(*json, [&](const std::string &id) { return embedded("rubicbg/" + id + ".nss"); }, id());
}

Catalog loadAll()
{
    Catalog all;
    for (auto c : { BundledSource{}.load(), RubicSource{}.load() }) {
        for (auto &s : c.snippets) {
            if (all.find(s.id)) {
                all.errors.push_back(s.id + ": already taken by another source");
                continue;
            }
            all.snippets.push_back(std::move(s));
        }
        all.errors.insert(all.errors.end(), c.errors.begin(), c.errors.end());
    }
    return all;
}

// --- requirements -----------------------------------------------------------------------

Probe systemProbe()
{
    Probe p;
    p.fileExists = [](const std::wstring &path) {
        DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    p.expand = [](const std::wstring &text) {
        DWORD n = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
        if (n == 0)
            return text;
        std::wstring out(n, L'\0');
        n = ExpandEnvironmentStringsW(text.c_str(), out.data(), n);
        out.resize(n > 0 ? n - 1 : 0);
        return out;
    };
    p.onPath = [](const std::wstring &exe) {
        wchar_t buf[MAX_PATH];
        if (SearchPathW(nullptr, exe.c_str(), nullptr, MAX_PATH, buf, nullptr) > 0)
            return true;
        const std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + exe;
        for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
            HKEY h = nullptr;
            if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
                RegCloseKey(h);
                return true;
            }
        }
        return false;
    };
    return p;
}

Requirement checkRequirement(std::string_view requirement, const Probe &probe)
{
    Requirement r;
    const auto key = nss::toLower(trim(requirement));
    for (const auto &app : knownApps()) {
        if (key != app.id)
            continue;
        r.name = app.name;
        for (const auto *p : app.paths)
            if (probe.fileExists && probe.fileExists(probe.expand ? probe.expand(p) : std::wstring(p)))
                r.met = true;
        if (!r.met && app.exe && probe.onPath)
            r.met = probe.onPath(app.exe);
        return r;
    }
    const auto wide = nss::widen(trim(requirement));
    const bool isPath = key.find_first_of("\\/") != std::string::npos;
    // The file name without its extension names it: "C:\Tools\foo.exe" -> "foo".
    auto name = trim(requirement);
    if (auto slash = name.find_last_of("\\/"); slash != std::string::npos)
        name = name.substr(slash + 1);
    if (auto dot = name.find_last_of('.'); dot != std::string::npos && dot > 0)
        name = name.substr(0, dot);
    r.name = name.empty() ? std::string(requirement) : name;
    if (isPath) {
        r.met = probe.fileExists && probe.fileExists(probe.expand ? probe.expand(wide) : wide);
    } else {
        auto exe = wide;
        if (exe.find(L'.') == std::wstring::npos)
            exe += L".exe";
        r.met = probe.onPath && probe.onPath(exe);
    }
    return r;
}

bool versionMet(std::string_view minVersion, std::string_view running)
{
    auto min = updater::Version::parse(minVersion);
    auto cur = updater::Version::parse(running);
    if (!min || !cur)
        return true;
    // The core's number decides: a 2.0.6 beta has the 2.0.6 features.
    min->pre.clear();
    cur->pre.clear();
    return updater::compare(*cur, *min) >= 0;
}

// --- the files in the config folder -----------------------------------------------------

std::vector<Entry> parseList(std::string_view text)
{
    std::vector<Entry> out;
    std::map<std::string, std::string> hashes;
    const auto normalized = normalize(text); // lines() points into it
    for (auto line : lines(normalized)) {
        auto t = trim(line);
        if (startsWith(t, "// snippet ")) {
            // "// snippet <id> sha256=<hex>"
            auto rest = std::string_view(t).substr(11);
            auto space = rest.find(' ');
            auto id = std::string(rest.substr(0, space));
            if (space != std::string_view::npos) {
                auto h = trim(rest.substr(space + 1));
                if (startsWith(h, "sha256=") && isHex64(h.substr(7)))
                    hashes[id] = nss::toLower(h.substr(7));
            }
            continue;
        }
        auto id = importedId(t);
        if (id.empty() || std::any_of(out.begin(), out.end(), [&](const Entry &e) { return e.id == id; }))
            continue;
        auto h = hashes.find(id);
        out.push_back(Entry{ id, h != hashes.end() ? h->second : std::string() });
    }
    return out;
}

std::string generateList(const std::vector<Entry> &entries)
{
    std::string out;
    out += kListHeader;
    out += "\r\n";
    out += "// The snippets turned on on the Snippets page of Nilesoft Shell Pro. Each one is a copy\r\n";
    out += "// in imports\\snippets; the hash is that copy as the app wrote it, so a copy you edit\r\n";
    out += "// is never replaced by a newer version.\r\n";
    for (const auto &e : entries) {
        out += "\r\n// snippet " + e.id + " sha256=" + e.sha256 + "\r\n";
        out += "import 'snippets/" + e.id + ".nss'\r\n";
    }
    return out;
}

std::string updateList(const std::optional<std::string> &existing, const std::vector<Entry> &entries)
{
    if (!existing)
        return generateList(entries);
    auto find = [&](std::string_view id) {
        return std::find_if(entries.begin(), entries.end(), [&](const Entry &e) { return e.id == id; });
    };
    // "// snippet <id> sha256=<hex>" -> id, else "".
    auto hashLineId = [](std::string_view t) {
        if (!startsWith(t, "// snippet "))
            return std::string();
        auto rest = t.substr(11);
        return nss::toLower(rest.substr(0, rest.find(' ')));
    };
    auto hashLine = [](const Entry &e) { return "// snippet " + e.id + " sha256=" + e.sha256; };

    const auto normalized = normalize(*existing);
    std::vector<std::string> out;
    std::set<std::string> listed, hashed;
    bool first = true, removed = false;
    for (auto line : lines(normalized)) {
        auto t = trim(line);
        if (first && nss::OverridesWriter::isHeader(t)) {
            out.emplace_back(kListHeader);
            first = false;
            continue;
        }
        first = false;
        if (auto id = hashLineId(t); !id.empty()) {
            auto e = find(id);
            if (e == entries.end()) {
                removed = true; // the snippet went off
                continue;
            }
            hashed.insert(id);
            out.push_back(t == hashLine(*e) ? std::string(line) : hashLine(*e));
            continue;
        }
        if (auto id = importedId(t); !id.empty()) {
            auto e = find(id);
            if (e == entries.end()) {
                removed = true;
                continue;
            }
            if (!hashed.count(id) && !listed.count(id))
                out.push_back(hashLine(*e));
            hashed.insert(id);
            listed.insert(id);
        }
        out.emplace_back(line);
    }
    // Blank lines a removed snippet left behind: no more than one in a row, none at the end.
    std::vector<std::string> tidy;
    for (auto &l : out) {
        bool blank = trim(l).empty();
        if (removed && blank && (tidy.empty() || trim(tidy.back()).empty()))
            continue;
        tidy.push_back(std::move(l));
    }
    while (!tidy.empty() && trim(tidy.back()).empty())
        tidy.pop_back();

    std::string text;
    for (const auto &l : tidy)
        text += l + "\r\n";
    for (const auto &e : entries) {
        if (listed.count(e.id))
            continue;
        text += "\r\n" + hashLine(e) + "\r\n";
        text += "import 'snippets/" + e.id + ".nss'\r\n";
    }
    return text;
}

std::string copyPath(std::string_view id) { return std::string(kCopyDir) + "/" + std::string(id) + ".nss"; }

CopyState copyState(const Snippet &snippet, const ConfigFiles &files)
{
    if (!files.list)
        return CopyState::Off;
    auto entries = parseList(*files.list);
    auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry &e) { return e.id == snippet.id; });
    if (it == entries.end())
        return CopyState::Off;
    auto copy = files.copies.find(snippet.id);
    if (copy == files.copies.end() || !copy->second)
        return CopyState::Missing;
    auto hash = hashText(*copy->second);
    if (hash == snippet.sha256)
        return CopyState::Current;
    // Unedited: still what the app wrote. Without a recorded hash nothing says so: keep it.
    if (!it->sha256.empty() && hash == it->sha256)
        return CopyState::Outdated;
    return CopyState::Edited;
}

std::string listProblem(const ConfigFiles &files)
{
    if (!files.list)
        return {};
    auto doc = nss::NssDocument::fromText(normalize(*files.list));
    return menus::handEditProblem(doc, "settings-app-snippets.nss");
}

Plan planSet(const Catalog &catalog, const ConfigFiles &files, std::string_view id, bool on)
{
    Plan plan;
    plan.error = listProblem(files);
    if (!plan.error.empty())
        return plan;
    auto entries = files.list ? parseList(*files.list) : std::vector<Entry>{};
    auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry &e) { return e.id == id; });
    if (on) {
        const auto *s = catalog.find(id);
        if (!s) {
            plan.error = nsp::trf("There is no snippet called {}.", std::string(id));
            return plan;
        }
        const bool listed = it != entries.end();
        // A copy already there that the user edited (from an earlier time on) is kept.
        auto copy = files.copies.find(s->id);
        const bool keepCopy = listed && copy != files.copies.end() && copy->second &&
                              copyState(*s, files) == CopyState::Edited;
        if (!keepCopy)
            plan.writes.push_back(Write{ copyPath(s->id), toCrlf(s->text) });
        if (!listed)
            entries.push_back(Entry{ s->id, s->sha256 });
        else if (!keepCopy)
            it->sha256 = s->sha256;
        plan.writes.push_back(Write{ std::string(kListPath), updateList(files.list, entries) });
        plan.needsImport = true;
        plan.changes.push_back("turn on " + s->id);
        return plan;
    }
    if (it == entries.end()) {
        // Not on: only a stray copy to clean up.
        auto copy = files.copies.find(std::string(id));
        if (copy != files.copies.end() && copy->second)
            plan.removes.push_back(copyPath(id));
        return plan;
    }
    entries.erase(it);
    plan.writes.push_back(Write{ std::string(kListPath), updateList(files.list, entries) });
    plan.removes.push_back(copyPath(id));
    plan.changes.push_back("turn off " + std::string(id));
    return plan;
}

Plan planRefresh(const Catalog &catalog, const ConfigFiles &files)
{
    Plan plan;
    if (!files.list || !listProblem(files).empty())
        return plan; // a list that doesn't parse waits until the user fixes it
    auto entries = parseList(*files.list);
    bool listChanged = false;
    for (auto &e : entries) {
        const auto *s = catalog.find(e.id);
        if (!s)
            continue;
        auto state = copyState(*s, files);
        if (state == CopyState::Outdated || state == CopyState::Missing) {
            plan.writes.push_back(Write{ copyPath(s->id), toCrlf(s->text) });
            plan.changes.push_back((state == CopyState::Missing ? "restore " : "update ") + s->id);
        }
        if ((state == CopyState::Outdated || state == CopyState::Missing || state == CopyState::Current) &&
            e.sha256 != s->sha256) {
            e.sha256 = s->sha256;
            listChanged = true;
        }
    }
    if (listChanged || !plan.writes.empty())
        plan.writes.push_back(Write{ std::string(kListPath), updateList(files.list, entries) });
    return plan;
}

bool ensureImported(nss::NssDocument &root)
{
    int index = menus::MenuWriter::findImport(root, kListPath);
    if (index < 0)
        return root.appendImport(kListPath);
    auto list = root.imports();
    if (size_t(index) < list.size() && !list[size_t(index)]->enabled)
        return root.toggleImport(size_t(index), true);
    return false;
}

// --- categories -------------------------------------------------------------------------

const std::vector<std::string> &knownCategories()
{
    static const std::vector<std::string> list = { "files", "folders", "archives", "terminal", "security", "system", "tools" };
    return list;
}

const char *categoryLabel(std::string_view category)
{
    if (category == "files")
        return nsp::mark("Files");
    if (category == "folders")
        return nsp::mark("Folders");
    if (category == "archives")
        return nsp::mark("Archives");
    if (category == "terminal")
        return nsp::mark("Terminal");
    if (category == "security")
        return nsp::mark("Security");
    if (category == "system")
        return nsp::mark("Windows");
    if (category == "tools")
        return nsp::mark("Apps");
    return nullptr;
}

} // namespace snippets
