cmake_minimum_required(VERSION 3.21)

# Nilesoft Shell Settings (settings.exe). It talks to the core only through shell.exe,
# files and HKCU: nothing here includes src/shared or links the core.

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

project(NilesoftShellSettings VERSION ${SHELL_VERSION}.${SHELL_VERSION_REV} LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

# Fluent widgets; Palette.color-scheme switches light/dark at runtime.
set(SLINT_STYLE "fluent" CACHE STRING "Slint widget style" FORCE)
set(DEFAULT_SLINT_EMBED_RESOURCES "embed-files" CACHE STRING "" FORCE)
find_package(Slint 1.18 REQUIRED)

set(SETTINGS_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bin")

configure_file(res/settings.rc.in "${CMAKE_CURRENT_BINARY_DIR}/settings.rc" @ONLY)

add_executable(settings WIN32
    src/main.cpp
    src/AppController.cpp
    src/Dwm.cpp
    src/LogReader.cpp
    src/Registry.cpp
    src/ShellCli.cpp
    src/Status.cpp
    src/Worker.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/settings.rc"
)
slint_target_sources(settings ui/app.slint)

target_include_directories(settings PRIVATE src third_party "${CMAKE_CURRENT_SOURCE_DIR}/res")
target_compile_definitions(settings PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    SETTINGS_VERSION="${SHELL_VERSION}")
target_compile_options(settings PRIVATE /utf-8 /permissive- /Zc:__cplusplus /EHsc)
# /W4 for our own sources only; the Slint-generated code is noisy at that level.
file(GLOB SETTINGS_OWN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set_source_files_properties(${SETTINGS_OWN_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4")
target_link_libraries(settings PRIVATE Slint::Slint dwmapi shell32 shlwapi advapi32 ole32 user32)
# A WIN32 executable that keeps the standard main().
target_link_options(settings PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)

# The settings model library (NSS lexer, document, config graph) is built separately.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nss/CMakeLists.txt")
    add_subdirectory(nss)
endif()
if(TARGET nsscore)
    target_link_libraries(settings PRIVATE nsscore)
    target_compile_definitions(settings PRIVATE SETTINGS_HAVE_NSSCORE=1)
endif()

set_target_properties(settings PROPERTIES
    OUTPUT_NAME settings
    RUNTIME_OUTPUT_DIRECTORY "${SETTINGS_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SETTINGS_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SETTINGS_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SETTINGS_BIN_DIR}/debug"
    VS_DEBUGGER_WORKING_DIRECTORY "${SETTINGS_BIN_DIR}")

# slint_cpp.dll goes next to settings.exe.
add_custom_command(TARGET settings POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:settings> $<TARGET_FILE_DIR:settings>
    COMMAND_EXPAND_LISTS)
