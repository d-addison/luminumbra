<#
.SYNOPSIS
  Native Windows render qualification wrapper. Runs tools/perf/run_qualification.py against the
  release client and writes <OutDir>\qualification.json.

.DESCRIPTION
  NEEDS-NATIVE-RUN. This requires the RTX 5070 Ti, with displays at 3840x1600 and 3440x1440
  attached. It cannot be judged on any other machine.

  The wrapper refuses to run if any render-affecting override is set in the current process
  (LUMIN_GL_DEBUG, LUMIN_RENDER_SCALE, LUMIN_TREE_IMPOSTORS, LUMIN_ATMOS, LUMIN_MOON,
  LUMIN_CLOUD_QUALITY, LUMIN_SSAO_QUALITY, LUMIN_MOON_WRAP_FLOOR, LUMIN_GRADE, LUMIN_CAVE_AO,
  LUMIN_SCENT_DECAL, LUMIN_RHI, LUMIN_FRAME_SCAN_SETTLE, LUMINUMBRA_VISUAL_SWEEP,
  LUMINUMBRA_VISUAL_SWEEP_WINTER, LUMINUMBRA_ATMOS_MOTION_CAPTURE, LUMINUMBRA_JOB_THROTTLE).
  Exit code 2 names them. The wrapper exports nothing that changes render state: it only
  reads the environment and passes its own arguments to the Python runner.

  Before the run it writes <OutDir>\host.json with the OS version, the GPU names and driver
  versions (Win32_VideoController), and the PowerShell version. The run then executes
  "python tools/perf/run_qualification.py --client <Exe> --out-dir <OutDir>
  --timeout-seconds <N>", plus --declare-refresh and --only for each entry given, and
  --dry-run when -DryRun is set. The Python exit code is propagated.

  The qualification is judged only by qualification.json (render_floor_qualified). The
  process exit code and the host record are not a qualification verdict.

  Relative -Exe and -OutDir paths are resolved against the repository root.

.PARAMETER Exe
  Release client executable. Default: build/release/bin/luminumbra_client_app.exe

.PARAMETER OutDir
  Directory receiving cells/, work/, host.json and qualification.json. Required.

.PARAMETER DeclareRefresh
  Operator refresh rates for displays the matrix leaves null, as 'display-id=hz'. Repeatable.

.PARAMETER Only
  Cell ids to run (partial run). Repeatable.

.PARAMETER TimeoutSeconds
  Per-client process timeout in seconds. Default: 900.

.PARAMETER DryRun
  Write the qualification plan only; launch nothing.

.EXAMPLE
  pwsh tools/gates/run-render-qualification.ps1 -OutDir build/qualification-native `
    -DeclareRefresh '3840x1600=165' -DeclareRefresh '3440x1440=144'
#>
[CmdletBinding()]
param(
    [string]$Exe = "build/release/bin/luminumbra_client_app.exe",
    [Parameter(Mandatory = $true)]
    [string]$OutDir,
    [string[]]$DeclareRefresh = @(),
    [string[]]$Only = @(),
    [int]$TimeoutSeconds = 900,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tools/gates -> repo root
Set-Location $repo

$overrides = @(
    'LUMIN_GL_DEBUG', 'LUMIN_RENDER_SCALE', 'LUMIN_TREE_IMPOSTORS', 'LUMIN_ATMOS', 'LUMIN_MOON',
    'LUMIN_CLOUD_QUALITY', 'LUMIN_SSAO_QUALITY', 'LUMIN_MOON_WRAP_FLOOR', 'LUMIN_GRADE',
    'LUMIN_CAVE_AO', 'LUMIN_SCENT_DECAL', 'LUMIN_RHI', 'LUMIN_FRAME_SCAN_SETTLE',
    'LUMINUMBRA_VISUAL_SWEEP', 'LUMINUMBRA_VISUAL_SWEEP_WINTER',
    'LUMINUMBRA_ATMOS_MOTION_CAPTURE', 'LUMINUMBRA_JOB_THROTTLE'
)
$set = @($overrides | Where-Object { [Environment]::GetEnvironmentVariable($_) -ne $null })
if ($set.Count -gt 0) {
    Write-Host "[render-qualification] refusing to run: render overrides set: $($set -join ', ')"
    exit 2
}

if ($TimeoutSeconds -lt 1) {
    Write-Host "[render-qualification] -TimeoutSeconds must be at least 1"
    exit 2
}

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $repo $Path))
}

$exePath = Resolve-RepoPath $Exe
$outPath = Resolve-RepoPath $OutDir
if (-not $DryRun -and -not (Test-Path $exePath)) {
    Write-Host "[render-qualification] client exe not found at '$exePath' - build it first"
    exit 2
}

New-Item -ItemType Directory -Force -Path $outPath | Out-Null

# Host record first, so a failed run still says what machine produced it.
$os = Get-CimInstance Win32_OperatingSystem
$gpus = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion)
$host_record = [ordered]@{
    os_caption      = $os.Caption
    os_version      = $os.Version
    gpus            = $gpus
    powershell      = $PSVersionTable.PSVersion.ToString()
}
$hostPath = Join-Path $outPath "host.json"
$host_record | ConvertTo-Json -Depth 5 | Set-Content -Path $hostPath -Encoding UTF8
Write-Host "[render-qualification] host record -> $hostPath"

$argv = @(
    "tools/perf/run_qualification.py",
    "--client", $exePath,
    "--out-dir", $outPath,
    "--timeout-seconds", [string]$TimeoutSeconds
)
foreach ($entry in $DeclareRefresh) { $argv += @("--declare-refresh", $entry) }
foreach ($cell in $Only) { $argv += @("--only", $cell) }
if ($DryRun) { $argv += "--dry-run" }

Write-Host "[render-qualification] python $($argv -join ' ')"
& python @argv
$code = $LASTEXITCODE

$qualification = Join-Path $outPath "qualification.json"
Write-Host "[render-qualification] qualification.json: $qualification"
exit $code
