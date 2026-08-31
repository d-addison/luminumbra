<#
.SYNOPSIS
    Installs or repairs the canonical Luminumbra nightly scheduled task.

.DESCRIPTION
    This is an explicit, idempotent registration helper. It makes no change when
    the existing root task already has the exact non-elevated current-user
    principal, runner, debug preset, working directory, daily 02:00
    trigger, and bounded execution settings. Use -WhatIf to inspect a proposed
    registration. The nightly runner never invokes this helper automatically.
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "Medium")]
param(
    [ValidateSet("02:00")]
    [string]$DailyAt = "02:00"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptDir "nightly-provenance.ps1")

foreach ($cmdlet in @(
    "Get-ScheduledTask",
    "New-ScheduledTaskAction",
    "New-ScheduledTaskPrincipal",
    "New-ScheduledTaskSettingsSet",
    "New-ScheduledTaskTrigger",
    "Register-ScheduledTask",
    "Set-ScheduledTask")) {
    if ($null -eq (Get-Command $cmdlet -ErrorAction SilentlyContinue)) {
        throw "Windows Task Scheduler cmdlet '$cmdlet' is unavailable."
    }
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $scriptDir "..\..")).Path
$runnerPath = (Resolve-Path -LiteralPath (Join-Path $scriptDir "run-nightly-gate.ps1")).Path
$taskName = "Luminumbra Nightly Gate"
$taskPath = "\"
$powerShellExe = (Get-Command powershell.exe -ErrorAction Stop).Source
$actionArguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -BuildPreset debug' -f $runnerPath
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$identitySid = $identity.User.Value
$taskPrincipal = New-ScheduledTaskPrincipal `
    -UserId $identity.Name `
    -LogonType Interactive `
    -RunLevel Limited
$executionTimeLimit = New-TimeSpan -Hours 8

$action = New-ScheduledTaskAction `
    -Execute $powerShellExe `
    -Argument $actionArguments `
    -WorkingDirectory $repoRoot
$at = [datetime]::ParseExact(
    $DailyAt,
    "HH:mm",
    [Globalization.CultureInfo]::InvariantCulture)
$trigger = New-ScheduledTaskTrigger -Daily -At $at
$settings = New-ScheduledTaskSettingsSet `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit $executionTimeLimit

$existing = @(Get-ScheduledTask `
    -TaskName $taskName `
    -TaskPath $taskPath `
    -ErrorAction SilentlyContinue)
if ($existing.Count -gt 1) {
    throw "Expected at most one '$taskName' task at '$taskPath'."
}

$alreadyCanonical = $false
$canonicalFailure = ""
if ($existing.Count -eq 1) {
    $current = $existing[0]
    try {
        $null = Assert-NightlyRegisteredTaskDefinition `
            -Task $current `
            -ExpectedTaskName $taskName `
            -ExpectedTaskPath $taskPath `
            -ExpectedPrincipalSid $identitySid `
            -ExpectedActionExecute $powerShellExe `
            -ExpectedActionArguments $actionArguments `
            -ExpectedWorkingDirectory $repoRoot `
            -ExpectedDailyAt $DailyAt `
            -ExpectedExecutionTimeLimit $executionTimeLimit
        $alreadyCanonical = $true
    } catch {
        $canonicalFailure = $_.Exception.Message
    }
}

if ($existing.Count -eq 1) {
    try {
        Assert-NightlyTaskRepairAllowed `
            -IsCanonical $alreadyCanonical `
            -TaskState ([string]$existing[0].State)
    } catch {
        throw "$($_.Exception.Message) Definition mismatch: $canonicalFailure"
    }
}

if ($alreadyCanonical) {
    Write-Host "Nightly task is already canonical: $taskPath$taskName"
    exit 0
}

$operation = if ($existing.Count -eq 1) { "Repair canonical nightly task" } else { "Register canonical nightly task" }
if (-not $PSCmdlet.ShouldProcess("$taskPath$taskName", $operation)) {
    exit 0
}

if ($existing.Count -eq 1) {
    # Repair every field owned by the canonical contract, including the
    # deliberately non-elevated current-user principal.
    Set-ScheduledTask `
        -TaskName $taskName `
        -TaskPath $taskPath `
        -Action $action `
        -Trigger $trigger `
        -Settings $settings `
        -Principal $taskPrincipal | Out-Null
    Write-Host "Repaired nightly task: $taskPath$taskName"
} else {
    Register-ScheduledTask `
        -TaskName $taskName `
        -TaskPath $taskPath `
        -Action $action `
        -Trigger $trigger `
        -Settings $settings `
        -Principal $taskPrincipal `
        -Description "Luminumbra full nightly gate (debug build/tests/frontier/determinism)." | Out-Null
    Write-Host "Registered nightly task: $taskPath$taskName"
}

Write-Host "Action: $powerShellExe $actionArguments"
Write-Host "Daily:  $DailyAt local time"
