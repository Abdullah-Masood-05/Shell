#pragma once

// The translator preview for the .slint strings. CMakeLists.txt force-includes this file
// (/FI) into the C++ that slint-compiler generates from ui/app.slint, and nowhere else.
//
// With bundled translations, every @tr() compiles to
//   slint::private_api::translate_from_bundle(slint_translation_bundle_N, args)
// where slint_translation_bundle_N is a plain array: the English text, then one entry per
// bundled language. Slint 1.18 can't load a catalog at run time in this setup (its gettext
// path is Unix only and needs libintl, and set_translator() is ignored for bundled
// strings), so the overloads below take those calls instead: a template that takes the
// array itself is a better match than Slint's std::span version. Without an active preview
// they pass straight through to Slint.
//
// The bundled lookup gets no msgctxt, so po2inc bundles one more language, zz-keys (always
// last), whose "translation" is each string's key: U+E000, msgctxt and U+E004 when there is
// one, then msgid. A string the .pot doesn't have yet falls back to its English text as
// the key.
//
// If a Slint update changes this code shape, the overloads stop matching and the preview
// covers only the C++ strings; the app itself works as before. tests/test_pofile.cpp and
// the --po snapshot in src/settings/README.md check it.

#include <slint.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "Tr.h"

namespace nsp::preview_hook {

inline std::string key(const char8_t *keyEntry, const char8_t *english)
{
    return nsp::detail::previewKey(reinterpret_cast<const char *>(keyEntry), reinterpret_cast<const char *>(english));
}

} // namespace nsp::preview_hook

namespace slint::private_api {

template<std::size_t N>
inline SharedString translate_from_bundle(const char8_t *const (&strs)[N],
                                          cbindgen_private::Slice<SharedString> arguments)
{
    const std::span<const char8_t *const> all(strs, N);
    if (!nsp::testActive())
        return translate_from_bundle(all, arguments);
    const char *found = nsp::detail::testLookup(nsp::preview_hook::key(N > 1 ? strs[N - 1] : nullptr, strs[0]));
    // Slint shows English during the preview, so the one-entry list is what it formats
    // (and it keeps track of the language for update_all_translations()).
    const char8_t *const one[1] = { found ? reinterpret_cast<const char8_t *>(found) : strs[0] };
    return translate_from_bundle(std::span<const char8_t *const>(one, 1), arguments);
}

template<std::size_t N, std::size_t M, class Rules>
inline SharedString translate_from_bundle_with_plural(const char8_t *const (&strs)[N], const uint32_t (&indices)[M],
                                                      const Rules &plural_rules,
                                                      cbindgen_private::Slice<SharedString> arguments, int n)
{
    const std::span<const char8_t *const> all(strs, N);
    const std::span<const uint32_t> idx(indices, M);
    if (!nsp::testActive())
        return translate_from_bundle_with_plural(all, idx, plural_rules, arguments, n);
    // Language i has strs[indices[i-1] .. indices[i]]; zz-keys is the last one.
    const char8_t *keyEntry = nullptr;
    if (M >= 2 && indices[M - 1] > indices[M - 2] && indices[M - 2] < N)
        keyEntry = strs[indices[M - 2]];
    const char *found = nsp::detail::testLookupPlural(nsp::preview_hook::key(keyEntry, strs[0]), n);
    const std::array<uintptr_t (*const)(int32_t), 1> englishRule{ nullptr };
    if (found) {
        const char8_t *const one[1] = { reinterpret_cast<const char8_t *>(found) };
        const uint32_t oneIdx[1] = { 1 };
        return translate_from_bundle_with_plural(std::span<const char8_t *const>(one, 1),
                                                 std::span<const uint32_t>(oneIdx, 1), englishRule, arguments, n);
    }
    // English: the singular and plural, picked by English's rule.
    const uint32_t englishIdx[1] = { N >= 2 ? 2u : 1u };
    return translate_from_bundle_with_plural(all, std::span<const uint32_t>(englishIdx, 1), englishRule, arguments, n);
}

} // namespace slint::private_api
