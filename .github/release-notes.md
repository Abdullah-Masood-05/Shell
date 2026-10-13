**Nilesoft Shell Pro {{VERSION}}** is a community fork of [Nilesoft Shell](https://github.com/moudey/Shell) by Moudey and Nilesoft: the same context-menu extension and `.nss` configuration language, with security and stability fixes, a per-user mode that needs no administrator rights, and an app to set it up and configure it. It is not an official Nilesoft release.

Documentation: https://abdullah-masood-05.github.io/Shell/ (see *About this fork* for the full list of changes).

## Highlights

**Security**
- Registration no longer widens the access rights on the program folder or gives the Users group full control of the Windows 11 context-menu CLSID key. `scripts/repair-treatas-acl.ps1` repairs machines where an earlier build already did.
- Windows' own DLLs are no longer loaded through the DLL search path; `shell.dll` and `shell.exe` are built with Control Flow Guard.

**Stability and menu fixes**
- COM is no longer initialised from `DllMain`; import nesting is bounded, so a self-importing config can't crash Explorer.
- The configuration reloads when `shell.nss` or any import changes, and a broken config is retried once it has been saved again.
- Fixes for COM reference counting, registry and UTF-8 handling, path buffers, duplicate removal, orphan separators, lazily filled system submenus (Send to, New), negative monitor coordinates and per-window DPI.
- Shell no longer changes your submenu delay, selection-fade setting or Explorer's working directory.

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
