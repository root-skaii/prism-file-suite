# Keeps the translation files in sync with the code.
#   tools\lang.ps1            rewrites lang\template.lang from every tr("...") / tr_n("..","..",n) in src\, prints a status per language
#   tools\lang.ps1 -Update    also adds the new keys (empty) to each lang\*.lang and moves unused ones to the end
#   tools\lang.ps1 -New de -Name Deutsch [-Plural one-other|none|french|slavic]   starts lang\de.lang from the template
param([switch]$Update, [string]$New, [string]$Name, [string]$Plural = 'one-other')

$root = Split-Path $PSScriptRoot -Parent
$lang = Join-Path $root 'lang'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$lit  = '"((?:[^"\\]|\\.)*)"'

# key -> first file it is used in, in source order
$keys = New-Object System.Collections.Specialized.OrderedDictionary   # (case-sensitive: "done" and "Done" are two strings)
# C escapes the .lang format does not know: \xHH runs are utf-8 bytes
function Unescape([string]$s) {
    [regex]::Replace($s, '(?:\\x[0-9a-fA-F]{2})+', {
        param($m)
        $bytes = [regex]::Matches($m.Value, '[0-9a-fA-F]{2}') | ForEach-Object { [Convert]::ToByte($_.Value, 16) }
        [Text.Encoding]::UTF8.GetString([byte[]]$bytes)
    })
}

$src = Get-ChildItem (Join-Path $root 'src') -Recurse -Include *.cpp, *.hpp | Where-Object { $_.Name -ne 'i18n.hpp' } | Sort-Object FullName
foreach ($f in $src) {
    $text = [IO.File]::ReadAllText($f.FullName, $utf8)
    $rel  = $f.FullName.Substring($root.Length + 5).Replace('\', '/')
    # tr("..")  trf("..", args)  N_("..")
    foreach ($m in [regex]::Matches($text, "\b(?:tr|trf|N_)\(\s*$lit")) {
        $k = Unescape $m.Groups[1].Value
        if (-not $keys.Contains($k)) { $keys[$k] = $rel }
    }
    # tr_n("one", "other", n)  trf_n("one", "other", n, args)
    foreach ($m in [regex]::Matches($text, "\b(?:tr_n|trf_n)\(\s*$lit\s*,\s*$lit")) {
        $k = (Unescape $m.Groups[1].Value) + '|' + (Unescape $m.Groups[2].Value)
        if (-not $keys.Contains($k)) { $keys[$k] = $rel }
    }
}

function Write-Entries($path, $header, $entries, $unused) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($h in $header) { [void]$sb.Append($h).Append("`n") }
    $last = ''
    foreach ($e in $entries) {
        if ($e.File -ne $last) { [void]$sb.Append("`n# ").Append($e.File).Append("`n"); $last = $e.File }
        [void]$sb.Append('"').Append($e.Key).Append('" = "').Append($e.Value).Append('"').Append("`n")
    }
    if ($unused.Count -gt 0) {
        [void]$sb.Append("`n# unused (no longer in the code)`n")
        foreach ($u in $unused) { [void]$sb.Append('"').Append($u.Key).Append('" = "').Append($u.Value).Append('"').Append("`n") }
    }
    [IO.File]::WriteAllText($path, $sb.ToString(), $utf8)
}

function Read-Lang($path) {
    $header = @(); $map = New-Object System.Collections.Specialized.OrderedDictionary
    foreach ($line in [IO.File]::ReadAllLines($path, $utf8)) {
        if ($line -match "^$lit\s*=\s*$lit\s*$") { $map[$matches[1]] = $matches[2] }
        elseif ($line -match '^@') { $header += $line }
    }
    return @{ Header = $header; Map = $map }
}

if (-not (Test-Path $lang)) { New-Item -ItemType Directory $lang | Out-Null }

$entries = foreach ($k in $keys.Keys) { [pscustomobject]@{ Key = $k; Value = ''; File = $keys[$k] } }
Write-Entries (Join-Path $lang 'template.lang') @('# PrismFS translation template: copy to <code>.lang (see README.md)', '@name = ', '@plural = one-other') $entries @()
"template.lang: $($keys.Count) strings"

if ($New) {
    $dst = Join-Path $lang "$New.lang"
    if (Test-Path $dst) { throw "$dst exists" }
    Write-Entries $dst @("# PrismFS translation: $Name", "@name = $Name", "@plural = $Plural") $entries @()
    "created $dst"
}

foreach ($f in Get-ChildItem $lang -Filter *.lang | Where-Object { $_.Name -ne 'template.lang' }) {
    $cur  = Read-Lang $f.FullName
    $done = 0; $missing = 0
    $out  = foreach ($k in $keys.Keys) {
        $v = if ($cur.Map.Contains($k)) { $cur.Map[$k] } else { '' }
        if ($v) { $done++ } else { $missing++ }
        [pscustomobject]@{ Key = $k; Value = $v; File = $keys[$k] }
    }
    $unused = foreach ($k in $cur.Map.Keys) { if (-not $keys.Contains($k)) { [pscustomobject]@{ Key = $k; Value = $cur.Map[$k] } } }
    $unused = @($unused)
    '{0,-12} {1} translated, {2} missing, {3} unused' -f $f.Name, $done, $missing, $unused.Count
    if ($Update) {
        $hdr = if ($cur.Header.Count -gt 0) { @("# PrismFS translation") + $cur.Header } else { @('@name = ' + $f.BaseName) }
        Write-Entries $f.FullName $hdr $out $unused
    }
}
