<#
.SYNOPSIS
    Repairs the ACL on the Windows 11 context-menu CLSID key.
.DESCRIPTION
    Older Shell builds, when registering with -treat, fell back on
    ERROR_ACCESS_DENIED to taking ownership of

        HKLM\SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}

    and granting BUILTIN\Users GENERIC_ALL with inheritance - permanently, and
    inherited by the TreatAs subkey underneath it. That key decides which
    handler owns the Windows 11 Explorer context menu, so on an affected machine
    any unprivileged account can repoint TreatAs or InprocServer32 at a DLL of
    its own and have every other user's Explorer load it, administrators
    included.

    Current builds borrow only the access they need and put the owner and DACL
    back, but a machine that ever ran an affected build keeps the widened DACL
    until something restores it. This script does that:

      * every explicit BUILTIN\Users ACE is removed and replaced with ReadKey,
        which is what a stock Windows CLSID key grants
      * ownership returns to NT SERVICE\TrustedInstaller
      * the TreatAs value is left alone, so Shell keeps working

    Requires elevation. Safe to run more than once.
.PARAMETER WhatIfOnly
    Report the current state and change nothing.
.EXAMPLE
    .\scripts\repair-treatas-acl.ps1 -WhatIfOnly
.EXAMPLE
    .\scripts\repair-treatas-acl.ps1
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param([switch]$WhatIfOnly)

$ErrorActionPreference = 'Stop'
$key  = 'SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}'
$path = "Registry::HKEY_LOCAL_MACHINE\$key"

$users = New-Object System.Security.Principal.SecurityIdentifier(
    [System.Security.Principal.WellKnownSidType]::BuiltinUsersSid, $null)

# Anything beyond ReadKey / GENERIC_READ granted to Users counts as widened.
$readOnly = [int]([System.Security.AccessControl.RegistryRights]::ReadKey) -bor [int]0x80000000

function Get-UsersAces([string]$p) {
    (Get-Acl $p).GetAccessRules($true, $true, [System.Security.Principal.SecurityIdentifier]) |
        Where-Object { $_.IdentityReference -eq $users -and $_.AccessControlType -eq 'Allow' }
}

function Test-Widened([string]$p) {
    [bool](Get-UsersAces $p | Where-Object { ([int]$_.RegistryRights -band (-bnot $readOnly)) -ne 0 })
}

function Show-State([string]$label) {
    $acl = Get-Acl $path
    Write-Output $label
    Write-Output "  owner: $($acl.Owner)"
    Get-UsersAces $path | ForEach-Object {
        "    Users: {0,-14} inherited={1} flags={2}" -f $_.RegistryRights, $_.IsInherited, $_.InheritanceFlags
    } | ForEach-Object { Write-Output $_ }
    foreach ($sub in 'TreatAs', 'InProcServer32') {
        if (Test-Path "$path\$sub") {
            Write-Output ("  {0}: Users write access = {1}" -f $sub, $(if (Test-Widened "$path\$sub") { 'PRESENT' } else { 'none' }))
        }
    }
}

if (-not (Test-Path $path)) {
    Write-Output "Key not present: HKLM\$key - nothing to repair."
    return
}

Show-State 'BEFORE'

if ($WhatIfOnly) { return }

# --- 1. Narrow BUILTIN\Users back to read ---------------------------------
# PurgeAccessRules drops every explicit rule for this identity; SetSecurityInfo
# (under Set-Acl) re-propagates the inheritable ACEs to existing subkeys, so
# TreatAs loses the inherited write access as well.
$acl = Get-Acl $path
$acl.PurgeAccessRules($users)
$acl.AddAccessRule((New-Object System.Security.AccessControl.RegistryAccessRule(
    $users,
    [System.Security.AccessControl.RegistryRights]::ReadKey,
    [System.Security.AccessControl.InheritanceFlags]::ContainerInherit,
    [System.Security.AccessControl.PropagationFlags]::None,
    [System.Security.AccessControl.AccessControlType]::Allow)))
Set-Acl -Path $path -AclObject $acl
Write-Output '  DACL updated: BUILTIN\Users narrowed to ReadKey'

# --- 2. Hand ownership back to TrustedInstaller ---------------------------
# Needs SeRestorePrivilege: an administrator can take ownership, but giving it
# to a principal they are not a member of is a restore operation.
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class ShellRepairPriv {
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] public struct TOKEN_PRIVILEGES { public uint Count; public LUID Luid; public uint Attr; }
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);
    [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool LookupPrivilegeValue(string sys, string name, out LUID luid);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TOKEN_PRIVILEGES np, uint len, IntPtr prev, IntPtr ret);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static bool Enable(string name) {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28 /* ADJUST_PRIVILEGES | QUERY */, out tok)) return false;
        try {
            LUID luid;
            if (!LookupPrivilegeValue(null, name, out luid)) return false;
            TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
            tp.Count = 1; tp.Luid = luid; tp.Attr = 0x2;
            if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero)) return false;
            return Marshal.GetLastWin32Error() == 0; // not ERROR_NOT_ALL_ASSIGNED
        } finally { CloseHandle(tok); }
    }
}
'@
if (-not ('ShellRepairPriv' -as [type])) { Add-Type -TypeDefinition $sig | Out-Null }

$okRestore = [ShellRepairPriv]::Enable('SeRestorePrivilege')
$okTake    = [ShellRepairPriv]::Enable('SeTakeOwnershipPrivilege')
Write-Output "  SeRestorePrivilege=$okRestore SeTakeOwnershipPrivilege=$okTake"

try {
    $acl = Get-Acl $path
    $acl.SetOwner((New-Object System.Security.Principal.NTAccount('NT SERVICE\TrustedInstaller')))
    Set-Acl -Path $path -AclObject $acl
    Write-Output '  owner restored to NT SERVICE\TrustedInstaller'
}
catch {
    Write-Output "  owner NOT restored: $($_.Exception.Message)"
    Write-Output '  (the DACL fix above is the part that matters)'
}

Show-State 'AFTER'

# The redirect must survive all of this.
$treat = (Get-ItemProperty "$path\TreatAs" -Name '(default)' -ErrorAction SilentlyContinue).'(default)'
Write-Output "  TreatAs value: $treat"
