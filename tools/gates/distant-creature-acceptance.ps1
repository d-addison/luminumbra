# Runs prebuilt, local native binaries. Enabled population starts on real tick one. Qualification covers 30 simulation minutes
# through the same populated host and continuation assertions as the short run.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CommonTests,
    [Parameter(Mandatory = $true)][string]$HostTests,
    [Parameter(Mandatory = $true)][string]$DressingTests,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [switch]$Qualification
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
foreach ($binary in @($CommonTests, $HostTests, $DressingTests)) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Native test binary is absent: $binary"
    }
}
[void](New-Item -ItemType Directory -Path $OutDir -Force)
$outRoot = (Resolve-Path -LiteralPath $OutDir).Path
$resultPath = Join-Path $outRoot 'acceptance.json'
if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath }
$previousWorkers = [Environment]::GetEnvironmentVariable('LUMINUMBRA_JOB_WORKERS', 'Process')
$previousQualification = [Environment]::GetEnvironmentVariable('LUMINUMBRA_CREATURE_QUALIFICATION', 'Process')
$previousTemp = [Environment]::GetEnvironmentVariable('TEMP', 'Process')
$previousTmp = [Environment]::GetEnvironmentVariable('TMP', 'Process')
$temporary = Join-Path $outRoot ('work-' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $temporary)
function Invoke-NativeAcceptance {
    param([string]$Binary, [string]$Filter, [string]$Name, [int]$MinimumTests)
    $xmlPath = Join-Path $outRoot ($Name + '.xml')
    $logPath = Join-Path $outRoot ($Name + '.log')
    if (Test-Path -LiteralPath $xmlPath) { Remove-Item -LiteralPath $xmlPath }
    & $Binary ("--gtest_filter=" + $Filter) ("--gtest_output=xml:" + $xmlPath) 2>&1 |
        Tee-Object -FilePath $logPath | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Native acceptance failed: $Name (exit $LASTEXITCODE)" }
    if (-not (Test-Path -LiteralPath $xmlPath)) { throw "Native report absent: $Name" }
    [xml]$report = Get-Content -LiteralPath $xmlPath -Raw
    if ([int]$report.testsuites.tests -lt $MinimumTests -or
        [int]$report.testsuites.failures -ne 0 -or [int]$report.testsuites.disabled -ne 0) {
        throw "Incomplete or failed native report: $Name"
    }
    foreach ($case in $report.SelectNodes('//testcase')) {
        if ($case.status -ne 'run' -or $case.SelectSingleNode('skipped') -ne $null) {
            throw "Unevaluated native case: $Name"
        }
    }
}
function Assert-RequiredCases {
    param([string]$ReportName, [string[]]$Names)
    [xml]$report = Get-Content -LiteralPath (Join-Path $outRoot ($ReportName + '.xml')) -Raw
    foreach ($name in $Names) {
        $matching = @($report.SelectNodes('//testcase') | Where-Object { $_.name -eq $name })
        if ($matching.Count -ne 1) { throw "Required native regression absent or duplicated: $name" }
    }
}
try {
    [Environment]::SetEnvironmentVariable('LUMINUMBRA_JOB_WORKERS', '1', 'Process')
    [Environment]::SetEnvironmentVariable('TEMP', $temporary, 'Process')
    [Environment]::SetEnvironmentVariable('TMP', $temporary, 'Process')
    [Environment]::SetEnvironmentVariable('LUMINUMBRA_CREATURE_QUALIFICATION', '0', 'Process')
    Invoke-NativeAcceptance $DressingTests 'WorldDressing.*' 'dressing' 6
    Invoke-NativeAcceptance $CommonTests 'CreatureHostAcceptance.*' 'continuation' 6
    Invoke-NativeAcceptance $HostTests 'CreatureHostAcceptance.*:HostSchedulerFault.*:LockstepReplayHardening.*:WorldOpenRefusal.LegacyEnabledWorldWithoutS8RecordsAdoptsAndStamps:WorldOpenRefusal.PresentMalformedCreatureOrActivationDataRefuses:WorldOpenRefusal.PresentContentMismatchPreservesCurrentWorld' 'hosts' 16
    Assert-RequiredCases 'dressing' @(
        'EnabledDressingAttachesPersistedCreaturesWithoutSpawning',
        'EnabledRestartDoesNotDuplicateAmbientCreaturesOrWaterHoles',
        'FlagOffPlacementAndConsumeBytesMatchLegacyGolden')
    Assert-RequiredCases 'continuation' @(
        'SaveLoadContinuationAcrossAllCreatureTiers',
        'SaveLoadContinuationIncludesBirthsAndPersistentReferences',
        'SaveLoadContinuationIncludesPendingTravelAndFields',
        'SaveLoadContinuationIncludesCompletedWorkAndActivationOutbox',
        'RegisteredPopulationWaitsForFirstRealTick')
    Assert-RequiredCases 'hosts' @(
        'FreshEnabledServerUsesRegisteredPopulationContract',
        'FlagOffHostSaveFileCountHashAndArtifactBytesMatchGolden',
        'RestoredEnabledServerDoesNotRespawnRoster',
        'MixedPlayersAndCreaturesStepExactlyOnce',
        'ContinuationNegativeControlsCoverEveryPersistedComponent',
        'EnabledFreshWorldCreatureReplayMatchesStartAndCheckpoints',
        'ChangedCreaturePolicyRefusesBeforeReplayTicks',
        'ChangedPopulationContractRefusesBeforeReplayTicks',
        'EnabledCreatureFaultAbortsRecordingWithoutCompletedSidecar',
        'LegacyEnabledWorldWithoutS8RecordsAdoptsAndStamps',
        'PresentMalformedCreatureOrActivationDataRefuses',
        'PresentContentMismatchPreservesCurrentWorld',
        'PopulationFaultSavesCoherentStateAndReturnsFaultExit',
        'CreatureConsumerFaultStopsRemainingFrameTicks',
        'ActivationDeliveryFaultRetainsRecoverableOutbox')
    if ($Qualification) {
        [Environment]::SetEnvironmentVariable('LUMINUMBRA_CREATURE_QUALIFICATION', '1', 'Process')
        Invoke-NativeAcceptance $HostTests 'CreatureHostAcceptance.PopulatedHeadlessQualification' 'qualification-30-minutes' 1
    }
    $qualificationStatus = 'unrun'
    if ($Qualification) { $qualificationStatus = 'passed' }
    [ordered]@{ schema = 'luminumbra.creature_acceptance.v1'; short = 'passed'; qualification = $qualificationStatus } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outRoot 'acceptance.json') -Encoding UTF8
} finally {
    [Environment]::SetEnvironmentVariable('LUMINUMBRA_JOB_WORKERS', $previousWorkers, 'Process')
    [Environment]::SetEnvironmentVariable('LUMINUMBRA_CREATURE_QUALIFICATION', $previousQualification, 'Process')
    [Environment]::SetEnvironmentVariable('TEMP', $previousTemp, 'Process')
    [Environment]::SetEnvironmentVariable('TMP', $previousTmp, 'Process')
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
