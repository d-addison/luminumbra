#!/usr/bin/env python3
"""File-only acceptance preflight regressions; never launch a native client."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


GATES = Path(__file__).resolve().parent
SHELL = shutil.which("pwsh") or shutil.which("powershell")


@unittest.skipUnless(SHELL, "PowerShell 5.1 or 7 is required for file-only gate tests")
class AcceptancePreflightTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def run_script(self, script, *args):
        return subprocess.run(
            [SHELL, "-NoProfile", "-NonInteractive", "-File", str(script), *map(str, args)],
            capture_output=True, text=True, timeout=30,
        )

    def run_orchestrator(self, out, *args):
        # No executable exists: stale output must be refused before client
        # lookup, and a native session cannot run even if preflight regresses.
        return self.run_script(
            GATES / "run-v03-acceptance.ps1", "-Package", self.root / "absent-package",
            "-FixtureDir", self.root / "absent-fixture", "-OutDir", out, *args,
        )

    def test_nonempty_output_is_refused_without_changing_evidence(self):
        for entry in ("serial", "v03-acceptance-receipt.json", ".hidden-evidence"):
            with self.subTest(entry=entry):
                out = self.root / entry.replace(".", "_")
                out.mkdir()
                existing = out / entry
                if entry == "serial":
                    existing.mkdir()
                else:
                    existing.write_bytes(b"previous evidence\n")
                result = self.run_orchestrator(out)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("OutDir must be empty or absent", result.stdout + result.stderr)
                self.assertEqual(list(out.iterdir()), [existing])
                if existing.is_file():
                    self.assertEqual(existing.read_bytes(), b"previous evidence\n")
                else:
                    self.assertEqual(list(existing.iterdir()), [])

    def test_file_output_is_refused_without_overwriting_it(self):
        out = self.root / "output-file"
        out.write_bytes(b"previous evidence\n")
        result = self.run_orchestrator(out)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("OutDir must be empty or absent", result.stdout + result.stderr)
        self.assertEqual(out.read_bytes(), b"previous evidence\n")

    def test_dry_run_preserves_existing_output(self):
        out = self.root / "output"
        out.mkdir()
        evidence = out / "evidence.json"
        evidence.write_bytes(b"previous evidence\n")
        result = self.run_orchestrator(out, "-DryRun")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(list(out.iterdir()), [evidence])
        self.assertEqual(evidence.read_bytes(), b"previous evidence\n")

    def primary_world(self, before, after):
        saves = self.root / "saves"
        saves.mkdir()
        for name in after:
            (saves / name).mkdir()
        before_file = self.root / "before.json"
        before_file.write_text(json.dumps(before), encoding="utf-8")
        harness = self.root / "primary-world-test.ps1"
        harness.write_text(r'''
param([string]$Driver, [string]$Saves, [string]$BeforeFile)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
# Import only the function under test, leaving the dispatcher unexecuted.
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Driver, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw "driver parse errors: $errors" }
$definitions = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -ceq "Get-PrimaryWorld"
}, $true))
if ($definitions.Count -ne 1) { throw "expected one primary-world function" }
. ([scriptblock]::Create($definitions[0].Extent.Text))
$savesRoot = $Saves
$before = @(ConvertFrom-Json -InputObject ([System.IO.File]::ReadAllText($BeforeFile)))
try {
    Get-PrimaryWorld -BeforeNames $before
} catch {
    Write-Host $_.Exception.Message
    exit 1
}
''', encoding="utf-8")
        return self.run_script(harness, "-Driver", GATES / "run-saved-world-lifecycle.ps1",
                               "-Saves", saves, "-BeforeFile", before_file)

    def test_primary_world_is_the_only_new_save(self):
        result = self.primary_world(["fixture", "existing"], ["fixture", "created", "existing"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "created")

    def test_existing_saves_cannot_substitute_for_a_created_world(self):
        result = self.primary_world(["fixture", "existing"], ["fixture", "existing"])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("requires exactly one new world", result.stdout)
        self.assertIn("found 0: []", result.stdout)

    def test_multiple_new_saves_are_ambiguous(self):
        result = self.primary_world(["fixture"], ["fixture", "created-b", "created-a"])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("found 2: [created-a, created-b]", result.stdout)

    def test_empty_save_directory_before_session_is_supported(self):
        result = self.primary_world([], ["created"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "created")


if __name__ == "__main__":
    unittest.main()
