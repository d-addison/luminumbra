<#
.SYNOPSIS
    Local scheduled wrapper for the full Luminumbra nightly gate.

.DESCRIPTION
    Intended to be driven by the exact Windows Task Scheduler contract installed
    explicitly with register-nightly-gate-task.ps1. This script never registers
    or changes the task itself.

    The wrapper prepends C:\msys64\ucrt64\bin to PATH, runs each gate step in order,
    records pass/fail and duration, continues after failures, writes dated markdown
    and JSON reports under build/gate-artifacts/nightly/, and exits non-zero if any
    step failed.
#>
[CmdletBinding()]
param(
    [ValidateSet("debug")]
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
$ArtifactDir = Join-Path $RepoRoot "build\gate-artifacts\nightly"
$provenanceScript = Join-Path $ScriptDir "nightly-provenance.ps1"
if (-not (Test-Path -LiteralPath $provenanceScript)) {
    throw "Nightly provenance helper is missing: $provenanceScript"
}
. $provenanceScript

$runStartedAt = Get-Date

$scheduledTaskName = "Luminumbra Nightly Gate"
$scheduledTaskPath = "\"
$nightlyRunnerPath = (Resolve-Path -LiteralPath $MyInvocation.MyCommand.Path).Path
$scheduledPowerShell = (Get-Command powershell.exe -ErrorAction Stop).Source
$scheduledArguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -BuildPreset debug' -f $nightlyRunnerPath

if ($null -eq (Get-Command Get-ScheduledTask -ErrorAction SilentlyContinue)) {
    throw "Nightly scheduler provenance requires Get-ScheduledTask."
}
$registeredTasks = @(Get-ScheduledTask `
    -TaskName $scheduledTaskName `
    -TaskPath $scheduledTaskPath `
    -ErrorAction Stop)
if ($registeredTasks.Count -ne 1) {
    throw "Nightly scheduler provenance requires exactly one registered '$scheduledTaskPath$scheduledTaskName' task."
}
$registeredTask = $registeredTasks[0]
if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
    [string]$registeredTask.State, "Running")) {
    throw "Nightly scheduler provenance requires the registered task state to be Running."
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$schedulerDefinitionAtStart = Assert-NightlyRegisteredTaskDefinition `
    -Task $registeredTask `
    -ExpectedTaskName $scheduledTaskName `
    -ExpectedTaskPath $scheduledTaskPath `
    -ExpectedPrincipalSid ([string]$identity.User.Value) `
    -ExpectedActionExecute $scheduledPowerShell `
    -ExpectedActionArguments $scheduledArguments `
    -ExpectedWorkingDirectory $RepoRoot `
    -ExpectedDailyAt "02:00" `
    -ExpectedExecutionTimeLimit ([timespan]::FromHours(8))

# Bind the process to the live Task Scheduler instance before executing any
# gate. Schedule.Service collections are 1-based COM collections; do not pipe
# or foreach-enumerate them because Windows PowerShell 5.1 can silently expose
# an incomplete view through the automation enumerator.
$scheduleService = $null
$runningTasks = $null
$runningTaskSnapshots = New-Object System.Collections.Generic.List[object]
try {
    $scheduleService = New-Object -ComObject Schedule.Service
    $scheduleService.Connect()
    $runningTasks = $scheduleService.GetRunningTasks(0)
    for ($index = 1; $index -le [int]$runningTasks.Count; $index++) {
        $runningTask = $runningTasks.Item($index)
        $runningTaskSnapshots.Add([pscustomobject]@{
            Path = [string]$runningTask.Path
            CurrentAction = [string]$runningTask.CurrentAction
            InstanceGuid = [string]$runningTask.InstanceGuid
            EnginePID = $runningTask.EnginePID
        }) | Out-Null
    }
} catch {
    throw "Nightly scheduler provenance could not enumerate running tasks: $($_.Exception.Message)"
} finally {
    if ($null -ne $runningTasks -and
        [Runtime.InteropServices.Marshal]::IsComObject($runningTasks)) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($runningTasks)
    }
    if ($null -ne $scheduleService -and
        [Runtime.InteropServices.Marshal]::IsComObject($scheduleService)) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($scheduleService)
    }
}
$schedulerInstance = Assert-NightlyRunningTaskProvenance `
    -RunningTasks $runningTaskSnapshots.ToArray() `
    -ExpectedTaskPath "$scheduledTaskPath$scheduledTaskName" `
    -ExpectedCurrentAction $scheduledPowerShell `
    -ExpectedProcessId ([uint32]$PID)
