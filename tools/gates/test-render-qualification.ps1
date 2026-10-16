<#
.SYNOPSIS
    File-only native qualification wrapper contract tests. Run with -NoProfile.
.DESCRIPTION
    Invokes the real wrapper with function stubs for python and Get-CimInstance.
    The runner stub records argv and supplies exit codes; no client, Python, or
    hardware discovery runs. Owns and removes one fixture directory in the repository.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = [System.IO.Path]::GetFullPath((Join-Path $scriptDir "../.."))
$wrapper = Join-Path $scriptDir "run-render-qualification.ps1"
$fixtureName = ".render-qualification-contract-" + [guid]::NewGuid().ToString("N")
$fixtureRoot = Join-Path $repo $fixtureName
$recordPath = Join-Path $fixtureRoot "runner argv.json"
$clientRelative = Join-Path $fixtureName "client fixture.exe"
$packageRelative = Join-Path $fixtureName "package fixture"
$manifestRelative = Join-Path $fixtureName "build manifest.json"
$outRelative = Join-Path $fixtureName "output fixture"
$outPath = Join-Path $repo $outRelative
$childHarness = Join-Path $fixtureRoot "wrapper process.ps1"
$childConfiguration = Join-Path $fixtureRoot "wrapper parameters.xml"
$powershell = (Get-Process -Id $PID).Path
$checks = 0

