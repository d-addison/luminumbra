<#
.SYNOPSIS
    Shared provenance predicates for the Luminumbra scheduled nightly gate.

.DESCRIPTION
    The assertion functions in this file are intentionally free of Task
    Scheduler and filesystem queries. Callers provide snapshots of COM objects,
    the current Git HEAD, and the current time so the security-sensitive
    decisions can be covered by deterministic behavior tests.
#>

function Resolve-NightlyPrincipalSidValue {
    [CmdletBinding()]
    param(
        [AllowNull()]
        [string]$UserId,
        [AllowNull()]
        [scriptblock]$AccountSidResolver
    )

    if ([string]::IsNullOrWhiteSpace($UserId)) {
        return $null
    }

    try {
        return (New-Object Security.Principal.SecurityIdentifier($UserId)).Value
    } catch {
        # Task Scheduler commonly normalizes DOMAIN\user to the short account
        # name. Resolve either representation before comparing identities.
    }

    try {
        $resolved = if ($null -ne $AccountSidResolver) {
            & $AccountSidResolver $UserId
        } else {
            $account = New-Object Security.Principal.NTAccount($UserId)
            $account.Translate([Security.Principal.SecurityIdentifier]).Value
        }
        if ([string]::IsNullOrWhiteSpace([string]$resolved)) {
            return $null
        }
        return (New-Object Security.Principal.SecurityIdentifier(
            [string]$resolved)).Value
    } catch {
        return $null
    }
}

function Test-NightlyPrincipalIdentity {
    [CmdletBinding()]
    param(
        [AllowNull()]
        [string]$PrincipalUserId,
        [AllowNull()]
        [string]$CurrentSid,
        [AllowNull()]
        [scriptblock]$AccountSidResolver
    )

    $principalSid = Resolve-NightlyPrincipalSidValue `
        -UserId $PrincipalUserId `
        -AccountSidResolver $AccountSidResolver
    return -not [string]::IsNullOrWhiteSpace($principalSid) -and
        -not [string]::IsNullOrWhiteSpace($CurrentSid) -and
        [StringComparer]::OrdinalIgnoreCase.Equals($principalSid, $CurrentSid)
}

function Test-NightlyTrueValue {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)
    return [StringComparer]::OrdinalIgnoreCase.Equals([string]$Value, "True")
}

function ConvertTo-NightlyTaskDefinitionSnapshot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [object]$Task,
        [AllowNull()]
        [scriptblock]$AccountSidResolver
    )

    if ($null -eq $Task) {
        throw "Nightly task definition: registered task is missing."
    }

    $principal = $Task.Principal
    $principalSid = if ($null -eq $principal) {
        $null
    } else {
        Resolve-NightlyPrincipalSidValue `
            -UserId ([string]$principal.UserId) `
            -AccountSidResolver $AccountSidResolver
    }

    $actions = New-Object System.Collections.Generic.List[object]
    foreach ($action in @($Task.Actions)) {
        if ($null -ne $action) {
            $actions.Add([pscustomobject]@{
                execute = [string]$action.Execute
                arguments = [string]$action.Arguments
                working_directory = [string]$action.WorkingDirectory
            }) | Out-Null
        }
    }

    $triggers = New-Object System.Collections.Generic.List[object]
    foreach ($trigger in @($Task.Triggers)) {
        if ($null -eq $trigger) {
            continue
        }
        $dailyAt = $null
        try {
            $dailyAt = ([DateTimeOffset]::Parse(
                [string]$trigger.StartBoundary,
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::RoundtripKind)).ToString(
                    "HH:mm", [Globalization.CultureInfo]::InvariantCulture)
        } catch {
            $dailyAt = $null
        }
        $triggers.Add([pscustomobject]@{
            enabled = Test-NightlyTrueValue $trigger.Enabled
            days_interval = [int]$trigger.DaysInterval
            daily_at = $dailyAt
            end_boundary = [string]$trigger.EndBoundary
        }) | Out-Null
    }

    $settings = $Task.Settings
    $executionTimeLimit = $null
    if ($null -ne $settings) {
        try {
            $limit = if ($settings.ExecutionTimeLimit -is [TimeSpan]) {
                [TimeSpan]$settings.ExecutionTimeLimit
            } else {
                [System.Xml.XmlConvert]::ToTimeSpan(
                    [string]$settings.ExecutionTimeLimit)
            }
            $executionTimeLimit = [System.Xml.XmlConvert]::ToString($limit)
        } catch {
            $executionTimeLimit = $null
        }
    }

    return [pscustomobject]@{
        task_name = [string]$Task.TaskName
        task_path = [string]$Task.TaskPath
        state = [string]$Task.State
        principal = [pscustomobject]@{
            sid = [string]$principalSid
            logon_type = if ($null -eq $principal) {
                ""
            } else {
                [string]$principal.LogonType
            }
            run_level = if ($null -eq $principal) {
                ""
            } else {
                [string]$principal.RunLevel
            }
        }
        actions = $actions.ToArray()
        triggers = $triggers.ToArray()
        settings = [pscustomobject]@{
            enabled = $null -ne $settings -and
                (Test-NightlyTrueValue $settings.Enabled)
            multiple_instances = if ($null -eq $settings) {
                ""
            } else {
                [string]$settings.MultipleInstances
            }
            start_when_available = $null -ne $settings -and
                (Test-NightlyTrueValue $settings.StartWhenAvailable)
            execution_time_limit = [string]$executionTimeLimit
        }
    }
}

