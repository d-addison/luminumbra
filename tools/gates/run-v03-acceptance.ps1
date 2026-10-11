# NEEDS-NATIVE-RUN
#
# v0.3 acceptance orchestrator. Never run in CI: it drives the sibling native-run
# drivers and the QA client on the qualified Windows machine. Works under Windows
# PowerShell 5.1 and pwsh.
#
# Runs every section, writes <OutDir>\v03-acceptance-receipt.json
# (schema luminumbra.v03_acceptance.v1) and evaluates it with
# tools/ci/check_v03_receipt.py. Exit code is the checker's exit code
# (0 pass, 1 fail, 2 invalid receipt, 3 unevaluated).
#
# -Package must be a DISPOSABLE copy of the QA client package. -OutDir must be
# fresh: the saved-world lifecycle section refuses a non-empty lifecycle subdirectory.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\gates\run-v03-acceptance.ps1 `
#       -Package D:\accept\package-copy -FixtureDir D:\accept\fixture -OutDir D:\accept\out `
#       -BuildDir D:\build\debug -PerfEvidence D:\perf\pointer.json -AudioChecklist D:\audio\checklist.json
#
# Plan only, no sibling runs and no disk writes:
#   pwsh -NoProfile -File tools/gates/run-v03-acceptance.ps1 -Package P -FixtureDir F -OutDir O -DryRun

param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$FixtureDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string[]]$DeclareRefresh,
    [switch]$SkipRender,
    [switch]$SkipLifecycle,
    [string]$BuildDir,
    [string]$PerfEvidence,
    [string]$AudioChecklist,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 300,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path (Join-Path $PSScriptRoot "lib") "AcceptanceCommon.ps1")

$ExeName = "luminumbra_client_qa_app.exe"
$repoRoot = Get-AbsolutePath (Join-Path (Join-Path $PSScriptRoot "..") "..")
$checkerPy = Join-Path $repoRoot "tools/ci/check_v03_receipt.py"
$PackageAbs = Get-AbsolutePath $Package
$FixtureAbs = Get-AbsolutePath $FixtureDir
$OutAbs = Get-AbsolutePath $OutDir
$receiptPath = Join-Path $OutAbs "v03-acceptance-receipt.json"
$verdictPath = Join-Path $OutAbs "v03-verdict.json"

function New-Section {
    param(
        [Parameter(Mandatory = $true)][string]$Status,
        [string]$Evidence = "",
        [string]$Detail = ""
    )
    return [ordered]@{ status = $Status; evidence = $Evidence; detail = $Detail }
}

function Invoke-SiblingScript {
    # Runs a sibling driver with splatted parameters and returns its exit code
    # (or $null when it never set one). Clears $LASTEXITCODE first so a stale
    # value from an earlier native call cannot be mistaken for the sibling's.
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][hashtable]$Params
    )
    $global:LASTEXITCODE = $null
    & $Path @Params | Out-Host
    return $global:LASTEXITCODE
}

if ($DryRun) {
    Write-Host "[dry-run] no sections will run and no files will be written"
    Write-Host ("[dry-run] render_qualification: {0} -Package {1} -OutDir {2}" -f (Join-Path $PSScriptRoot "run-render-qualification.ps1"), $PackageAbs, (Join-Path $OutAbs "render"))
    Write-Host ("[dry-run] saved_world_lifecycle: {0} -Package {1} -FixtureDir {2} -OutDir {3}" -f (Join-Path $PSScriptRoot "run-saved-world-lifecycle.ps1"), $PackageAbs, $FixtureAbs, (Join-Path $OutAbs "lifecycle"))
    Write-Host ("[dry-run] noui_load: {0} -Package {1} -WorldId <fixture_world_id> -OutDir {2}" -f (Join-Path $PSScriptRoot "run-noui-load.ps1"), $PackageAbs, (Join-Path $OutAbs "noui-load"))
    Write-Host ("[dry-run] serial_load_20: 20 x {0} --load-world <fixture_world_id> --no-ui --hidden-window --render-benchmark ..." -f (Join-Path $PackageAbs "luminumbra_client_qa_app.exe"))
    Write-Host "[dry-run] net_soak: ctest --test-dir <BuildDir> -R NetSoak (only with -BuildDir)"
    Write-Host "[dry-run] perf_pointer: pass only with -PerfEvidence pointing at an existing file"
    Write-Host "[dry-run] audio_off: read -AudioChecklist JSON (three boolean fields)"
    Write-Host ("[dry-run] python {0} {1} --verdict-out {2}" -f $checkerPy, $receiptPath, $verdictPath)
    exit 0
}

