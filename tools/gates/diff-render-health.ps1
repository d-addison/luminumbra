# Field-level recursive diff between two render-health analysis JSON files.
#
# Usage:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/gates/diff-render-health.ps1 `
#       -BaselinePath tools/gates/baselines/render-health-baseline.json `
#       -CurrentPath  build/debug/test-artifacts/render/render-health-analysis.json
#
# Volatile fields are excluded from the comparison:
#   - any *gpu_ms values (per-run GPU timings vary; structure and pass NAMES must still match)
#   - any *timestamp* fields
#
# Exit code 0 when no non-volatile field differs; exit code 1 otherwise, printing
# the JSON path of every differing field.

param(
    [Parameter(Mandatory = $true)][string]$BaselinePath,
    [Parameter(Mandatory = $true)][string]$CurrentPath
)

$ErrorActionPreference = "Stop"

function Test-VolatilePath {
    param([string]$Path)
    if ($Path -match 'gpu_ms') { return $true }
    if ($Path -match 'timestamp') { return $true }
    return $false
}

function Compare-JsonNode {
    param(
        $Baseline,
        $Current,
        [string]$Path,
        [System.Collections.Generic.List[string]]$Diffs
    )

    if (Test-VolatilePath -Path $Path) { return }

    $baseIsNull = $null -eq $Baseline
    $curIsNull = $null -eq $Current
    if ($baseIsNull -and $curIsNull) { return }
    if ($baseIsNull -or $curIsNull) {
        $Diffs.Add(("{0}: baseline=<{1}> current=<{2}>" -f $Path, $(if ($baseIsNull) { "null" } else { $Baseline }), $(if ($curIsNull) { "null" } else { $Current })))
        return
    }

    $baseIsObject = $Baseline -is [System.Management.Automation.PSCustomObject]
    $curIsObject = $Current -is [System.Management.Automation.PSCustomObject]
    if ($baseIsObject -or $curIsObject) {
        if (-not ($baseIsObject -and $curIsObject)) {
            $Diffs.Add(("{0}: type mismatch baseline=<{1}> current=<{2}>" -f $Path, $Baseline.GetType().Name, $Current.GetType().Name))
            return
        }
        $baseProps = @($Baseline.PSObject.Properties.Name)
        $curProps = @($Current.PSObject.Properties.Name)
        foreach ($name in $baseProps) {
            $childPath = if ([string]::IsNullOrEmpty($Path)) { $name } else { "$Path.$name" }
            if ($curProps -notcontains $name) {
                if (-not (Test-VolatilePath -Path $childPath)) {
                    $Diffs.Add("${childPath}: present in baseline, missing in current")
                }
                continue
            }
            Compare-JsonNode -Baseline $Baseline.$name -Current $Current.$name -Path $childPath -Diffs $Diffs
        }
        foreach ($name in $curProps) {
            if ($baseProps -notcontains $name) {
                $childPath = if ([string]::IsNullOrEmpty($Path)) { $name } else { "$Path.$name" }
                if (-not (Test-VolatilePath -Path $childPath)) {
                    $Diffs.Add("${childPath}: missing in baseline, present in current")
                }
            }
        }
        return
    }

    $baseIsArray = $Baseline -is [System.Array]
    $curIsArray = $Current -is [System.Array]
    if ($baseIsArray -or $curIsArray) {
        $baseItems = @($Baseline)
        $curItems = @($Current)
        if ($baseItems.Count -ne $curItems.Count) {
            $Diffs.Add(("{0}: array length baseline={1} current={2}" -f $Path, $baseItems.Count, $curItems.Count))
            return
        }
        for ($i = 0; $i -lt $baseItems.Count; $i++) {
            Compare-JsonNode -Baseline $baseItems[$i] -Current $curItems[$i] -Path "$Path[$i]" -Diffs $Diffs
        }
        return
    }

    # Scalar leaf comparison.
    if ($Baseline -ne $Current) {
        $Diffs.Add(("{0}: baseline=<{1}> current=<{2}>" -f $Path, $Baseline, $Current))
    }
}

if (-not (Test-Path $BaselinePath)) {
    Write-Host "diff-render-health: baseline not found: $BaselinePath"
    exit 1
}
if (-not (Test-Path $CurrentPath)) {
    Write-Host "diff-render-health: current analysis not found: $CurrentPath"
    exit 1
}

$baseline = Get-Content $BaselinePath -Raw | ConvertFrom-Json
$current = Get-Content $CurrentPath -Raw | ConvertFrom-Json

$diffs = New-Object 'System.Collections.Generic.List[string]'
Compare-JsonNode -Baseline $baseline -Current $current -Path "" -Diffs $diffs

if ($diffs.Count -gt 0) {
    Write-Host "diff-render-health: $($diffs.Count) non-volatile field difference(s) vs baseline:"
    foreach ($diff in $diffs) {
        Write-Host "  $diff"
    }
    exit 1
}

Write-Host "diff-render-health: no non-volatile field differences (gpu_ms/timestamp fields excluded)."
exit 0
