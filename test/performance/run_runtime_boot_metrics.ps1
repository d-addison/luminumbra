param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [int]$Frames = 60,
    [int]$TimeoutSeconds = 90,
    [int]$FinalWindowFrames = 10
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "Executable not found: $ExePath"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$artifactRoot = Split-Path -Parent $OutputDir
if (-not $artifactRoot) {
    $artifactRoot = $OutputDir
}

$jsonPath = Join-Path $OutputDir "runtime_boot.json"
$csvPath = Join-Path $OutputDir "runtime_boot.csv"
$summaryPath = Join-Path $OutputDir "runtime_boot_summary.json"
$budgetPath = Join-Path $OutputDir "performance_overhaul_budget.json"
$renderPerfDir = Join-Path $artifactRoot "render_perf"
New-Item -ItemType Directory -Force -Path $renderPerfDir | Out-Null
$uploadBacklogPath = Join-Path $renderPerfDir "upload_backlog.json"

foreach ($staleArtifact in @($jsonPath, $csvPath, $summaryPath, $budgetPath, $uploadBacklogPath)) {
    if (Test-Path -LiteralPath $staleArtifact) {
        Remove-Item -LiteralPath $staleArtifact -Force
    }
}

$stamp = Get-Date -Format "yyyyMMddHHmmss"
$stdout = Join-Path $artifactRoot "runtime_boot_ctest_$stamp.out.log"
$stderr = Join-Path $artifactRoot "runtime_boot_ctest_$stamp.err.log"
$arguments = @(
    "--runtime-boot-metrics",
    "--runtime-boot-frames",
    "$Frames",
    "--runtime-boot-output",
    $OutputDir
)

