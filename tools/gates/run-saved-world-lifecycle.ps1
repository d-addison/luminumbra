# NEEDS-NATIVE-RUN
#
# Saved-world lifecycle acceptance driver. Never run in CI: it launches
# luminumbra_client_qa_app.exe on the qualified Windows machine. Works under
# Windows PowerShell 5.1 and pwsh.
#
# -Package must be a DISPOSABLE copy of the QA client package. The run stages
# saved worlds into <Package>\worlds\saves and creates new worlds there.
# -OutDir must be empty or absent (fresh run). Exit codes: 0 pass, 1 fail,
# 2 invalid input to the checker, 3 unevaluated (from check_acceptance_receipt.py).
#
# Refusal variant directories under FixtureDir\refusals must be named exactly
# obsolete-metadata, future-metadata, corrupt-metadata, missing-metadata,
# missing-preset, truncated-region; otherwise the checker reports unevaluated.
#
# The create_edit_save and restart_select_load sessions share one runtime artifact
# directory (artifacts\persistence) because the load phase reads the save-phase
# marker file from there. shutdown.json is removed before each launch.
# Refusals require their exact variant diagnostic, exit 2, a fresh completed
# shutdown record with jobs drained, no timeout and unchanged saved bytes.
#
# Edit persistence evidence (receipt field edit_roundtrip): the create_edit_save and
# restart_select_load pair runs the client's own persistence_roundtrip_smoke scenario. Its
# save phase records the hash of the edited chunks and its load phase re-hashes the same
# chunks after a fresh load; this driver reads both phase artifacts and the checker fails
# unless the hashes match and at least one chunk was edited. The scenario works in its own
# session directory; the world-directory receipts prove that production loads (the
# switch_back sessions and --load-world) leave saved bytes unchanged except world_info.json.
# Visual confirmation of the edit in the production UI stays a manual checklist item.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\gates\run-saved-world-lifecycle.ps1 `
#       -Package D:\accept\package-copy -FixtureDir D:\accept\fixture `
#       -OutDir D:\accept\out\lifecycle -SwitchCount 6 -TimeoutSeconds 300
#
# Plan only, no disk writes:
#   pwsh -NoProfile -File tools/gates/run-saved-world-lifecycle.ps1 -Package P -FixtureDir F -OutDir O -DryRun

param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$FixtureDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [ValidateRange(2, 1000)][int]$SwitchCount = 6,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 300,
    [string]$Preset = "default",
    [string]$ExpectedTreeDigest = "",
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path (Join-Path $PSScriptRoot "lib") "AcceptanceCommon.ps1")

$ExeName = "luminumbra_client_qa_app.exe"
$repoRoot = Get-AbsolutePath (Join-Path (Join-Path $PSScriptRoot "..") "..")
$saveReceiptPy = Join-Path $repoRoot "tools/persistence/save_receipt.py"
$checkerPy = Join-Path $repoRoot "tools/persistence/check_acceptance_receipt.py"

$PackageAbs = Get-AbsolutePath $Package
$FixtureAbs = Get-AbsolutePath $FixtureDir
$OutAbs = Get-AbsolutePath $OutDir
$exePath = Join-Path $PackageAbs $ExeName
$savesRoot = Join-Path (Join-Path $PackageAbs "worlds") "saves"
$fixtureSaves = Join-Path (Join-Path $FixtureAbs "worlds") "saves"
$refusalsRoot = Join-Path $FixtureAbs "refusals"
$logsDir = Join-Path $OutAbs "logs"
$artifactsDir = Join-Path $OutAbs "artifacts"
$receiptsDir = Join-Path $OutAbs "receipts"
$crashDir = Join-Path $OutAbs "crashes"
$sessionDir = Join-Path $OutAbs "session"
$receiptPath = Join-Path $OutAbs "acceptance-receipt.json"
$verdictPath = Join-Path $OutAbs "acceptance-verdict.json"

