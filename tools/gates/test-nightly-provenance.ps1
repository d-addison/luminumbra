<#
.SYNOPSIS
    Pure behavior tests for nightly scheduler/report provenance predicates.

.DESCRIPTION
    Uses injected COM-like objects, identities, clocks, and Git revisions. It
    never connects to or mutates Windows Task Scheduler.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptDir "nightly-provenance.ps1")

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Because
    )
    if (-not $Condition) {
        throw "Expected true: $Because"
    }
}

function Assert-False {
    param(
        [bool]$Condition,
        [string]$Because
    )
    if ($Condition) {
        throw "Expected false: $Because"
    }
}

function Assert-Throws {
    param(
        [scriptblock]$Action,
        [string]$Because
    )
    try {
        & $Action | Out-Null
    } catch {
        return
    }
    throw "Expected rejection: $Because"
}

$testSid = "S-1-5-21-1111111111-2222222222-3333333333-1001"
$sidResolver = {
    param([string]$UserId)
    if ($UserId -eq "gateuser" -or $UserId -eq "BUILDHOST\gateuser") {
        return $testSid
    }
    return $null
}.GetNewClosure()

Assert-True `
    (Test-NightlyPrincipalIdentity -PrincipalUserId "gateuser" `
        -CurrentSid $testSid -AccountSidResolver $sidResolver) `
    "a Task Scheduler short account name resolves to the current SID"
Assert-True `
    (Test-NightlyPrincipalIdentity `
        -PrincipalUserId "BUILDHOST\gateuser" `
        -CurrentSid $testSid -AccountSidResolver $sidResolver) `
    "a qualified account name resolves to the current SID"
Assert-True `
    (Test-NightlyPrincipalIdentity -PrincipalUserId $testSid `
        -CurrentSid $testSid -AccountSidResolver $sidResolver) `
    "an already-normalized SID compares directly"
Assert-False `
    (Test-NightlyPrincipalIdentity -PrincipalUserId "Mallory" `
        -CurrentSid $testSid -AccountSidResolver $sidResolver) `
    "an unresolved identity is rejected"

$null = Assert-NightlyTrackedChangePolicy -ChangedPaths @()
Assert-Throws {
    Assert-NightlyTrackedChangePolicy -ChangedPaths @(
        "src/luminumbra_client/main_client.cpp")
} "a tracked source change"
Assert-Throws {
    Assert-NightlyTrackedChangePolicy -ChangedPaths @(
        "docs/development.md") -AllowedPaths @("README.md")
} "a path outside an explicit allowlist"

$expectedPath = "\Luminumbra Nightly Gate"
$expectedAction = "C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe"
$expectedPid = [uint32]4242
$expectedTaskName = "Luminumbra Nightly Gate"
$expectedTaskRoot = "\"
$expectedArguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "C:\fixture\luminumbra\tools\gates\run-nightly-gate.ps1" -BuildPreset debug'
$expectedWorkingDirectory = "C:\fixture\luminumbra"

function New-TaskDefinitionFixture {
    param([string]$UserId = "gateuser")
    return [pscustomobject]@{
        TaskName = $expectedTaskName
        TaskPath = $expectedTaskRoot
        State = "Running"
        Principal = [pscustomobject]@{
            UserId = $UserId
            LogonType = "Interactive"
            RunLevel = "Limited"
        }
        Actions = @([pscustomobject]@{
            Execute = $expectedAction
            Arguments = $expectedArguments
            WorkingDirectory = $expectedWorkingDirectory
        })
        Triggers = @([pscustomobject]@{
            Enabled = $true
            DaysInterval = 1
            StartBoundary = "2026-07-11T02:00:00-04:00"
            EndBoundary = ""
        })
        Settings = [pscustomobject]@{
            Enabled = $true
            MultipleInstances = "IgnoreNew"
            StartWhenAvailable = $true
            ExecutionTimeLimit = "PT8H"
        }
    }
}

function Assert-TestTaskDefinition {
    param([object]$Task)
    return Assert-NightlyRegisteredTaskDefinition `
        -Task $Task `
        -ExpectedTaskName $expectedTaskName `
        -ExpectedTaskPath $expectedTaskRoot `
        -ExpectedPrincipalSid $testSid `
        -ExpectedActionExecute $expectedAction `
        -ExpectedActionArguments $expectedArguments `
        -ExpectedWorkingDirectory $expectedWorkingDirectory `
        -ExpectedDailyAt "02:00" `
        -ExpectedExecutionTimeLimit ([timespan]::FromHours(8)) `
        -AccountSidResolver $sidResolver
}

foreach ($principalId in @(
    "gateuser", "BUILDHOST\gateuser", $testSid)) {
    $null = Assert-TestTaskDefinition `
        -Task (New-TaskDefinitionFixture -UserId $principalId)
}

