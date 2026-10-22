#!/usr/bin/env python3
"""GCC cohort package closure with full-script isolated command stubs."""

import argparse
import importlib.util
import json
from pathlib import Path
import sys
import unittest

fixture_path = Path(__file__).with_name("test_ucrt_attribution.py")
spec = importlib.util.spec_from_file_location("ucrt_attribution_fixture", fixture_path)
if spec is None or spec.loader is None:
    raise RuntimeError(f"cannot load {fixture_path}")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
WORKFLOW = Path(__file__).resolve().parents[2] / ".github/workflows/ucrt-attribution.yml"


class GccCohortTests(unittest.TestCase):
    run_script = fixture.AttributionTests.run_script
    assert_finalized = fixture.AttributionTests.assert_finalized

    def test_dedicated_setup_provisions_libatomic_before_attribution(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        setup = workflow.index("      - name: Set up MSYS2 UCRT64")
        revisions = workflow.index("      - name: Record revisions and runner image")
        run = workflow.index("      - name: Run package cohort cells")
        self.assertIn("            mingw-w64-ucrt-x86_64-libatomic\n", workflow[setup:revisions])
        self.assertLess(revisions, run)

    def cohort_baseline(self, with_atomic=True):
        packages = {"crt": "1.0-1", "gcc": "16.2.0-5", "cc-libs": "16.2.0-5",
                    "libgcc": "16.2.0-5", "libstdc++": "16.2.0-5"}
        if with_atomic:
            # A synthetic independent version proves restoration uses the
            # recorded atomic version, rather than inferring it from GCC.
            packages["libatomic"] = "17.3.0-6"
        return packages

    def assert_package_state(self, artifacts, packages):
        expected = {"mingw-w64-ucrt-x86_64-" + key: value for key, value in packages.items()}
        self.assertEqual(json.loads(artifacts["fixture/package-state.json"]), expected)
        self.assertEqual(dict(line.split() for line in artifacts["fixture/package-snapshot.txt"].splitlines()),
                         expected)

    def test_gcc_known_pair_is_staged_swapped_together_and_restored(self):
        packages = self.cohort_baseline()
        result, artifacts, calls, summary = self.run_script("cohort_good", cells="3 4", packages=packages)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_finalized(artifacts, summary, "COMPLETE")
        for cell in (3, 4):
            self.assertEqual(json.loads(artifacts[f"diag/cell-{cell}/status.json"])["status"], "OK")
        # Identity is collected through hot shell queries after each transaction,
        # so these assertions also catch a stale snapshot after a successful -U.
        for name in ("gcc", "cc-libs", "libgcc", "libstdc++", "libatomic"):
            self.assertIn(f"mingw-w64-ucrt-x86_64-{name} 16.2.0-4\n",
                          artifacts["diag/cell-3/identity.txt"])
            self.assertIn(f"mingw-w64-ucrt-x86_64-{name} {packages[name]}\n",
                          artifacts["diag/cell-4/identity.txt"])
        transactions = [(index, call) for index, call in enumerate(calls)
                        if call[:2] == ["pacman", "-U"]]
        self.assertEqual(len(transactions), 2)
        first_index, downgrade = transactions[0]
        restore_index, restore = transactions[1]
        for name in ("cc-libs", "libatomic"):
            self.assertTrue(any(path.endswith(f"-{name}-16.2.0-4-any.pkg.tar.zst")
                                for path in downgrade[3:]))
        self.assertFalse(any("gcc-libs-" in path or "libgfortran-" in path for path in downgrade[3:]))
        atomic_archive = "mingw-w64-ucrt-x86_64-libatomic-17.3.0-6-any.pkg.tar.zst"
        staged_downloads = [call for call in calls[:first_index] if call[0] == "curl"]
        for suffix in ("", ".sig"):
            self.assertTrue(any(call[-1].endswith(atomic_archive + suffix) for call in staged_downloads))
            self.assertIn(atomic_archive + suffix, artifacts["diag/baseline-sha256.txt"])
        self.assertTrue(any(path.endswith(atomic_archive) for path in restore[3:]))
        self.assertTrue(all("/baseline/" in path for path in restore[3:]))
        self.assertFalse(any(call[0] == "curl" for call in calls[restore_index:]))
        self.assert_package_state(artifacts, packages)

    def test_missing_atomic_refuses_selected_gcc_before_staging_or_work(self):
        packages = self.cohort_baseline(with_atomic=False)
        result, artifacts, calls, summary = self.run_script("cohort_good", cells="0 3", packages=packages)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assert_finalized(artifacts, summary, "INFRASTRUCTURE_FAILED")
        self.assertIn("GCC prerequisite missing", artifacts["diag/reason.txt"])
        self.assertIn("cc-libs=16.2.0-4", artifacts["diag/reason.txt"])
        self.assertFalse(any(call[0] in ("curl", "cmake", "probe", "perf") or
                             call[:2] == ["pacman", "-U"] for call in calls))
        self.assertEqual(artifacts["diag/rows.md"], "")
        self.assert_package_state(artifacts, packages)

    def test_missing_atomic_does_not_block_baseline_or_crt_only(self):
        packages = self.cohort_baseline(with_atomic=False)
        for cells in ("0", "1 2"):
            with self.subTest(cells=cells):
                result, artifacts, calls, summary = self.run_script("cohort_good", cells=cells, packages=packages)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_finalized(artifacts, summary, "COMPLETE")
                for cell in cells.split():
                    self.assertEqual(json.loads(artifacts[f"diag/cell-{cell}/status.json"])["status"], "OK")
                self.assert_package_state(artifacts, packages)

    def test_unknown_dependency_refusal_never_repairs_or_measures(self):
        packages = self.cohort_baseline()
        result, artifacts, calls, summary = self.run_script("cohort_unknown_dependency", cells="3", packages=packages)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_finalized(artifacts, summary, "COMPLETE")
        cell = json.loads(artifacts["diag/cell-3/status.json"])
        self.assertEqual((cell["status"], cell["probe_status"], cell["perf_status"]),
                         ("INSTALL_FAILED", "NOT_RUN", "NOT_RUN"))
        self.assertIsNone(cell["prep_ms"])
        self.assertIsNone(cell["wall_seconds"])
        transactions = [call for call in calls if call[:2] == ["pacman", "-U"]]
        self.assertEqual(len(transactions), 1)
        self.assertFalse(any(call[0] in ("cmake", "probe", "perf") for call in calls))
        self.assert_package_state(artifacts, packages)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--bash")
    options, remaining = parser.parse_known_args()
    fixture.BASH = fixture.bash_executable(options.bash)
    unittest.main(argv=[sys.argv[0], *remaining])
