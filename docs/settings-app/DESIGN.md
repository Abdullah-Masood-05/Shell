# Nilesoft Shell Pro, the app: design (2026-10-12)

(Called Nilesoft Shell Settings, `settings.exe`, until 2026-10-13; now the one Start menu entry, *Nilesoft Shell Pro*, `NilesoftShellPro.exe`.)

Slint 1.18.1 C++ app built with MSVC and CMake, in `src/settings/`. It never includes `src/shared`. It talks to the core only through `shell.exe` commands, files, and HKCU, and never needs admin.

## Facts verified in the core
- **Later values win:** a later `theme{}` / `settings{}` overrides an earlier one key by key (Parser.cpp:603, 638, 707-973). The dotted form `theme.background.opacity = 50` works too.
- **One parse error turns the whole menu off** (Initializer.cpp:148-161). It retries only after the root file or the failing file is written again. So every save must be validated first (`shell.exe -config:check`).
- **Theme value ranges:**
  - `item.radius` is a step 0..4 (0/4/8/12/16 px).
  - Theme names: auto/classic/white/black/modern. Density is `theme.view` (compact/small/medium/large/wide).
  - `effect`: 0 none, 1 transparent, 2 blur, 3 acrylic, or `[3, tint, opacity]`. See below for the full semantics.
- **Background effect, opacity and tint** (ContextMenu.cpp, the theme set-up around line 2730 and the background around 3195; drawing around 5420 and 5615; Theme.h `Modern`/`Light`/`Dark`/`White`/`Black`):
  - **Start:** the effect is 3 when Windows *Transparency effects* (`Personalize\EnableTransparency`) is on, else 0.
  - **`background.effect`:** a number (above 5 counts as 0; 4 mica and 5 tabbed are drawn as acrylic), `none`/`transparent`/`blur`/`acrylic`, or `auto`/`default`. **`auto` is stronger than 3:** it turns transparency on even when Windows has it off, and gives 3 on Windows 11 (2 on Windows 10). An explicit 2 or 3 with Windows transparency off becomes 0 (solid). No composition or high contrast always means 0.
  - **Array form `[n, tint, opacity]`:** `n` as above, then the tint colour, then the tint's alpha in percent (0..100, `Color::opacity`). This only sets the **tint**. It doesn't touch `background.opacity`.
  - **`background.tintcolor`:** read only when the effect is 2 or 3 and transparency is on. It wins over the array's tint. The alpha of the colour is the tint's strength: `#RRGGBB` (alpha FF) is an opaque tint that hides the blur, and `#2C2C2C40` is light.
  - **Tint default:** the base theme picks it, built with "transparency on" whenever the effect isn't 0. Modern dark, Auto and Classic on Windows 11 use the background colour at alpha 0x40 (dark `#2C2C2C40`). Modern light uses alpha 0xCF for blur and white at alpha 0 (`#FFFFFF00`) for acrylic. White and Black use alpha 0xFF. A tint that is exactly 0 is sent as `0x01000000`.
  - **`background.opacity`:** 0..100, the alpha of `background.color` (`Theme::opacity`, `percent * 255 / 100`). With an effect, the core paints the background colour at that alpha **over** the blur/acrylic window. 0 shows only the effect and its tint; 100 is solid, and then the blur window isn't even created (`background.opacity < 0xFF` is required). With effect 0 it's ignored (alpha forced to 0xFF).
  - **`opacity = auto`** (or unset, or any non-number) under an effect resolves to 0, so with transparency on, `opacity = 0` and `opacity = auto` draw the same menu. `color = auto` keeps the theme's colour (alpha 0 under an effect).
  - **DWM:** a separate window under the menu gets `SetWindowCompositionAttribute(WCA_ACCENT_POLICY)`. Effect 2 is `ACCENT_ENABLE_BLURBEHIND`. Effect 3 is `ACCENT_ENABLE_ACRYLICBLURBEHIND` (build 17134 and later, else blur) with flag 2 and GradientColor = the tint. The core's colours are stored ABGR, which is what GradientColor wants. `DWMWA_SYSTEMBACKDROP_TYPE` (Mica and the like) is defined but not used for menus.
  - **The app follows this:** the Effect chips write only `background.effect`. An `[n, tint, opacity]` array keeps its tint for blur and acrylic. Opacity is cleared (back to auto) only when it's 100, which would hide the effect. An auto/unset opacity shows as "Auto (≈N %)" and is never written until the slider moves, and 0 is never written on the user's behalf. Tint writes `background.tintcolor` with its alpha (`#RRGGBBAA`). Notes next to Effect, Tint and Opacity say what each does. When Transparency effects is off, the Effect note warns that 2/3 draw solid. The example presets' `opacity=0` (Acrylic, Gradient, Clear acrylic, copied verbatim from the maintainer) is the same as auto under an effect, so they keep it.
