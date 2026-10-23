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

## Translations

The app's text is in English, Simplified Chinese (`zh-CN`) and Traditional Chinese (`zh-TW`);
Spanish (`es`) is started. Each language is one gettext file,
`translations/<code>/LC_MESSAGES/nilesoft-shell-pro.po`, next to the template
`translations/nilesoft-shell-pro.pot`. The build embeds them: the `.slint` strings through
Slint's bundled translations (`SLINT_BUNDLE_TRANSLATIONS` in `CMakeLists.txt`), the C++
strings through `tools/po2inc` (`cmake/Translations.cmake`), so the exe still needs no files
at run time.

### Translating the app (no programming needed)

1. **Get the file.** If the language has a folder under `translations/` (`es` for Spanish),
   take its `nilesoft-shell-pro.po`. Otherwise copy `translations/nilesoft-shell-pro.pot`
   and name the copy after the language, for example `fr.po`.
2. **Translate it** in [Poedit](https://poedit.net/) (free; for a `.pot` use *Create new
   translation*, which also sets the plural rules) or in Notepad++ or any text editor: fill
   in each `msgstr ""` under its `msgid`, and save as UTF-8. Keep the placeholders (`{}`,
   `{0}`, `{n}`, `{{ }}`), and keep keywords, file names and app names as they are. Empty or
   fuzzy (*Needs work* in Poedit) entries show in English.
3. **Test it** in the app: Preferences > Language > *Test a translation file…*, then pick the
   file (or start `NilesoftShellPro.exe --po "C:\path\to\fr.po"`). The whole app shows the
   file's text for that session, with a bar at the top: "Showing translations from fr.po.
   Untranslated text stays in English", how many of the app's strings it translates, how
   many are missing or fuzzy, and *Reload* (read the file again after saving it) and *Stop*.
   Nothing is saved.
4. **Share it** in [GitHub Discussions](https://github.com/Abdullah-Masood-05/Shell/discussions),
   or open a pull request that puts it at `translations/<code>/LC_MESSAGES/nilesoft-shell-pro.po`
   (`scripts/update-translations.ps1 -Import fr.po` merges a file someone sent).

The same steps are on the website: [Preferences > Translating the app](https://abdullah-masood-05.github.io/Shell/app/preferences/#translate).

### For developers

- **Marking text.** In `.slint` files: `@tr("Text")`, `@tr("Saved {}", name)`,
  `@tr("{n} file" | "{n} files" % count)`, `@tr("Context" => "Text")`. Literal braces are
  `{{` and `}}`. In C++: `nsp::tr`, `nsp::trc`, `nsp::trf`, `nsp::trn` and `nsp::mark` from
  `src/Tr.h`. Config keywords, file names, code and app names are not marked.
- **Updating the catalogs** after changing text (needs `slint-tr-extractor`:
  `cargo install slint-tr-extractor --version 1.18.1`, or build `tools/tr-extractor` in the
  Slint source and pass `-Extractor <path>`):

  ```powershell
  pwsh scripts/update-translations.ps1
  ```

  It rewrites the `.pot`, adds new strings to every `.po` with an empty `msgstr`, keeps the
  existing translations, and moves strings the app no longer has to the end as `#~`
  entries. `-Check` only tells whether the `.pot` is current. `-Import file.po` takes over
  the translated entries of a `.po` someone sent back (its `Language:` header picks the
  language).
- **Translating.** Fill in the `msgstr` lines (a text editor, Poedit or Lokalize). Keep the
  placeholders (`{}`, `{0}`, `{n}`, `{{ }}`), and keep keywords, file names and app names
  as they are. Untranslated or `fuzzy` entries show in English. The entry with context
  `UI font family` is the font the app uses for that language, not text.
- **Adding a language.**

  ```powershell
  pwsh scripts/update-translations.ps1 -NewLanguage ja -NativeName "日本語"
  ```

  Then set `Plural-Forms` in the new file's header, translate, and rebuild. Once at least
  half of the template is translated (`nsp::languages()`; `po2inc` counts it), the language
  appears in Preferences > Language under its native name, and *Auto* picks it when it
  matches the Windows display language (an exact tag first, then the same base language).
  Below that it is built in but hidden, so a Spanish Windows doesn't switch to a UI that is
  mostly English with a few Spanish words; a label like "(incomplete)" would still have done
  that through *Auto*. Chinese is the one special case: `zh-TW`, `zh-HK`, `zh-MO` and
  `zh-Hant` go to `zh-TW`, other `zh` to `zh-CN` (`resolveLanguage` in `src/Tr.cpp`).
- **Checking a language** without changing the setting: `NilesoftShellPro.exe --lang zh-TW`
  (`auto`, `en` or a listed code; that run only).
- **The translator preview** (*Test a translation file…*, `--po <file>`) reads any `.po` at
  run time. C++ strings: `nsp::loadTestFile` (`src/Tr.cpp`, parser and plural rules in
  `src/PoFile.cpp`) replaces the catalog. `.slint` strings: Slint 1.18 has no run-time
  catalog for a static Windows build (its gettext path is Unix only and needs libintl, and
  `slint::set_translator()` needs `SLINT_FEATURE_EXPERIMENTAL` and is ignored for bundled
  strings). So `src/TrPreviewHook.h`, force-included into the generated code only, adds
  overloads of Slint's `translate_from_bundle` that take the bundle arrays the generated
  code passes; they look the string up in the preview's file and otherwise pass straight
  through. The bundled lookup has no msgctxt, so `po2inc` bundles one more language,
  `zz-keys`, whose "translation" is each string's context and msgid. After a Slint upgrade,
  check the preview still covers the `.slint` text (a `--po` snapshot with a few strings
  translated); if the generated code changed shape, only the C++ strings follow the file.