// po2inc: embeds the translation catalogs into the app for the C++ side (src/Tr.cpp), and
// prepares the folder Slint bundles its translations from.
//
//   po2inc <translations dir> <domain> <output .inc> <slint translations dir>
//
// Reads <dir>/<lang>/LC_MESSAGES/<domain>.po for every <lang> folder (sorted by name) and
// the template <dir>/<domain>.pot, and writes:
//
//   - <output .inc>: one table per language (the msgctxt/msgid key, msgstr, and the plural
//     forms), the language's Plural-Forms and how many strings it translates, and the
//     template's keys. Untranslated and fuzzy entries are left out, so they fall back to
//     English like Slint does. The strings stay in their .po escaped form, which is also
//     valid C++ (\n \t \" \\).
//   - <slint translations dir>: a copy of every language's .po plus one more language,
//     zz-keys, whose "translation" of each string is the string's key: U+E000, then
//     msgctxt and U+E004 when there is one, then msgid. Slint bundles it like the others,
//     and src/TrPreviewHook.h reads it to find a string's context in the translator
//     preview (the bundled lookup only passes the English text). The app never selects it.
//
// Outputs are only rewritten when they change.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char *kKeysLanguage = "zz-keys";

struct Entry
{
    std::string ctx, id, plural;
    std::vector<std::string> str; // msgstr, or msgstr[0..n]
    bool hasCtx = false, fuzzy = false;

    std::string key() const { return hasCtx ? ctx + "\\x04" + id : id; } // escaped, like the .po
    bool translated() const { return !fuzzy && !str.empty() && !str[0].empty(); }
};

struct Catalog
{
    std::string code, name, pluralForms = "nplurals=2; plural=(n != 1);";
    std::vector<Entry> entries; // translated entries only
};

// The text between the first and last quote of a line ("..." in .po syntax), escapes kept.
bool quoted(const std::string &line, std::string &out)
{
    auto a = line.find('"');
    auto b = line.rfind('"');
    if (a == std::string::npos || b <= a)
        return false;
    out = line.substr(a + 1, b - a - 1);
    return true;
}

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeIfChanged(const fs::path &p, const std::string &text)
{
    if (fs::exists(p) && readFile(p) == text)
        return true;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out << text;
    return bool(out);
}

// Every entry of a .po or .pot (obsolete ones skipped), and the header's msgstr.
bool parse(const fs::path &file, std::vector<Entry> &entries, std::string &header, std::string &error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + file.string();
        return false;
    }
    Entry cur;
    bool any = false, pendingFuzzy = false;
    std::string *target = nullptr;
    auto flush = [&] {
        if (any)
            entries.push_back(cur);
        cur = Entry{};
        any = false;
        target = nullptr;
    };
    std::string line;
    int lineNo = 0;
    bool first = true;
    while (std::getline(in, line)) {
        ++lineNo;
        if (first) {
            first = false;
            if (line.rfind("\xEF\xBB\xBF", 0) == 0)
                line.erase(0, 3);
        }
        line = trim(line);
        if (line.empty()) {
            flush();
            continue;
        }
        if (line[0] == '#') {
            if (line.rfind("#~", 0) == 0)
                continue; // obsolete
            if (line.rfind("#,", 0) == 0 && line.find("fuzzy") != std::string::npos)
                pendingFuzzy = true;
            continue;
        }
        std::string value;
        if (line[0] == '"') {
            if (!target || !quoted(line, value)) {
                error = file.string() + ":" + std::to_string(lineNo) + ": unexpected string";
                return false;
            }
            *target += value;
            continue;
        }
        auto startEntry = [&] {
            // A keyword that begins a new entry after a msgstr closes the previous one.
            if (any && !cur.str.empty())
                flush();
            if (!any) {
                cur.fuzzy = pendingFuzzy;
                pendingFuzzy = false;
            }
            any = true;
        };
        if (!quoted(line, value)) {
            error = file.string() + ":" + std::to_string(lineNo) + ": expected a quoted string";
            return false;
        }
        if (line.rfind("msgctxt", 0) == 0) {
            startEntry();
            cur.hasCtx = true;
            cur.ctx = value;
            target = &cur.ctx;
        } else if (line.rfind("msgid_plural", 0) == 0) {
            cur.plural = value;
            target = &cur.plural;
        } else if (line.rfind("msgid", 0) == 0) {
            if (!cur.hasCtx || !cur.str.empty())
                startEntry();
            cur.id = value;
            target = &cur.id;
        } else if (line.rfind("msgstr", 0) == 0) {
            size_t index = 0;
            if (line.size() > 6 && line[6] == '[')
                index = size_t(std::stoul(line.substr(7)));
            if (cur.str.size() <= index)
                cur.str.resize(index + 1);
            cur.str[index] = value;
            target = &cur.str[index];
        } else {
            error = file.string() + ":" + std::to_string(lineNo) + ": unknown keyword";
            return false;
        }
    }
    flush();

    auto it = std::find_if(entries.begin(), entries.end(), [](const Entry &e) { return e.id.empty() && !e.hasCtx; });
    if (it != entries.end()) {
        header = it->str.empty() ? std::string{} : it->str[0];
        entries.erase(it);
    }
    return true;
}

