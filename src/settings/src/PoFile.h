#pragma once

// A gettext .po file read at run time, for the translator preview (Preferences > Language >
// Test a translation file, or --po <file>). The bundled languages don't go through here:
// tools/po2inc embeds them at build time.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nsp::po {

struct Entry
{
    std::string context; // msgctxt, when hasContext
    bool hasContext = false;
    std::string id;       // msgid
    std::string idPlural; // msgid_plural, or empty
    std::vector<std::string> str; // msgstr, or msgstr[0..n]
    bool fuzzy = false;
    int line = 0; // where the entry starts, 1-based

    // msgctxt "\x04" msgid, or msgid: the key src/Tr.cpp looks strings up by.
    std::string key() const;
    // Not fuzzy and msgstr (msgstr[0]) not empty: the app shows it.
    bool translated() const { return !fuzzy && !str.empty() && !str[0].empty(); }
};

struct File
{
    std::string header;         // the msgstr of the msgid "" entry, unescaped
    std::vector<Entry> entries; // in file order, without the header and obsolete (#~) entries

    // A "Name: value" line of the header ("" when it is missing).
    std::string headerField(std::string_view name) const;
};

// Reads .po text: UTF-8 with or without a BOM, LF or CRLF. The strings are unescaped
// (\n \t \r \" \\ \a \b \f \v, octal \ooo and \xHH). On a syntax error: false and
// "line N: what" in error.
bool parse(std::string_view text, File &out, std::string &error);

// The plural rule of a Plural-Forms header, "nplurals=3; plural=(n%10==1 ? 0 : 1);".
// The expression is gettext's C subset: n, numbers, ( ), ! * / % + - < > <= >= == != && || ?:.
class PluralRule
{
public:
    PluralRule(); // English: nplurals=2; plural=(n != 1)

    // false (and the rule unchanged) when the header can't be read.
    bool parse(std::string_view pluralForms);

    int nplurals() const { return nplurals_; }
    // The form for a count, 0 .. nplurals-1.
    size_t index(unsigned long long n) const;

    struct Node;

private:
    int nplurals_ = 2;
    std::shared_ptr<const Node> expr_;
};

} // namespace nsp::po
