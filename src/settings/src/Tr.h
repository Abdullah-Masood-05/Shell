#pragma once

// Translations for strings the C++ side shows in the UI (toasts, banners, status text,
// model rows). The .slint files use @tr(); both read the same catalogs,
// translations/<lang>/LC_MESSAGES/nilesoft-shell-pro.po, which the build embeds
// (Slint through SLINT_BUNDLE_TRANSLATIONS, this file through tools/po2inc.cpp), so the
// exe needs no files at run time. The translator preview (loadTestFile below) is the one
// exception: it reads a .po file a translator is working on, for that session.
//
// Marking strings (scripts/update-translations.ps1 finds them by these names, so keep
// one plain string literal as the first argument, no concatenation, no raw strings):
//   nsp::tr("Text")                      the translation, or "Text" in English
//   nsp::trc("Context", "Text")          the same with a gettext context (msgctxt)
//   nsp::trf("Saved {}", name)           with {} or {0} {1} placeholders, like @tr()
//   nsp::trn("{n} file", "{n} files", n) a count; {n} is the count
//   nsp::mark("Text")                    only marks a literal stored in a table; pass the
//                                        stored value through nsp::tr() where it is shown

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace nsp {

// A bundled language. English is the source language and is not in the list.
struct Language
{
    const char *code;       // the folder name under translations/, e.g. "zh-CN"
    const char *nativeName; // the .po header X-Language-Name, e.g. "简体中文"
    size_t translated;      // strings of the template (.pot) it translates
};

// The languages Preferences lists and Auto may pick: those with at least half of the
// app's strings translated. A language that is just starting stays out of the way until
// then; its translator checks it with the translator preview (loadTestFile).
std::span<const Language> languages();
// Every bundled language, listed or not.
std::span<const Language> allLanguages();
// How many strings the app has (the template's entries).
size_t stringCount();

// The app's language setting: "" follows Windows, "en" is English, else a code above.
// Resolves "" with resolveLanguage(windowsLanguages(), ...).
std::string resolveSetting(std::string_view setting);

// The best bundled language for a list of BCP-47 tags in order of preference (as
// Windows reports them): "" for English. Chinese maps by script and region:
// zh-TW, zh-HK, zh-MO and zh-Hant* to zh-TW, any other zh to zh-CN.
std::string resolveLanguage(std::span<const std::string> preferred, std::span<const std::string> available);

// The Windows display languages, most preferred first (GetUserPreferredUILanguages).
std::vector<std::string> windowsLanguages();

// Switches the C++ catalog ("" or "en": English) and runs the onLanguageChanged
// callbacks. Slint's own switch (slint::select_bundled_translation) is the caller's job.
// Returns false (and keeps English) for a code that is not bundled. Ends a translator
// preview.
bool setLanguage(std::string_view code);
// The language shown: a bundled code, "" for English, or the Language header of the
// translator preview's file.
const std::string &currentLanguage();

// The translator preview: a .po file read at run time replaces the catalog for this
// session, for the C++ strings here and for the .slint strings through
// src/TrPreviewHook.h. Untranslated and fuzzy entries show in English. The caller selects
// Slint's English bundle ("") so the hook falls back to English, and calls
// slint::update_all_translations() after a load.
struct TestStats
{
    std::string language;  // the file's Language header, or ""
    size_t total = 0;      // strings in the app
    size_t translated = 0; // of those, translated in the file
    size_t fuzzy = 0;      // of those, marked fuzzy in the file (not shown)
    size_t missing = 0;    // total - translated: untranslated, fuzzy or not in the file
    size_t unknown = 0;    // translated entries for strings this version doesn't have
};
// Reads and applies a .po file (any language code) and runs the onLanguageChanged
// callbacks. On an error (unreadable, a syntax error with its line) nothing changes and
// error says why.
bool loadTestFile(const std::filesystem::path &file, TestStats &stats, std::string &error);
// The same from text in memory (tests).
bool loadTestText(std::string_view text, TestStats &stats, std::string &error);
bool testActive();

// Called after setLanguage() so code that pushed translated text to the UI can push it
// again. Callbacks live as long as the process; register them once.
void onLanguageChanged(std::function<void()> fn);

const char *tr(const char *msgid);
const char *trc(const char *context, const char *msgid);
constexpr const char *mark(const char *msgid) { return msgid; }

// "{}" takes the next argument, "{0}" a given one, "{n}" the count of trn(); "{{" and
// "}}" are literal braces.
std::string format(std::string_view fmt, std::span<const std::string> args, long long n = 0);

namespace detail {
template<class T>
std::string arg(const T &v)
{
    if constexpr (std::is_arithmetic_v<T>)
        return std::to_string(v);
    else
        return std::string(std::string_view(v));
}
std::string pluralLookup(const char *singular, const char *plural, long long n);
// For src/TrPreviewHook.h: the preview's translation of a key (msgctxt "\x04" msgid, or
// msgid), nullptr when it has none. The plural one picks the form for n.
const char *testLookup(std::string_view key);
const char *testLookupPlural(std::string_view key, long long n);
// The key of a bundled .slint string: from its zz-keys entry (tools/po2inc: U+E000, then
// msgctxt and U+E004 when there is one, then msgid), else the English text.
std::string previewKey(const char *keyEntry, const char *english);
} // namespace detail

template<class... A>
std::string trf(const char *msgid, const A &...args)
{
    const std::string list[] = { std::string(), detail::arg(args)... };
    return format(tr(msgid), std::span<const std::string>(list + 1, sizeof...(A)));
}

template<class... A>
std::string trn(const char *singular, const char *plural, long long n, const A &...args)
{
    const std::string list[] = { std::string(), detail::arg(args)... };
    return format(detail::pluralLookup(singular, plural, n), std::span<const std::string>(list + 1, sizeof...(A)), n);
}

} // namespace nsp
