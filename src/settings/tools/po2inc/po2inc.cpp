// po2inc: embeds the translation catalogs into the app for the C++ side (src/Tr.cpp).
//
//   po2inc <translations dir> <domain> <output .inc>
//
// Reads <dir>/<lang>/LC_MESSAGES/<domain>.po for every <lang> folder (sorted by name)
// and writes one table per language: the msgctxt/msgid key and the msgstr. Untranslated
// and fuzzy entries are left out, so they fall back to English like Slint does. The
// strings stay in their .po escaped form, which is also valid C++ (\n \t \" \\).
// The output is only rewritten when it changes.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Entry
{
    std::string ctx, id, plural;
    std::vector<std::string> str; // msgstr, or msgstr[0..n]
    bool hasCtx = false, fuzzy = false;
};

struct Catalog
{
    std::string code, name;
    int nplurals = 2;
    std::vector<Entry> entries;
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

bool parse(const fs::path &file, Catalog &cat, std::string &error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + file.string();
        return false;
    }
    std::vector<Entry> entries;
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

    for (auto &e : entries) {
        if (e.id.empty() && !e.hasCtx) {
            // The header: "Name: value\n" lines.
            std::string header = e.str.empty() ? std::string{} : e.str[0];
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
                else if (key == "Plural-Forms") {
                    auto np = val.find("nplurals=");
                    if (np != std::string::npos)
                        cat.nplurals = std::max(1, std::atoi(val.c_str() + np + 9));
                }
            }
            continue;
        }
        if (e.fuzzy || e.str.empty() || e.str[0].empty())
            continue;
        cat.entries.push_back(std::move(e));
    }
    if (cat.name.empty())
        cat.name = cat.code;
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: po2inc <translations dir> <domain> <output .inc>\n";
        return 2;
    }
    const fs::path dir = fs::path(reinterpret_cast<const char8_t *>(argv[1]));
    const std::string domain = argv[2];
    const fs::path outPath = fs::path(reinterpret_cast<const char8_t *>(argv[3]));

    std::vector<fs::path> langs;
    std::error_code ec;
    for (auto &d : fs::directory_iterator(dir, ec))
        if (d.is_directory() && fs::is_regular_file(d.path() / "LC_MESSAGES" / (domain + ".po")))
            langs.push_back(d.path());
    std::sort(langs.begin(), langs.end());

    std::ostringstream o;
    o << "// Generated by tools/po2inc from translations/*/LC_MESSAGES/" << domain << ".po. Do not edit.\n"
      << "#pragma once\n\n#include <cstddef>\n\n"
      << "struct TrEntry\n{\n    const char *key;    // msgid, or msgctxt \"\\x04\" msgid\n"
      << "    const char *str;    // msgstr, or msgstr[0]\n    const char *plural; // msgstr[1], or nullptr\n};\n\n"
      << "struct TrLanguage\n{\n    const char *code;\n    const char *name;\n    int nplurals;\n"
      << "    const TrEntry *entries;\n    size_t count;\n};\n\n";
    std::vector<Catalog> cats;
    for (auto &l : langs) {
        Catalog cat;
        cat.code = l.filename().string();
        std::string error;
        if (!parse(l / "LC_MESSAGES" / (domain + ".po"), cat, error)) {
            std::cerr << "po2inc: " << error << "\n";
            return 1;
        }
        cats.push_back(std::move(cat));
    }
    for (size_t i = 0; i < cats.size(); ++i) {
        if (cats[i].entries.empty())
            continue;
        o << "// " << cats[i].code << "\ninline constexpr TrEntry kTr" << i << "[] = {\n";
        for (auto &e : cats[i].entries) {
            o << "    { ";
            if (e.hasCtx)
                o << "\"" << e.ctx << "\" \"\\x04\" ";
            o << "\"" << e.id << "\", \"" << e.str[0] << "\", ";
            if (e.str.size() > 1 && !e.str[1].empty())
                o << "\"" << e.str[1] << "\"";
            else
                o << "nullptr";
            o << " },\n";
        }
        o << "};\n\n";
    }
    o << "inline constexpr TrLanguage kTrLanguages[] = {\n";
    for (size_t i = 0; i < cats.size(); ++i) {
        o << "    { \"" << cats[i].code << "\", \"" << cats[i].name << "\", " << cats[i].nplurals << ", ";
        if (cats[i].entries.empty())
            o << "nullptr, 0 },\n";
        else
            o << "kTr" << i << ", sizeof kTr" << i << " / sizeof kTr" << i << "[0] },\n";
    }
    if (cats.empty())
        o << "    { nullptr, nullptr, 1, nullptr, 0 },\n";
    o << "};\n";

    const std::string text = o.str();
    {
        std::ifstream old(outPath, std::ios::binary);
        std::string prev((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        if (prev == text)
            return 0;
    }
    fs::create_directories(outPath.parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary);
    out << text;
    return out ? 0 : 1;
}
