# oracle_vs_replay_count.ps1 -- test-audit (/).
#
# Classifies every TEST in the adversarial *_hardening_test.cpp suite as either an
# ORACLE / value-asserting test (independent recompute, golden literal, exact threshold,
# unit-magnitude, or other absolute-value check) or a DETERMINISM (run==replay) test (a
# self-comparison that re-runs/re-builds the same inputs and asserts two runs are equal).
#
# WHY (critique rev 11, MEDIUM 16): a run==replay self-comparison cannot fail unless the
# code is non-deterministic -- it is a REPRODUCIBILITY guard, NOT a correctness proof. The
# headline "N hardening tests" conflates strong oracles with weak self-compares. This
# emits the two numbers separately so a handoff can quote them honestly.
#
# Heuristic (per TEST body, brace-balanced):
#   replay  <- name matches RunEqualsReplay|SameOrder|ByteExactReplay|Reproducible|RunEqualsRun
#              OR the body's only equality is a run==run/a==b self-compare with no golden.
#   oracle  <- everything else that asserts a VALUE: name contains Oracle/Golden, OR the body
#              has an EXPECT_NEAR / EXPECT_FLOAT_EQ / EXPECT_EQ against a literal/recompute,
#              an exact threshold, a unit-length/magnitude check, or a NaN/bounds assertion.
# A test counted as replay is NOT also counted as oracle (replay takes precedence by name).
#
# This is a coarse static tally for REPORTING, not a gate. Usage:
#   pwsh -File tools/oracle_vs_replay_count.ps1 [-OutFile <path>]

