<#
.SYNOPSIS
    Re-extracts the app's translatable strings and merges them into every language.

.DESCRIPTION
    Nilesoft Shell Pro (src/settings) keeps its translations in
    src/settings/translations/<lang>/LC_MESSAGES/nilesoft-shell-pro.po, with the template
    nilesoft-shell-pro.pot next to the language folders. This script:

      1. runs slint-tr-extractor over ui/**/*.slint (the @tr() strings),
      2. finds nsp::tr / trc / trf / trn / mark calls in src/*.cpp and src/*.h,
      3. writes the combined template (.pot),
      4. merges it into each .po: translations of strings that still exist are kept,
         new strings are added untranslated, and strings no longer in the app are kept
         at the end as obsolete (#~) entries so a translation can be brought back.

    Run it after changing UI text, then translate the empty msgstr "" entries.
    A new language: create translations/<code>/LC_MESSAGES/ and run with -NewLanguage.

    slint-tr-extractor is Slint's tools/tr-extractor. Install it with
    `cargo install slint-tr-extractor --version 1.18.1`, or build it from the Slint source
    tree (`cargo build --release -p slint-tr-extractor`) and pass -Extractor.

.PARAMETER Extractor
    Path to slint-tr-extractor.exe. Default: PATH, then D:\Projects\_deps\bin.

.PARAMETER NewLanguage
    Also create the .po for this code (for example de or ja), copied from the template.

.PARAMETER NativeName
    The language's own name for -NewLanguage, shown in Preferences (for example Deutsch).

.PARAMETER Import
    .po files with translations to take over, for example one a translator sent back.
    Each file's "Language:" header picks the language; its translated entries replace
    the ones in that language's .po.

.PARAMETER Check
    Only report whether the template is up to date; exit 1 when it isn't (for CI).

.EXAMPLE
    pwsh scripts/update-translations.ps1

.EXAMPLE
    pwsh scripts/update-translations.ps1 -NewLanguage ja -NativeName "日本語"
#>
[CmdletBinding()]
param(
    [string]$Extractor,
    [string]$NewLanguage,
    [string]$NativeName,
    [switch]$Check,
    [string[]]$Import
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$domain = 'nilesoft-shell-pro'
$appDir = Join-Path $PSScriptRoot '..\src\settings' | Resolve-Path | Select-Object -ExpandProperty Path
$trDir = Join-Path $appDir 'translations'
$potPath = Join-Path $trDir "$domain.pot"
$utf8 = [System.Text.UTF8Encoding]::new($false)

# --- the extractor --------------------------------------------------------------------
if (-not $Extractor) {
    $cmd = Get-Command slint-tr-extractor -ErrorAction SilentlyContinue
    if ($cmd) { $Extractor = $cmd.Source }
    elseif (Test-Path 'D:\Projects\_deps\bin\slint-tr-extractor.exe') { $Extractor = 'D:\Projects\_deps\bin\slint-tr-extractor.exe' }
    else { throw 'slint-tr-extractor not found: install it (cargo install slint-tr-extractor --version 1.18.1) or pass -Extractor.' }
}

# --- .po reading and writing ------------------------------------------------------------
# Strings stay in their escaped .po form (\n, \", \\), which is also how they appear in
# C++ and .slint string literals, so no unescaping is needed anywhere.

function New-Entry {
    [pscustomobject]@{
        Ctx = $null; Id = ''; Plural = $null; Str = [System.Collections.Generic.List[string]]::new()
        Comments = [System.Collections.Generic.List[string]]::new()  # "# " translator comments
        Extracted = [System.Collections.Generic.List[string]]::new() # "#." comments
        Refs = [System.Collections.Generic.List[string]]::new()      # "#:" references
        Flags = [System.Collections.Generic.List[string]]::new()     # "#," flags
        Obsolete = $false
    }
}

function Get-Key($e) { if ($null -ne $e.Ctx) { "$($e.Ctx)`u{4}$($e.Id)" } else { $e.Id } }

function Read-Po([string]$path) {
    $entries = [System.Collections.Generic.List[object]]::new()
    $cur = New-Entry; $any = $false; $target = $null
    $flush = {
        if ($script:any) { $entries.Add($script:cur) }
        $script:cur = New-Entry; $script:any = $false; $script:target = $null
    }
    $script:cur = $cur; $script:any = $any; $script:target = $target
    foreach ($raw in [System.IO.File]::ReadAllLines($path, $utf8)) {
        $line = $raw.Trim()
        if ($line -eq '') { & $flush; continue }
        $obsolete = $false
        if ($line.StartsWith('#~')) { $obsolete = $true; $line = $line.Substring(2).Trim(); if ($line -eq '') { continue } }
        if ($line.StartsWith('#')) {
            if ($script:any -and $script:cur.Str.Count -gt 0) { & $flush }
            if ($line.StartsWith('#:')) { $script:cur.Refs.AddRange([string[]]($line.Substring(2).Trim() -split '\s+' | Where-Object { $_ })) }
            elseif ($line.StartsWith('#,')) { $script:cur.Flags.AddRange([string[]]($line.Substring(2).Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })) }
            elseif ($line.StartsWith('#.')) { $script:cur.Extracted.Add($line.Substring(2).Trim()) }
            elseif ($line.StartsWith('#|')) { }
            else { $script:cur.Comments.Add($line.Substring(1).TrimStart()) }
            $script:any = $true
            continue
        }
        if ($line -notmatch '^(msgctxt|msgid_plural|msgid|msgstr(?:\[(\d+)\])?)?\s*"(.*)"$') {
            # slint-tr-extractor copies the // comment above a @tr() without a "#." prefix.
            if (-not $obsolete -and -not $line.StartsWith('msg') -and -not $line.StartsWith('"')) {
                if ($script:any -and $script:cur.Str.Count -gt 0) { & $flush }
                $script:cur.Extracted.Add($line); $script:any = $true
                continue
            }
            throw "${path}: cannot read line: $raw"
        }
        $kw = $Matches[1]; $value = $Matches[3]; $index = if ($Matches[2]) { [int]$Matches[2] } else { 0 }
        if (-not $kw) {
            if ($null -eq $script:target) { throw "${path}: string without keyword: $raw" }
            switch ($script:target) {
                'ctx' { $script:cur.Ctx += $value }
                'id' { $script:cur.Id += $value }
                'plural' { $script:cur.Plural += $value }
                default { $script:cur.Str[[int]$script:target] += $value }
            }
            continue
        }
        if (($kw -eq 'msgctxt' -or $kw -eq 'msgid') -and $script:cur.Str.Count -gt 0) { & $flush }
        if ($obsolete) { $script:cur.Obsolete = $true }
        $script:any = $true
        switch -regex ($kw) {
            '^msgctxt$' { $script:cur.Ctx = $value; $script:target = 'ctx' }
            '^msgid$' { $script:cur.Id = $value; $script:target = 'id' }
            '^msgid_plural$' { $script:cur.Plural = $value; $script:target = 'plural' }
            '^msgstr' {
                while ($script:cur.Str.Count -le $index) { $script:cur.Str.Add('') }
                $script:cur.Str[$index] = $value; $script:target = "$index"
            }
        }
    }
    & $flush
    return , $entries
}

function Format-Entry($e, [bool]$withRefs = $true) {
    $sb = [System.Text.StringBuilder]::new()
    $p = if ($e.Obsolete) { '#~ ' } else { '' }
    foreach ($c in $e.Comments) { [void]$sb.Append("# $c`n") }
    foreach ($c in $e.Extracted) { [void]$sb.Append("#. $c`n") }
    if ($withRefs -and $e.Refs.Count -and -not $e.Obsolete) { [void]$sb.Append("#: $($e.Refs -join ' ')`n") }
    if ($e.Flags.Count) { [void]$sb.Append("#, $($e.Flags -join ', ')`n") }
    if ($null -ne $e.Ctx) { [void]$sb.Append("${p}msgctxt `"$($e.Ctx)`"`n") }
    [void]$sb.Append("${p}msgid `"$($e.Id)`"`n")
    if ($null -ne $e.Plural) {
        [void]$sb.Append("${p}msgid_plural `"$($e.Plural)`"`n")
        for ($i = 0; $i -lt [Math]::Max(1, $e.Str.Count); $i++) {
            $v = if ($i -lt $e.Str.Count) { $e.Str[$i] } else { '' }
            [void]$sb.Append("${p}msgstr[$i] `"$v`"`n")
        }
    } else {
        $v = if ($e.Str.Count) { $e.Str[0] } else { '' }
        if ($e.Id -eq '' -and $null -eq $e.Ctx) {
            # The header: one "Field: value\n" per line, like msgmerge writes it.
            [void]$sb.Append("msgstr `"`"`n")
            foreach ($f in ($v -split '(?<=\\n)' | Where-Object { $_ })) { [void]$sb.Append("`"$f`"`n") }
        } else {
            [void]$sb.Append("${p}msgstr `"$v`"`n")
        }
    }
    return $sb.ToString()
}

function Format-Header([string[]]$fields, [string[]]$comments) {
    $s = ($comments | ForEach-Object { "# $_`n" }) -join ''
    $s += "msgid `"`"`nmsgstr `"`"`n"
    foreach ($f in $fields) { $s += "`"$f\n`"`n" }
    return $s
}

# --- 1. the .slint strings ---------------------------------------------------------------
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "nsp-tr-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    $slintPot = Join-Path $tmp 'slint.pot'
    Push-Location $appDir
    try {
        $files = Get-ChildItem -Recurse -Filter *.slint ui | Sort-Object FullName |
            ForEach-Object { [System.IO.Path]::GetRelativePath($appDir, $_.FullName).Replace('\', '/') }
        & $Extractor --no-default-translation-context -d $domain --package-name 'Nilesoft Shell Pro' -o $slintPot @files
        if ($LASTEXITCODE -ne 0) { throw "slint-tr-extractor failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
    $slintEntries = Read-Po $slintPot
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# --- 2. the C++ strings -----------------------------------------------------------------
$lit = '"((?:[^"\\\r\n]|\\.)*)"'
$cppRx = [regex]::new("\bnsp::(tr|trc|trf|trn|mark)\(\s*$lit(?:\s*,\s*$lit)?")
$cppEntries = [System.Collections.Generic.List[object]]::new()
foreach ($f in Get-ChildItem (Join-Path $appDir 'src') -File -Include *.cpp, *.h -Recurse | Sort-Object FullName) {
    $rel = [System.IO.Path]::GetRelativePath($appDir, $f.FullName).Replace('\', '/')
    if ($rel -eq 'src/Tr.h') { continue } # the API description
    $text = [System.IO.File]::ReadAllText($f.FullName, $utf8)
    foreach ($m in $cppRx.Matches($text)) {
        $line = ($text.Substring(0, $m.Index) -split "`n").Count
        $e = New-Entry
        switch ($m.Groups[1].Value) {
            'trc' {
                if (-not $m.Groups[3].Success) { throw "${rel}:${line}: nsp::trc needs a context and a string literal" }
                $e.Ctx = $m.Groups[2].Value; $e.Id = $m.Groups[3].Value
            }
            'trn' {
                if (-not $m.Groups[3].Success) { throw "${rel}:${line}: nsp::trn needs two string literals" }
                $e.Id = $m.Groups[2].Value; $e.Plural = $m.Groups[3].Value
            }
            default { $e.Id = $m.Groups[2].Value }
        }
        if ($e.Id -match '\\[xuU0-7]') { throw "${rel}:${line}: use the character itself, not a \x or \u escape, in translatable strings" }
        $e.Refs.Add("${rel}:$line")
        $cppEntries.Add($e)
    }
}

# --- 3. the template ---------------------------------------------------------------------
$template = [ordered]@{}
foreach ($e in @($slintEntries) + @($cppEntries)) {
    if ($e.Id -eq '' -and $null -eq $e.Ctx) { continue } # a header
    $k = Get-Key $e
    if ($template.Contains($k)) {
        $t = $template[$k]
        foreach ($r in $e.Refs) { if (-not $t.Refs.Contains($r)) { $t.Refs.Add($r) } }
        if ($null -eq $t.Plural -and $null -ne $e.Plural) { $t.Plural = $e.Plural }
    } else {
        $n = New-Entry
        $n.Ctx = $e.Ctx; $n.Id = $e.Id; $n.Plural = $e.Plural
        $n.Refs.AddRange([string[]]$e.Refs); $n.Extracted.AddRange([string[]]$e.Extracted)
        $template[$k] = $n
    }
}

$potHeader = Format-Header -comments @(
    'Nilesoft Shell Pro: strings of the app (src/settings).',
    'Generated by scripts/update-translations.ps1. Do not edit; edit the .po files.') -fields @(
    'Project-Id-Version: Nilesoft Shell Pro',
    'MIME-Version: 1.0',
    'Content-Type: text/plain; charset=UTF-8',
    'Content-Transfer-Encoding: 8bit',
    'Language: ',
    'X-Language-Name: ',
    'Plural-Forms: nplurals=2; plural=(n != 1);')
$pot = $potHeader + (($template.Values | ForEach-Object { "`n" + (Format-Entry $_) }) -join '')
$pot = $pot.Replace("`r`n", "`n")

$old = if (Test-Path $potPath) { [System.IO.File]::ReadAllText($potPath, $utf8) } else { '' }
if ($Check) {
    if ($old -ne $pot) { Write-Host "$potPath is out of date: run scripts/update-translations.ps1"; exit 1 }
    Write-Host "$potPath is up to date ($($template.Count) strings)"; exit 0
}
New-Item -ItemType Directory -Force $trDir | Out-Null
[System.IO.File]::WriteAllText($potPath, $pot, $utf8)
Write-Host "$($template.Count) strings ($(@($slintEntries).Count - 1) in .slint files, $($cppEntries.Count) C++ calls) -> $potPath"

# --- 4. merge into each language ---------------------------------------------------------
if ($NewLanguage) {
    $dir = Join-Path $trDir "$NewLanguage\LC_MESSAGES"
    $po = Join-Path $dir "$domain.po"
    if (-not (Test-Path $po)) {
        New-Item -ItemType Directory -Force $dir | Out-Null
        $name = if ($NativeName) { $NativeName } else { $NewLanguage }
        $h = Format-Header -comments @("Nilesoft Shell Pro: $name translation.") -fields @(
            'Project-Id-Version: Nilesoft Shell Pro',
            'MIME-Version: 1.0',
            'Content-Type: text/plain; charset=UTF-8',
            'Content-Transfer-Encoding: 8bit',
            "Language: $NewLanguage",
            "X-Language-Name: $name",
            'Plural-Forms: nplurals=2; plural=(n != 1);')
        [System.IO.File]::WriteAllText($po, $h, $utf8)
        Write-Host "created $po (check Plural-Forms for the language)"
    }
}

# -Import: translated entries by language and key.
$imported = @{}
foreach ($file in $Import) {
    $entries = Read-Po (Resolve-Path $file).Path
    $h = $entries | Where-Object { $_.Id -eq '' -and $null -eq $_.Ctx } | Select-Object -First 1
    if (-not $h -or -not $h.Str.Count -or $h.Str[0] -notmatch 'Language: *([A-Za-z_-]+)') { throw "${file}: no Language: header" }
    $lang = $Matches[1].Replace('_', '-')
    if (-not $imported[$lang]) { $imported[$lang] = @{} }
    foreach ($e in $entries) {
        if ($e -ne $h -and -not $e.Obsolete -and $e.Str.Count -and $e.Str[0] -ne '' -and -not $e.Flags.Contains('fuzzy')) {
            $imported[$lang][(Get-Key $e)] = $e
        }
    }
}

foreach ($po in Get-ChildItem $trDir -Directory | ForEach-Object { Join-Path $_.FullName "LC_MESSAGES\$domain.po" } | Where-Object { Test-Path $_ }) {
    $lang = Split-Path (Split-Path (Split-Path $po)) -Leaf
    $fromImport = if ($imported[$lang]) { $imported[$lang] } else { @{} }
    $existing = Read-Po $po
    $header = $existing | Where-Object { $_.Id -eq '' -and $null -eq $_.Ctx -and -not $_.Obsolete } | Select-Object -First 1
    $nplurals = 2
    if ($header -and $header.Str.Count -and $header.Str[0] -match 'nplurals=(\d+)') { $nplurals = [int]$Matches[1] }
    $byKey = @{}
    foreach ($e in $existing) { if ($e -ne $header) { $byKey[(Get-Key $e)] = $e } }

    $sb = [System.Text.StringBuilder]::new()
    if ($header) { [void]$sb.Append((Format-Entry $header $false)) }
    $done = 0
    foreach ($k in $template.Keys) {
        $t = $template[$k]
        $n = New-Entry
        $n.Ctx = $t.Ctx; $n.Id = $t.Id; $n.Plural = $t.Plural
        $n.Refs.AddRange([string[]]$t.Refs); $n.Extracted.AddRange([string[]]$t.Extracted)
        $prev = $byKey[$k]
        if ($prev) {
            $n.Comments.AddRange([string[]]$prev.Comments)
            $n.Flags.AddRange([string[]]$prev.Flags)
            $n.Str.AddRange([string[]]$prev.Str)
            $byKey.Remove($k)
        }
        if ($fromImport[$k]) {
            $n.Str.Clear(); $n.Str.AddRange([string[]]$fromImport[$k].Str)
            $n.Flags.Remove('fuzzy') | Out-Null
        }
        if ($null -ne $n.Plural) { while ($n.Str.Count -lt $nplurals) { $n.Str.Add('') } }
        elseif ($n.Str.Count -eq 0) { $n.Str.Add('') }
        if ($n.Str[0] -ne '' -and -not $n.Flags.Contains('fuzzy')) { $done++ }
        [void]$sb.Append("`n" + (Format-Entry $n))
    }
    # Strings the app no longer has: kept as obsolete, translated ones only.
    foreach ($e in $byKey.Values | Where-Object { $_.Str.Count -and $_.Str[0] -ne '' }) {
        $e.Obsolete = $true
        [void]$sb.Append("`n" + (Format-Entry $e $false))
    }
    [System.IO.File]::WriteAllText($po, $sb.ToString().Replace("`r`n", "`n"), $utf8)
    Write-Host ("{0,-6} {1}/{2} translated" -f $lang, $done, $template.Count)
}