function Get-CommonFlags {
    param([Parameter(Mandatory = $true)][string]$ArtifactDir)
    return @(
        "--no-audio", "--no-ui", "--hidden-window",
        "--hang-watchdog-seconds", "60",
        "--crash-dir", $crashDir,
        "--runtime-artifact-dir", $ArtifactDir
    )
}

function Show-PlannedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$Exe,
        [string[]]$Arguments = @()
    )
    Write-Host ("[dry-run] {0}: {1} {2}" -f $Label, $Exe, (Join-ProcessArguments -Values $Arguments))
}

function Invoke-Session {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$ArtifactDir
    )
    New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null
    # The persistence phases share their marker, but must never share shutdown proof.
    $shutdownPath = Join-Path $ArtifactDir "shutdown.json"
    if (Test-Path -LiteralPath $shutdownPath) {
        Remove-Item -LiteralPath $shutdownPath -Force
    }
    $run = Invoke-ClientSession -Exe $exePath -ArgumentList $Arguments `
        -WorkingDirectory $PackageAbs `
        -StdoutPath (Join-Path $logsDir "$Name.stdout.log") `
        -StderrPath (Join-Path $logsDir "$Name.stderr.log") `
        -TimeoutSeconds $TimeoutSeconds
    $shutdown = Read-ShutdownRecord -ArtifactDir $ArtifactDir
    Write-Host ("session {0}: exit {1}, timed_out {2}" -f $Name, $run.exit_code, $run.timed_out)
    return [ordered]@{
        name = $Name
        kind = $Kind
        exit_code = [int]$run.exit_code
        timed_out = [bool]$run.timed_out
        shutdown = $shutdown
    }
}

function Invoke-Snapshot {
    # Writes a save_receipt snapshot to $OutFile and returns the save_receipts entry.
    param(
        [Parameter(Mandatory = $true)][string]$WorldDir,
        [Parameter(Mandatory = $true)][string]$OutFile,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $result = Invoke-PythonTool -ScriptPath $saveReceiptPy -ArgumentList @("snapshot", $WorldDir, "--out", $OutFile)
    if ($result.exit_code -ne 0) {
        throw "save_receipt snapshot failed for $Label (exit $($result.exit_code)): $($result.stderr)"
    }
    $receipt = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($OutFile, [System.Text.Encoding]::UTF8))
    return [ordered]@{
        label = $Label
        tree_digest = [string]$receipt.tree_digest
        file_count = [int]$receipt.file_count
    }
}

function Get-DiffChanges {
    # Runs `save_receipt.py diff A B` and parses its "added: / removed: / changed:" lines.
    param(
        [Parameter(Mandatory = $true)][string]$AFile,
        [Parameter(Mandatory = $true)][string]$BFile
    )
    $result = Invoke-PythonTool -ScriptPath $saveReceiptPy -ArgumentList @("diff", $AFile, $BFile)
    if ($result.exit_code -ne 0 -and $result.exit_code -ne 1) {
        throw "save_receipt diff failed (exit $($result.exit_code)): $($result.stderr)"
    }
    $changes = [ordered]@{ added = @(); removed = @(); changed = @() }
    foreach ($line in ($result.stdout -split '\r?\n')) {
        if ($line -match '^(added|removed|changed): (.*)$') {
            $key = $Matches[1]
            $changes[$key] = @($changes[$key]) + @($Matches[2])
        }
    }
    return $changes
}

function Get-PrimaryWorld {
    # Newest world directory under saves that is not the fixture world.
    $candidates = @(Get-ChildItem -LiteralPath $savesRoot -Directory |
        Where-Object { $_.Name -ne $fixtureWorldId } |
        Sort-Object -Property CreationTimeUtc -Descending)
    if ($candidates.Count -eq 0) {
        throw "create_edit_save produced no new world under $savesRoot"
    }
    return $candidates[0].Name
}