bool parseLanguage(const fs::path &file, Catalog &cat, std::string &error)
{
    std::vector<Entry> all;
    std::string header;
    if (!parse(file, all, header, error))
        return false;
    // The header: "Name: value\n" lines.
    size_t pos = 0;
    while (pos < header.size()) {
        size_t end = header.find("\\n", pos);
        std::string field = header.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? header.size() : end + 2;
        auto colon = field.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = trim(field.substr(0, colon)), val = trim(field.substr(colon + 1));
        if (key == "X-Language-Name")
            cat.name = val;
        else if (key == "Plural-Forms" && !val.empty())
            cat.pluralForms = val;
    }
    for (auto &e : all)
        if (e.translated())
            cat.entries.push_back(std::move(e));
    if (cat.name.empty())
        cat.name = cat.code;
    return true;
}

std::string quote(const std::string &escaped) { return "\"" + escaped + "\""; }

} // namespace

int main(int argc, char **argv)
{
    if (argc != 5) {
        std::cerr << "usage: po2inc <translations dir> <domain> <output .inc> <slint translations dir>\n";
        return 2;
    }
    const fs::path dir = fs::path(reinterpret_cast<const char8_t *>(argv[1]));
    const std::string domain = argv[2];
    const fs::path outPath = fs::path(reinterpret_cast<const char8_t *>(argv[3]));
    const fs::path slintDir = fs::path(reinterpret_cast<const char8_t *>(argv[4]));

    std::vector<fs::path> langs;
    std::error_code ec;
    for (auto &d : fs::directory_iterator(dir, ec))
        if (d.is_directory() && fs::is_regular_file(d.path() / "LC_MESSAGES" / (domain + ".po")))
            langs.push_back(d.path());
    std::sort(langs.begin(), langs.end());

    // The template: every string the app has.
    std::vector<Entry> pot;
    {
        std::string header, error;
        const fs::path potPath = dir / (domain + ".pot");
        if (fs::exists(potPath) && !parse(potPath, pot, header, error)) {
            std::cerr << "po2inc: " << error << "\n";
            return 1;
        }
    }
    std::set<std::string> potKeys;
    for (const auto &e : pot)
        potKeys.insert(e.key());

    std::vector<Catalog> cats;
    for (auto &l : langs) {
        Catalog cat;
        cat.code = l.filename().string();
        std::string error;
        if (!parseLanguage(l / "LC_MESSAGES" / (domain + ".po"), cat, error)) {
            std::cerr << "po2inc: " << error << "\n";
            return 1;
        }
        cats.push_back(std::move(cat));
    }

    std::ostringstream o;
    o << "// Generated by tools/po2inc from translations/*/LC_MESSAGES/" << domain << ".po. Do not edit.\n"
      << "#pragma once\n\n#include <cstddef>\n\n"
      << "struct TrEntry\n{\n    const char *key;           // msgid, or msgctxt \"\\x04\" msgid\n"
      << "    const char *str;           // msgstr, or msgstr[0]\n"
      << "    const char *const *forms;  // msgstr[0..n] of a plural entry, or nullptr\n"
      << "    unsigned nforms;\n};\n\n"
      << "struct TrLanguage\n{\n    const char *code;\n    const char *name;\n"
      << "    const char *pluralForms;   // the Plural-Forms header\n"
      << "    size_t translated;         // strings of the template it translates\n"
      << "    const TrEntry *entries;\n    size_t count;\n};\n\n";
    for (size_t i = 0; i < cats.size(); ++i) {
        if (cats[i].entries.empty())
            continue;
        for (size_t j = 0; j < cats[i].entries.size(); ++j) {
            const auto &e = cats[i].entries[j];
            if (e.plural.empty() || e.str.size() < 2)
                continue;
            o << "inline constexpr const char *kTrForms" << i << "_" << j << "[] = { ";
            for (size_t f = 0; f < e.str.size(); ++f)
                o << (f ? ", " : "") << quote(e.str[f]);
            o << " };\n";
        }
        o << "// " << cats[i].code << "\ninline constexpr TrEntry kTr" << i << "[] = {\n";
        for (size_t j = 0; j < cats[i].entries.size(); ++j) {
            const auto &e = cats[i].entries[j];
            o << "    { ";
            if (e.hasCtx)
                o << quote(e.ctx) << " \"\\x04\" ";
            o << quote(e.id) << ", " << quote(e.str[0]) << ", ";
            if (!e.plural.empty() && e.str.size() >= 2)
                o << "kTrForms" << i << "_" << j << ", " << e.str.size();
            else
                o << "nullptr, 0";
            o << " },\n";
        }
        o << "};\n\n";
    }
    o << "inline constexpr TrLanguage kTrLanguages[] = {\n";
    for (size_t i = 0; i < cats.size(); ++i) {
        size_t translated = 0;
        for (const auto &e : cats[i].entries)
            translated += potKeys.count(e.key());
        o << "    { " << quote(cats[i].code) << ", " << quote(cats[i].name) << ", " << quote(cats[i].pluralForms) << ", "
          << translated << ", ";
        if (cats[i].entries.empty())
            o << "nullptr, 0 },\n";
        else
            o << "kTr" << i << ", sizeof kTr" << i << " / sizeof kTr" << i << "[0] },\n";
    }
    if (cats.empty())
        o << "    { nullptr, nullptr, nullptr, 0, nullptr, 0 },\n";
    o << "};\n\n";
    o << "// The template's keys (" << domain << ".pot): every string the app has.\n"
      << "inline constexpr const char *kTrTemplateKeys[] = {\n";
    for (const auto &e : pot)
        o << "    " << (e.hasCtx ? quote(e.ctx) + " \"\\x04\" " : std::string()) << quote(e.id) << ",\n";
    if (pot.empty())
        o << "    nullptr,\n";
    o << "};\ninline constexpr size_t kTrTemplateCount = " << pot.size() << ";\n";

    if (!writeIfChanged(outPath, o.str())) {
        std::cerr << "po2inc: cannot write " << outPath.string() << "\n";
        return 1;
    }

    // Slint's folder: the languages as they are, and zz-keys.
    for (auto &l : langs) {
        const fs::path rel = fs::path(l.filename()) / "LC_MESSAGES" / (domain + ".po");
        if (!writeIfChanged(slintDir / rel, readFile(l / "LC_MESSAGES" / (domain + ".po")))) {
            std::cerr << "po2inc: cannot write " << (slintDir / rel).string() << "\n";
            return 1;
        }
    }
    std::ostringstream k;
    k << "# Generated by tools/po2inc: each string's key, for the translator preview. Do not edit.\n"
      << "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Language: " << kKeysLanguage
      << "\\n\"\n\"Plural-Forms: nplurals=1; plural=0;\\n\"\n";
    for (const auto &e : pot) {
        const std::string key = "\xEE\x80\x80" + (e.hasCtx ? e.ctx + "\xEE\x80\x84" : std::string()) + e.id;
        k << "\n";
        if (e.hasCtx)
            k << "msgctxt " << quote(e.ctx) << "\n";
        k << "msgid " << quote(e.id) << "\n";
        if (!e.plural.empty())
            k << "msgid_plural " << quote(e.plural) << "\nmsgstr[0] " << quote(key) << "\n";
        else
            k << "msgstr " << quote(key) << "\n";
    }
    const fs::path keysPath = slintDir / kKeysLanguage / "LC_MESSAGES" / (domain + ".po");
    if (!writeIfChanged(keysPath, k.str())) {
        std::cerr << "po2inc: cannot write " << keysPath.string() << "\n";
        return 1;
    }
    // A language folder removed from the sources goes from Slint's folder too.
    for (auto &d : fs::directory_iterator(slintDir, ec)) {
        const std::string name = d.path().filename().string();
        if (d.is_directory() && name != kKeysLanguage &&
            std::none_of(langs.begin(), langs.end(), [&](const fs::path &l) { return l.filename().string() == name; }))
            fs::remove_all(d.path(), ec);
    }
    return 0;
}
