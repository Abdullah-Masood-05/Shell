# The app's translations: translations/<lang>/LC_MESSAGES/nilesoft-shell-pro.po.
#
# Both sides embed the same .po files, so the exe needs no files or gettext at run time:
#   - the .slint @tr() strings through Slint's bundled translations
#     (SLINT_BUNDLE_TRANSLATIONS, set on the target in CMakeLists.txt);
#   - the C++ strings (src/Tr.h) through tools/po2inc, which turns the .po files into
#     generated/TranslationsData.inc.
# Slint bundles a copy of the folder that po2inc writes (generated/slint-translations):
# the same .po files plus zz-keys, each string's context and msgid, which the translator
# preview needs (src/TrPreviewHook.h).
# scripts/update-translations.ps1 (repo root) re-extracts the strings and merges them.

set(SETTINGS_TRANSLATIONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/translations")
set(SETTINGS_TRANSLATION_DOMAIN "nilesoft-shell-pro")
set(TRANSLATIONS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SETTINGS_SLINT_TRANSLATIONS_DIR "${TRANSLATIONS_GENERATED_DIR}/slint-translations")

# A new language folder re-runs the configure step, so the glob stays current.
file(GLOB _po_files CONFIGURE_DEPENDS
    "${SETTINGS_TRANSLATIONS_DIR}/*/LC_MESSAGES/${SETTINGS_TRANSLATION_DOMAIN}.po")
list(SORT _po_files)

add_executable(po2inc tools/po2inc/po2inc.cpp)
target_compile_options(po2inc PRIVATE /utf-8 /permissive- /EHsc /W4)

set(_slint_po_files "${SETTINGS_SLINT_TRANSLATIONS_DIR}/zz-keys/LC_MESSAGES/${SETTINGS_TRANSLATION_DOMAIN}.po")
foreach(_po IN LISTS _po_files)
    file(RELATIVE_PATH _rel "${SETTINGS_TRANSLATIONS_DIR}" "${_po}")
    list(APPEND _slint_po_files "${SETTINGS_SLINT_TRANSLATIONS_DIR}/${_rel}")
endforeach()

add_custom_command(
    OUTPUT "${TRANSLATIONS_GENERATED_DIR}/TranslationsData.inc" ${_slint_po_files}
    COMMAND po2inc "${SETTINGS_TRANSLATIONS_DIR}" "${SETTINGS_TRANSLATION_DOMAIN}"
            "${TRANSLATIONS_GENERATED_DIR}/TranslationsData.inc" "${SETTINGS_SLINT_TRANSLATIONS_DIR}"
    DEPENDS po2inc ${_po_files} "${SETTINGS_TRANSLATIONS_DIR}/${SETTINGS_TRANSLATION_DOMAIN}.pot"
    COMMENT "Embedding the translations for the C++ strings"
    VERBATIM)
add_custom_target(translations_data DEPENDS "${TRANSLATIONS_GENERATED_DIR}/TranslationsData.inc")