function Get-EditRoundtrip {
    # Reads the save and load phase artifacts of persistence_roundtrip_smoke. Returns $null
    # (checker: unevaluated) when either is missing or unparseable.
    param([Parameter(Mandatory = $true)][string]$ArtifactDir)
    $docs = @{}
    foreach ($phase in @("save", "load")) {
        $path = Join-Path $ArtifactDir ("persistence-runtime-roundtrip-phase-" + $phase + ".json")
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
        try {
            $docs[$phase] = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8))
        } catch {
            return $null
        }
    }
    foreach ($phase in @("save", "load")) {
        if (-not (Test-JsonProperty -Object $docs[$phase] -Name "schema") -or
            $docs[$phase].schema -isnot [string] -or
            $docs[$phase].schema -cne "luminumbra.persistence_runtime_roundtrip_phase.v1" -or
            -not (Test-JsonProperty -Object $docs[$phase] -Name "phase") -or
            $docs[$phase].phase -isnot [string] -or
            $docs[$phase].phase -cne $phase) { return $null }
        if (-not (Test-JsonProperty -Object $docs[$phase] -Name "world_hash")) { return $null }
    }
    if (-not (Test-JsonProperty -Object $docs["save"] -Name "edited_chunk_ids")) { return $null }
    if ($docs["save"].edited_chunk_ids -isnot [System.Array]) { return $null }
    return [ordered]@{
        save_phase_schema = $docs["save"].schema
        load_phase_schema = $docs["load"].schema
        save_world_hash = $docs["save"].world_hash
        load_world_hash = $docs["load"].world_hash
        edited_chunk_count = [int]@($docs["save"].edited_chunk_ids).Count
    }
}

function Test-ProbeNumberArray {
    param($Value, [int]$Length)
    if ($Value -isnot [System.Array]) { return $false }
    $items = @($Value)
    if ($items.Count -ne $Length) { return $false }
    foreach ($item in $items) {
        if ($item -isnot [double] -and $item -isnot [int] -and $item -isnot [long]) { return $false }
    }
    return $true
}

function Test-GBufferProbeDocument {
    # True only when benchmark.json exists, parses, and has a non-empty gbuffer_probes array.
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    try {
        $doc = ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8))
    } catch {
        return $false
    }
    if (-not (Test-JsonProperty -Object $doc -Name "gbuffer_probes")) { return $false }
    if ($doc.gbuffer_probes -isnot [System.Array]) { return $false }
    $probes = @($doc.gbuffer_probes)
    if ($probes.Count -eq 0) { return $false }
    foreach ($probe in $probes) {
        foreach ($name in @("uv", "depth", "view_position", "material", "albedo")) {
            if (-not (Test-JsonProperty -Object $probe -Name $name)) { return $false }
        }
        if (-not (Test-ProbeNumberArray -Value $probe.uv -Length 2)) { return $false }
        if (-not (Test-ProbeNumberArray -Value $probe.view_position -Length 3)) { return $false }
        if (-not (Test-ProbeNumberArray -Value $probe.albedo -Length 3)) { return $false }
        if ($probe.depth -isnot [double] -and $probe.depth -isnot [int] -and $probe.depth -isnot [long]) { return $false }
        if ($probe.material -isnot [double] -and $probe.material -isnot [int] -and $probe.material -isnot [long]) { return $false }
    }
    return $true
}

function Get-RefusalDiagnostic {
    # Exact prefixes from SavedWorldCatalog, WorldSaveService and TerrainPresetLoader.
    # Variable parser detail, version and file path are permitted only after the prefix.
    param([string]$Variant, [string]$Text)
    $patterns = @{
        "obsolete-metadata" = 'This world predates the v0\.3\.0 format and cannot be opened\. Create a new world; migration is not supported\.'
        "future-metadata" = 'unsupported future world metadata container version [0-9]+'
        "corrupt-metadata" = 'Corrupt world metadata: \[json\.exception\.parse_error\.[0-9]+\]'
        "missing-metadata" = 'World unavailable: its save directory or metadata is missing or unreadable\.'
        "missing-preset" = 'failed to open world preset: [^\r\n]+'
        "truncated-region" = 'region file (?:is missing the LMR1 magic|record manifest is truncated|record payload is truncated): [^\r\n]+'
    }
    if (-not $patterns.ContainsKey($Variant)) { return $false }
    return [regex]::IsMatch($Text, 'World open refused: ' + $patterns[$Variant])
}

