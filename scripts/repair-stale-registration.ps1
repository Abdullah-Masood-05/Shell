<#
.SYNOPSIS
    Finds, and with -Fix repairs, Nilesoft Shell registrations that still point
    at an old copy of shell.dll.
.DESCRIPTION
    Shell registers up to three COM classes, all served by shell.dll:

        {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}  context menu
        {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF2}  icon overlay
        {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}  folder extensions
                                                (Drive\shellex\FolderExtensions)

    Installing the MSI over a manual install (for example one in
    C:\Windows\nilesoft) repointed the first two at the new shell.dll but left
    the third naming the old one. Explorer then loads both DLLs, both hook the
    same menu functions, and right-click shows no menu at all. Builds after
    2.0.0-beta.1 repair this themselves when they register (the MSI runs
    shell.exe -register); this script is for machines that are already
    affected.

    Without -Fix it only reports: every registration of the three CLSIDs (HKLM,
    HKCU and their WOW6432Node views) whose DLL is not the one in the install
    folder, plus the Nilesoft shell.dll files currently loaded in Explorer.

    With -Fix (elevated):
      * machine context menu / icon overlay: repointed to the install folder's
        shell.dll (removed if that file is missing)
      * machine folder extensions: removed, with its
        Drive\shellex\FolderExtensions entry (current builds do not use it)
      * per-user icon overlay / folder extensions: removed
      * per-user context menu: removed only if its DLL no longer exists (a
        working per-user registration of another copy is reported, not touched)
      * WOW6432Node (32-bit view) copies: removed
    Files are never deleted: an old C:\Windows\nilesoft folder stays on disk, it
    just stops being registered. Explorer is not restarted; do that afterwards.

    Supports -WhatIf and -Confirm. Safe to run more than once.
.PARAMETER InstallDir
    The folder whose shell.dll is the right one. Default: the folder of the
    machine context-menu registration if that shell.dll exists, else
    "$env:ProgramFiles\Nilesoft Shell".
.PARAMETER Fix
    Repair what the report lists. Needs an elevated PowerShell for HKLM.
.EXAMPLE
    .\scripts\repair-stale-registration.ps1
.EXAMPLE
    .\scripts\repair-stale-registration.ps1 -Fix -WhatIf
.EXAMPLE
    .\scripts\repair-stale-registration.ps1 -Fix
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir,
    [switch]$Fix
)

$ErrorActionPreference = 'Stop'

$Roles = [ordered]@{
    '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}' = 'contextMenu'
    '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF2}' = 'iconOverlay'
    '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}' = 'folderExtensions'
}
$FolderExtensionsKey = 'Software\Classes\Drive\shellex\FolderExtensions\{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}'
$OverlayKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers\ @nilesoft.shell'

# Always the 64-bit registry view, even from a 32-bit PowerShell; the
# WOW6432Node copies are addressed by name.
$View = [Microsoft.Win32.RegistryView]::Registry64
$Hives = @{
    HKLM = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $View)
    HKCU = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $View)
}
$Stores = @(
    @{ Hive = 'HKLM'; Classes = 'Software\Classes';              Scope = 'machine' }
    @{ Hive = 'HKCU'; Classes = 'Software\Classes';              Scope = 'user' }
    @{ Hive = 'HKLM'; Classes = 'Software\Classes\WOW6432Node';  Scope = 'machineWow64' }
    @{ Hive = 'HKCU'; Classes = 'Software\Classes\WOW6432Node';  Scope = 'userWow64' }
)

function Get-ServerPath([string]$hive, [string]$classes, [string]$clsid) {
    $key = $Hives[$hive].OpenSubKey("$classes\CLSID\$clsid\InprocServer32")
    if (-not $key) { return $null }
    try {
        $raw = $key.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    } finally { $key.Close() }
    if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
    return [Environment]::ExpandEnvironmentVariables([string]$raw).Trim().Trim('"')
}

