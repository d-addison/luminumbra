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
# Evidence must be a regular file within OutDir; external perf evidence and
# audio checklists are copied there before the receipt is evaluated.
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
    # $Extra carries optional section keys (for example required_tests, discovered_tests, skipped_tests).
    param(
        [Parameter(Mandatory = $true)][string]$Status,
        [string]$Evidence = "",
        [string]$Detail = "",
        [hashtable]$Extra = @{}
    )
    $section = [ordered]@{ status = $Status; evidence = $Evidence; detail = $Detail }
    foreach ($key in $Extra.Keys) { $section[$key] = $Extra[$key] }
    return $section
}

function Invoke-SiblingScript {
    # In PS 5.1 and 7, & invokes a script file in its own scope: exit N ends
    # that script and sets LASTEXITCODE in the caller; it does not exit the host.
    # Do not dot-source drivers. Capture terminating errors separately from exits
    # so a stale native exit code or partial evidence cannot turn an error into PASS.
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][hashtable]$Params
    )
    $global:LASTEXITCODE = $null
    try {
        & $Path @Params | Out-Host
        return @{ exit_code = $global:LASTEXITCODE; error = "" }
    } catch {
        return @{ exit_code = $null; error = $_.ToString() }
    }
}

if ($DryRun) {
    Write-Host "[dry-run] no sections will run and no files will be written"
    Write-Host ("[dry-run] render_qualification: {0} -Package {1} -OutDir {2}" -f (Join-Path $PSScriptRoot "run-render-qualification.ps1"), $PackageAbs, (Join-Path $OutAbs "render"))
    Write-Host ("[dry-run] saved_world_lifecycle: {0} -Package {1} -FixtureDir {2} -OutDir {3}" -f (Join-Path $PSScriptRoot "run-saved-world-lifecycle.ps1"), $PackageAbs, $FixtureAbs, (Join-Path $OutAbs "lifecycle"))
    Write-Host ("[dry-run] noui_load: {0} -Package {1} -WorldId <fixture_world_id> -OutDir {2}" -f (Join-Path $PSScriptRoot "run-noui-load.ps1"), $PackageAbs, (Join-Path $OutAbs "noui-load"))
    Write-Host ("[dry-run] serial_load_20: 20 x {0} --load-world <fixture_world_id> --no-ui --hidden-window --render-benchmark ..." -f (Join-Path $PackageAbs "luminumbra_client_qa_app.exe"))
    Write-Host "[dry-run] net_soak: ctest --test-dir <BuildDir> -N -R NetSoak (discovery), then ctest --test-dir <BuildDir> -R NetSoak (only with -BuildDir)"
    Write-Host "[dry-run] perf_pointer: pass only with -PerfEvidence pointing at an existing file"
    Write-Host "[dry-run] audio_off: read -AudioChecklist JSON (three boolean fields)"
    Write-Host ("[dry-run] python {0} {1} --verdict-out {2}" -f $checkerPy, $receiptPath, $verdictPath)
    exit 0
}

$sections = [ordered]@{}
$orchestratorError = ""