# ---------------------------------------------------------------------------
# Plan (dry run) - placeholders stand in for world ids that are only known at run time.
# ---------------------------------------------------------------------------
$fixtureWorldPlaceholder = "<fixture_world_id>"
$primaryPlaceholder = "<primary_world_id>"
$persistenceArtifact = Join-Path $artifactsDir "persistence"
$secondArtifact = Join-Path $artifactsDir "second_seed"

if ($DryRun) {
    Write-Host "[dry-run] no files will be written"
    Write-Host ("[dry-run] stage: copy {0}\* into {1} (refuse if a target exists)" -f $fixtureSaves, $savesRoot)
    Show-PlannedCommand -Label "snapshot before_edit" -Exe "python" -Arguments @($saveReceiptPy, "snapshot", (Join-Path $savesRoot $fixtureWorldPlaceholder), "--out", (Join-Path $receiptsDir "before_edit.json"))
    Show-PlannedCommand -Label "create_edit_save" -Exe $exePath -Arguments (@("--scenario", "persistence_roundtrip_smoke", "--persistence-phase", "save", "--persistence-session-dir", $sessionDir, "--auto-create-world", "--auto-enter-world") + (Get-CommonFlags -ArtifactDir $persistenceArtifact))
    Write-Host ("[dry-run] snapshot after_save (primary): {0}" -f (Join-Path $savesRoot $primaryPlaceholder))
    Show-PlannedCommand -Label "restart_select_load" -Exe $exePath -Arguments (@("--scenario", "persistence_roundtrip_smoke", "--persistence-phase", "load", "--persistence-session-dir", $sessionDir) + (Get-CommonFlags -ArtifactDir $persistenceArtifact))
    Show-PlannedCommand -Label "second_seed" -Exe $exePath -Arguments (@("--scenario", "auto_world_smoke", "--auto-create-world", "--auto-enter-world", "--world-preset", $Preset, "--timed-run", "20") + (Get-CommonFlags -ArtifactDir $secondArtifact))
    for ($i = 1; $i -le $SwitchCount; $i++) {
        if ($i % 2 -eq 1) { $switchWorld = $fixtureWorldPlaceholder } else { $switchWorld = $primaryPlaceholder }
        $switchArtifact = Join-Path $artifactsDir ("switch_back_" + $i)
        $switchBench = Join-Path (Join-Path $artifactsDir ("switch-" + $i)) "benchmark.json"
        $switchFrame = Join-Path (Join-Path $artifactsDir ("switch-" + $i)) "frame.ppm"
        $switchArgs = @("--load-world", $switchWorld, "--cam-pos", "0,120,0", "--cam-yaw", "0", "--cam-pitch", "-0.2", "--capture-size", "1280x720", "--render-benchmark", $switchBench, "--render-benchmark-schema", "v3", "--render-benchmark-warmup", "30", "--render-benchmark-frames", "8", "--render-benchmark-screenshot", $switchFrame) + (Get-CommonFlags -ArtifactDir $switchArtifact)
        Show-PlannedCommand -Label ("switch_back_" + $i) -Exe $exePath -Arguments $switchArgs
    }
    Write-Host "[dry-run] after the switch_back sessions: snapshot after_reload (primary) and fixture_after_loads (fixture)"
    Write-Host "[dry-run] reload_changes = diff after_save.json after_reload.json; fixture_changes = diff before_edit.json fixture_after_loads.json"
    Write-Host ("[dry-run] refusals: each directory under {0} is copied into {1} and run with --load-world <dir name> (120 s)" -f $refusalsRoot, $savesRoot)
    Write-Host "[dry-run] package identity, acceptance-receipt.json, then check_acceptance_receipt.py"
    exit 0
}

