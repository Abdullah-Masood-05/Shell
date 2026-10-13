# Nilesoft Shell Pro, the app: design (2026-10-12)

(Called Nilesoft Shell Settings, `settings.exe`, until 2026-10-13; now the one Start menu entry, *Nilesoft Shell Pro*, `NilesoftShellPro.exe`.)

Slint 1.18.1 C++ app built with MSVC and CMake, in `src/settings/`. It never includes `src/shared`. It talks to the core only through `shell.exe` commands, files, and HKCU, and never needs admin.

## Facts verified in the core
- **Later values win:** a later `theme{}` / `settings{}` overrides an earlier one key by key (Parser.cpp:603, 638, 707-973). The dotted form `theme.background.opacity = 50` works too.
- **One parse error turns the whole menu off** (Initializer.cpp:148-161). It retries only after the root file or the failing file is written again. So every save must be validated first (`shell.exe -config:check`).
- **Theme value ranges:**
  - `item.radius` is a step 0..4 (0/4/8/12/16 px).
  - Theme names: auto/classic/white/black/modern. Density is `theme.view` (compact/small/medium/large/wide).
  - `effect`: 0 none, 1 transparent, 2 blur, 3 acrylic, or `[3, tint, opacity]`.
- **Config order:** HKCU `config` value, then `%APPDATA%\Nilesoft\Shell\shell.nss`, then the install folder.
- **shell.exe quirks:** it is a GUI-subsystem exe, so always pass `-silent`. The log is `shell.log` next to shell.dll.
- **Runtime libraries:** the core uses the static CRT (`/MT`). NilesoftShellPro.exe does too: Slint is built from source as a static library (with `+crt-static`) and linked in, so the app is one exe with no slint_cpp.dll and no VC++ runtime. See `src/settings/README.md`.

