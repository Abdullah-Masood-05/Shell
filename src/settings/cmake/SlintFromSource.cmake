# Builds Slint (the C++ API crate slint-cpp) from source as a static library, linked
# into settings.exe together with the static CRT, so the exe has no runtime dependency
# beyond Windows system DLLs (no slint_cpp.dll, no VC++ Redistributable).
#
# Source: SLINT_SOURCE_DIR (cache var or env var) pointing at a checkout of the
# v${SETTINGS_SLINT_VERSION} tag; otherwise FetchContent clones the tag from GitHub.
# Needs Rust (stable, x86_64-pc-windows-msvc) and cargo on PATH.

set(SETTINGS_SLINT_VERSION "1.18.1")

if(NOT SLINT_SOURCE_DIR AND DEFINED ENV{SLINT_SOURCE_DIR})
    set(SLINT_SOURCE_DIR "$ENV{SLINT_SOURCE_DIR}")
endif()
set(SLINT_SOURCE_DIR "${SLINT_SOURCE_DIR}" CACHE PATH
    "Local Slint v${SETTINGS_SLINT_VERSION} source tree (empty: clone from GitHub)")

# One static CRT for everything: our code, nsscore, and the Rust staticlib (crt-static
# below). Rust always links the release CRT, so Debug uses /MT as well (not /MTd).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Slint options. Plain cache defaults (no FORCE), so -D on the command line still wins.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build Slint as a static library")
set(SLINT_FEATURE_BACKEND_WINIT ON CACHE BOOL "")
set(SLINT_FEATURE_RENDERER_FEMTOVG ON CACHE BOOL "")
set(SLINT_FEATURE_RENDERER_SOFTWARE ON CACHE BOOL "")
set(SLINT_FEATURE_ACCESSIBILITY ON CACHE BOOL "")
set(SLINT_FEATURE_RENDERER_SKIA OFF CACHE BOOL "")
set(SLINT_FEATURE_INTERPRETER OFF CACHE BOOL "")
set(SLINT_FEATURE_LIVE_PREVIEW OFF CACHE BOOL "")
set(SLINT_FEATURE_SYSTEM_TRAY OFF CACHE BOOL "")
set(SLINT_FEATURE_TESTING OFF CACHE BOOL "")
set(SLINT_FEATURE_GETTEXT OFF CACHE BOOL "")

include(FetchContent)
if(SLINT_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_SLINT "${SLINT_SOURCE_DIR}")
endif()
FetchContent_Declare(Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    GIT_TAG v${SETTINGS_SLINT_VERSION}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR api/cpp)
FetchContent_MakeAvailable(Slint)

# Rust side of the static CRT: rustc links libcmt instead of msvcrt, and cc-rs builds
# the C parts of dependencies with /MT.
corrosion_add_target_rustflags(slint_cpp "-Ctarget-feature=+crt-static")

# Win32 import libraries the static slint_cpp.lib needs on top of what Corrosion detects
# (it probes a dummy crate, not slint-cpp). rustc's own list
# (--print=native-static-libs) points at windows.0.5x.0.lib from the windows-targets
# crates; these are the matching Windows SDK libraries (imm32 and uxtheme come from winit).
target_link_libraries(Slint INTERFACE imm32 uxtheme shlwapi opengl32 dwmapi)
# The Slint headers always declare their C API __declspec(dllimport); with the static
# library that is resolved locally and only produces LNK4217 noise.
target_link_options(Slint INTERFACE /IGNORE:4217)

# Slint's generated headers use nameless unions (C4201) and forward-declare HWND__ /
# HINSTANCE__ as classes (C4099 against the Windows SDK). Treat them as external headers
# so the app's own /W4 build stays clean.
set_target_properties(Slint PROPERTIES SYSTEM TRUE)
target_compile_options(Slint INTERFACE /wd4099)
