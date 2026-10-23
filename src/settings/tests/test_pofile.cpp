// The translator preview: reading a .po file at run time (src/PoFile.h) and showing it
// through nsp::tr (src/Tr.h).

#include "doctest.h"

#include "PoFile.h"
#include "Tr.h"

#include <string>

namespace {

struct EnglishAfter
{
    ~EnglishAfter() { nsp::setLanguage(""); }
};

const char *kSpanish = R"po(# Spanish test file
msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\n"
"Language: es\n"
"Plural-Forms: nplurals=2; plural=(n != 1);\n"

#: ui/pages/preferences.slint:69
msgid "Language"
msgstr "Idioma"

msgctxt "Language"
msgid "Auto"
msgstr "Automático"

msgctxt "Theme name"
msgid "Auto"
msgstr "Auto (tema)"

#, fuzzy
msgid "Preferences"
msgstr "Preferencias"

msgid "Close"
msgstr ""

msgid "{n} file"
msgid_plural "{n} files"
msgstr[0] "{n} archivo"
msgstr[1] "{n} archivos"

msgid "A string this app doesn't have"
msgstr "Una cadena que la app no tiene"

#~ msgid "Gone"
#~ msgstr "Ido"
)po";

} // namespace

TEST_CASE("po: entries, contexts, plurals and escapes")
{
    const char *text = "\xEF\xBB\xBF"
                       "msgid \"\"\r\n"
                       "msgstr \"\"\r\n"
                       "\"Language: de\\n\"\r\n"
                       "\"Plural-Forms: nplurals=2; plural=n != 1;\\n\"\r\n"
                       "\r\n"
                       "#. a comment for translators\r\n"
                       "#: src/x.cpp:1\r\n"
                       "msgctxt \"Menu\"\r\n"
                       "msgid \"Open\"\r\n"
                       "msgstr \"Öffnen\"\r\n"
                       "\r\n"
                       "msgid \"\"\r\n"
                       "\"Two \"\r\n"
                       "\"lines\"\r\n"
                       "msgstr \"Zwei\\nZeilen \\\"quoted\\\" \\\\ tab\\t \\x41\\101\"\r\n"
                       "\r\n"
                       "#, c-format, fuzzy\r\n"
                       "msgid \"Fuzzy\"\r\n"
                       "msgstr \"Unscharf\"\r\n"
                       "\r\n"
                       "msgid \"{n} day\"\r\n"
                       "msgid_plural \"{n} days\"\r\n"
                       "msgstr[0] \"{n} Tag\"\r\n"
                       "msgstr[1] \"{n} Tage\"\r\n"
                       "#~ msgid \"Old\"\r\n"
                       "#~ msgstr \"Alt\"\r\n";
    nsp::po::File f;
    std::string error;
    REQUIRE_MESSAGE(nsp::po::parse(text, f, error), error);
    CHECK(f.headerField("Language") == "de");
    CHECK(f.headerField("plural-forms") == "nplurals=2; plural=n != 1;");
    REQUIRE(f.entries.size() == 4);

    CHECK(f.entries[0].hasContext);
    CHECK(f.entries[0].context == "Menu");
    CHECK(f.entries[0].key() == std::string("Menu\x04Open"));
    CHECK(f.entries[0].str[0] == "Öffnen");
    CHECK(f.entries[0].translated());

    CHECK(f.entries[1].id == "Two lines");
    CHECK(f.entries[1].str[0] == "Zwei\nZeilen \"quoted\" \\ tab\t AA");

    CHECK(f.entries[2].fuzzy);
    CHECK_FALSE(f.entries[2].translated());

    CHECK(f.entries[3].idPlural == "{n} days");
    REQUIRE(f.entries[3].str.size() == 2);
    CHECK(f.entries[3].str[1] == "{n} Tage");
}

TEST_CASE("po: syntax errors name the line")
{
    nsp::po::File f;
    std::string error;
    CHECK_FALSE(nsp::po::parse("msgid \"a\"\nmsgstr \"b\n", f, error));
    CHECK(error.rfind("line 2:", 0) == 0);
    CHECK_FALSE(nsp::po::parse("msgid \"a\"\nmsgstr \"b\"\nnonsense \"c\"\n", f, error));
    CHECK(error.rfind("line 3:", 0) == 0);
    CHECK_FALSE(nsp::po::parse("msgid \"a\"\n\nmsgid \"b\"\nmsgstr \"c\"\n", f, error)); // a has no msgstr
    CHECK_FALSE(nsp::po::parse("msgstr \"x\"\n", f, error));
    CHECK_FALSE(nsp::po::parse("msgid \"bad \\q escape\"\nmsgstr \"\"\n", f, error));
}