function Assert-NightlyTaskDefinitionSnapshot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [object]$Snapshot,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedTaskName,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedTaskPath,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedPrincipalSid,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedActionExecute,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedActionArguments,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedWorkingDirectory,
        [string]$ExpectedDailyAt = "02:00",
        [timespan]$ExpectedExecutionTimeLimit = ([timespan]::FromHours(8))
    )

    if ($null -eq $Snapshot) {
        throw "Nightly task definition: snapshot is missing."
    }
    if (-not [StringComparer]::Ordinal.Equals(
            [string]$Snapshot.task_name, $ExpectedTaskName) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$Snapshot.task_path, $ExpectedTaskPath)) {
        throw "Nightly task definition: task name/path are not canonical."
    }

    if ($null -eq $Snapshot.principal -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$Snapshot.principal.sid, $ExpectedPrincipalSid) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$Snapshot.principal.logon_type, "Interactive") -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$Snapshot.principal.run_level, "Limited")) {
        throw "Nightly task definition: principal must be the current SID with Interactive/Limited execution."
    }

    $actions = @($Snapshot.actions)
    if ($actions.Count -ne 1 -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$actions[0].execute, $ExpectedActionExecute) -or
        -not [StringComparer]::Ordinal.Equals(
            [string]$actions[0].arguments, $ExpectedActionArguments) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$actions[0].working_directory,
            $ExpectedWorkingDirectory)) {
        throw "Nightly task definition: require exactly one canonical action, arguments, and working directory."
    }

    $triggers = @($Snapshot.triggers)
    if ($triggers.Count -ne 1 -or
        -not (Test-NightlyTrueValue $triggers[0].enabled) -or
        [int]$triggers[0].days_interval -ne 1 -or
        -not [StringComparer]::Ordinal.Equals(
            [string]$triggers[0].daily_at, $ExpectedDailyAt) -or
        -not [string]::IsNullOrWhiteSpace(
            [string]$triggers[0].end_boundary)) {
        throw "Nightly task definition: require one enabled daily $ExpectedDailyAt trigger with no end boundary."
    }

    $expectedLimit = [System.Xml.XmlConvert]::ToString(
        $ExpectedExecutionTimeLimit)
    if ($null -eq $Snapshot.settings -or
        -not (Test-NightlyTrueValue $Snapshot.settings.enabled) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$Snapshot.settings.multiple_instances, "IgnoreNew") -or
        -not (Test-NightlyTrueValue `
            $Snapshot.settings.start_when_available) -or
        -not [StringComparer]::Ordinal.Equals(
            [string]$Snapshot.settings.execution_time_limit,
            $expectedLimit)) {
        throw "Nightly task definition: settings must be enabled, IgnoreNew, StartWhenAvailable, with execution limit $expectedLimit."
    }

    return $Snapshot
}

function Assert-NightlyRegisteredTaskDefinition {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [object]$Task,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedTaskName,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedTaskPath,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedPrincipalSid,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedActionExecute,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedActionArguments,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedWorkingDirectory,
        [string]$ExpectedDailyAt = "02:00",
        [timespan]$ExpectedExecutionTimeLimit = ([timespan]::FromHours(8)),
        [AllowNull()]
        [scriptblock]$AccountSidResolver
    )

    $snapshot = ConvertTo-NightlyTaskDefinitionSnapshot `
        -Task $Task -AccountSidResolver $AccountSidResolver
    return Assert-NightlyTaskDefinitionSnapshot `
        -Snapshot $snapshot `
        -ExpectedTaskName $ExpectedTaskName `
        -ExpectedTaskPath $ExpectedTaskPath `
        -ExpectedPrincipalSid $ExpectedPrincipalSid `
        -ExpectedActionExecute $ExpectedActionExecute `
        -ExpectedActionArguments $ExpectedActionArguments `
        -ExpectedWorkingDirectory $ExpectedWorkingDirectory `
        -ExpectedDailyAt $ExpectedDailyAt `
        -ExpectedExecutionTimeLimit $ExpectedExecutionTimeLimit
}

function Assert-NightlyTaskRepairAllowed {
    [CmdletBinding()]
    param(
        [bool]$IsCanonical,
        [AllowNull()]
        [string]$TaskState
    )

    if (-not $IsCanonical -and
        [StringComparer]::OrdinalIgnoreCase.Equals($TaskState, "Running")) {
        throw "Nightly task repair: refusing to replace a noncanonical definition while its instance is Running."
    }
}

function Assert-NightlyRunningTaskProvenance {
    [CmdletBinding()]
    param(
        [AllowEmptyCollection()]
        [object[]]$RunningTasks,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedTaskPath,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedCurrentAction,
        [Parameter(Mandatory = $true)]
        [uint32]$ExpectedProcessId
    )

    if ($ExpectedProcessId -eq 0) {
        throw "Nightly scheduler provenance: expected process ID must be non-zero."
    }

    $matching = New-Object System.Collections.Generic.List[object]
    foreach ($candidate in @($RunningTasks)) {
        if ($null -ne $candidate -and
            [StringComparer]::OrdinalIgnoreCase.Equals(
                [string]$candidate.Path, $ExpectedTaskPath)) {
            $matching.Add($candidate) | Out-Null
        }
    }
    if ($matching.Count -ne 1) {
        throw "Nightly scheduler provenance: expected exactly one running '$ExpectedTaskPath' instance; found $($matching.Count)."
    }

    $instance = $matching[0]
    $currentAction = [string]$instance.CurrentAction
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
        $currentAction, $ExpectedCurrentAction)) {
        throw "Nightly scheduler provenance: CurrentAction '$currentAction' does not match '$ExpectedCurrentAction'."
    }

    $instanceGuid = [guid]::Empty
    $instanceGuidText = [string]$instance.InstanceGuid
    if ([string]::IsNullOrWhiteSpace($instanceGuidText) -or
        -not [guid]::TryParse($instanceGuidText, [ref]$instanceGuid) -or
        $instanceGuid -eq [guid]::Empty) {
        throw "Nightly scheduler provenance: InstanceGuid is missing or invalid."
    }

    $enginePid = [uint32]0
    if (-not [uint32]::TryParse(
            [string]$instance.EnginePID,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$enginePid) -or
        $enginePid -eq 0 -or
        $enginePid -ne $ExpectedProcessId) {
        throw "Nightly scheduler provenance: EnginePID '$($instance.EnginePID)' does not match runner PID '$ExpectedProcessId'."
    }

    return [pscustomobject]@{
        task_path = [string]$instance.Path
        current_action = $currentAction
        instance_guid = $instanceGuid.ToString("D")
        engine_pid = $enginePid
    }
}

function ConvertTo-NightlyDateTimeOffset {
    [CmdletBinding()]
    param(
        [AllowNull()]
        [object]$Value,
        [Parameter(Mandatory = $true)]
        [string]$FieldName
    )

    $parsed = [datetimeoffset]::MinValue
    if (-not [datetimeoffset]::TryParse(
        [string]$Value,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind,
        [ref]$parsed)) {
        throw "Nightly report provenance: '$FieldName' is not a round-trip timestamp."
    }
    return $parsed
}

function Assert-NightlyReportProvenance {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [object]$Report,
        [Parameter(Mandatory = $true)]
        [string]$CurrentGitHead,
        [Parameter(Mandatory = $true)]
        [datetimeoffset]$Now,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedTaskPath,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedCurrentAction,
        [timespan]$MaximumAge = ([timespan]::FromHours(26)),
        [timespan]$MaximumFutureSkew = ([timespan]::FromMinutes(5))
    )

    if ($MaximumAge -lt [timespan]::Zero -or
        $MaximumFutureSkew -lt [timespan]::Zero) {
        throw "Nightly report provenance: freshness bounds must be non-negative."
    }

    $runStartedAt = ConvertTo-NightlyDateTimeOffset `
        -Value $Report.run_started_at -FieldName "run_started_at"
    $completedAt = ConvertTo-NightlyDateTimeOffset `
        -Value $Report.completed_at -FieldName "completed_at"
    $generatedAt = ConvertTo-NightlyDateTimeOffset `
        -Value $Report.generated_at -FieldName "generated_at"

    if ($runStartedAt -ge $completedAt -or $completedAt -ne $generatedAt) {
        throw "Nightly report provenance: require run_started_at < completed_at == generated_at."
    }
    if ($generatedAt -gt $Now.Add($MaximumFutureSkew)) {
        throw "Nightly report provenance: generated_at exceeds the permitted future clock skew."
    }
    if ($generatedAt -lt $Now.Subtract($MaximumAge)) {
        throw "Nightly report provenance: generated_at is older than the permitted evidence age."
    }

    $headAtStart = [string]$Report.git_head_at_start
    $headAtCompletion = [string]$Report.git_head_at_completion
    if ([string]::IsNullOrWhiteSpace($CurrentGitHead) -or
        [string]::IsNullOrWhiteSpace($headAtStart) -or
        [string]::IsNullOrWhiteSpace($headAtCompletion) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            $headAtStart, $headAtCompletion) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            $headAtStart, $CurrentGitHead)) {
        throw "Nightly report provenance: report Git HEAD does not match the current checkout."
    }

    if ($null -eq $Report.scheduler_instance) {
        throw "Nightly report provenance: scheduler_instance is missing."
    }
    $scheduler = $Report.scheduler_instance
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$scheduler.task_path, $ExpectedTaskPath) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$scheduler.current_action, $ExpectedCurrentAction)) {
        throw "Nightly report provenance: scheduler path/action do not match the canonical task."
    }

    $instanceGuid = [guid]::Empty
    if ([string]::IsNullOrWhiteSpace([string]$scheduler.instance_guid) -or
        -not [guid]::TryParse(
            [string]$scheduler.instance_guid, [ref]$instanceGuid) -or
        $instanceGuid -eq [guid]::Empty) {
        throw "Nightly report provenance: scheduler instance GUID is missing or invalid."
    }
    $enginePid = [uint32]0
    if (-not [uint32]::TryParse(
            [string]$scheduler.engine_pid,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$enginePid) -or
        $enginePid -eq 0) {
        throw "Nightly report provenance: scheduler engine PID is missing or invalid."
    }

    return [pscustomobject]@{
        run_started_at = $runStartedAt
        completed_at = $completedAt
        generated_at = $generatedAt
        instance_guid = $instanceGuid
        engine_pid = $enginePid
    }
}