$gitHeadAtStart = Get-NightlyGitHead -RepoRoot $RepoRoot
Assert-NightlyTrackedTreeClean -RepoRoot $RepoRoot

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
Set-Location $RepoRoot

$results = New-Object System.Collections.Generic.List[object]

function Format-CommandLine {
    param([string]$File, [string[]]$Arguments)
    $parts = @($File) + $Arguments
    return ($parts | ForEach-Object {
        if ($_ -match "\s") { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join " "
}

function Invoke-NightlyStep {
    param(
        [string]$Name,
        [string]$File,
        [string[]]$Arguments,
        [string]$Notes = ""
    )

    $started = Get-Date
    $duration = [TimeSpan]::Zero
    $exitCode = 0
    $outputText = ""
    $commandLine = Format-CommandLine -File $File -Arguments $Arguments

    Write-Host "==> $Name"
    try {
        $global:LASTEXITCODE = 0
        # Windows PowerShell 5.1 promotes native stderr records to terminating
        # errors when the wrapper-wide preference is Stop. CMake legitimately
        # writes deprecation warnings to stderr, so capture native output under
        # Continue and trust the native exit code. Script/cmdlet failures still
        # surface through their non-zero process exit.
        $savedErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "Continue"
            $output = & $File @Arguments 2>&1
            $nativeExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
        }
        $duration = (Get-Date) - $started
        if ($nativeExitCode -ne $null) {
            $exitCode = [int]$nativeExitCode
        }
        $outputText = ($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    } catch {
        $duration = (Get-Date) - $started
        $exitCode = 1
        $outputText = $_.Exception.Message
        if ($_.ScriptStackTrace) {
            $outputText += [Environment]::NewLine + $_.ScriptStackTrace
        }
    }

    $status = if ($exitCode -eq 0) { "PASS" } else { "FAIL" }
    $results.Add([pscustomobject]@{
        name = $Name
        command = $commandLine
        status = $status
        exit_code = $exitCode
        duration_seconds = [math]::Round($duration.TotalSeconds, 3)
        started_at = $started.ToString("o")
        notes = $Notes
        output = $outputText
    }) | Out-Null
}

function Add-SkippedStep {
    param(
        [string]$Name,
        [string]$Command,
        [string]$Notes
    )
    $results.Add([pscustomobject]@{
        name = $Name
        command = $Command
        status = "SKIPPED"
        exit_code = 0
        duration_seconds = 0.0
        started_at = (Get-Date).ToString("o")
        notes = $Notes
        output = ""
    }) | Out-Null
}

$frontierScript = Join-Path $RepoRoot "tools\gates\validate-engine-frontier.ps1"
$determinismScript = Join-Path $RepoRoot "tools\gates\validate-determinism-matrix.ps1"

Invoke-NightlyStep `
    -Name "Build" `
    -File "cmake" `
    -Arguments @("--build", "--preset", $BuildPreset) `
    -Notes "Build is not covered by validate-engine-frontier.ps1 -Mode All."

Invoke-NightlyStep `
    -Name "UnitTests" `
    -File "ctest" `
    -Arguments @("--preset", $BuildPreset, "--output-on-failure") `
    -Notes "Serial by design; parallel -j gives false failures in this tree."

Invoke-NightlyStep `
    -Name "EngineFrontierAll" `
    -File "powershell.exe" `
    -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $frontierScript, "-Mode", "All", "-BuildPreset", $BuildPreset) `
    -Notes "Default frontier lane; excludes Build, UnitTests, and heavy visual/network lanes."

if ((Test-Path -LiteralPath $determinismScript) -and
    ((Get-Content -LiteralPath $determinismScript -Raw) -match '\[switch\]\s*\$Quick')) {
    Invoke-NightlyStep `
        -Name "DeterminismMatrixQuick" `
        -File "powershell.exe" `
        -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $determinismScript, "-Quick") `
        -Notes "Quick determinism matrix verified from validate-determinism-matrix.ps1."
} else {
    Add-SkippedStep `
        -Name "DeterminismMatrixQuick" `
        -Command "$determinismScript -Quick" `
        -Notes "Skipped: validate-determinism-matrix.ps1 or its -Quick switch was not found."
}

$failed = @($results | Where-Object { $_.status -eq "FAIL" })
$overall = if ($failed.Count -eq 0) { "PASS" } else { "FAIL" }
$gitHeadAtCompletion = Get-NightlyGitHead -RepoRoot $RepoRoot
Assert-NightlyTrackedTreeClean -RepoRoot $RepoRoot
if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
    $gitHeadAtStart, $gitHeadAtCompletion)) {
    throw "Nightly gate Git HEAD drifted from '$gitHeadAtStart' to '$gitHeadAtCompletion'; refusing to publish mixed-revision evidence."
}
$completedAt = Get-Date
$generatedAt = $completedAt
$stamp = $generatedAt.ToString("yyyy-MM-dd-HHmmss", [Globalization.CultureInfo]::InvariantCulture)
$markdownPath = Join-Path $ArtifactDir "$stamp.md"
$jsonPath = Join-Path $ArtifactDir "$stamp.json"

$report = [pscustomobject]@{
    schema = "luminumbra.nightly_gate.v1"
    # generated_at is captured once and is also the canonical filename stamp.
    generated_at = $generatedAt.ToString("o")
    run_started_at = $runStartedAt.ToString("o")
    completed_at = $completedAt.ToString("o")
    git_head_at_start = $gitHeadAtStart
    git_head_at_completion = $gitHeadAtCompletion
    repo_root = $RepoRoot
    build_preset = $BuildPreset
    path_prefix = "C:\msys64\ucrt64\bin"
    overall_status = $overall
    markdown_report = $markdownPath
    json_report = $jsonPath
    scheduler_contract = [pscustomobject]@{
        task_name = $scheduledTaskName
        task_path = $scheduledTaskPath
        action_execute = $scheduledPowerShell
        action_arguments = $scheduledArguments
        action_working_directory = $RepoRoot
        last_run_match_window_seconds = 120
    }
    scheduler_instance = $schedulerInstance
    scheduler_definition_at_start = $schedulerDefinitionAtStart
    # Windows PowerShell 5.1 can throw "Argument types do not match" when a
    # generic List[object] is wrapped directly in @(...). Materialize its
    # native array explicitly so the report is emitted after the final stage.
    steps = $results.ToArray()
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# Nightly gate $stamp") | Out-Null
$lines.Add("") | Out-Null
$lines.Add("- Repo: ``$RepoRoot``") | Out-Null
$lines.Add("- Build preset: ``$BuildPreset``") | Out-Null
$lines.Add("- PATH prefix: ``C:\msys64\ucrt64\bin``") | Out-Null
$lines.Add("- Overall: **$overall**") | Out-Null
$lines.Add("") | Out-Null
$lines.Add("| Step | Status | Exit | Duration | Command | Notes |") | Out-Null
$lines.Add("| --- | --- | ---: | ---: | --- | --- |") | Out-Null
foreach ($r in $results) {
    $cmd = $r.command.Replace("|", "\|")
    $notes = $r.notes.Replace("|", "\|")
    $lines.Add("| $($r.name) | $($r.status) | $($r.exit_code) | $($r.duration_seconds)s | ``$cmd`` | $notes |") | Out-Null
}
$lines.Add("") | Out-Null
$lines.Add("## Output excerpts") | Out-Null
foreach ($r in $results) {
    $lines.Add("") | Out-Null
    $lines.Add("### $($r.name)") | Out-Null
    $lines.Add("") | Out-Null
    if ([string]::IsNullOrWhiteSpace($r.output)) {
        $lines.Add("_No output captured._") | Out-Null
    } else {
        $excerpt = $r.output
        if ($excerpt.Length -gt 12000) {
            $excerpt = $excerpt.Substring($excerpt.Length - 12000)
            $excerpt = "[truncated to last 12000 chars]" + [Environment]::NewLine + $excerpt
        }
        $lines.Add('```') | Out-Null
        $lines.Add($excerpt) | Out-Null
        $lines.Add('```') | Out-Null
    }
}

$lines | Set-Content -LiteralPath $markdownPath -Encoding UTF8
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

Write-Host "Nightly gate report: $markdownPath"
Write-Host "Nightly gate JSON:   $jsonPath"

if ($failed.Count -gt 0) {
    exit 1
}
exit 0
