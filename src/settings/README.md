# Nilesoft Shell Pro, the app (NilesoftShellPro.exe)

A Slint 1.18.1 C++ app. The build writes `src/bin/NilesoftShellPro.exe`, next to `shell.exe`
and `shell.dll`. The folder and the CMake target are still called `settings`; the exe, its
window, the Start menu entry and the docs say Nilesoft Shell Pro. Running `shell.exe` with no
arguments opens it, and its settings stay under `HKCU\Software\Nilesoft\Shell\Settings`.

## Runtime dependencies: none

The release build is a single self-contained `NilesoftShellPro.exe`:

- Slint is built from source as a **static library** (`BUILD_SHARED_LIBS=OFF`) and linked
  into the exe. `slint_cpp.dll` is not built, needed or shipped any more.
- Everything uses the **static CRT**: the C++ code (`settings`, `nsscore`, `settings_tests`)
  is built with `/MT` (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`), and the Rust side with
  `-Ctarget-feature=+crt-static` (so rustc links `libcmt` and cc-rs builds bundled C code
  with `/MT`). Users don't need the VC++ Redistributable.

`dumpbin /dependents src\bin\NilesoftShellPro.exe` lists only Windows system DLLs:
```
kernel32  user32  gdi32  advapi32  shell32  shlwapi  ole32  oleaut32  combase
comctl32  uxtheme  dwmapi  dwrite  imm32  opengl32  uiautomationcore
bcrypt  bcryptprimitives  ntdll  api-ms-win-core-synch-l1-2-0  api-ms-win-core-winrt-error-l1-1-0
```

The exe is about 16 MB (the prebuilt-SDK build was 1.5 MB plus a 27 MB `slint_cpp.dll`).

## Building (release, self-contained)

Prerequisites: VS 2022 Build Tools (MSVC v143, Windows SDK), CMake 3.25+, Rust stable
for `x86_64-pc-windows-msvc` (rustup default; Slint 1.18.1 needs Rust 1.92+), git.

```bat
rem optional: a local checkout instead of a clone at configure time
git clone --depth 1 --branch v1.18.1 https://github.com/slint-ui/slint D:\Projects\_deps\slint-src-1.18.1
set SLINT_SOURCE_DIR=D:\Projects\_deps\slint-src-1.18.1

cd src\settings
cmake --preset x64-release
cmake --build --preset x64-release
```

Without `SLINT_SOURCE_DIR` (env or `-DSLINT_SOURCE_DIR=...`), CMake's FetchContent clones
tag `v1.18.1` into the build tree. Corrosion (Slint's CMake/cargo bridge, v0.6.1) is cloned
from GitHub at configure time as well, and cargo downloads the crates in Slint's
`Cargo.lock` on the first build. The Slint version lives in `cmake/SlintFromSource.cmake`
(`SETTINGS_SLINT_VERSION`).

Slint options used (`cmake/SlintFromSource.cmake`; each one can be overridden with `-D`):

| Option | Value |
| --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` (static `slint_cpp.lib`) |
| `SLINT_STYLE` | `fluent` |
| `SLINT_FEATURE_BACKEND_WINIT` | `ON` |
| `SLINT_FEATURE_RENDERER_FEMTOVG` | `ON` (OpenGL, the default renderer) |
| `SLINT_FEATURE_RENDERER_SOFTWARE` | `ON` (fallback; `SLINT_BACKEND=winit-software`) |
| `SLINT_FEATURE_ACCESSIBILITY` | `ON` (AccessKit / UI Automation) |
| `SLINT_FEATURE_RENDERER_SKIA` | `OFF` (needs large prebuilt Skia binaries built for `/MD`) |
| `SLINT_FEATURE_INTERPRETER`, `_LIVE_PREVIEW`, `_SYSTEM_TRAY`, `_TESTING`, `_GETTEXT` | `OFF` |

The `slint-compiler` (a build-time host tool) is built from the same source tree. To skip
that, pass `-DSLINT_COMPILER=<path to slint-compiler.exe 1.18.1>`.

### Build time and caching (CI)

A clean build takes about 4 minutes on a 16-thread desktop: almost all of it is cargo
compiling Slint, its ~500 dependencies and `slint-compiler`. Rebuilds after changing
only the app's C++ or `.slint` files take seconds, as long as the build tree is kept.

For CI, cache:

- `src/settings/out/x64-release/` (the CMake build tree; Corrosion's cargo target dir is
  `out/x64-release/cargo/build`, which holds everything expensive), and
- `~/.cargo/registry` and `~/.cargo/git` (downloaded crates).

Key the cache on the Slint version (`SETTINGS_SLINT_VERSION` in
`cmake/SlintFromSource.cmake`), the Rust version (`rustc --version`), the MSVC toolset
version, and a hash of `cmake/SlintFromSource.cmake`. If the key changes, the build is
simply a cold one again.

## Quick dev builds with the prebuilt Slint SDK

The `x64-release-prebuilt` and `x64-debug` presets use the prebuilt Slint C++ SDK
(`SETTINGS_SLINT_FROM_SOURCE=OFF`) found via `SLINT_DIR` (env or `-D`, default
`D:/Projects/_deps/slint-cpp-1.18.1`). Those builds use the dynamic CRT (`/MD`), copy
`slint_cpp.dll` next to `NilesoftShellPro.exe` and need the VC++ runtime: **don't ship them**.

```bat
cd src\settings
cmake --preset x64-release-prebuilt
cmake --build --preset x64-release-prebuilt
```

The release preset builds Slint from source in every configuration; `-DSETTINGS_SLINT_FROM_SOURCE=OFF`
switches any build tree to the SDK.

## Tests

```bat
cd src\settings
out\x64-release\tests\Release\settings_tests.exe
```

`nsscore` (the `.nss` model library in `nss/`) has its own tests, built standalone:

```bat
cmake -S src\settings\nss -B src\settings\out\nss -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build src\settings\out\nss --config Release
src\settings\out\nss\Release\nsscore_tests.exe
```
