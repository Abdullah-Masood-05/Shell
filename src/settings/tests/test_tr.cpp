// Tr: the app's language choice and the C++ translation lookup (src/Tr.h).

#include "doctest.h"

#include "Tr.h"

#include <string>
#include <vector>

namespace {

std::string resolve(std::vector<std::string> preferred)
{
    const std::vector<std::string> available{ "zh-CN", "zh-TW" };
    return nsp::resolveLanguage(preferred, available);
}

// Restores English when a test ends, whatever it switched to.
struct EnglishAfter
{
    ~EnglishAfter() { nsp::setLanguage(""); }
};

} // namespace

TEST_CASE("tr: language from the Windows display languages")
{
    CHECK(resolve({ "zh-CN" }) == "zh-CN");
    CHECK(resolve({ "zh-SG" }) == "zh-CN");
    CHECK(resolve({ "zh-Hans-CN" }) == "zh-CN");
    CHECK(resolve({ "zh" }) == "zh-CN");
    CHECK(resolve({ "zh-TW" }) == "zh-TW");
    CHECK(resolve({ "zh-HK" }) == "zh-TW");
    CHECK(resolve({ "zh-MO" }) == "zh-TW");
    CHECK(resolve({ "zh-Hant" }) == "zh-TW");
    CHECK(resolve({ "zh-Hant-HK" }) == "zh-TW");
    CHECK(resolve({ "ZH_tw" }) == "zh-TW");

    CHECK(resolve({ "en-US" }) == "");
    CHECK(resolve({ "de-DE" }) == "");
    CHECK(resolve({}) == "");
    // The first language that is bundled wins; English counts even though it has no .po.
    CHECK(resolve({ "de-DE", "zh-TW", "en-US" }) == "zh-TW");
    CHECK(resolve({ "en-GB", "zh-CN" }) == "");

    // Other languages: an exact tag first, then the same base language.
    const std::vector<std::string> more{ "de", "pt-BR", "pt-PT" };
    CHECK(nsp::resolveLanguage(std::vector<std::string>{ "de-AT" }, more) == "de");
    CHECK(nsp::resolveLanguage(std::vector<std::string>{ "pt-PT" }, more) == "pt-PT");
    CHECK(nsp::resolveLanguage(std::vector<std::string>{ "pt-AO" }, more) == "pt-BR");
}

TEST_CASE("tr: the setting")
{
    CHECK(nsp::resolveSetting("en") == "");
    CHECK(nsp::resolveSetting("zh-CN") == "zh-CN");
    CHECK(nsp::resolveSetting("zh-tw") == "zh-TW");
    CHECK(nsp::resolveSetting("xx") == "");
}

TEST_CASE("tr: bundled languages")
{
    bool cn = false, tw = false;
    for (const auto &l : nsp::languages()) {
        cn |= std::string(l.code) == "zh-CN" && std::string(l.nativeName) == "简体中文";
        tw |= std::string(l.code) == "zh-TW" && std::string(l.nativeName) == "繁體中文";
    }
    CHECK(cn);
    CHECK(tw);
}

TEST_CASE("tr: Arabic")
{
    EnglishAfter english;
    bool listed = false;
    std::vector<std::string> available;
    for (const auto &l : nsp::languages()) {
        listed |= std::string(l.code) == "ar" && std::string(l.nativeName) == "العربية";
        available.emplace_back(l.code);
    }
    CHECK(listed);
    // Auto: any Arabic Windows display language.
    CHECK(nsp::resolveLanguage(std::vector<std::string>{ "ar-SA" }, available) == "ar");
    CHECK(nsp::resolveLanguage(std::vector<std::string>{ "ar-EG" }, available) == "ar");
    CHECK(nsp::resolveSetting("ar") == "ar");

    // Six plural forms: 0, 1, 2, 3-10, 11-99, 100-102 (by n % 100).
    REQUIRE(nsp::setLanguage("ar"));
    CHECK(std::string(nsp::tr("Language")) == "اللغة");
    const std::string rlm = "‏"; // starts the line right to left, see README.md
    CHECK(nsp::trn("{n} file", "{n} files", 0) == rlm + "0 ملف");
    CHECK(nsp::trn("{n} file", "{n} files", 1) == "ملف واحد");
    CHECK(nsp::trn("{n} file", "{n} files", 2) == "ملفان");
    CHECK(nsp::trn("{n} file", "{n} files", 7) == rlm + "7 ملفات");
    CHECK(nsp::trn("{n} file", "{n} files", 42) == rlm + "42 ملفًا");
    CHECK(nsp::trn("{n} file", "{n} files", 100) == rlm + "100 ملف");
    CHECK(nsp::trn("{n} file", "{n} files", 105) == rlm + "105 ملفات");
}

TEST_CASE("tr: lookup")
{
    EnglishAfter english;
    CHECK(std::string(nsp::tr("Language")) == "Language");

    int changes = 0;
    nsp::onLanguageChanged([&changes] { ++changes; });

    REQUIRE(nsp::setLanguage("zh-CN"));
    CHECK(nsp::currentLanguage() == "zh-CN");
    CHECK(changes == 1);
    CHECK(std::string(nsp::tr("Language")) == "语言");
    CHECK(std::string(nsp::trc("Language", "Auto")) == "自动");
    // A string the catalog doesn't have stays English.
    CHECK(std::string(nsp::tr("No such string in the catalog")) == "No such string in the catalog");

    REQUIRE(nsp::setLanguage("zh-TW"));
    CHECK(std::string(nsp::tr("Language")) == "語言");

    CHECK_FALSE(nsp::setLanguage("xx")); // not bundled: English
    CHECK(std::string(nsp::tr("Language")) == "Language");
    CHECK(nsp::currentLanguage() == "");
    CHECK(changes == 3);

    nsp::setLanguage(""); // no change, no callback
    CHECK(changes == 3);
}

TEST_CASE("tr: format")
{
    const std::vector<std::string> args{ "a", "b" };
    CHECK(nsp::format("{} and {}", args) == "a and b");
    CHECK(nsp::format("{1} before {0}", args) == "b before a");
    CHECK(nsp::format("{{literal}} {}", args) == "{literal} a");
    CHECK(nsp::format("{n} items", {}, 3) == "3 items");
    CHECK(nsp::format("{} {} {}", args) == "a b "); // a missing argument is empty
    CHECK(nsp::trf("Version {} of {}", "2.0", 3) == "Version 2.0 of 3");
    CHECK(nsp::trn("{n} file", "{n} files", 1) == "1 file");
    CHECK(nsp::trn("{n} file", "{n} files", 4) == "4 files");
}