try {
    # Every client-driving section needs the executable; fail fast if it is absent.
    $exePath = Get-ClientExe -Package $PackageAbs -Name $ExeName

    # --- render_qualification -------------------------------------------------
    $renderScript = Join-Path $PSScriptRoot "run-render-qualification.ps1"
    $renderOut = Join-Path $OutAbs "render"
    if ($SkipRender) {
        $sections["render_qualification"] = New-Section -Status "unevaluated" -Detail "skipped by -SkipRender"
    } elseif (-not (Test-Path -LiteralPath $renderScript -PathType Leaf)) {
        $sections["render_qualification"] = New-Section -Status "fail" -Detail "run-render-qualification.ps1 missing"
    } else {
        $renderParams = @{ Package = $PackageAbs; OutDir = $renderOut }
        if ($null -ne $DeclareRefresh -and $DeclareRefresh.Count -gt 0) { $renderParams["DeclareRefresh"] = $DeclareRefresh }
        $renderRun = Invoke-SiblingScript -Path $renderScript -Params $renderParams
        $renderRc = $renderRun.exit_code
        $qualPath = Join-Path $renderOut "qualification.json"
        if ($renderRun.error.Length -gt 0) {
            $sections["render_qualification"] = New-Section -Status "fail" -Detail ("run-render-qualification.ps1 error: " + $renderRun.error)
        } elseif ($null -eq $renderRc) {
            $sections["render_qualification"] = New-Section -Status "fail" -Detail "no exit code from run-render-qualification.ps1"
        } elseif ([int]$renderRc -eq 3) {
            $sections["render_qualification"] = New-Section -Status "unevaluated" -Detail "run-render-qualification.ps1 exit 3"
        } elseif ([int]$renderRc -ne 0) {
            $sections["render_qualification"] = New-Section -Status "fail" -Detail ("run-render-qualification.ps1 exit " + [int]$renderRc)
        } elseif (-not (Test-Path -LiteralPath $qualPath -PathType Leaf)) {
            $sections["render_qualification"] = New-Section -Status "unevaluated" -Detail "render/qualification.json missing"
        } else {
            try {
                $qual = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($qualPath, [System.Text.Encoding]::UTF8))
                if ((Test-JsonProperty -Object $qual -Name "render_floor_qualified") -and $qual.render_floor_qualified -eq $true) {
                    $sections["render_qualification"] = New-Section -Status "pass" -Evidence "render/qualification.json" -Detail "render floor qualified"
                } else {
                    $sections["render_qualification"] = New-Section -Status "fail" -Evidence "render/qualification.json" -Detail "render_floor_qualified is not true"
                }
            } catch {
                $sections["render_qualification"] = New-Section -Status "fail" -Evidence "render/qualification.json" -Detail ("cannot read qualification.json: " + $_.Exception.Message)
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
        $lifecycleRun = Invoke-SiblingScript -Path $lifecycleScript -Params $lifecycleParams
        $lifecycleRc = $lifecycleRun.exit_code
        $lifecycleEvidence = "lifecycle/acceptance-receipt.json"
        if ($lifecycleRun.error.Length -gt 0) {
            $sections["saved_world_lifecycle"] = New-Section -Status "fail" -Detail ("run-saved-world-lifecycle.ps1 error: " + $lifecycleRun.error)
        } elseif ($null -eq $lifecycleRc) {
            $sections["saved_world_lifecycle"] = New-Section -Status "fail" -Detail "no exit code from run-saved-world-lifecycle.ps1"
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
        $noUiRun = Invoke-SiblingScript -Path $noUiScript -Params @{ Package = $PackageAbs; WorldId = $fixtureWorldId; OutDir = $noUiOut; TimeoutSeconds = $TimeoutSeconds }
        $noUiRc = $noUiRun.exit_code
        $noUiEvidence = "noui-load/noui-load-receipt.json"
        $noUiReceipt = Join-Path $noUiOut "noui-load-receipt.json"
        if ($noUiRun.error.Length -gt 0) {
            $sections["noui_load"] = New-Section -Status "fail" -Detail ("run-noui-load.ps1 error: " + $noUiRun.error)
        } elseif ($null -eq $noUiRc) {
            $sections["noui_load"] = New-Section -Status "fail" -Detail "no exit code from run-noui-load.ps1"
        } elseif ([int]$noUiRc -eq 3) {
            $sections["noui_load"] = New-Section -Status "unevaluated" -Evidence $noUiEvidence -Detail "no-UI load unevaluated (exit 3)"
        } elseif ([int]$noUiRc -ne 0) {
            $sections["noui_load"] = New-Section -Status "fail" -Evidence $noUiEvidence -Detail ("run-noui-load.ps1 exit " + [int]$noUiRc)
        } else {
            try {
                $noUiParsed = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($noUiReceipt, [System.Text.Encoding]::UTF8))
                if ((Test-JsonProperty -Object $noUiParsed -Name "verdict") -and $noUiParsed.verdict -eq "pass") {
                    $sections["noui_load"] = New-Section -Status "pass" -Evidence $noUiEvidence -Detail "no-UI load verdict pass"
                } else {
                    $sections["noui_load"] = New-Section -Status "fail" -Evidence $noUiEvidence -Detail "no-UI load verdict is not pass"
                }
            } catch {
                $sections["noui_load"] = New-Section -Status "fail" -Evidence $noUiEvidence -Detail ("cannot read noui-load-receipt.json: " + $_.Exception.Message)
            }
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

            # Discovery before the run: which NetSoak tests does this build list?
            $listRun = Invoke-ClientSession -Exe $ctest.Source `
                -ArgumentList @("--test-dir", $buildAbs, "-N", "-R", "NetSoak") `
                -WorkingDirectory $OutAbs `
                -StdoutPath (Join-Path $netOut "ctest-list.stdout.log") `
                -StderrPath (Join-Path $netOut "ctest-list.stderr.log") `
                -TimeoutSeconds 300
            $listText = Read-TextOrEmpty -Path (Join-Path $netOut "ctest-list.stdout.log")
            $discoveredNames = @()
            foreach ($line in ($listText -split '\r?\n')) {
                if ($line -match '^\s*Test\s+#\d+:\s+(\S+)') { $discoveredNames += $Matches[1] }
            }
            $discoveredNames = @($discoveredNames)

            $netRun = Invoke-ClientSession -Exe $ctest.Source `
                -ArgumentList @("--test-dir", $buildAbs, "-R", "NetSoak") `
                -WorkingDirectory $OutAbs `
                -StdoutPath (Join-Path $netOut "ctest.stdout.log") `
                -StderrPath (Join-Path $netOut "ctest.stderr.log") `
                -TimeoutSeconds 3600
            $netText = (Read-TextOrEmpty -Path (Join-Path $netOut "ctest.stdout.log")) + "`n" + (Read-TextOrEmpty -Path (Join-Path $netOut "ctest.stderr.log"))
            $netEvidence = "net-soak/ctest.stdout.log"
            $summary = [regex]::Match($netText, '(\d+)% tests passed, (\d+) tests? failed out of (\d+)')

            # Tests reported Skipped / Not Run / Disabled, from per-test result lines and the
            # "did not run" summary block. Keyed by name so one test counted twice is still one.
            $notRunNames = @{}
            foreach ($line in ($netText -split '\r?\n')) {
                if ($line -match 'Test\s+#\d+:\s+(\S+).*\*\*\*(Skipped|Not Run|Disabled)') {
                    $notRunNames[$Matches[1]] = $true
                } elseif ($line -match '^\s*\d+\s+-\s+(\S+)\s+\((Skipped|Not Run|Disabled)\)') {
                    $notRunNames[$Matches[1]] = $true
                }
            }
            $skippedTests = [int]$notRunNames.Count
            $netExtra = @{
                required_tests = @("NetSoak")
                discovered_tests = $discoveredNames
                skipped_tests = $skippedTests
            }

            if ($netRun.timed_out) {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail "ctest NetSoak timed out" -Extra $netExtra
            } elseif ([int]$netRun.exit_code -ne 0) {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail ("ctest exit " + [int]$netRun.exit_code) -Extra $netExtra
            } elseif (-not ($discoveredNames -contains "NetSoak")) {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail "NetSoak not among discovered tests" -Extra $netExtra
            } elseif ($skippedTests -gt 0) {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail ("skipped, not run or disabled tests: " + $skippedTests) -Extra $netExtra
            } elseif ($netText -match 'No tests were found') {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail "zero NetSoak tests discovered" -Extra $netExtra
            } elseif (-not $summary.Success) {
                $sections["net_soak"] = New-Section -Status "unevaluated" -Evidence $netEvidence -Detail "ctest summary line not found" -Extra $netExtra
            } elseif ([int]$summary.Groups[2].Value -ne 0 -or [int]$summary.Groups[3].Value -lt 1) {
                $sections["net_soak"] = New-Section -Status "fail" -Evidence $netEvidence -Detail ("ctest: " + $summary.Value) -Extra $netExtra
            } else {
                $sections["net_soak"] = New-Section -Status "pass" -Evidence $netEvidence -Detail ("ctest: " + $summary.Value) -Extra $netExtra
            }
        }
    }

    # --- perf_pointer ---------------------------------------------------------
    if ([string]::IsNullOrEmpty($PerfEvidence)) {
        $sections["perf_pointer"] = New-Section -Status "unevaluated" -Detail "no -PerfEvidence given"
    } elseif (-not (Test-Path -LiteralPath $PerfEvidence -PathType Leaf)) {
        $sections["perf_pointer"] = New-Section -Status "unevaluated" -Detail "perf evidence file not found"
    } else {
        $perfCopy = Join-Path $OutAbs "perf-evidence.json"
        if ((Get-AbsolutePath $PerfEvidence) -ne $perfCopy) {
            Copy-Item -LiteralPath $PerfEvidence -Destination $perfCopy -Force
        }
        $sections["perf_pointer"] = New-Section -Status "pass" -Evidence "perf-evidence.json" -Detail "perf evidence file present"
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
        $audioCopy = Join-Path $OutAbs "audio-checklist.json"
        if ((Get-AbsolutePath $AudioChecklist) -ne $audioCopy) {
            Copy-Item -LiteralPath $AudioChecklist -Destination $audioCopy -Force
        }
        $audioEvidence = "audio-checklist.json"
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

} catch {
    $orchestratorError = $_.Exception.Message
    Write-Host ("error: " + $orchestratorError)
}

# Finalize even after a section/preflight error. Never leave a partial run without
# a receipt and checker verdict; sections that did not finish cannot pass.
foreach ($name in @("render_qualification", "saved_world_lifecycle", "noui_load", "serial_load_20", "net_soak", "perf_pointer", "audio_off")) {
    if (-not $sections.Contains($name)) {
        $sections[$name] = New-Section -Status "fail" -Detail ("section did not complete: " + $orchestratorError)
    }
}

try {
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
