**Nilesoft Shell Pro {{VERSION}}** is a community fork of [Nilesoft Shell](https://github.com/moudey/Shell) by Moudey and Nilesoft: the same context-menu extension and `.nss` configuration language, with security and stability fixes, a per-user mode that needs no administrator rights, and an app to set it up and configure it. It is not an official Nilesoft release.

Documentation: https://abdullah-masood-05.github.io/Shell/ (see *About this fork* for the full list of changes).

## New in 2.0.4

- Appearance and Behavior controls are never greyed out because a theme or settings file from an older version uses an expression for that value. Changing it in the app now simply overrides the file, and the row says so.

## New in 2.0.3

- Banners (for example *Updated to 2.0.2*) grow with their text, so the second line is no longer cut off.
- Success, warning and error banners use colours that fit the app's dark blue theme.

## New in 2.0.2

- **Sharper look:** cards, panels, inputs and buttons use near-square 2–3 px corners; toggles, dots and slider thumbs stay round.
- **Open with:** "Open in VS Code", "Open in Zed" and the other apps you switch on appear as separate items directly under WinRAR (or after *Open in new window*), instead of in an "Open in" submenu. Switching an app off also hides that app's own menu entries (for example *PowerShell 7* or *Open Git Bash here*); the row lists what it hides.
- **Other apps' menu items:** entries that other programs added can be hidden one by one, and leftovers from uninstalled programs (whose program no longer exists) are flagged with a one-click *Hide*.
- **Acrylic:** choosing Acrylic no longer writes an opacity of 0; an `auto` opacity shows as *Auto*, the tint writes `background.tintcolor`, and the live preview matches the real menu.
- **Config Editor:** syntax highlighting while editing, and tabs shown as indentation (saved back unchanged).

## New in 2.0.1

- **In-app updates.** Nilesoft Shell Pro checks this project's GitHub releases when it starts (at most once a day; *Check for updates automatically* in Preferences, on by default) and shows *Update available* in the sidebar and a banner on Home with the release notes, *Download and install* and *Skip this version*.
- The update that fits your install is downloaded (`setup-x64.msi` / `setup-arm64.msi` for an MSI install, the portable zip for a portable copy) and checked against the release's `SHA256SUMS.txt` before anything is installed. MSI updates ask for administrator approval once and reopen the app; portable updates need no administrator rights, replace the files in place (a `shell.dll` Explorer has loaded is renamed aside) and offer to restart Explorer. Your configuration in `%APPDATA%` is not touched.
- Optional: *Download and install updates automatically* (off by default) downloads and verifies updates in the background, then offers *Restart to update*. A *Beta* channel also offers pre-releases.
- The app shows its full version, including a pre-release suffix such as `2.1.0-beta.1`, in the sidebar and in About.

## New in 2.0.0

- **A new look for the app:** a blue-tinted dark theme on #101823 (and a matching light theme), a refreshed sidebar, cards and controls on every page.
- **Themes gallery** on the Appearance page: the original Nilesoft Shell example themes (Default, Shell 1.8, Acrylic, Gradient, Clear acrylic, Accent outline, Flat, Blue, Gray, Green, Yellow), previewed on your wallpaper and applied with one click.
- **Open with:** add "Open with" items for VS Code, Zed, Cursor, Windsurf, Sublime Text, Notepad++, JetBrains IDEs, Visual Studio, Windows Terminal and more, detected automatically, plus your own apps.

## Fixed since the betas

- **No context menu after installing over an older manual install** (for example a copy in `C:\Windows\nilesoft`). Registering left one of Shell's COM entries pointing at the old DLL, so Explorer loaded two copies and showed no menu. Registering now repoints or removes stale entries (and `-register` no longer reads uninitialised options), a second copy in the same Explorer no longer hooks menus, and `shell -status` lists any `conflicts`. The app shows a warning with a one-click fix. Already affected? Installing this build repairs it; otherwise use *Fix* on the app's Home page, or [`scripts/repair-stale-registration.ps1`](https://github.com/Abdullah-Masood-05/Shell/blob/main/scripts/repair-stale-registration.ps1) `-Fix` as administrator.
- **Taskbar menu showed only Terminal and Go To.** `this.count` counted every configured item instead of the menu's own Windows items, which hid the taskbar block (Apps, Windows, Desktop, Settings, Task Manager, Taskbar settings). Fixed.
- **Menu items with no title** (for example *File manage*, and Apps / Windows / Task Manager on the taskbar) when `shell.nss` has no `import lang` block. A `loc.*` title that no language file defines now falls back to English, then to a readable name, so a title is never empty. `shell -config:init` adds the localization block to an older `shell.nss` (the original is kept as `shell.nss.old`), and `shell -config:check <file> -titles:<context>` lists the evaluated titles.
- **Installer:** installing a build of the same version over another (for example beta.2 over beta.1) now upgrades it and replaces `shell.exe`, `shell.dll` and the app instead of keeping the old files.
- `shell.log` is written to `%LOCALAPPDATA%\Nilesoft\Shell` when the program folder isn't writable, so errors are visible again.
- First-run setup: the *Pick a look* step works (Auto, Modern, Classic, White, Black), and it no longer offers a per-user registration on top of an all-users one.
- One app: a single Start menu entry, *Nilesoft Shell Pro* (`NilesoftShellPro.exe`); the separate *Nilesoft Shell Settings* entry is gone.
- The submenu-delay switch can turn off the shipped delay; links to a config line jump to that line; folder icons are visible in the dark theme; picking the saved value no longer counts as a change.

## Highlights

**Security**
- Registration no longer widens the access rights on the program folder or gives the Users group full control of the Windows 11 context-menu CLSID key. `scripts/repair-treatas-acl.ps1` repairs machines where an earlier build already did.
- Windows' own DLLs are no longer loaded through the DLL search path; `shell.dll` and `shell.exe` are built with Control Flow Guard.

**Stability and menu fixes**
- COM is no longer initialised from `DllMain`; import nesting is bounded, so a self-importing config can't crash Explorer.
- The configuration reloads when `shell.nss` or any import changes, and a broken config is retried once it has been saved again.
- Fixes for COM reference counting, registry and UTF-8 handling, path buffers, duplicate removal, orphan separators, lazily filled system submenus (Send to, New), negative monitor coordinates and per-window DPI.
- Shell no longer changes your selection-fade setting or Explorer's working directory. The submenu delay is one visible line in `shell.nss` (`showdelay = 200`), and the app can turn it off.

**Per-user mode (no admin)**
- `shell -register -user [-treat]` registers under `HKEY_CURRENT_USER`, including the Windows 11 classic-menu redirect.
- `%APPDATA%\Nilesoft\Shell\shell.nss` is used when it exists (`shell -config:init` creates it).
- `-restart` restarts Explorer for the current session only; `-status` and `-config:check` report the state as JSON.

**The Nilesoft Shell Pro app** (`NilesoftShellPro.exe`, new)
- The one Start menu entry, *Nilesoft Shell Pro*; double-clicking `shell.exe` opens it too. Checks the installation, enables Shell for you or for all users, edits the configuration and restores backups. A single self-contained exe: no runtime DLLs and no VC++ Redistributable.

**Setup**
- Upgrading no longer deletes your `shell.nss`; all language files are installed and picked from your Windows locale.
- New default menus (Develop, Copy, New file, System) from `imports/develop.nss` and `imports/my-tools.nss`.

## Install

| Download | For | Notes |
| --- | --- | --- |
| `setup-x64.msi` | Windows x64 | Per-machine install to `C:\Program Files\Nilesoft Shell`. Needs administrator rights once. Includes the app. |
| `setup-arm64.msi` | Windows on ARM | As above; the app is the x64 build and runs under emulation. |
| `setup-x86.msi` | 32-bit Windows | No app; the Start menu entry opens `shell.exe`. |
| `NilesoftShellPro-{{VERSION}}-x64-portable.zip` | Windows x64, one user | No administrator rights. Extract to e.g. `%LOCALAPPDATA%\Programs\NilesoftShellPro`, then run `NilesoftShellPro.exe`, or `shell.exe -register -user -treat` and `shell.exe -restart`. See `PORTABLE.txt` in the zip. |

The MSI uses the same upgrade code as Nilesoft Shell, so it upgrades an existing Nilesoft Shell installation in place (same folder, your `shell.nss` is kept). It appears as "Nilesoft Shell Pro" in Installed apps. Installing a later upstream Nilesoft Shell release over it replaces it the same way.

Verify downloads against `SHA256SUMS.txt`:

```powershell
Get-FileHash .\setup-x64.msi -Algorithm SHA256
```

## Unsigned binaries

The MSIs, `shell.exe`, `shell.dll` and `NilesoftShellPro.exe` are **not code-signed**. Windows SmartScreen may show "Windows protected your PC" when you run the installer or `NilesoftShellPro.exe`; choose *More info*, then *Run anyway*, if you trust this release. Check the SHA-256 hash first.

## Known limitations / not yet field-tested

Be careful with this build on a machine you depend on.

- **Per-user registration** (`-register -user`) is covered by our own testing of the registry it writes, but has not yet been exercised by us in a live Explorer session across sign-out / sign-in and multiple users.
- **The Windows 11 per-user TreatAs redirect** (`-register -user -treat`, the classic menu for one user) has not been exercised in a live Windows 11 Explorer by us. If the modern menu stays, register for all users with the MSI instead.
- **Automatic config reload** (when `shell.nss` or an import changes) has not been exercised in a live Explorer by us; if a change doesn't show, run `shell.exe -restart`.
- The app is new: treat it as a preview. It snapshots the configuration to `%APPDATA%\Nilesoft\Shell\backups` before every save.
- If both the MSI (all users) and a per-user registration exist, the per-user one wins for that user.
- `NilesoftShellPro.exe` is x64 only; there is no 32-bit build of the app.

## Known issues / upgrading from a manual install

If Nilesoft Shell was installed by hand before (for example copied to `C:\Windows\nilesoft` and registered from there), 2.0.0-beta.1 left one of the old registrations in place: the folder-extensions class `{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}` kept pointing at the old `shell.dll`. Explorer then loads both DLLs and right-click shows **no menu at all**, even though `shell.exe -status` reports Shell as registered.

- **This release repairs it during setup.** The MSI runs `shell.exe -register`, which now repoints or removes every Shell CLSID (context menu, icon overlay, folder extensions) that names a different `shell.dll`, and logs each repair. `-unregister` removes all three and their `FolderExtensions` / overlay entries. The old files are not deleted; they just stop being registered. Restart Explorer (or sign out and back in) afterwards so the old DLL is unloaded.
- **Already affected?** Run `scripts\repair-stale-registration.ps1` to see what is stale, then the same with `-Fix` from an elevated PowerShell (`-Fix -WhatIf` previews), and restart Explorer.
- `shell.exe -status` now lists stale registrations under `"conflicts"`, the Nilesoft `shell.dll` files loaded in Explorer under `"loadedInExplorer"`, and the log file under `"log"`.
- A second copy of `shell.dll` in the same process no longer installs its hooks a second time; it logs a warning instead. (An old upstream DLL loaded first keeps the menu until the registration is repaired.)
- `shell.dll` installed in Program Files cannot write `shell.log` next to itself from Explorer, so it now logs to `%LOCALAPPDATA%\Nilesoft\Shell\shell.log`. The app reads that file.

Report problems at https://github.com/Abdullah-Masood-05/Shell/issues and include the output of `shell.exe -status`.

## Credits

Nilesoft Shell is created by Moudey and Nilesoft and released under the MIT License. Many fixes were ported, with credit in each commit, from the opj161, TCNOco, arvaidasre and iMAboud forks.