- **Config order:** HKCU `config` value, then `%APPDATA%\Nilesoft\Shell\shell.nss`, then the install folder.
- **shell.exe quirks:** it is a GUI-subsystem exe, so always pass `-silent`. The log is `shell.log` next to shell.dll.
- **Runtime libraries:** the core uses the static CRT (`/MT`). NilesoftShellPro.exe does too: Slint is built from source as a static library (with `+crt-static`) and linked in, so the app is one exe with no slint_cpp.dll and no VC++ runtime. See `src/settings/README.md`.

## Pages
- **Home:** health card (registration none/user/machine/both, TreatAs state ours/foreign/absent, DLL path, versions). Active config card (path and where it comes from, "use per-user config"). Diagnostics from the end of `shell.log`, linked to the editor. Quick actions.
- **Onboarding** (first run): 1) `-config:init` 2) `-register -user` 3) pick a look, then restart Explorer.
- **Appearance:** a Themes gallery on top (the original project's example themes from `src/settings/presets/themes`, one card each: the wallpaper with a mini menu in that theme, name, tags, an Active badge, and a "…" menu with Apply and Export to clipboard; Apply stages the keys as pending changes, asking first when it replaces theme settings changed in the app); the base theme (the five built-in theme cards); then the theme in use (thumbnail of the current look, name, "In use" / "Not saved yet") with four tabs:
  - **Colors:** a palette of six real keys: Background `theme.background.color`, Text `theme.item.text.normal`, Highlight `theme.item.back.select`, Highlighted text `theme.item.text.select`, Disabled text `theme.item.text.normal.disabled`, Border `theme.border.color`. A swatch opens the colour picker; the row under the palette has the hex box, origin and reset. Dark mode auto/on/off.
  - **Typography:** font, size, weight, density (`theme.view`).
  - **Effects:** effect plus tint, opacity, menu and item corners (5 steps), border size, shadow.
  - **Advanced:** item padding and margin, image size and gap, disabled-item highlight, separators.
  - Live preview with Desktop/Explorer and Light/Dark toggles, and a link to the Config Editor for keys the page doesn't have. Overridden keys show a "reset" dot. No highlight-style or accent-intensity controls: the core has no such keys.
- **Behavior:** the `settings{}` block (priority, showdelay, tip, screenshot, modify.*, new.*).
- **Rules & Filters:** an `exclude.*` builder plus a raw expression box.
- **Context Menus:** import-module toggles and a list of `modify(find=...)` rules (hide, move, rename).
- **Menu Builder:** a tree of menu/item/separator in an app-owned file, with drag to reorder.
- **Open with:** editors and terminals detected read-only (fixed folders, Toolbox scripts, PATH) plus custom apps, written to the app-owned `imports/settings-app-openwith.nss` as top-level "Open in <app>" items right under WinRAR, else under "Open in new window", else at the top (`pos=indexof(...)`; the core keeps same-indexof items in file order), or optionally one "Open in" submenu there; other apps' own menu items listed read-only from the registry, each switchable off with a `remove()` rule in the same file; a menu preview and a dry-run Test that only shows the command line.
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
- **Background:** the real wallpaper (`SPI_GETDESKWALLPAPER`), shrunk and blurred for the blur effect. For acrylic, a faint layer of the tint's own colour goes on top (`theme::acrylicLayer`, about 10 % where the tint is clear), then the tint at its alpha, then the background colour at its opacity. There is no fixed grey "luminosity" layer: the real menu is as see-through as its tint.
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
  - **Dark** (2026-10-14, blue, built on the user's #101823): sidebar #0C121B, content #101823, cards #152031, fields #1A2638, hovered tiles #1F2D42, stroke #FFFFFF14 / #FFFFFF26, text #E5E7EB / #94A3B8 / #8391A6.
  - **Light:** sidebar #EAEFF7, content #F3F6FB, cards #FFFFFF, fields #F5F8FC, stroke #0F172A14, text #0F172A / #475569 / #5B6B82.
  - **Accent:** the app's own blue, not the Windows accent: #4F8CFF (dark) / #2563EB (light) for icons, lines and links; filled controls #2F6FEB / #2563EB with white text (>= 4.5:1). The selected sidebar item is filled with the accent at 16% (10% in light). Semantic colours unchanged. Body text is >= 4.7:1 on every surface in both themes.
  - Std widgets that would take the Windows accent are replaced: `AppButton`, `ValueSlider`, `ToggleSwitch` (Toggle), `Segmented`, `ChipGroup`.
  - Spacing on a 4 px grid; radius 4 (chips), 6 (controls), 10 (cards).
- **Type:** Segoe UI Variable (14 body, 16 card title, 20 subtitle, 28 page title, 12 caption). Cascadia Mono in the editor.
- **Cards:** an optional header row (icon, 16 px title, description, an action button on the right). Choice groups are one recessed well with the selection filled with the accent. Motion 120-200 ms, ease-out.
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
