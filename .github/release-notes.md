**Nilesoft Shell Pro {{VERSION}}** is a community fork of [Nilesoft Shell](https://github.com/moudey/Shell) by Moudey and Nilesoft: the same context-menu extension and `.nss` configuration language, with security and stability fixes, a per-user mode that needs no administrator rights, and an app to set it up and configure it. It is not an official Nilesoft release.

> [!CAUTION]
> **If the original Nilesoft Shell is installed, uninstall it before installing Nilesoft Shell Pro.** Pro has its own name and does not replace the original (installed with its own installer, winget, Scoop or Chocolatey, or a portable copy), so both stay registered with Explorer and collide: the context menu can stop appearing or show up twice. Uninstall it from *Settings > Apps > Installed apps*; for a portable copy run `shell -unregister -restart` from its folder and delete the folder. Back up your `shell.nss` and `imports` folder first if you want to keep your configuration. Updating from an earlier Nilesoft Shell Pro version is fine.

Documentation: https://abdullah-masood-05.github.io/Shell/. *About this fork* there lists everything this fork changes, and each earlier release lists its own changes.

## What's new in {{VERSION}}

{{CHANGES}}

## Install

| Download | For | Notes |
| --- | --- | --- |
| `setup-x64.msi` | Windows x64 | Per-machine install to `C:\Program Files\Nilesoft Shell`. Needs administrator rights once. Includes the app. |
| `setup-arm64.msi` | Windows on ARM | As above; the app is the x64 build and runs under emulation. |
| `setup-x86.msi` | 32-bit Windows | No app; the Start menu entry opens `shell.exe`. |
| `NilesoftShellPro-{{VERSION}}-x64-portable.zip` | Windows x64, one user | No administrator rights. Extract to e.g. `%LOCALAPPDATA%\Programs\NilesoftShellPro`, then run `NilesoftShellPro.exe`, or `shell.exe -register -user -treat` and `shell.exe -restart`. See `PORTABLE.txt` in the zip. |

The MSI upgrades an earlier Nilesoft Shell Pro installation in place (same folder, your `shell.nss` is kept) and appears as "Nilesoft Shell Pro" in Installed apps. It does not reliably replace the original Nilesoft Shell: uninstall that first (see the warning above).

Verify downloads against `SHA256SUMS.txt`:

```powershell
Get-FileHash .\setup-x64.msi -Algorithm SHA256
```

## Unsigned binaries

The MSIs, `shell.exe`, `shell.dll` and `NilesoftShellPro.exe` are **not code-signed**. Windows SmartScreen may show "Windows protected your PC" when you run the installer or `NilesoftShellPro.exe`; choose *More info*, then *Run anyway*, if you trust this release. Check the SHA-256 hash first.

Report problems at https://github.com/Abdullah-Masood-05/Shell/issues and include the output of `shell.exe -status`. Questions and ideas are welcome in [Discussions](https://github.com/Abdullah-Masood-05/Shell/discussions).

## Credits

Nilesoft Shell is created by Moudey and Nilesoft and released under the MIT License. Many fixes were ported, with credit in each commit, from the opj161, TCNOco, arvaidasre, iMAboud and iGerman00 forks and from pull requests to Nilesoft Shell by YESNLars, haoze-evolluling, xiaobsh, denismattos, v4806, jtabox, loliri and MissinLinkk05551.
