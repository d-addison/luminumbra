# NEEDS-NATIVE-RUN
#
# No-UI world load driver. Never run in CI: it launches luminumbra_client_qa_app.exe
# on the qualified Windows machine. Works under Windows PowerShell 5.1 and pwsh.
#
# Loads -WorldId from <Package>\worlds\saves with --no-ui, captures a render
# benchmark frame, and checks exit code, PPM validity and mean luma, the loaded
# capture_context.world_id, and 1280x720 in both the PPM and benchmark. The
# documented pre-fix failure is exit code 134.
#
# Writes <OutDir>\noui-load-receipt.json. Exit 0 on pass, 1 on fail.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\gates\run-noui-load.ps1 `
#       -Package D:\accept\package-copy -WorldId 0123abcd -OutDir D:\accept\out\noui -TimeoutSeconds 300

param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$WorldId,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path (Join-Path $PSScriptRoot "lib") "AcceptanceCommon.ps1")

$ExeName = "luminumbra_client_qa_app.exe"
$PackageAbs = Get-AbsolutePath $Package
$OutAbs = Get-AbsolutePath $OutDir
$receiptPath = Join-Path $OutAbs "noui-load-receipt.json"
$ppmPath = Join-Path $OutAbs "frame.ppm"
$benchPath = Join-Path $OutAbs "benchmark.json"
$artifactDir = Join-Path $OutAbs "artifact"

function Read-PpmHeader {
    # Parses a binary PPM header. Returns @{ magic; width; height; maxval; data_offset }.
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)
    $pos = 0
    $tokens = @()
    while ($tokens.Count -lt 4) {
        while ($pos -lt $Bytes.Length) {
            $b = $Bytes[$pos]
            if ($b -eq 35) {
                while ($pos -lt $Bytes.Length -and $Bytes[$pos] -ne 10 -and $Bytes[$pos] -ne 13) { $pos++ }
            } elseif ($b -eq 32 -or $b -eq 9 -or $b -eq 10 -or $b -eq 13 -or $b -eq 11 -or $b -eq 12) {
                $pos++
            } else {
                break
            }
        }
        $start = $pos
        while ($pos -lt $Bytes.Length) {
            $b = $Bytes[$pos]
            if ($b -eq 32 -or $b -eq 9 -or $b -eq 10 -or $b -eq 13 -or $b -eq 11 -or $b -eq 12 -or $b -eq 35) { break }
            $pos++
        }
        if ($pos -eq $start) { throw "truncated PPM header" }
        $tokens += [System.Text.Encoding]::ASCII.GetString($Bytes, $start, $pos - $start)
    }
    # The single whitespace byte after maxval separates the header from the pixel data.
    return @{
        magic = $tokens[0]
        width = [int]$tokens[1]
        height = [int]$tokens[2]
        maxval = [int]$tokens[3]
        data_offset = $pos + 1
    }
}

function Get-PpmMeanLuma {
    # Returns @{ ok; mean; reason }. Mean of 0.2126R+0.7152G+0.0722B scaled to 0..1.
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return @{ ok = $false; mean = $null; reason = "frame.ppm missing" }
    }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $header = Read-PpmHeader -Bytes $bytes
    if ($header.magic -cne "P6") { return @{ ok = $false; mean = $null; reason = "PPM magic is not P6" } }
    if ($header.width -le 0 -or $header.height -le 0) { return @{ ok = $false; mean = $null; reason = "PPM dimensions invalid" } }
    if ($header.width -ne 1280 -or $header.height -ne 720) { return @{ ok = $false; mean = $null; reason = "PPM dimensions must be 1280x720" } }
    if ($header.maxval -ne 255) { return @{ ok = $false; mean = $null; reason = "PPM maxval is not 255" } }
    $pixels = [int64]$header.width * [int64]$header.height
    $need = [int64]$header.data_offset + $pixels * 3
    if ([int64]$bytes.Length -lt $need) { return @{ ok = $false; mean = $null; reason = "PPM data truncated" } }
    $sum = 0.0
    $k = [int]$header.data_offset
    $count = [int]$pixels
    for ($p = 0; $p -lt $count; $p++) {
        $sum += 0.2126 * $bytes[$k] + 0.7152 * $bytes[$k + 1] + 0.0722 * $bytes[$k + 2]
        $k += 3
    }
    $mean = $sum / ($pixels * 255.0)
    return @{ ok = $true; mean = [math]::Round($mean, 6); reason = "" }
}