function Assert-TaskDefinitionMutationRejected {
    param(
        [scriptblock]$Mutation,
        [string]$Because
    )
    $fixture = New-TaskDefinitionFixture
    & $Mutation $fixture
    Assert-Throws {
        Assert-TestTaskDefinition -Task $fixture
    } $Because
}

$definitionMutations = @(
    @({ param($t) $t.TaskName = "Wrong Task" }, "wrong task name"),
    @({ param($t) $t.TaskPath = "\Wrong\" }, "wrong task path"),
    @({ param($t) $t.Principal.UserId = "Mallory" }, "wrong principal SID"),
    @({ param($t) $t.Principal.LogonType = "ServiceAccount" }, "wrong logon type"),
    @({ param($t) $t.Principal.RunLevel = "Highest" }, "wrong run level"),
    @({ param($t) $t.Actions = @() }, "zero registered actions"),
    @({ param($t) $t.Actions = @($t.Actions[0], $t.Actions[0]) }, "two registered actions"),
    @({ param($t) $t.Actions[0].Execute = "C:\Windows\System32\cmd.exe" }, "wrong registered execute"),
    @({ param($t) $t.Actions[0].Arguments = "-File wrong.ps1" }, "wrong registered arguments"),
    @({ param($t) $t.Actions[0].WorkingDirectory = "D:\Wrong" }, "wrong registered working directory"),
    @({ param($t) $t.Triggers = @() }, "zero registered triggers"),
    @({ param($t) $t.Triggers = @($t.Triggers[0], $t.Triggers[0]) }, "two registered triggers"),
    @({ param($t) $t.Triggers[0].Enabled = $false }, "disabled trigger"),
    @({ param($t) $t.Triggers[0].DaysInterval = 2 }, "non-daily trigger"),
    @({ param($t) $t.Triggers[0].StartBoundary = "2026-07-11T03:00:00-04:00" }, "wrong trigger time"),
    @({ param($t) $t.Triggers[0].EndBoundary = "2026-07-12T02:00:00-04:00" }, "bounded trigger"),
    @({ param($t) $t.Settings.Enabled = $false }, "disabled settings"),
    @({ param($t) $t.Settings.MultipleInstances = "Parallel" }, "wrong multiple-instance policy"),
    @({ param($t) $t.Settings.StartWhenAvailable = $false }, "StartWhenAvailable disabled"),
    @({ param($t) $t.Settings.ExecutionTimeLimit = "PT7H" }, "wrong execution limit")
)
foreach ($case in $definitionMutations) {
    Assert-TaskDefinitionMutationRejected `
        -Mutation $case[0] -Because $case[1]
}

$definitionSnapshot = Assert-TestTaskDefinition `
    -Task (New-TaskDefinitionFixture)
$reportedDefinition = $definitionSnapshot |
    ConvertTo-Json -Depth 6 | ConvertFrom-Json
$null = Assert-NightlyTaskDefinitionSnapshot `
    -Snapshot $reportedDefinition `
    -ExpectedTaskName $expectedTaskName `
    -ExpectedTaskPath $expectedTaskRoot `
    -ExpectedPrincipalSid $testSid `
    -ExpectedActionExecute $expectedAction `
    -ExpectedActionArguments $expectedArguments `
    -ExpectedWorkingDirectory $expectedWorkingDirectory
$reportedDefinition.actions[0].arguments = "-File forged.ps1"
Assert-Throws {
    Assert-NightlyTaskDefinitionSnapshot `
        -Snapshot $reportedDefinition `
        -ExpectedTaskName $expectedTaskName `
        -ExpectedTaskPath $expectedTaskRoot `
        -ExpectedPrincipalSid $testSid `
        -ExpectedActionExecute $expectedAction `
        -ExpectedActionArguments $expectedArguments `
        -ExpectedWorkingDirectory $expectedWorkingDirectory
} "a forged reported start definition"

$null = Assert-NightlyTaskRepairAllowed `
    -IsCanonical $true -TaskState "Running"
$null = Assert-NightlyTaskRepairAllowed `
    -IsCanonical $false -TaskState "Ready"
Assert-Throws {
    Assert-NightlyTaskRepairAllowed `
        -IsCanonical $false -TaskState "Running"
} "repairing a noncanonical running task"

function New-RunningTaskFixture {
    param(
        [string]$Path = $expectedPath,
        [string]$CurrentAction = $expectedAction,
        [object]$EnginePID = $expectedPid,
        [string]$InstanceGuid = "{7727128E-EDCA-4A40-B363-6FC83E2EB4AC}"
    )
    return [pscustomobject]@{
        Path = $Path
        CurrentAction = $CurrentAction
        EnginePID = $EnginePID
        InstanceGuid = $InstanceGuid
    }
}

$validInstance = New-RunningTaskFixture
$validatedInstance = Assert-NightlyRunningTaskProvenance `
    -RunningTasks @($validInstance) `
    -ExpectedTaskPath $expectedPath `
    -ExpectedCurrentAction $expectedAction `
    -ExpectedProcessId $expectedPid
Assert-True ($validatedInstance.engine_pid -eq $expectedPid) `
    "one canonical COM-like instance is accepted"

$unrelatedInstance = New-RunningTaskFixture -Path "\Microsoft\Windows\CacheTask"
$validatedAmongUnrelated = Assert-NightlyRunningTaskProvenance `
    -RunningTasks @($unrelatedInstance, $validInstance) `
    -ExpectedTaskPath $expectedPath `
    -ExpectedCurrentAction $expectedAction `
    -ExpectedProcessId $expectedPid
Assert-True ($validatedAmongUnrelated.engine_pid -eq $expectedPid) `
    "unrelated global running tasks do not count as nightly instances"

Assert-Throws {
    Assert-NightlyRunningTaskProvenance `
        -RunningTasks @() `
        -ExpectedTaskPath $expectedPath `
        -ExpectedCurrentAction $expectedAction `
        -ExpectedProcessId $expectedPid
} "zero nightly instances"
Assert-Throws {
    Assert-NightlyRunningTaskProvenance `
        -RunningTasks @($validInstance, (New-RunningTaskFixture)) `
        -ExpectedTaskPath $expectedPath `
        -ExpectedCurrentAction $expectedAction `
        -ExpectedProcessId $expectedPid
} "two nightly instances"
Assert-Throws {
    Assert-NightlyRunningTaskProvenance `
        -RunningTasks @($unrelatedInstance) `
        -ExpectedTaskPath $expectedPath `
        -ExpectedCurrentAction $expectedAction `
        -ExpectedProcessId $expectedPid
} "wrong task path"
Assert-Throws {
    Assert-NightlyRunningTaskProvenance `
        -RunningTasks @((New-RunningTaskFixture -EnginePID 4243)) `
        -ExpectedTaskPath $expectedPath `
        -ExpectedCurrentAction $expectedAction `
        -ExpectedProcessId $expectedPid
} "wrong engine PID"
Assert-Throws {
    Assert-NightlyRunningTaskProvenance `
        -RunningTasks @((New-RunningTaskFixture -CurrentAction "C:\Windows\System32\cmd.exe")) `
        -ExpectedTaskPath $expectedPath `
        -ExpectedCurrentAction $expectedAction `
        -ExpectedProcessId $expectedPid
} "wrong CurrentAction"
Assert-Throws {
    Assert-NightlyRunningTaskProvenance `
        -RunningTasks @((New-RunningTaskFixture -InstanceGuid "not-a-guid")) `
        -ExpectedTaskPath $expectedPath `
        -ExpectedCurrentAction $expectedAction `
        -ExpectedProcessId $expectedPid
} "malformed instance GUID"
Assert-Throws {
    Assert-NightlyRunningTaskProvenance `
        -RunningTasks @((New-RunningTaskFixture -InstanceGuid ([guid]::Empty.ToString()))) `
        -ExpectedTaskPath $expectedPath `
        -ExpectedCurrentAction $expectedAction `
        -ExpectedProcessId $expectedPid
} "empty instance GUID"

$now = [datetimeoffset]::Parse("2026-07-11T12:00:00-04:00")
$head = "0123456789abcdef0123456789abcdef01234567"
$reportScheduler = [pscustomobject]@{
    task_path = $expectedPath
    current_action = $expectedAction
    instance_guid = "7727128e-edca-4a40-b363-6fc83e2eb4ac"
    engine_pid = $expectedPid
}

function New-ReportFixture {
    param(
        [datetimeoffset]$RunStartedAt,
        [datetimeoffset]$CompletedAt,
        [datetimeoffset]$GeneratedAt,
        [string]$HeadAtStart = $head,
        [string]$HeadAtCompletion = $head,
        [object]$SchedulerInstance = $reportScheduler
    )
    return [pscustomobject]@{
        run_started_at = $RunStartedAt.ToString("o")
        completed_at = $CompletedAt.ToString("o")
        generated_at = $GeneratedAt.ToString("o")
        git_head_at_start = $HeadAtStart
        git_head_at_completion = $HeadAtCompletion
        scheduler_instance = $SchedulerInstance
    }
}

$validReport = New-ReportFixture `
    -RunStartedAt $now.AddHours(-1) `
    -CompletedAt $now.AddMinutes(-1) `
    -GeneratedAt $now.AddMinutes(-1)
$null = Assert-NightlyReportProvenance `
    -Report $validReport -CurrentGitHead $head -Now $now `
    -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction

# Both documented freshness boundaries are inclusive.
foreach ($boundary in @($now.AddHours(-26), $now.AddMinutes(5))) {
    $boundaryReport = New-ReportFixture `
        -RunStartedAt $boundary.AddMinutes(-1) `
        -CompletedAt $boundary `
        -GeneratedAt $boundary
    $null = Assert-NightlyReportProvenance `
        -Report $boundaryReport -CurrentGitHead $head -Now $now `
        -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
}

$staleAt = $now.AddHours(-26).AddSeconds(-1)
Assert-Throws {
    Assert-NightlyReportProvenance `
        -Report (New-ReportFixture -RunStartedAt $staleAt.AddMinutes(-1) `
            -CompletedAt $staleAt -GeneratedAt $staleAt) `
        -CurrentGitHead $head -Now $now `
        -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
} "a report older than 26 hours"
$futureAt = $now.AddMinutes(5).AddSeconds(1)
Assert-Throws {
    Assert-NightlyReportProvenance `
        -Report (New-ReportFixture -RunStartedAt $now `
            -CompletedAt $futureAt -GeneratedAt $futureAt) `
        -CurrentGitHead $head -Now $now `
        -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
} "a report more than five minutes in the future"
Assert-Throws {
    Assert-NightlyReportProvenance `
        -Report $validReport `
        -CurrentGitHead "ffffffffffffffffffffffffffffffffffffffff" -Now $now `
        -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
} "a report from another Git HEAD"
Assert-Throws {
    Assert-NightlyReportProvenance `
        -Report (New-ReportFixture -RunStartedAt $now.AddHours(-1) `
            -CompletedAt $now.AddMinutes(-1) `
            -GeneratedAt $now.AddMinutes(-1) `
            -HeadAtCompletion "ffffffffffffffffffffffffffffffffffffffff") `
        -CurrentGitHead $head -Now $now `
        -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
} "a report whose start and completion Git HEADs drift"
Assert-Throws {
    Assert-NightlyReportProvenance `
        -Report (New-ReportFixture -RunStartedAt $now.AddMinutes(-1) `
            -CompletedAt $now.AddMinutes(-1) -GeneratedAt $now.AddMinutes(-1)) `
        -CurrentGitHead $head -Now $now `
        -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
} "a zero-length run interval"
Assert-Throws {
    Assert-NightlyReportProvenance `
        -Report (New-ReportFixture -RunStartedAt $now `
            -CompletedAt $now.AddMinutes(-1) -GeneratedAt $now.AddMinutes(-1)) `
        -CurrentGitHead $head -Now $now `
        -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
} "a reversed run interval"
Assert-Throws {
    Assert-NightlyReportProvenance `
        -Report (New-ReportFixture -RunStartedAt $now.AddHours(-1) `
            -CompletedAt $now.AddMinutes(-2) -GeneratedAt $now.AddMinutes(-1)) `
        -CurrentGitHead $head -Now $now `
        -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
} "completed_at differing from generated_at"

foreach ($badScheduler in @(
    [pscustomobject]@{
        task_path = "\Wrong Task"; current_action = $expectedAction
        instance_guid = $reportScheduler.instance_guid; engine_pid = $expectedPid
    },
    [pscustomobject]@{
        task_path = $expectedPath; current_action = "C:\Windows\System32\cmd.exe"
        instance_guid = $reportScheduler.instance_guid; engine_pid = $expectedPid
    },
    [pscustomobject]@{
        task_path = $expectedPath; current_action = $expectedAction
        instance_guid = "not-a-guid"; engine_pid = $expectedPid
    },
    [pscustomobject]@{
        task_path = $expectedPath; current_action = $expectedAction
        instance_guid = $reportScheduler.instance_guid; engine_pid = 0
    }
)) {
    Assert-Throws {
        Assert-NightlyReportProvenance `
            -Report (New-ReportFixture -RunStartedAt $now.AddHours(-1) `
                -CompletedAt $now.AddMinutes(-1) `
                -GeneratedAt $now.AddMinutes(-1) `
                -SchedulerInstance $badScheduler) `
            -CurrentGitHead $head -Now $now `
            -ExpectedTaskPath $expectedPath -ExpectedCurrentAction $expectedAction
    } "a report with malformed scheduler instance fields"
}

Write-Host "Nightly provenance behavior tests: PASS"