param(
    [string]$OutFile = "build/gate-artifacts/test-audit/oracle-vs-replay-report.md"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

$files = @(
    "test/ai/flocking_evolution_hardening_test.cpp",
    "test/ai/perception_scent_hardening_test.cpp",
    "test/ai/utility_instinct_hardening_test.cpp",
    "test/common/fields_hardening_test.cpp",
    "test/common/lockstep_replay_hardening_test.cpp",
    "test/common/replication_hardening_test.cpp",
    "test/common/world_streaming_hardening_test.cpp",
    "test/persistence/persistence_hardening_test.cpp",
    "test/shield/meshing_hardening_test.cpp",
    "test/shield/worldgen_hardening_test.cpp",
    "test/shield/worldgen_layers_hardening_test.cpp",
    "test/sim/environment_hardening_test.cpp",
    "test/sim/gamelibs_hardening_test.cpp",
    "test/sim/lifecycle_hardening_test.cpp",
    "test/sim/movement_hardening_test.cpp",
    "test/sim/social_hardening_test.cpp"
)

# Split a file's text into (testName, body) pairs by brace-balancing from each TEST(...) {.
function Get-Tests([string]$text) {
    $tests = @()
    $rx = [regex]'TEST(?:_F)?\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*\)'
    foreach ($m in $rx.Matches($text)) {
        $suite = $m.Groups[1].Value
        $name  = $m.Groups[2].Value
        # find the opening brace after the match
        $i = $text.IndexOf('{', $m.Index + $m.Length)
        if ($i -lt 0) { continue }
        $depth = 0; $j = $i
        while ($j -lt $text.Length) {
            if ($text[$j] -eq '{') { $depth++ }
            elseif ($text[$j] -eq '}') { $depth--; if ($depth -eq 0) { break } }
            $j++
        }
        $end = [Math]::Min($j, $text.Length - 1)
        $len = [Math]::Max(0, $end - $i + 1)
        $body = $text.Substring($i, $len)
        $tests += [pscustomobject]@{ Suite = $suite; Name = $name; Body = $body }
    }
    return $tests
}

$replayNameRx = '(?i)(RunEqualsReplay|SameOrder|ByteExactReplay|RunEqualsRun|ReplaySameOrder)'
$oracleNameRx = '(?i)(Oracle|Golden)'

$rows = @()
$totalOracle = 0
$totalReplay = 0

foreach ($rel in $files) {
    $path = Join-Path $repoRoot $rel
    if (-not (Test-Path $path)) { continue }
    $text = Get-Content -Raw -Path $path
    $tests = Get-Tests $text
    $oracle = 0
    $replay = 0
    foreach ($t in $tests) {
        if ($t.Name -match $replayNameRx) {
            $replay++
            continue
        }
        if ($t.Name -match $oracleNameRx) {
            $oracle++
            continue
        }
        # body-level: a self-compare (run==run / a==b with NO golden literal) is replay;
        # anything asserting an absolute value (NEAR/FLOAT_EQ to a literal, threshold, NaN,
        # bounds, magnitude) is an oracle/value-asserting test.
        $hasValueAssert = ($t.Body -match 'EXPECT_(NEAR|FLOAT_EQ|DOUBLE_EQ|GT|GE|LT|LE)') -or
                          ($t.Body -match 'isnan|isinf|isfinite|EXPECT_(TRUE|FALSE)')
        $looksSelfCompare = ($t.Body -match 'run\(\w*\)\s*==\s*run\(' ) -or
                            ($t.Body -match 'EXPECT_(EQ|TRUE)\(\s*run')
        if ($looksSelfCompare -and -not $hasValueAssert) {
            $replay++
        } elseif ($hasValueAssert) {
            $oracle++
        } else {
            # an EXPECT_EQ with no obvious literal/recompute and no self-compare marker:
            # treat as value-asserting (it pins an exact expected value).
            $oracle++
        }
    }
    $totalOracle += $oracle
    $totalReplay += $replay
    $rows += [pscustomobject]@{ File = $rel; Oracle = $oracle; Replay = $replay; Total = ($oracle + $replay) }
}

$grandTotal = $totalOracle + $totalReplay

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("# Oracle vs replay test tally -- adversarial hardening suite")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Generated by ``tools/oracle_vs_replay_count.ps1`` (test-audit, ).")
[void]$sb.AppendLine("Coarse STATIC classification for reporting -- not a gate. See the standing note below.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("| File | Oracle / value-asserting | Determinism (run==replay) | Total |")
[void]$sb.AppendLine("|---|--:|--:|--:|")
foreach ($r in $rows) {
    [void]$sb.AppendLine("| $($r.File) | $($r.Oracle) | $($r.Replay) | $($r.Total) |")
}
[void]$sb.AppendLine("| **TOTAL** | **$totalOracle** | **$totalReplay** | **$grandTotal** |")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Two headline numbers")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- **Oracle / value-asserting tests: $totalOracle**")
[void]$sb.AppendLine("- **Determinism (run==replay) tests: $totalReplay**")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Standing note ()")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("> **run==replay tests are reproducibility guards, NOT correctness proofs.** A")
[void]$sb.AppendLine("> run==replay self-comparison re-runs the SAME inputs and asserts the two runs are")
[void]$sb.AppendLine("> equal; it can only fail if the code is non-deterministic, and gives ZERO signal on")
[void]$sb.AppendLine("> whether the computed values are CORRECT. A same-order replay also cannot catch")
[void]$sb.AppendLine("> order-dependence (the boids bug a8b689c). Correctness lives in the ORACLE column:")
[void]$sb.AppendLine("> independent recompute, golden literals, exact thresholds, unit-magnitude and")
[void]$sb.AppendLine("> NaN/bounds checks, and SEEDED-SHUFFLE order-independence variants.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("_The heuristic is coarse (static name + body grep); treat the split as indicative,")
[void]$sb.AppendLine("not exact. The point is the RATIO and the standing note, not a precise count._")

$outPath = Join-Path $repoRoot $OutFile
Set-Content -Path $outPath -Value $sb.ToString() -Encoding utf8

Write-Host "oracle/value-asserting: $totalOracle   determinism(run==replay): $totalReplay   total: $grandTotal"
Write-Host "wrote $OutFile"