TEST_CASE("po: plural rules")
{
    nsp::po::PluralRule en;
    CHECK(en.nplurals() == 2);
    CHECK(en.index(1) == 0);
    CHECK(en.index(0) == 1);
    CHECK(en.index(5) == 1);

    nsp::po::PluralRule zh;
    REQUIRE(zh.parse("nplurals=1; plural=0;"));
    CHECK(zh.index(1) == 0);
    CHECK(zh.index(7) == 0);

    nsp::po::PluralRule fr;
    REQUIRE(fr.parse("nplurals=2; plural=(n > 1);"));
    CHECK(fr.index(0) == 0);
    CHECK(fr.index(1) == 0);
    CHECK(fr.index(2) == 1);

    nsp::po::PluralRule ru;
    REQUIRE(ru.parse("nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"));
    CHECK(ru.nplurals() == 3);
    CHECK(ru.index(1) == 0);
    CHECK(ru.index(21) == 0);
    CHECK(ru.index(11) == 2);
    CHECK(ru.index(3) == 1);
    CHECK(ru.index(14) == 2);
    CHECK(ru.index(22) == 1);
    CHECK(ru.index(5) == 2);

    nsp::po::PluralRule ar;
    REQUIRE(ar.parse("nplurals=6; plural=n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : n%100>=3 && n%100<=10 ? 3 : n%100>=11 ? 4 : 5;"));
    CHECK(ar.index(0) == 0);
    CHECK(ar.index(2) == 2);
    CHECK(ar.index(105) == 3);
    CHECK(ar.index(111) == 4);
    CHECK(ar.index(100) == 5);

    nsp::po::PluralRule bad;
    CHECK_FALSE(bad.parse("nplurals=2; plural=(n != );"));
    CHECK_FALSE(bad.parse("plural=n != 1;"));
    CHECK(bad.nplurals() == 2); // unchanged: English
    nsp::po::PluralRule notOp;
    REQUIRE(notOp.parse("nplurals=2; plural=!(n == 1);"));
    CHECK(notOp.index(1) == 0);
    CHECK(notOp.index(2) == 1);
}

TEST_CASE("tr: the translator preview")
{
    EnglishAfter english;
    static int changes = 0; // callbacks live as long as the process
    nsp::onLanguageChanged([] { ++changes; });

    nsp::TestStats stats;
    std::string error;
    REQUIRE_MESSAGE(nsp::loadTestText(kSpanish, stats, error), error);
    CHECK(changes == 1);
    CHECK(nsp::testActive());
    CHECK(nsp::currentLanguage() == "es");
    CHECK(stats.language == "es");
    CHECK(stats.total == nsp::stringCount());
    CHECK(stats.translated == 4); // Language, Language|Auto, Theme name|Auto, {n} file
    CHECK(stats.fuzzy == 1);
    CHECK(stats.unknown == 1);
    CHECK(stats.missing == stats.total - stats.translated);

    CHECK(std::string(nsp::tr("Language")) == "Idioma");
    CHECK(std::string(nsp::trc("Language", "Auto")) == "Automático");
    CHECK(std::string(nsp::trc("Theme name", "Auto")) == "Auto (tema)");
    CHECK(std::string(nsp::tr("Preferences")) == "Preferences"); // fuzzy: English
    CHECK(std::string(nsp::tr("Close")) == "Close");             // untranslated: English
    CHECK(nsp::trn("{n} file", "{n} files", 1) == "1 archivo");
    CHECK(nsp::trn("{n} file", "{n} files", 3) == "3 archivos");

    // What src/TrPreviewHook.h asks for.
    CHECK(std::string(nsp::detail::testLookup(std::string("Theme name\x04"
                                                          "Auto"))) == "Auto (tema)");
    CHECK(nsp::detail::testLookup("Close") == nullptr);
    CHECK(std::string(nsp::detail::testLookupPlural("{n} file", 0)) == "{n} archivos");

    // A file with a syntax error leaves the preview as it was.
    CHECK_FALSE(nsp::loadTestText("msgid \"x\"\nmsgstr \"y\n", stats, error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK(std::string(nsp::tr("Language")) == "Idioma");
    CHECK_FALSE(nsp::loadTestText("msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=ISO-8859-1\\n\"\n", stats, error));
    CHECK(nsp::testActive());

    // Picking a language ends the preview.
    REQUIRE(nsp::setLanguage("zh-CN"));
    CHECK_FALSE(nsp::testActive());
    CHECK(std::string(nsp::tr("Language")) == "语言");
    CHECK(nsp::detail::testLookup("Language") == nullptr);
}

TEST_CASE("tr: the preview hook reads the key language")
{
    // U+E000 [msgctxt U+E004] msgid, see tools/po2inc.
    const char8_t *withContext = u8"Theme nameAuto";
    const char8_t *plain = u8"Close";
    auto key = [](const char8_t *k, const char8_t *en) {
        return nsp::detail::previewKey(reinterpret_cast<const char *>(k), reinterpret_cast<const char *>(en));
    };
    CHECK(key(withContext, u8"Auto") == std::string("Theme name\x04"
                                                    "Auto"));
    CHECK(key(plain, u8"Close") == "Close");
    CHECK(key(nullptr, u8"Not in the .pot yet") == "Not in the .pot yet");
    CHECK(key(u8"关闭", u8"Close") == "Close"); // not a key: the English text
}

TEST_CASE("tr: languages under half translated are not listed")
{
    const size_t total = nsp::stringCount();
    REQUIRE(total > 0);
    for (const auto &l : nsp::allLanguages()) {
        bool listed = false;
        for (const auto &m : nsp::languages())
            listed |= std::string(m.code) == l.code;
        CHECK_MESSAGE(listed == (l.translated * 2 >= total), l.code);
    }
}
