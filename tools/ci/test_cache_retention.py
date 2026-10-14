"""Exercise the trusted cleanup workflow's policy without calling GitHub."""

import ast
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class CacheRetentionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        workflow = (ROOT / ".github/workflows/cache-cleanup.yml").read_text()
        body = workflow.split("python3 - <<'PY'\n", 1)[1].split("          PY\n", 1)[0]
        tree = ast.parse("\n".join(line[10:] for line in body.splitlines()))
        # Execute only the pure policy; API commands cannot run in these tests.
        policy = ast.Module(body=[node for node in tree.body
                                  if isinstance(node, (ast.Assign, ast.FunctionDef))
                                  and (isinstance(node, ast.FunctionDef)
                                       or node.targets[0].id in {"PREFIXES", "BUDGET"})], type_ignores=[])
        namespace = {}
        exec(compile(policy, str(ROOT / ".github/workflows/cache-cleanup.yml"), "exec"), namespace)
        cls.obsolete = staticmethod(namespace["obsolete_caches"])
        cls.budget = namespace["BUDGET"]

    def cache(self, identity, key, ref="refs/heads/devel", size=250_000_000):
        return dict(id=identity, key=key, ref=ref, size_in_bytes=size,
                    created_at=f"2026-10-13T00:00:{identity:02d}Z")

    def test_keeps_newest_generation_across_shas_and_configurations_per_branch(self):
        caches = [self.cache(1, "linux-x64-gcc-ccache-oldconfig-oldsha"),
                  self.cache(3, "linux-x64-gcc-ccache-newconfig-newsha"),
                  self.cache(2, "linux-x64-gcc-ccache-oldconfig-main", "refs/heads/main"),
                  self.cache(4, "perf-linux-x64-gcc-ccache-preview")]
        self.assertEqual(self.obsolete(caches), [1])

    def test_reclaims_legacy_constant_preview_key_after_rolling_save(self):
        self.assertEqual(self.obsolete([
            self.cache(1, "perf-linux-x64-gcc-ccache-config"),
            self.cache(2, "perf-linux-x64-gcc-ccache-config-sha"),
        ]), [1])

    def test_total_budget_includes_unmanaged_and_package_caches(self):
        caches = [self.cache(1, "package-old", size=4_000_000_000),
                  self.cache(2, "package-new", size=4_000_000_000),
                  self.cache(3, "perf-linux-x64-gcc-ccache-sha")]
        removed = self.obsolete(caches)
        self.assertEqual(removed, [1])
        self.assertLessEqual(sum(c["size_in_bytes"] for c in caches if c["id"] not in removed), self.budget)

    def test_oversized_cache_and_equal_timestamp_ties(self):
        caches = [self.cache(1, "tidy-cache-Linux-a"), self.cache(2, "tidy-cache-Linux-b"),
                  self.cache(3, "oversized", size=self.budget + 1)]
        caches[0]["created_at"] = caches[1]["created_at"]
        self.assertEqual(self.obsolete(caches), [3, 1])

    def test_empty_listing(self):
        self.assertEqual(self.obsolete([]), [])

    def test_cleanup_tracks_actual_workflow_names_and_never_checks_out_code(self):
        cleanup = (ROOT / ".github/workflows/cache-cleanup.yml").read_text()
        self.assertNotIn("actions/checkout", cleanup)
        triggers = cleanup.split("    workflows: [", 1)[1].split("]", 1)[0].split(", ")
        for name in ("ci", "asan", "performance"):
            workflow = (ROOT / f".github/workflows/{name}.yml").read_text()
            self.assertIn(workflow.splitlines()[0].removeprefix("name: "), triggers)

    def test_preview_key_refreshes_with_candidate_and_retains_restore_prefix(self):
        workflow = (ROOT / ".github/workflows/performance.yml").read_text()
        preview = workflow.split("  preview-frame:\n", 1)[1].split("  server-smoke-comparison:\n", 1)[0]
        keys = [line.strip().removeprefix("key: ") for line in preview.splitlines()
                if line.strip().startswith("key: ")]
        self.assertEqual(len(keys), 2)
        self.assertEqual(keys[0], keys[1])
        self.assertIn("${{ github.sha }}", keys[0])
        self.assertIn("restore-keys: |", preview)
        self.assertIn("            perf-linux-x64-gcc-ccache-\n", preview)
        self.assertIn("CCACHE_MAXSIZE: 250M", workflow)
        self.assertIn("run: ccache --cleanup", preview)


if __name__ == "__main__":
    unittest.main()
