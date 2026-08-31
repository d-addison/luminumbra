<#
.SYNOPSIS
  Moving-hitch regression gate. Productionizes the --profile-fly profiler (the SLOWFRAME per-phase
  logger) into a pass/fail check that LOCKS IN the 2026-06-23 streaming-burst amortization wins:
  water-sim head-of-line block (was ~1500ms), foliage surf-grid re-query (was ~996ms), scatter
  rebuild (was ~132ms). It drives the player forward at constant sprint speed through freshly-streamed
  terrain and asserts no per-phase cost regresses past its ceiling.

.DESCRIPTION
  Runs the RELEASE client headless:
    luminumbra_client_app.exe --auto-create-world --auto-enter-world --profile-fly <Seconds> --no-audio
  The SLOWFRAME logger (main_client.cpp, threshold 12ms) emits one line per slow frame with the
  per-phase split (sim/stream/render/present + foliage_inst/scatter) and a SHIELD stream-split
  (water/activation/meshing). This script parses those, EXCLUDES the one-time world-entry settling
  (first WarmupFrames slow-frames), and fails if any phase exceeds its ceiling. Ceilings are set well
  above observed sprint-burst worst-case (~60-80ms) but far below the pre-fix regressions (500-1500ms),
  so they catch a re-introduction of the O(all-active) / per-frame-re-query bugs without flapping on
  normal bursts.

  Requires a GPU (the client renders). Intended to run in the GPU gate lane alongside the visual gates.

.NOTES
  Output is UTF-16 with embedded nulls -> we strip \0 before regex. See memory
  [[bash-tool-sandboxed-use-powershell]] / [[moving-lag-streaming-amortization]].
#>
[CmdletBinding()]
param(
    [string]$Exe = "build/release/bin/luminumbra_client_app.exe",
    [int]$Seconds = 22,
    [int]$WarmupFrames = 40,   # skip the one-time world-entry settling (render=0 all-other frames)
    [string]$ArtifactDir = "build/gate-artifacts",
    # Per-phase ceilings (ms). Observed sprint-burst worst ~60-80ms; pre-fix bugs were 500-1500ms.
    [double]$WaterCeil   = 220.0,
    [double]$FoliageCeil = 220.0,
    [double]$ScatterCeil = 90.0
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tools/gates -> repo root
Set-Location $repo

if (-not (Test-Path $Exe)) {
    throw "client exe not found at '$Exe' - build it first: cmake --build --preset release --target luminumbra_client_app"
}

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null
$log = Join-Path $ArtifactDir "moving-hitch.txt"

# Kill any stale instance so the relink/run doesn't hit a file lock.
Get-Process luminumbra_client_app -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

Write-Host "[moving-hitch] running $Exe --profile-fly $Seconds ..."
& $Exe --auto-create-world --auto-enter-world --profile-fly $Seconds --no-audio *> $log
Write-Host "[moving-hitch] client exited ($LASTEXITCODE); parsing $log"

# Read + strip embedded nulls (UTF-16 / interleaved-null output).
$raw = [System.IO.File]::ReadAllText((Resolve-Path $log))
$clean = $raw -replace "`0", ""
$slowLines  = [regex]::Matches($clean, 'SLOWFRAME[^\r\n]*') | ForEach-Object { $_.Value }
$splitLines = [regex]::Matches($clean, 'stream-split:[^\r\n]*') | ForEach-Object { $_.Value }

if ($slowLines.Count -eq 0) {
    throw "no SLOWFRAME lines in $log - did the client enter the world / is the SLOWFRAME logger present?"
}

# Skip the world-entry settling window, then take the worst of each phase across the steady state.
$steady = if ($slowLines.Count -gt $WarmupFrames) { $slowLines[$WarmupFrames..($slowLines.Count - 1)] } else { @() }
$steadySplit = if ($splitLines.Count -gt $WarmupFrames) { $splitLines[$WarmupFrames..($splitLines.Count - 1)] } else { @() }

function Max-Phase([string[]]$lines, [string]$key) {
    $m = 0.0
    foreach ($l in $lines) {
        $hit = [regex]::Match($l, [regex]::Escape($key) + '=([0-9.]+)')
        if ($hit.Success) { $v = [double]$hit.Groups[1].Value; if ($v -gt $m) { $m = $v } }
    }
    return $m
}

$worstFoliage = Max-Phase $steady 'foliage_inst'
$worstScatter = Max-Phase $steady 'scatter'
$worstWater   = Max-Phase $steadySplit 'water'
$worstTotal   = 0.0
foreach ($l in $steady) {
    $hit = [regex]::Match($l, 'SLOWFRAME ([0-9.]+)ms')
    if ($hit.Success) { $v = [double]$hit.Groups[1].Value; if ($v -gt $worstTotal) { $worstTotal = $v } }
}

Write-Host ("[moving-hitch] steady-state worst: water={0:N1}ms (<= {1}) foliage_inst={2:N1}ms (<= {3}) scatter={4:N1}ms (<= {5}) | worst-frame-total={6:N1}ms" -f `
    $worstWater, $WaterCeil, $worstFoliage, $FoliageCeil, $worstScatter, $ScatterCeil, $worstTotal)

$fail = @()
if ($worstWater   -gt $WaterCeil)   { $fail += ("water-sim regressed: {0:N1}ms > {1}ms ceiling (head-of-line block / O(all-active) snapshot?)" -f $worstWater, $WaterCeil) }
if ($worstFoliage -gt $FoliageCeil) { $fail += ("foliage rebuild regressed: {0:N1}ms > {1}ms ceiling (per-frame surf-grid re-query / cache invalidated?)" -f $worstFoliage, $FoliageCeil) }
if ($worstScatter -gt $ScatterCeil) { $fail += ("scatter rebuild regressed: {0:N1}ms > {1}ms ceiling (per-chunk scatter cache bypassed?)" -f $worstScatter, $ScatterCeil) }

if ($fail.Count -gt 0) {
    Write-Host "[moving-hitch] FAIL:" -ForegroundColor Red
    $fail | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}

Write-Host "[moving-hitch] PASS - all steady-state phase costs within ceilings." -ForegroundColor Green
exit 0
