cmake_minimum_required(VERSION 3.21)

# Nilesoft Shell Pro, the app (NilesoftShellPro.exe; the CMake target is still called
# "settings"). It talks to the core only through shell.exe, files and HKCU: nothing here
# includes src/shared or links the core. (settings_tests includes src/shared/ConfigUpgrade.h,
# the logic behind `shell.exe -config:upgrade`, to test it.)

# The version comes from the core's Resource.h, read by regex (never #included).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../shared/Resource.h" _resource_h)
foreach(_part MAJOR MINOR BUILD REV)
    if(_resource_h MATCHES "#define[ \t]+VERSION_${_part}[ \t]+([0-9]+)")
        set(SHELL_VERSION_${_part} ${CMAKE_MATCH_1})
    else()
        message(FATAL_ERROR "VERSION_${_part} not found in src/shared/Resource.h")
    endif()
endforeach()
if(_resource_h MATCHES "#define[ \t]+VERSION_YEAR[ \t]+([0-9]+)")
    set(SHELL_VERSION_YEAR ${CMAKE_MATCH_1})
else()
    set(SHELL_VERSION_YEAR 2024)
endif()
set(SHELL_VERSION "${SHELL_VERSION_MAJOR}.${SHELL_VERSION_MINOR}.${SHELL_VERSION_BUILD}")

# The full version the app shows and compares with GitHub releases, including any
# pre-release suffix (2.0.1-beta.1). release.yml passes the tag without the "v"
# (-DSETTINGS_VERSION_TAG=2.0.1-beta.1); empty means the Resource.h version, so a cached
# build tree never keeps an old release's tag.
set(SETTINGS_VERSION_TAG "" CACHE STRING "Full app version, e.g. 2.0.1 or 2.0.1-beta.1 (empty: the Resource.h version)")
if(SETTINGS_VERSION_TAG STREQUAL "")
    set(SETTINGS_FULL_VERSION "${SHELL_VERSION}")
else()
    string(REGEX REPLACE "^v" "" SETTINGS_FULL_VERSION "${SETTINGS_VERSION_TAG}")
