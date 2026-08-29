<#
.SYNOPSIS
    Resolve a Luminumbra crash-<ts>.txt stack trace to file:line via addr2line.

.DESCRIPTION
    The in-process crash handler (main_client.cpp WriteCrashStackTrace) writes each
    frame as `#N 0x<absaddr>  <module>+0x<RVA>  <symbol>`. DbgHelp can't read mingw
    DWARF line info, so this script feeds the per-frame RVA (relative to the module's
    preferred ImageBase, read from the binary) to addr2line to recover exact file:line.

    Usage:
      tools/gates/symbolize-crash.ps1 [-CrashFile <path>] [-Exe <path>]
    Defaults: newest build/*/crashes/crash-*.txt, matched against build/<tree>/bin.
#>
[CmdletBinding()]
param(
    [string]$CrashFile = '',
    [string]$Exe = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path

if (-not $CrashFile) {
    $CrashFile = Get-ChildItem -Path $root -Recurse -Filter 'crash-*.txt' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $CrashFile -or -not (Test-Path $CrashFile)) { Write-Error "No crash file found (pass -CrashFile)."; exit 1 }
Write-Host "Crash file: $CrashFile`n"

$addr2line = (Get-Command addr2line -ErrorAction SilentlyContinue).Source
if (-not $addr2line) { $addr2line = 'C:\msys64\ucrt64\bin\addr2line.exe' }

# Cache: module short-name -> @{ exe=<full path>; base=<ImageBase> }
$mods = @{}
function Resolve-Module([string]$short) {
    if ($mods.ContainsKey($short)) { return $mods[$short] }
    $exe = $Exe
    if (-not $exe) {
        $exe = Get-ChildItem -Path (Join-Path $root 'build') -Recurse -Filter $short -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
    $base = 0
    if ($exe -and (Test-Path $exe)) {
        $p = & 'C:\msys64\ucrt64\bin\objdump.exe' -p $exe 2>$null | Select-String 'ImageBase'
        if ($p) { $base = [Convert]::ToUInt64(($p.Line -replace '.*ImageBase\s+',''), 16) }
    }
    $info = @{ exe = $exe; base = $base }
    $mods[$short] = $info
    return $info
}

Get-Content $CrashFile | ForEach-Object {
    $line = $_
    if ($line -match '^(#\d+.*?0x[0-9A-Fa-f]+)\s+([^\s]+)\+0x([0-9A-Fa-f]+)\s*(.*)$') {
        $prefix = $Matches[1]; $modShort = $Matches[2]; $rva = [Convert]::ToUInt64($Matches[3],16); $sym = $Matches[4]
        $m = Resolve-Module $modShort
        if ($m.exe -and ($modShort -like '*luminumbra*')) {
            $vma = '0x{0:X}' -f ($m.base + $rva)
            $resolved = & $addr2line -e $m.exe -f -C $vma 2>$null
            $loc = ($resolved -join ' | ')
            Write-Host "$prefix  $modShort+0x$($Matches[3])  ->  $loc"
        } else {
            Write-Host "$line"
        }
    } else {
        Write-Host $line
    }
}