# ---------------------------------------------------------------------------
# Real run
# ---------------------------------------------------------------------------
try {
    $exePath = Get-ClientExe -Package $PackageAbs -Name $ExeName

    if (Test-Path -LiteralPath $OutAbs -PathType Container) {
        if (@(Get-ChildItem -LiteralPath $OutAbs -Force).Count -gt 0) {
            throw "OutDir must be empty or absent for a fresh run: $OutAbs"
        }
    }

    $fixtureWorldId = Get-FixtureWorldId -FixtureDir $FixtureAbs
    if ($fixtureWorldId.Length -eq 0 -or $fixtureWorldId -match '[\\/]') {
        throw "invalid fixture world id: '$fixtureWorldId'"
    }
    $fixtureWorldSource = Join-Path $fixtureSaves $fixtureWorldId
    if (-not (Test-Path -LiteralPath $fixtureWorldSource -PathType Container)) {
        throw "fixture world directory missing: $fixtureWorldSource"
    }

    New-Item -ItemType Directory -Force -Path $savesRoot, $logsDir, $artifactsDir, $receiptsDir, $crashDir, $sessionDir | Out-Null

    # 1. Stage the fixture into the package and take the before_edit receipt.
    $stagedItems = @(Get-ChildItem -LiteralPath $fixtureSaves -Force)
    foreach ($item in $stagedItems) {
        $target = Join-Path $savesRoot $item.Name
        if (Test-Path -LiteralPath $target) { throw "refusing to stage: $target already exists" }
    }
    foreach ($item in $stagedItems) {
        Copy-Item -LiteralPath $item.FullName -Destination $savesRoot -Recurse
    }
    $stagedFixture = Join-Path $savesRoot $fixtureWorldId
    $beforeEdit = Invoke-Snapshot -WorldDir $stagedFixture -OutFile (Join-Path $receiptsDir "before_edit.json") -Label "before_edit"
    $fixtureDigest = $beforeEdit.tree_digest
    $fixtureCount = $beforeEdit.file_count
    if ($ExpectedTreeDigest.Length -gt 0 -and $fixtureDigest -ne $ExpectedTreeDigest) {
        throw "staged fixture tree $fixtureDigest does not match -ExpectedTreeDigest $ExpectedTreeDigest"
    }

    $sessions = @()

    # 2. create_edit_save (shutdown record is read inside Invoke-Session, right after the session)
    $sessions += Invoke-Session -Name "create_edit_save" -Kind "create_edit_save" -ArtifactDir $persistenceArtifact `
        -Arguments (@("--scenario", "persistence_roundtrip_smoke", "--persistence-phase", "save", "--persistence-session-dir", $sessionDir, "--auto-create-world", "--auto-enter-world") + (Get-CommonFlags -ArtifactDir $persistenceArtifact))
    $primaryWorld = Get-PrimaryWorld
    $afterSave = Invoke-Snapshot -WorldDir (Join-Path $savesRoot $primaryWorld) -OutFile (Join-Path $receiptsDir "after_save.json") -Label "after_save"

    # 3. restart_select_load (reads the save-phase marker from the shared artifact dir)
    $sessions += Invoke-Session -Name "restart_select_load" -Kind "restart_select_load" -ArtifactDir $persistenceArtifact `
        -Arguments (@("--scenario", "persistence_roundtrip_smoke", "--persistence-phase", "load", "--persistence-session-dir", $sessionDir) + (Get-CommonFlags -ArtifactDir $persistenceArtifact))

    $editRoundtrip = Get-EditRoundtrip -ArtifactDir $persistenceArtifact

    # 4. second_seed (a second freshly created world)
    $sessions += Invoke-Session -Name "second_seed" -Kind "second_seed" -ArtifactDir $secondArtifact `
        -Arguments (@("--scenario", "auto_world_smoke", "--auto-create-world", "--auto-enter-world", "--world-preset", $Preset, "--timed-run", "20") + (Get-CommonFlags -ArtifactDir $secondArtifact))

    # 5. switch_back: fixed-pose render benchmark, alternating worlds (fixture first)
    #    (after_reload and fixture_after_loads are snapshotted after this loop)
    $switches = @()
    for ($i = 1; $i -le $SwitchCount; $i++) {
        if ($i % 2 -eq 1) { $switchWorld = $fixtureWorldId } else { $switchWorld = $primaryWorld }
        $name = "switch_back_$i"
        $switchArtifact = Join-Path $artifactsDir $name
        $switchDir = Join-Path $artifactsDir ("switch-" + $i)
        New-Item -ItemType Directory -Force -Path $switchDir | Out-Null
        $benchPath = Join-Path $switchDir "benchmark.json"
        $framePath = Join-Path $switchDir "frame.ppm"
        $switchArgs = @(
            "--load-world", $switchWorld,
            "--cam-pos", "0,120,0", "--cam-yaw", "0", "--cam-pitch", "-0.2",
            "--capture-size", "1280x720",
            "--render-benchmark", $benchPath,
            "--render-benchmark-schema", "v3",
            "--render-benchmark-warmup", "30",
            "--render-benchmark-frames", "8",
            "--render-benchmark-screenshot", $framePath
        ) + (Get-CommonFlags -ArtifactDir $switchArtifact)
        $sessions += Invoke-Session -Name $name -Kind "switch_back" -ArtifactDir $switchArtifact -Arguments $switchArgs

        $captureSha = $null
        if (Test-Path -LiteralPath $framePath -PathType Leaf) { $captureSha = Get-FileSha256Hex -Path $framePath }
        $probe = $null
        if (Test-GBufferProbeDocument -Path $benchPath) { $probe = "artifacts/switch-$i/benchmark.json" }
        $switches += [ordered]@{
            index = $i
            world = $switchWorld
            capture_sha256 = $captureSha
            g_buffer_probe = $probe
        }
    }

    # 5b. Snapshots after all loads: primary world (reload) and fixture world (fixture pair).
    $afterReload = Invoke-Snapshot -WorldDir (Join-Path $savesRoot $primaryWorld) -OutFile (Join-Path $receiptsDir "after_reload.json") -Label "after_reload"
    $reloadChanges = Get-DiffChanges -AFile (Join-Path $receiptsDir "after_save.json") -BFile (Join-Path $receiptsDir "after_reload.json")
    $fixtureAfterLoads = Invoke-Snapshot -WorldDir $stagedFixture -OutFile (Join-Path $receiptsDir "fixture_after_loads.json") -Label "fixture_after_loads"
    $fixtureChanges = Get-DiffChanges -AFile (Join-Path $receiptsDir "before_edit.json") -BFile (Join-Path $receiptsDir "fixture_after_loads.json")

    # 6. Refusal matrix: each FixtureDir\refusals\<variant> must fail cleanly and leave bytes unchanged.
    $refusals = @()
    if (Test-Path -LiteralPath $refusalsRoot -PathType Container) {
        $variantDirs = @(Get-ChildItem -LiteralPath $refusalsRoot -Directory | Sort-Object -Property Name)
        foreach ($variantDir in $variantDirs) {
            $variant = $variantDir.Name
            $target = Join-Path $savesRoot $variant
            if (Test-Path -LiteralPath $target) { throw "refusing to stage refusal variant: $target already exists" }
            Copy-Item -LiteralPath $variantDir.FullName -Destination $savesRoot -Recurse

            $preFile = Join-Path $receiptsDir ("refusal_" + $variant + "_before.json")
            $postFile = Join-Path $receiptsDir ("refusal_" + $variant + "_after.json")
            $preEntry = Invoke-Snapshot -WorldDir $target -OutFile $preFile -Label ("refusal_" + $variant + "_before")

            $refusalArtifact = Join-Path $artifactsDir ("refusal_" + $variant)
            New-Item -ItemType Directory -Force -Path $refusalArtifact | Out-Null
            $shutdownPath = Join-Path $refusalArtifact "shutdown.json"
            if (Test-Path -LiteralPath $shutdownPath) {
                Remove-Item -LiteralPath $shutdownPath -Force
            }
            $run = Invoke-ClientSession -Exe $exePath `
                -ArgumentList (@("--load-world", $variant, "--render-benchmark", (Join-Path $refusalArtifact "benchmark.json")) + (Get-CommonFlags -ArtifactDir $refusalArtifact)) `
                -WorkingDirectory $PackageAbs `
                -StdoutPath (Join-Path $logsDir ("refusal_" + $variant + ".stdout.log")) `
                -StderrPath (Join-Path $logsDir ("refusal_" + $variant + ".stderr.log")) `
                -TimeoutSeconds 120

            $logText = (Read-TextOrEmpty -Path (Join-Path $logsDir ("refusal_" + $variant + ".stdout.log"))) + "`n" + (Read-TextOrEmpty -Path (Join-Path $logsDir ("refusal_" + $variant + ".stderr.log")))
            $diagnostic = Get-RefusalDiagnostic -Variant $variant -Text $logText
            $shutdown = Read-ShutdownRecord -ArtifactDir $refusalArtifact
            $cleanTermination = $run.exit_code -eq 2 -and -not $run.timed_out -and
                $null -ne $shutdown -and $shutdown.schema -ceq "luminumbra.shutdown.v1" -and
                $shutdown.Contains("complete") -and $shutdown.complete -is [bool] -and
                $shutdown.complete -eq $true -and $shutdown.jobs_drained -is [bool] -and
                $shutdown.jobs_drained -eq $true
            $entry = [ordered]@{
                variant = $variant
                exit_code = [int]$run.exit_code
                diagnostic = [bool]($diagnostic -and $cleanTermination)
                expected_diagnostic = [bool]$diagnostic
                clean_termination = [bool]$cleanTermination
                shutdown = $shutdown
                timed_out = [bool]$run.timed_out
            }

            $postEntry = Invoke-Snapshot -WorldDir $target -OutFile $postFile -Label ("refusal_" + $variant + "_after")
            if ($preEntry.tree_digest -ne $postEntry.tree_digest) {
                $entry["diagnostic"] = $false
                $moved = Get-DiffChanges -AFile $preFile -BFile $postFile
                $entry["note"] = "save tree changed during refusal run: added=" + ($moved.added -join ",") + " removed=" + ($moved.removed -join ",") + " changed=" + ($moved.changed -join ",")
            }
            $refusals += $entry
            Write-Host ("refusal {0}: exit {1}, diagnostic {2}" -f $variant, $entry.exit_code, $entry.diagnostic)
        }
    }

    # 7. Package identity
    $exeSha = Get-FileSha256Hex -Path $exePath
    $versionFile = Join-Path $PackageAbs "version.txt"
    if (Test-Path -LiteralPath $versionFile -PathType Leaf) {
        $versionStamp = ([string](Read-TextOrEmpty -Path $versionFile)).Trim()
    } else {
        $versionStamp = [string](Get-Item -LiteralPath $exePath).VersionInfo.FileVersion
    }

    # 8. Receipt and checker
    $receipt = [ordered]@{
        schema = "luminumbra.saved_world_acceptance.v1"
        package = [ordered]@{
            exe_sha256 = $exeSha
            exe_name = $ExeName
            version_stamp = $versionStamp
        }
        fixture = [ordered]@{
            tree_digest = $fixtureDigest
            file_count = $fixtureCount
        }
        fixture_world_id = $fixtureWorldId
        primary_world_id = $primaryWorld
        edit_roundtrip = $editRoundtrip
        sessions = @($sessions)
        save_receipts = @($beforeEdit, $fixtureAfterLoads, $afterSave, $afterReload)
        reload_changes = $reloadChanges
        fixture_changes = $fixtureChanges
        switches = @($switches)
        refusals = @($refusals)
    }
    Write-JsonFile -Object $receipt -Path $receiptPath

    $check = Invoke-PythonTool -ScriptPath $checkerPy -ArgumentList @($receiptPath, "--min-switches", [string]$SwitchCount, "--verdict-out", $verdictPath)
    Write-Host $check.stdout.TrimEnd()
    if ($check.stderr.Trim().Length -gt 0) { Write-Host $check.stderr.TrimEnd() }
    exit $check.exit_code
} catch {
    Write-Host ("error: " + $_.Exception.Message)
    exit 1
}