## Pages
- **Home:** health card (registration none/user/machine/both, TreatAs state ours/foreign/absent, DLL path, versions). Active config card (path and where it comes from, "use per-user config"). Diagnostics from the end of `shell.log`, linked to the editor. Quick actions.
- **Onboarding** (first run): 1) `-config:init` 2) `-register -user` 3) pick a look, then restart Explorer.
- **Appearance:** a Themes gallery on top (the original project's example themes from `src/settings/presets/themes`, one card each: the wallpaper with a mini menu in that theme, name, tags, an Active badge, and a "…" menu with Apply and Export to clipboard; Apply stages the keys as pending changes, asking first when it replaces theme settings changed in the app); theme cards; dark mode auto/on/off; density; background (color, hex, eyedropper, opacity, effect plus tint); font, size, weight; radius as a 5-step control; border; shadow; an Advanced expander. Live preview with Desktop/Explorer and Light/Dark toggles. Overridden keys show a "reset" dot.
- **Behavior:** the `settings{}` block (priority, showdelay, tip, screenshot, modify.*, new.*).
- **Rules & Filters:** an `exclude.*` builder plus a raw expression box.
- **Context Menus:** import-module toggles and a list of `modify(find=...)` rules (hide, move, rename).
- **Menu Builder:** a tree of menu/item/separator in an app-owned file, with drag to reorder.
- **Config Editor:** import-graph file tree, editor, diagnostics.
- **Backups:** timeline, diff, restore, pin.
- **Preferences:** app theme, the Live apply option, external editor, how many backups to keep, and machine-wide registration (the only button with a UAC shield).
- **Footer:** "N unsaved · Discard · Save (Ctrl+S)". There is no restart step, because the DLL reloads when a file's modified time changes. **Ctrl+K** opens a command palette.

## Editing model: hybrid
- **Reading:** a lossless lexer and syntax tree (`NssLexer`, `NssDocument`) that keeps comments and whitespace. Only literal values are understood; expressions show as locked. `ConfigGraph` follows imports and builds a table of effective values with the file and line each comes from.
- **Writing theme and settings:** a generated, app-owned `imports/settings-app.nss` holding only the keys the user changed. It is imported last in the root shell.nss. A "Bake into theme.nss" action writes the values into the user's theme file with minimal syntax-tree edits.
- **Writing menus:** an app-owned `imports/settings-app-menus.nss`. Import toggles and modify rules use minimal tree edits.
- **Every save:**
  1. Reparse the new text.
  2. Check it against the schema and value ranges.
  3. Run `shell.exe -config:check` on a temp copy of the config tree.
  4. Make a `.bak` and a snapshot.
  5. Write atomically (`.tmp`, `FlushFileBuffers`, `ReplaceFileW`), keeping the encoding, BOM and line endings, and writing imports before the root.
  6. Watch `-status` and `shell.log` for 3 s and roll back automatically if the load failed.
- **Snapshots:** `%APPDATA%\Nilesoft\Shell\backups\YYYYMMDD-HHMMSS\` plus `manifest.json`. Keep 50 snapshots or 60 days; pinned ones are never deleted.

## Preview
`PreviewTheme` is computed by `ThemeModel::resolve()` from a copy of the defaults table in `Theme::Default()`, the accent color in `HKCU\...\DWM\AccentColor`, and `AppsUseLightTheme`.
- **Items:** Slint Rectangles and Text using the Segoe Fluent Icons system font, with the codepoints from images.nss.
- **Background:** the real wallpaper (`SPI_GETDESKWALLPAPER`), shrunk and blurred for the blur effect, with tint and noise added for acrylic.
- **Gradient:** `theme.background.gradient` is sampled at its start, middle and end and drawn as a linear (or centred radial) gradient over the background.
- **Caption:** "Approximate; right-click the desktop to see the real menu".

## Architecture (`src/settings/src`)
- **Core and process:**
  - `ShellCli`: CreateProcess with pipes, `CREATE_NO_WINDOW`, `-silent`, a timeout, and a vendored JSON parser. `elevate()` uses ShellExecuteEx runas.
  - `Registry`, `LogReader`.
- **Config:** `NssLexer`, `NssDocument`, `ConfigGraph`, `ThemeSchema`, `ThemeModel`, `OverridesWriter`, `ConfigStore` (save transaction), `Backups`.
- **Platform:**
  - `FileWatcher`: `ReadDirectoryChangesW` with a 300 ms debounce. It ignores changes whose content hash matches our own last write.
  - `Dwm`, `Wallpaper`, `FontEnum`.
- **Threading:** a single `Worker` (std::jthread plus a queue). Results come back through `slint::invoke_from_event_loop`. One controller per page.
- **UI files:** `ui/app.slint`, `types.slint`, `theme/{tokens,icons}`, `components/*`, `pages/*`, `preview/*`. Slint globals: `AppState`, `Tokens`, `Icons`.

## Look
- **Style and color tokens:** `SLINT_STYLE=fluent`, with `Palette.color-scheme` switched at runtime.
  - **Dark:** bg #202020, layers #2B2B2B/#323232, stroke #FFFFFF14, text #FFF / #FFFFFFC8 / #FFFFFF8B, accent = system accent (#60CDFF).
  - **Light:** #F3F3F3 / #FFF / #F9F9F9, text #1A1A1A / #5D5D5D, accent #005FB8.
  - Spacing on a 4 px grid; radius 4 for controls, 8 for cards.
- **Type:** Segoe UI Variable (14 body, 20 subtitle, 28 page title, 12 caption). Cascadia Mono in the editor.
- **Icons:** Fluent UI System Icons SVGs (MIT) for the app chrome. Segoe Fluent Icons as a system font for the preview only, since it can't be redistributed. Use the original Nilesoft Shell logo (`src/shared/Resource/main.ico`, `packages/assets/logo-*.png`).
- **Window:** keep the native title bar. Mica plus the immersive dark title bar through DWM. Whether Mica can show through a transparent Slint window is uncertain and needs a spike in M1; the fallback is a solid background.

## Build and deploy
- **CMake:** `src/settings/CMakeLists.txt` plus `CMakePresets.json` (`SLINT_DIR` defaults to `D:\Projects\_deps\slint-cpp-1.18.1`). Use `/ENTRY:mainCRTStartup`, `/MD`, and link dwmapi. Output goes to `src/bin/NilesoftShellPro.exe` (the CMake target is `settings`).
- **Version:** read from `src/shared/Resource.h` by regex, never by including it.
- **Manifest:** asInvoker, PerMonitorV2, Common Controls v6, UTF-8 code page.
- **CI:** add a CMake step to the x64 job, with the Slint source build cached. arm64 runs the x64 app under emulation.
- **WiX:** an `App` component group (NilesoftShellPro.exe only) and the one Start menu shortcut, *Nilesoft Shell Pro*, with the AppUserModelID `Nilesoft.ShellPro` that the app also sets on itself.

## Milestones
- **M1:** skeleton, sidebar, Home/status, onboarding, preferences, and the Mica/transparency spike.
- **M2:** Appearance, preview, the save transaction, and `-config:check` in the core.
- **M3:** Behavior, Rules, and the command palette.
- **M4:** Config Editor (highlighted read view plus a TextEdit).
- **M5:** Context Menus and Menu Builder.
- **M6:** Backups.
- **M7:** packaging (CI, WiX, docs).