function Test-SamePath([string]$a, [string]$b) {
    if (-not $a -or -not $b) { return $false }
    try {
        return [IO.Path]::GetFullPath($a).TrimEnd('\') -ieq [IO.Path]::GetFullPath($b).TrimEnd('\')
    } catch { return $a -ieq $b }
}

function Remove-Key([string]$hive, [string]$subkey) {
    $root = $Hives[$hive]
    $key = $root.OpenSubKey($subkey)
    if (-not $key) { return }
    $key.Close()
    if ($PSCmdlet.ShouldProcess("$hive\$subkey", 'Delete registry key')) {
        $root.DeleteSubKeyTree($subkey, $false)
        Write-Output "  removed $hive\$subkey"
    }
}

function Set-Server([string]$hive, [string]$classes, [string]$clsid, [string]$dll) {
    $subkey = "$classes\CLSID\$clsid\InprocServer32"
    if ($PSCmdlet.ShouldProcess("$hive\$subkey", "Set default value to '$dll'")) {
        $key = $Hives[$hive].CreateSubKey($subkey)
        try {
            $key.SetValue('', $dll, [Microsoft.Win32.RegistryValueKind]::String)
            $key.SetValue('ThreadingModel', 'Apartment', [Microsoft.Win32.RegistryValueKind]::String)
        } finally { $key.Close() }
        Write-Output "  repointed $hive\$subkey -> $dll"
    }
}

# --- Which shell.dll is the right one -------------------------------------
if (-not $InstallDir) {
    $machine = Get-ServerPath 'HKLM' 'Software\Classes' '{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}'
    if ($machine -and (Test-Path -LiteralPath $machine -PathType Leaf)) {
        $InstallDir = Split-Path -Parent $machine
    } else {
        $InstallDir = Join-Path $env:ProgramFiles 'Nilesoft Shell'
    }
}
$Dll = Join-Path $InstallDir 'shell.dll'
$DllExists = Test-Path -LiteralPath $Dll -PathType Leaf

Write-Output "Install folder: $InstallDir"
Write-Output ("shell.dll:      {0} ({1})" -f $Dll, $(if ($DllExists) { 'present' } else { 'MISSING' }))
Write-Output ''

# --- Report -----------------------------------------------------------------
$stale = @()
foreach ($store in $Stores) {
    foreach ($clsid in $Roles.Keys) {
        $server = Get-ServerPath $store.Hive $store.Classes $clsid
        if (-not $server -or (Test-SamePath $server $Dll)) { continue }
        $stale += [pscustomobject]@{
            Role    = $Roles[$clsid]
            Clsid   = $clsid
            Scope   = $store.Scope
            Hive    = $store.Hive
            Classes = $store.Classes
            Dll     = $server
            Exists  = (Test-Path -LiteralPath $server -PathType Leaf)
        }
    }
}

if ($stale.Count -eq 0) {
    Write-Output 'No stale registrations: every Nilesoft Shell CLSID points at the install folder.'
} else {
    Write-Output 'Stale registrations (CLSID naming another shell.dll):'
    $stale | Format-Table Role, Scope, Clsid, Dll, Exists -AutoSize | Out-String -Width 200 |
        ForEach-Object { $_.TrimEnd() } | Write-Output
}

$loaded = @()
foreach ($p in @(Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
    try {
        $loaded += $p.Modules | Where-Object { $_.ModuleName -ieq 'shell.dll' } | ForEach-Object { $_.FileName }
    } catch { }
}
$loaded = @($loaded | Sort-Object -Unique)
Write-Output ''
if ($loaded.Count -eq 0) {
    Write-Output 'Loaded in Explorer: no shell.dll'
} else {
    Write-Output 'Loaded in Explorer:'
    foreach ($l in $loaded) {
        Write-Output ("  {0}{1}" -f $l, $(if (Test-SamePath $l $Dll) { '' } else { '   <- not the install folder''s copy' }))
    }
}

if ($stale.Count -eq 0) { return }
if (-not $Fix) {
    Write-Output ''
    Write-Output 'Run again with -Fix (elevated) to repair, or -Fix -WhatIf to preview.'
    return
}

# --- Repair -----------------------------------------------------------------
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated -and ($stale | Where-Object Hive -eq 'HKLM') -and -not $WhatIfPreference) {
    throw 'The machine (HKLM) entries need an elevated PowerShell: run it as administrator and try again.'
}

Write-Output ''
Write-Output 'Repairing:'
$changed = $false
foreach ($s in $stale) {
    $clsidKey = "$($s.Classes)\CLSID\$($s.Clsid)"
    switch ($s.Scope) {
        'machine' {
            if ($s.Role -eq 'folderExtensions') {
                Remove-Key 'HKLM' $FolderExtensionsKey
                Remove-Key 'HKLM' $clsidKey
            } elseif ($DllExists) {
                Set-Server 'HKLM' $s.Classes $s.Clsid $Dll
            } else {
                if ($s.Role -eq 'iconOverlay') { Remove-Key 'HKLM' $OverlayKey }
                Remove-Key 'HKLM' $clsidKey
            }
        }
        'user' {
            if ($s.Role -eq 'contextMenu' -and $s.Exists) {
                Write-Output "  left alone: the per-user context menu registration of $($s.Dll)."
                Write-Output "    To remove it, run that copy's shell.exe -unregister -user."
            } else {
                if ($s.Role -eq 'folderExtensions') { Remove-Key 'HKCU' $FolderExtensionsKey }
                if ($s.Role -eq 'iconOverlay') { Remove-Key 'HKCU' $OverlayKey }
                Remove-Key 'HKCU' $clsidKey
                $changed = $true
            }
        }
        default {
            Remove-Key $s.Hive $clsidKey
        }
    }
    if ($s.Scope -ne 'user') { $changed = $true }
}

if ($changed -and -not $WhatIfPreference) {
    Write-Output ''
    Write-Output 'Done. Restart Explorer (Task Manager > Windows Explorer > Restart, or sign out and back in)'
    Write-Output 'so the old shell.dll is unloaded. The old files themselves were not deleted.'
}