$reasons = @()
$exitCode = -1
$timedOut = $true
$meanLuma = $null
$captureContextPresent = $false
$verdict = "fail"

try {
    $exePath = Get-ClientExe -Package $PackageAbs -Name $ExeName
    $worldDir = Join-Path (Join-Path (Join-Path $PackageAbs "worlds") "saves") $WorldId
    if (-not (Test-Path -LiteralPath $worldDir -PathType Container)) {
        throw "world $WorldId not found under $PackageAbs\worlds\saves"
    }

    New-Item -ItemType Directory -Force -Path $OutAbs, (Join-Path $OutAbs "crashes"), $artifactDir | Out-Null
    # Remove outputs from any earlier run so a stale frame can never pass.
    Remove-Item -LiteralPath $ppmPath, $benchPath -Force -ErrorAction SilentlyContinue

    $arguments = @(
        "--load-world", $WorldId,
        "--no-ui", "--hidden-window", "--no-audio",
        "--hang-watchdog-seconds", "60",
        "--crash-dir", (Join-Path $OutAbs "crashes"),
        "--runtime-artifact-dir", $artifactDir,
        "--capture-size", "1280x720",
        "--render-benchmark", $benchPath,
        "--render-benchmark-schema", "v3",
        "--render-benchmark-warmup", "30",
        "--render-benchmark-frames", "8",
        "--render-benchmark-screenshot", $ppmPath
    )
    $run = Invoke-ClientSession -Exe $exePath -ArgumentList $arguments `
        -WorkingDirectory $PackageAbs `
        -StdoutPath (Join-Path $OutAbs "noui-load.stdout.log") `
        -StderrPath (Join-Path $OutAbs "noui-load.stderr.log") `
        -TimeoutSeconds $TimeoutSeconds
    $exitCode = [int]$run.exit_code
    $timedOut = [bool]$run.timed_out

    if ($timedOut) { $reasons += "timed out after $TimeoutSeconds s" }
    if ($exitCode -ne 0) { $reasons += "exit code $exitCode (expected 0)" }

    $luma = Get-PpmMeanLuma -Path $ppmPath
    if (-not $luma.ok) {
        $reasons += $luma.reason
    } else {
        $meanLuma = $luma.mean
        if ($meanLuma -le 0.01) { $reasons += "mean luma $meanLuma is not above 0.01" }
    }

    if (-not (Test-Path -LiteralPath $benchPath -PathType Leaf)) {
        $reasons += "benchmark.json missing"
    } else {
        try {
            $bench = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($benchPath, [System.Text.Encoding]::UTF8))
            $captureContextPresent = Test-JsonProperty -Object $bench -Name "capture_context"
            if ($captureContextPresent) {
                if (-not (Test-JsonProperty -Object $bench.capture_context -Name "world_id")) {
                    $reasons += "capture_context.world_id absent from benchmark.json"
                } elseif ($bench.capture_context.world_id -isnot [string] -or $bench.capture_context.world_id -cne $WorldId) {
                    $reasons += "capture_context.world_id does not match requested WorldId '$WorldId'"
                }
            }
            foreach ($dimension in @("width", "height")) {
                if ($dimension -eq "width") { $expected = 1280 } else { $expected = 720 }
                if (-not (Test-JsonProperty -Object $bench -Name $dimension)) {
                    $reasons += "benchmark.$dimension absent from benchmark.json"
                } elseif (($bench.$dimension -isnot [int] -and $bench.$dimension -isnot [long]) -or $bench.$dimension -ne $expected) {
                    $reasons += "benchmark.$dimension must be $expected"
                }
            }
        } catch {
            $reasons += "benchmark.json does not parse"
        }
    }

    if (-not $captureContextPresent) { $reasons += "capture_context absent from benchmark.json" }

    if ($reasons.Count -eq 0) { $verdict = "pass" }
} catch {
    $reasons += $_.Exception.Message
    $verdict = "fail"
}

$receipt = [ordered]@{
    schema = "luminumbra.noui_load.v1"
    exit_code = $exitCode
    timed_out = $timedOut
    ppm_mean_luma = $meanLuma
    capture_context_present = $captureContextPresent
    verdict = $verdict
    reasons = @($reasons)
}
Write-JsonFile -Object $receipt -Path $receiptPath
Write-Host ("noui-load: verdict {0} (exit {1}, mean luma {2})" -f $verdict, $exitCode, $meanLuma)
foreach ($reason in $reasons) { Write-Host ("  - " + $reason) }

if ($verdict -eq "pass") { exit 0 }
exit 1