$process = Start-Process `
    -FilePath $ExePath `
    -ArgumentList $arguments `
    -WorkingDirectory (Split-Path -Parent $ExePath) `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr `
    -PassThru

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $process.Refresh()
}

if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
    throw "Runtime boot metrics harness timed out after $TimeoutSeconds seconds. stdout=$stdout stderr=$stderr"
}

$process.Refresh()
if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) {
    throw "Runtime boot metrics harness exited with code $($process.ExitCode). stdout=$stdout stderr=$stderr"
}
$process.Dispose()

if (-not (Test-Path -LiteralPath $jsonPath)) {
    throw "Missing runtime boot JSON artifact: $jsonPath"
}
if (-not (Test-Path -LiteralPath $csvPath)) {
    throw "Missing runtime boot CSV artifact: $csvPath"
}

$metrics = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
if ([int]$metrics.frames_recorded -ne $Frames) {
    throw "Expected $Frames recorded frames, got $($metrics.frames_recorded)"
}

if ((Test-Path -LiteralPath $stderr) -and (Get-Item -LiteralPath $stderr).Length -gt 0) {
    throw "Runtime boot harness wrote stderr. stdout=$stdout stderr=$stderr"
}

$rows = @(Import-Csv -LiteralPath $csvPath)
if ($rows.Count -ne $Frames) {
    throw "Expected $Frames CSV rows, got $($rows.Count)"
}

function Convert-Number {
    param([object]$Value)
    if ($null -eq $Value -or "$Value" -eq "") {
        return 0.0
    }
    return [double]$Value
}

function Sum-Column {
    param(
        [object[]]$Rows,
        [string]$Name
    )
    $sum = 0.0
    foreach ($row in $Rows) {
        $sum += Convert-Number $row.$Name
    }
    return $sum
}

function Max-Column {
    param(
        [object[]]$Rows,
        [string]$Name
    )
    $max = 0.0
    foreach ($row in $Rows) {
        $value = Convert-Number $row.$Name
        if ($value -gt $max) {
            $max = $value
        }
    }
    return $max
}

$finalRows = @($rows | Select-Object -Last $FinalWindowFrames)

$observed = [ordered]@{
    frames_recorded = [int]$metrics.frames_recorded
    p50_ms = [double]$metrics.frame_time_ms.p50
    p95_ms = [double]$metrics.frame_time_ms.p95
    p99_ms = [double]$metrics.frame_time_ms.p99
    max_snapshots = [int](Max-Column $rows "snapshots")
    max_terrain_visible_chunks = [int](Max-Column $rows "terrain_visible_chunks")
    max_terrain_draws = [int](Max-Column $rows "terrain_draws")
    max_shadow_draws = [int](Max-Column $rows "shadow_draws")
    total_culling_hierarchy_rebuilds = [int](Sum-Column $rows "culling_hierarchy_rebuilds")
    total_terrain_uploads = [int](Sum-Column $rows "terrain_uploads")
    total_terrain_uploads_deferred = [int](Sum-Column $rows "terrain_uploads_deferred")
    total_terrain_payload_bytes = [int64](Sum-Column $rows "terrain_payload_bytes")
    total_terrain_slots_created = [int](Sum-Column $rows "terrain_slots_created")
    total_terrain_slots_reused = [int](Sum-Column $rows "terrain_slots_reused")
    total_terrain_slots_grown = [int](Sum-Column $rows "terrain_slots_grown")
    total_terrain_upload_failures = [int](Sum-Column $rows "terrain_upload_failures")
    total_water_uploads = [int](Sum-Column $rows "water_uploads")
    total_water_uploads_deferred = [int](Sum-Column $rows "water_uploads_deferred")
    total_water_payload_bytes = [int64](Sum-Column $rows "water_payload_bytes")
    total_water_slots_created = [int](Sum-Column $rows "water_slots_created")
    total_water_slots_reused = [int](Sum-Column $rows "water_slots_reused")
    total_water_slots_grown = [int](Sum-Column $rows "water_slots_grown")
    total_water_upload_failures = [int](Sum-Column $rows "water_upload_failures")
    final_window_frames = $finalRows.Count
    final_window_terrain_uploads_deferred = [int](Sum-Column $finalRows "terrain_uploads_deferred")
    final_window_water_uploads_deferred = [int](Sum-Column $finalRows "water_uploads_deferred")
}

$budgets = [ordered]@{
    frames_required = $Frames
    max_p95_ms = 50.0
    max_p99_ms = 50.0
    min_final_frame_terrain_visible_chunks = 1
    max_final_window_terrain_uploads_deferred = [Math]::Max(1, $FinalWindowFrames * 2)
    max_final_window_water_uploads_deferred = [Math]::Max(1, $FinalWindowFrames)
    max_terrain_upload_failures = 0
    max_water_upload_failures = 0
    max_stderr_bytes = 0
}

$passed = $true
$failures = New-Object System.Collections.Generic.List[string]
if ($observed.p95_ms -gt $budgets.max_p95_ms) {
    $passed = $false
    $failures.Add("p95_ms $($observed.p95_ms) exceeds $($budgets.max_p95_ms)")
}
if ($observed.p99_ms -gt $budgets.max_p99_ms) {
    $passed = $false
    $failures.Add("p99_ms $($observed.p99_ms) exceeds $($budgets.max_p99_ms)")
}
if ([int]$metrics.last_frame.terrain_visible_chunks -lt $budgets.min_final_frame_terrain_visible_chunks) {
    $passed = $false
    $failures.Add("final terrain_visible_chunks $($metrics.last_frame.terrain_visible_chunks) below $($budgets.min_final_frame_terrain_visible_chunks)")
}
if ($observed.final_window_terrain_uploads_deferred -gt $budgets.max_final_window_terrain_uploads_deferred) {
    $passed = $false
    $failures.Add("final-window terrain deferrals $($observed.final_window_terrain_uploads_deferred) exceed $($budgets.max_final_window_terrain_uploads_deferred)")
}
if ($observed.final_window_water_uploads_deferred -gt $budgets.max_final_window_water_uploads_deferred) {
    $passed = $false
    $failures.Add("final-window water deferrals $($observed.final_window_water_uploads_deferred) exceed $($budgets.max_final_window_water_uploads_deferred)")
}
if ($observed.total_terrain_upload_failures -gt $budgets.max_terrain_upload_failures) {
    $passed = $false
    $failures.Add("terrain upload failures $($observed.total_terrain_upload_failures) exceed $($budgets.max_terrain_upload_failures)")
}
if ($observed.total_water_upload_failures -gt $budgets.max_water_upload_failures) {
    $passed = $false
    $failures.Add("water upload failures $($observed.total_water_upload_failures) exceed $($budgets.max_water_upload_failures)")
}

$terrainAccountedUploads = $observed.total_terrain_slots_created + $observed.total_terrain_slots_reused + $observed.total_terrain_slots_grown
if ($terrainAccountedUploads -lt $observed.total_terrain_uploads) {
    $passed = $false
    $failures.Add("terrain upload accounting $terrainAccountedUploads below uploads $($observed.total_terrain_uploads)")
}

$waterAccountedUploads = $observed.total_water_slots_created + $observed.total_water_slots_reused + $observed.total_water_slots_grown
if ($waterAccountedUploads -lt $observed.total_water_uploads) {
    $passed = $false
    $failures.Add("water upload accounting $waterAccountedUploads below uploads $($observed.total_water_uploads)")
}

$summary = [ordered]@{
    schema = "luminumbra.runtime_boot_summary.v1"
    stdout = $stdout
    stderr = $stderr
    artifacts = [ordered]@{
        runtime_boot_json = $jsonPath
        runtime_boot_csv = $csvPath
        upload_backlog_json = $uploadBacklogPath
    }
    observed = $observed
    budgets = $budgets
    passed = $passed
    failures = @($failures)
}

$budgetReport = [ordered]@{
    schema = "luminumbra.runtime_boot_safety_budget.v1"
    policy = "debug smoke safety limits; relative comparisons own regression decisions"
    observed = $observed
    budgets = $budgets
    passed = $passed
    failures = @($failures)
}

$uploadBacklogReport = [ordered]@{
    schema = "luminumbra.upload_backlog.v1"
    source = $csvPath
    frames_recorded = $Frames
    final_window_frames = $finalRows.Count
    terrain = [ordered]@{
        uploads = $observed.total_terrain_uploads
        uploads_deferred = $observed.total_terrain_uploads_deferred
        final_window_uploads_deferred = $observed.final_window_terrain_uploads_deferred
        payload_bytes = $observed.total_terrain_payload_bytes
        slots_created = $observed.total_terrain_slots_created
        slots_reused = $observed.total_terrain_slots_reused
        slots_grown = $observed.total_terrain_slots_grown
        upload_failures = $observed.total_terrain_upload_failures
        accounted_uploads = $terrainAccountedUploads
    }
    water = [ordered]@{
        uploads = $observed.total_water_uploads
        uploads_deferred = $observed.total_water_uploads_deferred
        final_window_uploads_deferred = $observed.final_window_water_uploads_deferred
        payload_bytes = $observed.total_water_payload_bytes
        slots_created = $observed.total_water_slots_created
        slots_reused = $observed.total_water_slots_reused
        slots_grown = $observed.total_water_slots_grown
        upload_failures = $observed.total_water_upload_failures
        accounted_uploads = $waterAccountedUploads
    }
    passed = $passed
    failures = @($failures)
}

$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath
$budgetReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $budgetPath
$uploadBacklogReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $uploadBacklogPath

if (-not $passed) {
    throw "Runtime boot budget check failed: $($failures -join '; '). summary=$summaryPath budget=$budgetPath stdout=$stdout stderr=$stderr"
}

Write-Host "Runtime boot metrics OK: $Frames frames -> $jsonPath"
Write-Host "Runtime boot summary: $summaryPath"
Write-Host "Performance budget report: $budgetPath"
Write-Host "Upload backlog report: $uploadBacklogPath"
exit 0