endif()
if(NOT SETTINGS_FULL_VERSION MATCHES "^([0-9]+[.][0-9]+[.][0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "SETTINGS_VERSION_TAG '${SETTINGS_FULL_VERSION}' is not a version like 2.0.1 or 2.0.1-beta.1")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL SHELL_VERSION)
    message(FATAL_ERROR "SETTINGS_VERSION_TAG '${SETTINGS_FULL_VERSION}' does not match src/shared/Resource.h (${SHELL_VERSION})")
endif()
message(STATUS "Nilesoft Shell Pro version: ${SETTINGS_FULL_VERSION}")

project(NilesoftShellPro VERSION ${SHELL_VERSION}.${SHELL_VERSION_REV} LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Slint either comes from source as a static library (default; NilesoftShellPro.exe then needs no
# DLLs beyond Windows itself, no VC++ Redistributable) or from the prebuilt SDK
# (slint_cpp.dll + dynamic CRT; quicker for dev builds). See README.md.
option(SETTINGS_SLINT_FROM_SOURCE "Build Slint from source as a static library and link the static CRT" ON)

# Fluent widgets; Palette.color-scheme switches light/dark at runtime.
set(SLINT_STYLE "fluent" CACHE STRING "Slint widget style" FORCE)
set(DEFAULT_SLINT_EMBED_RESOURCES "embed-files" CACHE STRING "" FORCE)

if(SETTINGS_SLINT_FROM_SOURCE)
    include(cmake/SlintFromSource.cmake)
else()
    # slint_cpp.dll needs the dynamic CRT (the core itself uses /MT).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    if(NOT SLINT_DIR)
        if(DEFINED ENV{SLINT_DIR})
            set(SLINT_DIR "$ENV{SLINT_DIR}")
        else()
            set(SLINT_DIR "D:/Projects/_deps/slint-cpp-1.18.1")
        endif()
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${SLINT_DIR}")
    find_package(Slint 1.18 REQUIRED)
endif()

set(SETTINGS_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bin")

configure_file(res/settings.rc.in "${CMAKE_CURRENT_BINARY_DIR}/settings.rc" @ONLY)

# The theme presets (presets/themes/*.nss), embedded as ThemePresetsData.inc.
include(cmake/ThemePresets.cmake)

# The bundled snippets (../bin/snippets: catalog.json and *.nss), embedded as SnippetsData.inc.
include(cmake/Snippets.cmake)

# The UI translations (translations/*/LC_MESSAGES/*.po), for Slint and for src/Tr.cpp.
include(cmake/Translations.cmake)

add_executable(settings WIN32
    src/main.cpp
    src/AppController.cpp
    src/AppearanceController.cpp
    src/BackupsController.cpp
    src/BackupsModel.cpp
    src/BehaviorController.cpp
    src/ConfigRules.cpp
    src/ConfigUpdate.cpp
    src/ConfigUpdateController.cpp
    src/ContextMenusController.cpp
    src/Diff.cpp
    src/Dwm.cpp
    src/EditorController.cpp
    src/EditorSession.cpp
    src/ExcludeRules.cpp
    src/FileWatcher.cpp
    src/FontEnum.cpp
    src/Highlight.cpp
    src/LogReader.cpp
    src/PendingChanges.cpp
    src/PresetsController.cpp
    src/ProgramIcon.cpp
    src/MapRules.cpp
    src/MenuBuilderController.cpp
    src/MenuMap.cpp
    src/MenuMapController.cpp
    src/MenuModel.cpp
    src/MenuWriter.cpp
    src/OnboardingController.cpp
    src/OpenWithController.cpp
    src/OpenWithModel.cpp
    src/OtherApps.cpp
    src/Registry.cpp
    src/SaveService.cpp
    src/ShellCli.cpp
    src/Snippets.cpp
    src/SnippetsController.cpp
    src/Status.cpp
    src/TaskbarApps.cpp
    src/ThemeModel.cpp
    src/ThemePresets.cpp
    src/Tr.cpp
    src/UpdateController.cpp
    src/Updater.cpp
    src/Wallpaper.cpp
    src/WindowsTitles.cpp
    src/Worker.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/settings.rc"
)
slint_target_sources(settings ui/app.slint)
# @tr() strings get no default context (the component name), so a string that several
# components and the C++ side share is translated once.
set_target_properties(settings PROPERTIES
    SLINT_BUNDLE_TRANSLATIONS "${SETTINGS_TRANSLATIONS_DIR}"
    SLINT_TRANSLATION_DOMAIN "${SETTINGS_TRANSLATION_DOMAIN}"
    SLINT_NO_DEFAULT_TRANSLATION_CONTEXT ON)
add_dependencies(settings translations_data)

target_include_directories(settings PRIVATE src third_party "${CMAKE_CURRENT_SOURCE_DIR}/res" "${THEME_PRESETS_GENERATED_DIR}" "${TRANSLATIONS_GENERATED_DIR}")
target_compile_definitions(settings PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    SETTINGS_VERSION="${SETTINGS_FULL_VERSION}")
target_compile_options(settings PRIVATE /utf-8 /permissive- /Zc:__cplusplus /EHsc)
# /W4 for our own sources only; the Slint-generated code is noisy at that level.
file(GLOB SETTINGS_OWN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set_source_files_properties(${SETTINGS_OWN_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4")
target_link_libraries(settings PRIVATE Slint::Slint dwmapi shell32 shlwapi advapi32 ole32 user32 comctl32 windowscodecs winhttp bcrypt version)
# A WIN32 executable that keeps the standard main().
target_link_options(settings PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)

# The settings model library (NSS lexer, document, config graph) is built separately.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nss/CMakeLists.txt")
    add_subdirectory(nss)
endif()
if(TARGET nsscore)
    target_link_libraries(settings PRIVATE nsscore)
    target_compile_definitions(settings PRIVATE SETTINGS_HAVE_NSSCORE=1)
else()
    message(FATAL_ERROR "nsscore (src/settings/nss) is required")
endif()

# Tests for the UI-free logic: Config Editor and Backups (Diff, Highlight, FileWatcher,
# EditorSession, BackupsModel) and the save path (ThemeModel, PendingChanges, SaveService core).
option(SETTINGS_BUILD_TESTS "Build settings_tests" ON)
if(SETTINGS_BUILD_TESTS AND TARGET nsscore)
    enable_testing()
    add_executable(settings_tests
        tests/main.cpp
        tests/test_logic.cpp
        tests/test_theme_model.cpp
        tests/test_pending.cpp
        tests/test_rules.cpp
        tests/test_save_roundtrip.cpp
        tests/test_menus.cpp
        tests/test_status.cpp
        tests/test_onboarding.cpp
        tests/test_openwith.cpp
        tests/test_otherapps.cpp
        tests/test_indexof.cpp
        tests/test_presets.cpp
        tests/test_updater.cpp
        tests/test_tr.cpp
        tests/test_config_upgrade.cpp
        tests/test_menumap.cpp
        tests/test_snippets.cpp
        tests/test_taskbar.cpp
        src/BackupsModel.cpp
        src/ConfigRules.cpp
        src/ConfigUpdate.cpp
        src/Diff.cpp
        src/EditorSession.cpp
        src/ExcludeRules.cpp
        src/FileWatcher.cpp
        src/Highlight.cpp
        src/PendingChanges.cpp
        src/Snippets.cpp
        src/Status.cpp
        src/TaskbarApps.cpp
        src/ThemeModel.cpp
        src/ThemePresets.cpp
        src/Tr.cpp
        src/MapRules.cpp
        src/MenuMap.cpp
        src/MenuModel.cpp
        src/MenuWriter.cpp
        src/OpenWithModel.cpp
        src/OtherApps.cpp
        src/Updater.cpp
        src/WindowsTitles.cpp
    )
    target_include_directories(settings_tests PRIVATE src third_party "${CMAKE_CURRENT_SOURCE_DIR}/nss/third_party/doctest" "${THEME_PRESETS_GENERATED_DIR}" "${TRANSLATIONS_GENERATED_DIR}")
    add_dependencies(settings_tests translations_data)
    target_compile_definitions(settings_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
        SETTINGS_SOURCE_BIN="${SETTINGS_BIN_DIR}"
        SETTINGS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SETTINGS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    target_compile_options(settings_tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus /EHsc /W4)
    target_link_libraries(settings_tests PRIVATE nsscore shell32 shlwapi ole32 advapi32 user32 winhttp bcrypt version)
    set_target_properties(settings_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME settings_tests COMMAND settings_tests)
endif()

set_target_properties(settings PROPERTIES
    OUTPUT_NAME NilesoftShellPro
    RUNTIME_OUTPUT_DIRECTORY "${SETTINGS_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SETTINGS_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SETTINGS_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SETTINGS_BIN_DIR}/debug"
    VS_DEBUGGER_WORKING_DIRECTORY "${SETTINGS_BIN_DIR}")

if(NOT SETTINGS_SLINT_FROM_SOURCE)
    # Prebuilt SDK only: slint_cpp.dll goes next to NilesoftShellPro.exe.
    add_custom_command(TARGET settings POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:settings> $<TARGET_FILE_DIR:settings>
        COMMAND_EXPAND_LISTS)
endif()