$sections = [ordered]@{}

try {
    # Every client-driving section needs the executable; fail fast if it is absent.
    $exePath = Get-ClientExe -Package $PackageAbs -Name $ExeName

    # --- render_qualification -------------------------------------------------
    $renderScript = Join-Path $PSScriptRoot "run-render-qualification.ps1"
    $renderOut = Join-Path $OutAbs "render"
    if (-not (Test-Path -LiteralPath $renderScript -PathType Leaf)) {
        $sections["render_qualification"] = New-Section -Status "unevaluated" -Detail "run-render-qualification.ps1 missing"
    } elseif ($SkipRender) {
        $sections["render_qualification"] = New-Section -Status "unevaluated" -Detail "skipped by -SkipRender"
    } else {
        $renderParams = @{ Package = $PackageAbs; OutDir = $renderOut }
        if ($null -ne $DeclareRefresh -and $DeclareRefresh.Count -gt 0) { $renderParams["DeclareRefresh"] = $DeclareRefresh }
        $renderRc = $null
        $renderErr = ""
        try {
            $renderRc = Invoke-SiblingScript -Path $renderScript -Params $renderParams
        } catch {
            $renderErr = $_.Exception.Message
        }
        $qualPath = Join-Path $renderOut "qualification.json"
        if ($null -eq $renderRc) {
            $sections["render_qualification"] = New-Section -Status "fail" -Detail ("no exit code from run-render-qualification.ps1: " + $renderErr)
        } elseif ([int]$renderRc -eq 3) {
            $sections["render_qualification"] = New-Section -Status "unevaluated" -Detail "run-render-qualification.ps1 exit 3"
        } elseif ([int]$renderRc -ne 0) {
            $sections["render_qualification"] = New-Section -Status "fail" -Detail ("run-render-qualification.ps1 exit " + [int]$renderRc)
        } elseif (-not (Test-Path -LiteralPath $qualPath -PathType Leaf)) {
            $sections["render_qualification"] = New-Section -Status "unevaluated" -Detail "render/qualification.json missing"
        } else {
            $qual = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($qualPath, [System.Text.Encoding]::UTF8))
            if ((Test-JsonProperty -Object $qual -Name "render_floor_qualified") -and $qual.render_floor_qualified -eq $true) {
                $sections["render_qualification"] = New-Section -Status "pass" -Evidence "render/qualification.json" -Detail "render floor qualified"
            } else {
                $sections["render_qualification"] = New-Section -Status "fail" -Evidence "render/qualification.json" -Detail "render_floor_qualified is not true"
            }
        }
    }

    # --- saved_world_lifecycle ------------------------------------------------
    $lifecycleScript = Join-Path $PSScriptRoot "run-saved-world-lifecycle.ps1"
    $lifecycleOut = Join-Path $OutAbs "lifecycle"
    if ($SkipLifecycle) {
        $sections["saved_world_lifecycle"] = New-Section -Status "unevaluated" -Detail "skipped by -SkipLifecycle"
    } else {
        $lifecycleParams = @{ Package = $PackageAbs; FixtureDir = $FixtureAbs; OutDir = $lifecycleOut; TimeoutSeconds = $TimeoutSeconds }
        $lifecycleRc = $null
        $lifecycleErr = ""
        try {
            $lifecycleRc = Invoke-SiblingScript -Path $lifecycleScript -Params $lifecycleParams
        } catch {
            $lifecycleErr = $_.Exception.Message
        }
        $lifecycleEvidence = "lifecycle/acceptance-receipt.json"
        if ($null -eq $lifecycleRc) {
            $sections["saved_world_lifecycle"] = New-Section -Status "fail" -Detail ("no exit code from run-saved-world-lifecycle.ps1: " + $lifecycleErr)
        } elseif ([int]$lifecycleRc -eq 0) {
            $sections["saved_world_lifecycle"] = New-Section -Status "pass" -Evidence $lifecycleEvidence -Detail "saved-world acceptance verdict pass"
        } elseif ([int]$lifecycleRc -eq 3) {
            $sections["saved_world_lifecycle"] = New-Section -Status "unevaluated" -Evidence $lifecycleEvidence -Detail "saved-world acceptance unevaluated (exit 3)"
        } else {
            $sections["saved_world_lifecycle"] = New-Section -Status "fail" -Evidence $lifecycleEvidence -Detail ("run-saved-world-lifecycle.ps1 exit " + [int]$lifecycleRc)
        }
    }

    # --- noui_load (against the fixture world) --------------------------------
    $fixtureWorldId = $null
    $fixtureError = ""
    try {
        $fixtureWorldId = Get-FixtureWorldId -FixtureDir $FixtureAbs
    } catch {
        $fixtureError = $_.Exception.Message
    }

    $noUiScript = Join-Path $PSScriptRoot "run-noui-load.ps1"
    $noUiOut = Join-Path $OutAbs "noui-load"
    if ($null -eq $fixtureWorldId) {
        $sections["noui_load"] = New-Section -Status "fail" -Detail ("fixture world id unavailable: " + $fixtureError)
    } else {
        $noUiRc = $null
        $noUiErr = ""
        try {
            $noUiRc = Invoke-SiblingScript -Path $noUiScript -Params @{ Package = $PackageAbs; WorldId = $fixtureWorldId; OutDir = $noUiOut; TimeoutSeconds = $TimeoutSeconds }
        } catch {
            $noUiErr = $_.Exception.Message
        }
        $noUiEvidence = "noui-load/noui-load-receipt.json"
        $noUiVerdict = ""
        $noUiReceipt = Join-Path $noUiOut "noui-load-receipt.json"
        if (Test-Path -LiteralPath $noUiReceipt -PathType Leaf) {
            $noUiParsed = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($noUiReceipt, [System.Text.Encoding]::UTF8))
            if (Test-JsonProperty -Object $noUiParsed -Name "verdict") { $noUiVerdict = [string]$noUiParsed.verdict }
        }
        if ($null -eq $noUiRc) {
            $sections["noui_load"] = New-Section -Status "fail" -Evidence $noUiEvidence -Detail ("no exit code from run-noui-load.ps1: " + $noUiErr)
        } elseif ([int]$noUiRc -eq 0 -and $noUiVerdict -eq "pass") {
            $sections["noui_load"] = New-Section -Status "pass" -Evidence $noUiEvidence -Detail "no-UI load verdict pass"
        } elseif ([int]$noUiRc -eq 3) {
            $sections["noui_load"] = New-Section -Status "unevaluated" -Evidence $noUiEvidence -Detail "no-UI load unevaluated (exit 3)"
        } else {
            $sections["noui_load"] = New-Section -Status "fail" -Evidence $noUiEvidence -Detail ("run-noui-load.ps1 exit " + [int]$noUiRc + ", verdict '" + $noUiVerdict + "'")
        }
    }

    # --- serial_load_20 -------------------------------------------------------
    $serialOut = Join-Path $OutAbs "serial"
    $serialReceipt = Join-Path $OutAbs "serial-load-20.json"
    if ($null -eq $fixtureWorldId) {
        $sections["serial_load_20"] = New-Section -Status "fail" -Detail ("fixture world id unavailable: " + $fixtureError)
    } else {
        if (Test-Path -LiteralPath $serialOut) { Remove-Item -LiteralPath $serialOut -Recurse -Force }
        $serialRuns = @()
        $serialFailure = $null
        for ($n = 1; $n -le 20; $n++) {
            $runName = "run-" + $n.ToString("00")
            $runDir = Join-Path $serialOut $runName
            $runArtifact = Join-Path $runDir "artifact"
            $runCrash = Join-Path $runDir "crashes"
            New-Item -ItemType Directory -Force -Path $runArtifact, $runCrash | Out-Null
            $frame = Join-Path $runDir "frame.ppm"
            $bench = Join-Path $runDir "benchmark.json"
            $runArgs = @(
                "--load-world", $fixtureWorldId,
                "--no-ui", "--hidden-window", "--no-audio",
                "--hang-watchdog-seconds", "60",
                "--crash-dir", $runCrash,
                "--runtime-artifact-dir", $runArtifact,
                "--capture-size", "1280x720",
                "--render-benchmark", $bench,
                "--render-benchmark-schema", "v3",
                "--render-benchmark-warmup", "30",
                "--render-benchmark-frames", "8",
                "--render-benchmark-screenshot", $frame
            )
            $run = Invoke-ClientSession -Exe $exePath -ArgumentList $runArgs `
                -WorkingDirectory $PackageAbs `
                -StdoutPath (Join-Path $runDir "stdout.log") `
                -StderrPath (Join-Path $runDir "stderr.log") `
                -TimeoutSeconds $TimeoutSeconds
            $shutdown = Read-ShutdownRecord -ArtifactDir $runArtifact
            $drained = $null
            if ($null -ne $shutdown) { $drained = $shutdown["jobs_drained"] }
            $serialRuns += [ordered]@{
                index = $n
                exit_code = [int]$run.exit_code
                timed_out = [bool]$run.timed_out
                jobs_drained = $drained
            }
            if ([int]$run.exit_code -ne 0 -or [bool]$run.timed_out -or $drained -ne $true) {
                $serialFailure = "serial run $n failed (exit $($run.exit_code), timed_out $($run.timed_out), jobs_drained $drained)"
                break
            }
        }
        $serialPass = ($null -eq $serialFailure -and $serialRuns.Count -eq 20)
        $serialDetail = "20 consecutive loads exit 0 with jobs_drained true"
        if (-not $serialPass) { $serialDetail = $serialFailure }
        Write-JsonFile -Object ([ordered]@{
            schema = "luminumbra.serial_load_20.v1"
            fixture_world_id = $fixtureWorldId
            runs = @($serialRuns)
            verdict = $(if ($serialPass) { "pass" } else { "fail" })
        }) -Path $serialReceipt
        if ($serialPass) {
            $sections["serial_load_20"] = New-Section -Status "pass" -Evidence "serial-load-20.json" -Detail $serialDetail
        } else {
            $sections["serial_load_20"] = New-Section -Status "fail" -Evidence "serial-load-20.json" -Detail $serialDetail
        }
    }

    # --- net_soak -------------------------------------------------------------
    if ([string]::IsNullOrEmpty($BuildDir)) {
        $sections["net_soak"] = New-Section -Status "unevaluated" -Detail "no build dir"
    } else {
        $netOut = Join-Path $OutAbs "net-soak"
        New-Item -ItemType Directory -Force -Path $netOut | Out-Null
        $ctest = Get-Command ctest -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $ctest) {
            $sections["net_soak"] = New-Section -Status "fail" -Detail "ctest not found on PATH"
        } else {
            $buildAbs = Get-AbsolutePath $BuildDir
            $netRun = Invoke-ClientSession -Exe $ctest.Source `
                -ArgumentList @("--test-dir", $buildAbs, "-R", "NetSoak") `
                -WorkingDirectory $OutAbs `
                -StdoutPath (Join-Path $netOut "ctest.stdout.log") `
                -StderrPath (Join-Path $netOut "ctest.stderr.log") `
                -TimeoutSeconds 3600
            $netText = (Read-TextOrEmpty -Path (Join-Path $netOut "ctest.stdout.log")) + "`n" + (Read-TextOrEmpty -Path (Join-Path $netOut "ctest.stderr.log"))
            $netEvidence = "net-soak/ctest.stdout.log"
            $summary = [regex]::Match($netText, '(\d+)% tests passed, (\d+) tests? failed out of (\d+)')
            if ($netRun.timed_out) {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail "ctest NetSoak timed out"
            } elseif ([int]$netRun.exit_code -ne 0) {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail ("ctest exit " + [int]$netRun.exit_code)
            } elseif ($netText -match 'No tests were found') {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail "zero NetSoak tests discovered"
            } elseif (-not $summary.Success) {
                $sections["net_soak"] = New-Section -Status "unevaluated" -Evidence $netEvidence -Detail "ctest summary line not found"
            } elseif ([int]$summary.Groups[2].Value -ne 0 -or [int]$summary.Groups[3].Value -lt 1) {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail ("ctest: " + $summary.Value)
            } else {
                $sections["net_soak"] = New-Section -Status "pass" -Evidence $netEvidence -Detail ("ctest: " + $summary.Value)
            }
        }
    }

    # --- perf_pointer ---------------------------------------------------------
    if ([string]::IsNullOrEmpty($PerfEvidence)) {
        $sections["perf_pointer"] = New-Section -Status "unevaluated" -Detail "no -PerfEvidence given"
    } elseif (-not (Test-Path -LiteralPath $PerfEvidence -PathType Leaf)) {
        $sections["perf_pointer"] = New-Section -Status "unevaluated" -Detail "perf evidence file not found"
    } else {
        $sections["perf_pointer"] = New-Section -Status "pass" -Evidence (Get-AbsolutePath $PerfEvidence) -Detail "perf evidence file present"
    }

    # --- audio_off ------------------------------------------------------------
    $audioFields = @("default_null_playback_with_saved_volume", "no_audio_precedence", "controls_disabled")
    if ([string]::IsNullOrEmpty($AudioChecklist) -or -not (Test-Path -LiteralPath $AudioChecklist -PathType Leaf)) {
        $sections["audio_off"] = New-Section -Status "unevaluated" -Detail "no audio checklist file"
    } else {
        $checklist = $null
        $checklistError = ""
        try {
            $checklist = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText((Get-AbsolutePath $AudioChecklist), [System.Text.Encoding]::UTF8))
        } catch {
            $checklistError = $_.Exception.Message
        }
        $audioEvidence = Get-AbsolutePath $AudioChecklist
        if ($null -eq $checklist) {
            $sections["audio_off"] = New-Section -Status "fail" -Evidence $audioEvidence -Detail ("checklist does not parse: " + $checklistError)
        } else {
            $audioProblems = @()
            $audioMissing = @()
            foreach ($field in $audioFields) {
                if (-not (Test-JsonProperty -Object $checklist -Name $field)) {
                    $audioMissing += $field
                } elseif ($checklist.$field -isnot [bool]) {
                    $audioProblems += ($field + " is not a boolean")
                } elseif ($checklist.$field -ne $true) {
                    $audioProblems += ($field + " is false")
                }
            }
            if ($audioProblems.Count -gt 0) {
                $sections["audio_off"] = New-Section -Status "fail" -Evidence $audioEvidence -Detail ($audioProblems -join "; ")
            } elseif ($audioMissing.Count -gt 0) {
                $sections["audio_off"] = New-Section -Status "unevaluated" -Evidence $audioEvidence -Detail ("checklist lacks: " + ($audioMissing -join ", "))
            } else {
                $sections["audio_off"] = New-Section -Status "pass" -Evidence $audioEvidence -Detail "all audio-off checklist items true"
            }
        }
    }

    # --- receipt and checker --------------------------------------------------
    Write-JsonFile -Object ([ordered]@{
        schema = "luminumbra.v03_acceptance.v1"
        sections = $sections
    }) -Path $receiptPath

    $check = Invoke-PythonTool -ScriptPath $checkerPy -ArgumentList @($receiptPath, "--verdict-out", $verdictPath)
    Write-Host $check.stdout.TrimEnd()
    if ($check.stderr.Trim().Length -gt 0) { Write-Host $check.stderr.TrimEnd() }
    exit $check.exit_code
} catch {
    Write-Host ("error: " + $_.Exception.Message)
    exit 1
}