function Assert-NightlyTrackedChangePolicy {
    [CmdletBinding()]
    param(
        [AllowEmptyCollection()]
        [object[]]$ChangedPaths,
        [string[]]$AllowedPaths = @()
    )

    $unexpected = New-Object System.Collections.Generic.List[string]
    foreach ($rawPath in @($ChangedPaths)) {
        $path = ([string]$rawPath).Trim().Replace("\", "/")
        if ([string]::IsNullOrWhiteSpace($path)) {
            continue
        }
        $allowed = $false
        foreach ($rawAllowedPath in $AllowedPaths) {
            $allowedPath = ([string]$rawAllowedPath).Trim().Replace("\", "/")
            if ([StringComparer]::OrdinalIgnoreCase.Equals($path, $allowedPath)) {
                $allowed = $true
                break
            }
        }
        if (-not $allowed -and -not $unexpected.Contains($path)) {
            $unexpected.Add($path) | Out-Null
        }
    }

    if ($unexpected.Count -gt 0) {
        throw "Nightly tree provenance: tracked paths differ from HEAD: $($unexpected.ToArray() -join ', ')."
    }
}

function Assert-NightlyTrackedTreeClean {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    # `git diff HEAD` combines index and worktree differences for files already
    # tracked by HEAD. Deliberately omit --no-index/--others: untracked build and
    # Local orchestration artifacts are outside this provenance policy.
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5.1 promotes native stderr (including harmless
        # autocrlf warnings) to ErrorRecord. Keep it available for diagnostics
        # without allowing it to terminate an otherwise successful git diff.
        $ErrorActionPreference = "Continue"
        $output = @(& git -C $RepoRoot --no-pager diff --no-ext-diff `
            --name-only HEAD -- 2>&1)
        $gitExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($gitExitCode -ne 0) {
        $message = ($output | ForEach-Object { $_.ToString() }) -join " "
        throw "Unable to inspect tracked nightly tree state for '$RepoRoot': $message"
    }
    $changedPaths = @($output | Where-Object {
        $_ -isnot [System.Management.Automation.ErrorRecord]
    })
    Assert-NightlyTrackedChangePolicy -ChangedPaths $changedPaths
}

function Get-NightlyGitHead {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = @(& git -C $RepoRoot rev-parse --verify HEAD 2>&1)
        $gitExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    $head = ($output | Where-Object {
        $_ -isnot [System.Management.Automation.ErrorRecord]
    } | ForEach-Object { $_.ToString() }) -join ""
    $head = $head.Trim()
    if ($gitExitCode -ne 0 -or $head -notmatch '^[0-9a-fA-F]{40}([0-9a-fA-F]{24})?$') {
        throw "Unable to capture a canonical Git HEAD for '$RepoRoot'."
    }
    return $head
}