# Keep this independent of the wrapper's list so removing a refusal fails a test.
$overrideNames = @(
    'LUMIN_GL_DEBUG', 'LUMIN_RENDER_SCALE', 'LUMIN_TREE_IMPOSTORS', 'LUMIN_ATMOS', 'LUMIN_MOON',
    'LUMIN_CLOUD_QUALITY', 'LUMIN_SSAO_QUALITY', 'LUMIN_MOON_WRAP_FLOOR', 'LUMIN_GRADE',
    'LUMIN_CAVE_AO', 'LUMIN_SCENT_DECAL', 'LUMIN_RHI', 'LUMIN_FRAME_SCAN_SETTLE',
    'LUMINUMBRA_VISUAL_SWEEP', 'LUMINUMBRA_VISUAL_SWEEP_WINTER',
    'LUMINUMBRA_ATMOS_MOTION_CAPTURE', 'LUMINUMBRA_JOB_THROTTLE'
)
$savedOverrides = @{}
foreach ($name in $overrideNames) {
    $savedOverrides[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

function Assert-Test {
    param([bool]$Condition, [string]$Because)
    if (-not $Condition) { throw "FAILED: $Because" }
    $script:checks++
}

function Remove-Override {
    param([string]$Name)
    if (Test-Path -LiteralPath "Env:$Name") { Remove-Item -LiteralPath "Env:$Name" }
}

# Function lookup intercepts the wrapper's command without a production test seam.
# In-process preflight checks use the same script invocation as the acceptance driver.
function python {
    if (Test-Path -LiteralPath $script:recordPath) { throw "runner invoked more than once" }
    $record = [ordered]@{ argv = @($args) }
    [System.IO.File]::WriteAllText($script:recordPath, (ConvertTo-Json -InputObject $record))
    $global:LASTEXITCODE = $script:runnerExitCode
}

function Get-CimInstance {
    param([string]$ClassName)
    switch -Exact ($ClassName) {
        "Win32_OperatingSystem" { return [pscustomobject]@{ Caption = "Fixture OS"; Version = "1" } }
        "Win32_VideoController" { return [pscustomobject]@{ Name = "Fixture GPU"; DriverVersion = "1" } }
        default { throw "unexpected host query: $ClassName" }
    }
}

function Invoke-Wrapper {
    param([hashtable]$Parameters = @{}, [int]$RunnerExitCode = 0,
        [switch]$UseDefaultExe, [switch]$AsProcess, [switch]$FailCleanup)
    $wrapperParameters = @{ Exe = $clientRelative; OutDir = $outRelative }
    foreach ($key in $Parameters.Keys) { $wrapperParameters[$key] = $Parameters[$key] }
    if ($UseDefaultExe) { $wrapperParameters.Remove("Exe") }
    if (Test-Path -LiteralPath $recordPath) { Remove-Item -LiteralPath $recordPath -Force }
    $script:runnerExitCode = $RunnerExitCode
    $global:LASTEXITCODE = $null
    # Relative paths must be resolved against the repository, even from another cwd.
    Push-Location -LiteralPath $fixtureRoot
    try {
        if ($AsProcess) {
            @{
                Wrapper = $wrapper; Parameters = $wrapperParameters
                RecordPath = $recordPath; RunnerExitCode = $RunnerExitCode
                FailCleanup = [bool]$FailCleanup
            } | Export-Clixml -LiteralPath $childConfiguration -Depth 5
            $output = @(& $powershell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
                -File $childHarness -Configuration $childConfiguration 2>&1)
        } else {
            $output = @(& $wrapper @wrapperParameters 6>&1)
        }
        $code = $global:LASTEXITCODE
    } finally { Pop-Location }
    Assert-Test ($null -ne $code) "wrapper explicitly exits"
    $invoked = Test-Path -LiteralPath $recordPath
    $recordedArgv = @()
    if ($invoked) {
        $recordedArgv = @((Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json).argv)
    }
    return [pscustomobject]@{
        Code = $code; Message = (($output | ForEach-Object { $_.ToString() }) -join "`n")
        Invoked = $invoked; Argv = $recordedArgv
    }
}

function Assert-Refused {
    param([hashtable]$Parameters, [string]$Message)
    $run = Invoke-Wrapper -Parameters $Parameters
    Assert-Test ($run.Code -eq 2) "preflight exits 2: $Message (got $($run.Code)): $($run.Message)"
    Assert-Test ($run.Message.Contains($Message)) "preflight explains refusal: $Message; got $($run.Message)"
    Assert-Test (-not $run.Invoked) "preflight never invokes the runner: $Message"
}

function Assert-RunnerArguments {
    param([object]$Run, [string]$Client, [string[]]$Tail = @(), [int]$TimeoutSeconds = 900)
    Assert-Test ($Run.Invoked) "runner was invoked: $($Run.Message)"
    Assert-Test ($Run.Argv.Count -ge 9) "runner receives required arguments"
    $hostPath = $Run.Argv[8]
    Assert-Test ([System.IO.Path]::GetDirectoryName($hostPath) -ceq $outPath) "host evidence is staged inside OutDir"
    Assert-Test ([System.IO.Path]::GetFileName($hostPath) -cmatch '^\.host-[0-9a-f-]{36}\.tmp$') "host evidence has a unique staged filename"
    Assert-Test (-not (Test-Path -LiteralPath $hostPath)) "staged host evidence is removed after runner exit"
    $expected = @(
        "tools/perf/run_qualification.py", "--client", $Client, "--out-dir", $outPath,
        "--timeout-seconds", [string]$TimeoutSeconds, "--host-record", $hostPath
    ) + $Tail
    Assert-Test ($Run.Argv.Count -eq $expected.Count) "runner argument count matches exactly"
    for ($index = 0; $index -lt $expected.Count; $index++) {
        Assert-Test ($Run.Argv[$index] -ceq $expected[$index]) "runner argv[$index] is '$($expected[$index])', got '$($Run.Argv[$index])'"
    }
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $repo $packageRelative) | Out-Null
    # These are inert files; the stub is the only runner command that can execute.
    [System.IO.File]::WriteAllText((Join-Path $repo $clientRelative), "fixture")
    [System.IO.File]::WriteAllText((Join-Path (Join-Path $repo $packageRelative) "luminumbra_client_qa_app.exe"), "fixture")
    [System.IO.File]::WriteAllText((Join-Path $repo $manifestRelative), "{}")
    # Reuse the exact stubs in a clean child process, invoking the wrapper like
    # the acceptance driver. Script exit returns to the caller in both supported
    # shells. Cleanup clears the stale native status, so only an explicit exit
    # can publish the captured runner status back to the caller.
    $harness = @'
param([string]$Configuration)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
$config = Import-Clixml -LiteralPath $Configuration
$recordPath = $config.RecordPath
$runnerExitCode = $config.RunnerExitCode
$failCleanup = $config.FailCleanup
$parameters = $config.Parameters
'@
    $harness += "`nfunction python {`n" + ${function:python}.ToString() + "`n}`n"
    $harness += "function Get-CimInstance {`n" + ${function:Get-CimInstance}.ToString() + "`n}`n"
    $harness += @'
function Remove-Item {
    param([string]$LiteralPath, [switch]$Force)
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath $LiteralPath -Force:$Force
    $global:LASTEXITCODE = $null
    if ($script:failCleanup) { throw "fixture cleanup failure" }
}
$global:LASTEXITCODE = $null
& $config.Wrapper @parameters
if ($null -eq $global:LASTEXITCODE) { throw "wrapper returned without exiting" }
exit $global:LASTEXITCODE
'@
    [System.IO.File]::WriteAllText($childHarness, $harness)
    # Newer runtimes retain an empty environment entry when passed a null string.
    # Remove the entry explicitly: presence itself is a qualification refusal.
    foreach ($name in $overrideNames) { Remove-Override $name }
    foreach ($name in $overrideNames) {
        Assert-Test (-not (Test-Path -LiteralPath "Env:$name")) "override is absent before isolated checks: $name"
    }

    # Presence alone refuses each override, even a value that looks disabled.
    # DryRun must not bypass either override refusal or argument preflight.
    foreach ($name in $overrideNames) {
        [Environment]::SetEnvironmentVariable($name, "0", "Process")
        try {
            foreach ($dryRun in @($false, $true)) {
                Assert-Refused @{ DryRun = $dryRun } "[render-qualification] refusing to run: render overrides set: $name"
            }
        } finally { Remove-Override $name }
    }
    foreach ($name in $overrideNames) { [Environment]::SetEnvironmentVariable($name, "0", "Process") }
    try {
        Assert-Refused @{} ("[render-qualification] refusing to run: render overrides set: " + ($overrideNames -join ", "))
    } finally {
        foreach ($name in $overrideNames) { Remove-Override $name }
    }

    foreach ($dryRun in @($false, $true)) {
        foreach ($timeout in @(0, -1)) {
            Assert-Refused @{ TimeoutSeconds = $timeout; DryRun = $dryRun } "[render-qualification] -TimeoutSeconds must be at least 1"
        }
        # Even explicitly supplying the default Exe conflicts with Package.
        foreach ($exe in @($clientRelative, "build/release/bin/luminumbra_client_app.exe")) {
            Assert-Refused @{ Package = $packageRelative; Exe = $exe; DryRun = $dryRun } "[render-qualification] use either -Package or -Exe"
        }
    }
    foreach ($exe in @((Join-Path $fixtureRoot "absent.exe"), $fixtureRoot)) {
        Assert-Refused @{ Exe = $exe } "[render-qualification] client exe not found at '$exe' - build it first"
    }
    # Exercise Package's missing-client branch without implicitly passing Exe.
    $missingPackage = Join-Path $fixtureRoot "absent package"
    $run = Invoke-Wrapper @{ Package = $missingPackage } -UseDefaultExe
    $missingClient = Join-Path $missingPackage "luminumbra_client_qa_app.exe"
    Assert-Test ($run.Code -eq 2) "missing package client exits 2"
    Assert-Test ($run.Message.Contains("client exe not found at '$missingClient' - build it first")) "missing package client is identified"
    Assert-Test (-not $run.Invoked) "missing package client never invokes the runner"

    $refresh = @("3440x1440=144", "3840x1600=120")
    $cells = @("cell fixture one", "cell-fixture-two")
    $tail = @("--build-manifest", (Join-Path $repo $manifestRelative))
    foreach ($entry in $refresh) { $tail += @("--declare-refresh", $entry) }
    foreach ($cell in $cells) { $tail += @("--only", $cell) }
    foreach ($exitCode in @(0, 1, 2, 3, 7)) {
        $parameters = @{
            BuildManifest = $manifestRelative; DeclareRefresh = $refresh; Only = $cells; TimeoutSeconds = 37
        }
        if ($exitCode -eq 1) {
            $parameters.Exe = Join-Path $repo $clientRelative
            $parameters.OutDir = $outPath
            $parameters.BuildManifest = Join-Path $repo $manifestRelative
        }
        $run = Invoke-Wrapper -Parameters $parameters -RunnerExitCode $exitCode -AsProcess
        Assert-Test ($run.Code -eq $exitCode) "runner exit $exitCode is propagated unchanged: $($run.Message)"
        Assert-RunnerArguments $run (Join-Path $repo $clientRelative) -Tail $tail -TimeoutSeconds 37
    }
    foreach ($exitCode in @(0, 7)) {
        $run = Invoke-Wrapper -RunnerExitCode $exitCode -AsProcess -FailCleanup
        Assert-Test ($run.Code -eq $exitCode) "cleanup failure preserves runner exit $exitCode`: $($run.Message)"
        Assert-RunnerArguments $run (Join-Path $repo $clientRelative)
    }
    foreach ($package in @($packageRelative, (Join-Path $repo $packageRelative))) {
        $run = Invoke-Wrapper @{ Package = $package } -UseDefaultExe
        Assert-Test ($run.Code -eq 0) "package client runs successfully"
        Assert-RunnerArguments $run (Join-Path (Join-Path $repo $packageRelative) "luminumbra_client_qa_app.exe")
    }
    $run = Invoke-Wrapper @{ DryRun = $true } -UseDefaultExe -AsProcess
    Assert-Test ($run.Code -eq 0) "default client supports dry run"
    Assert-RunnerArguments $run (Join-Path $repo "build/release/bin/luminumbra_client_app.exe") -Tail @("--dry-run")
    $run = Invoke-Wrapper @{ Exe = $missingClient; DryRun = $true } -AsProcess
    Assert-Test ($run.Code -eq 0) "dry run permits a missing explicit client"
    Assert-RunnerArguments $run $missingClient -Tail @("--dry-run")
    Write-Host "RenderQualificationWrapperContract: $checks assertions passed (stub runner; no native client)."
} finally {
    foreach ($name in $overrideNames) {
        if ($null -eq $savedOverrides[$name]) {
            Remove-Override $name
        } else {
            [Environment]::SetEnvironmentVariable($name, $savedOverrides[$name], "Process")
        }
    }
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force }
}
