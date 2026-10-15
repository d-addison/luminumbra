<#
.SYNOPSIS
    NEEDS-NATIVE-RUN: Pester-free Windows Job Object ownership regressions.
.DESCRIPTION
    Run with -NoProfile under Windows PowerShell 5.1 and PowerShell 7 on Windows
    8+ with Python installed. Launches only synthetic Python process trees;
    never launches an engine client, ctest, a build, or a network operation.
    Owns a temporary folder and retains a handle to one unrelated sentinel.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) {
    throw "NEEDS-NATIVE-RUN: Windows Job Object regressions require Windows"
}
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path (Join-Path $scriptDir "lib") "AcceptanceCommon.ps1")
$python = Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
$prefix = @()
if ($null -eq $python) {
    $python = Get-Command py -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $prefix = @("-3")
}
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("acceptance-ownership-" + [guid]::NewGuid().ToString("N"))
$checks = 0
$sentinel = $null

function Assert-Test {
    param([bool]$Condition, [string]$Because)
    if (-not $Condition) { throw "FAILED: $Because" }
    $script:checks++
}

# Discovery must be irrelevant, even if CIM is unavailable or returns stale PIDs.
function Get-CimInstance { throw "CIM must never be used for process ownership" }

try {
    New-Item -ItemType Directory -Path $tempRoot | Out-Null
    $fixture = Join-Path $tempRoot "process fixture.py"
    $fixtureText = @'
import json
import subprocess
import sys
import time
from pathlib import Path

role, directory = sys.argv[1:3]
root = Path(directory)
root.mkdir(parents=True, exist_ok=True)
(root / (role + ".started")).write_text("started")

def spawn(child):
    subprocess.Popen([sys.executable, __file__, child, directory])

def pulse():
    # Failed assertions must not leave synthetic workers running indefinitely.
    deadline = time.monotonic() + (120 if role == "sentinel" else 30)
    while time.monotonic() < deadline:
        with (root / (role + ".pulse")).open("a") as handle:
            handle.write("alive\n")
        time.sleep(0.05)

if role == "exit":
    print(json.dumps(sys.argv[3:]))
    print("stderr marker", file=sys.stderr)
    sys.exit(7)
if role == "exited-intermediate":
    spawn("intermediate")
elif role == "intermediate":
    spawn("worker")
    (root / "intermediate.exiting").write_text("exiting")
    sys.exit(0)
elif role == "late-child":
    time.sleep(1.5)
    # Race spawning against timeout; some workers may never execute Python.
    for index in range(12):
        spawn("worker" + str(index))
        time.sleep(0.2)
elif role == "parent-exit":
    spawn("worker")
    sys.exit(0)
elif role == "late-parent-exit":
    time.sleep(1.5)
    spawn("worker")
    sys.exit(0)
elif role == "immediate-child":
    spawn("worker")
pulse()
'@
    [System.IO.File]::WriteAllText($fixture, $fixtureText, [System.Text.Encoding]::ASCII)

    # Hold the original process handle for cleanup; never reopen by PID.
    $sentinelDir = Join-Path $tempRoot "unrelated"
    $sentinel = Start-Process -FilePath $python.Source `
        -ArgumentList (Join-ProcessArguments -Values (@($prefix) + @($fixture, "sentinel", $sentinelDir))) `
        -WorkingDirectory $tempRoot -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $tempRoot "sentinel.stdout.log") `
        -RedirectStandardError (Join-Path $tempRoot "sentinel.stderr.log")
    $null = $sentinel.Handle
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath (Join-Path $sentinelDir "sentinel.pulse"))) {
        if ($watch.ElapsedMilliseconds -ge 10000) { throw "sentinel failed to start" }
        Start-Sleep -Milliseconds 25
    }

    # Check exit-code preservation, working paths with spaces, and MSVCRT quoting.
    $exitDir = Join-Path $tempRoot "normal exit"
    $exitOut = Join-Path $tempRoot "exit.stdout.log"
    $exitErr = Join-Path $tempRoot "exit.stderr.log"
    $quoted = @("space word", 'quote"word', 'space trailing\')
    $run = Invoke-ClientSession -Exe $python.Source `
        -ArgumentList (@($prefix) + @($fixture, "exit", $exitDir) + $quoted) `
        -WorkingDirectory $tempRoot -StdoutPath $exitOut -StderrPath $exitErr -TimeoutSeconds 10
    Assert-Test ($run.exit_code -eq 7 -and -not $run.timed_out) "normal exit preserves status"
    $actual = ConvertFrom-Json -InputObject (Read-TextOrEmpty -Path $exitOut)
    Assert-Test ($actual.Count -eq $quoted.Count) "all quoted arguments arrive"
    for ($index = 0; $index -lt $quoted.Count; $index++) {
        Assert-Test ($actual[$index] -ceq $quoted[$index]) "argument $index survives quoting"
    }
    Assert-Test ((Read-TextOrEmpty -Path $exitErr).Contains("stderr marker")) "stderr is redirected"

    foreach ($scenario in @("immediate-child", "exited-intermediate", "late-child", "parent-exit", "late-parent-exit")) {
        $scenarioDir = Join-Path $tempRoot $scenario
        $watch.Restart()
        $run = Invoke-ClientSession -Exe $python.Source `
            -ArgumentList (@($prefix) + @($fixture, $scenario, $scenarioDir)) `
            -WorkingDirectory $tempRoot `
            -StdoutPath (Join-Path $tempRoot ($scenario + ".stdout.log")) `
            -StderrPath (Join-Path $tempRoot ($scenario + ".stderr.log")) -TimeoutSeconds 3
        Assert-Test ($run.timed_out -and $run.exit_code -eq -1) "$scenario times out, including after root exit"
        Assert-Test ($watch.ElapsedMilliseconds -lt 15000) "$scenario cleanup is bounded"
        $workers = @(Get-ChildItem -LiteralPath $scenarioDir -Filter "worker*.started")
        Assert-Test ($workers.Count -gt 0) "$scenario actually spawned a descendant"
        if ($scenario -eq "exited-intermediate") {
            Assert-Test (Test-Path -LiteralPath (Join-Path $scenarioDir "intermediate.exiting")) "intermediate exited before cleanup"
        }
        $pulses = @(Get-ChildItem -LiteralPath $scenarioDir -Filter "*.pulse")
        $lengths = @{}
        foreach ($pulse in $pulses) { $lengths[$pulse.Name] = $pulse.Length }
        Start-Sleep -Milliseconds 400
        $after = @(Get-ChildItem -LiteralPath $scenarioDir -Filter "*.pulse")
        Assert-Test ($after.Count -eq $pulses.Count) "$scenario has no newly active descendants after cleanup"
        foreach ($pulse in $after) {
            Assert-Test ($lengths[$pulse.Name] -eq $pulse.Length) "$scenario stopped $($pulse.Name)"
        }
        Assert-Test (-not $sentinel.HasExited) "$scenario leaves unrelated process alive"
        $sentinelPulse = Join-Path $sentinelDir "sentinel.pulse"
        $before = (Get-Item -LiteralPath $sentinelPulse).Length
        Start-Sleep -Milliseconds 200
        Assert-Test ((Get-Item -LiteralPath $sentinelPulse).Length -gt $before) "$scenario unrelated process still progresses"
    }
    Write-Host "PASS: $checks acceptance process ownership checks"
} finally {
    if ($null -ne $sentinel) {
        try {
            if (-not $sentinel.HasExited) { $sentinel.Kill() }
            if (-not $sentinel.WaitForExit(10000)) { throw "sentinel cleanup did not finish" }
        } finally {
            $sentinel.Dispose()
        }
    }
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}
