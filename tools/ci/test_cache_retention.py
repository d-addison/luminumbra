"""Exercise the trusted cleanup workflow's policy without calling GitHub."""

import ast
import contextlib
from datetime import datetime, timedelta, timezone
from functools import partial
import io
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
NOW = datetime(2026, 10, 14, 12, tzinfo=timezone.utc)


class FrozenDatetime(datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW.astimezone(tz)


class CacheRetentionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        workflow = (ROOT / ".github/workflows/cache-cleanup.yml").read_text()
        body = workflow.split("python3 - <<'PY'\n", 1)[1].split("          PY\n", 1)[0]
        cls.body = "\n".join(line[10:] for line in body.splitlines())
        tree = ast.parse(cls.body)
        # Execute only the pure policy; API commands cannot run in these tests.
        policy = ast.Module(body=[node for node in tree.body
                                  if isinstance(node, (ast.Assign, ast.FunctionDef, ast.ImportFrom))
                                  and (isinstance(node, (ast.FunctionDef, ast.ImportFrom))
                                       or node.targets[0].id in {"PREFIXES", "BUDGET", "RECENCY_GRACE"})], type_ignores=[])
        namespace = {}
        exec(compile(policy, str(ROOT / ".github/workflows/cache-cleanup.yml"), "exec"), namespace)
        cls.obsolete = staticmethod(partial(namespace["obsolete_caches"], now=NOW))
        cls.budget = namespace["BUDGET"]
        cls.grace = namespace["RECENCY_GRACE"]

    def setUp(self):
        # Freeze the inline workflow clock too; every API and delete is mocked.
        clock = patch("datetime.datetime", FrozenDatetime)
        clock.start()
        self.addCleanup(clock.stop)
        environment = patch.dict(os.environ, {"TRIGGER_SHA": "trigger"})
        environment.start()
        self.addCleanup(environment.stop)

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

    def test_creation_time_wins_for_versions_outside_grace_window(self):
        caches = [self.cache(1, "linux-x64-gcc-ccache-config-sha"),
                  self.cache(2, "linux-x64-gcc-ccache-config-sha")]
        caches[0]["created_at"] = "2026-10-13T01:00:00Z"
        caches[1]["last_accessed_at"] = "2026-10-14T00:00:00Z"
        self.assertEqual(self.obsolete(caches), [2])

    def test_six_hour_grace_uses_later_creation_or_access_and_includes_boundary(self):
        self.assertEqual(self.grace, timedelta(hours=6))
        caches = [self.cache(i, "tidy-cache-Linux-old") for i in range(1, 7)]
        cutoff = NOW - self.grace
        caches[0]["created_at"] = cutoff.isoformat()
        caches[1]["last_accessed_at"] = (cutoff + timedelta(seconds=1)).isoformat()
        caches[2]["created_at"] = (NOW - timedelta(hours=1)).isoformat()
        caches[2]["last_accessed_at"] = (cutoff - timedelta(days=1)).isoformat()
        caches[3]["created_at"] = (cutoff - timedelta(seconds=1)).isoformat()
        caches[3]["last_accessed_at"] = caches[3]["created_at"]
        caches[4]["last_accessed_at"] = None
        caches[5]["created_at"] = (NOW + timedelta(seconds=1)).isoformat()
        removed = self.obsolete(caches)
        self.assertEqual(removed, [4, 5])
        retained = [c for c in caches if c["id"] not in removed]
        self.assertEqual(self.obsolete(retained), [])

    def test_recent_unmanaged_cache_reserves_budget_before_old_caches(self):
        caches = [self.cache(1, "package-cache", size=5_000_000_000),
                  self.cache(2, "tidy-cache-Linux-old", size=2_000_000_000)]
        caches[0]["last_accessed_at"] = NOW.isoformat()
        self.assertEqual(self.obsolete(caches), [2])

    def test_live_heads_survive_later_saves_from_older_builds(self):
        heads = {"refs/heads/devel": "develhead", "refs/heads/main": "mainhead"}
        caches = [self.cache(1, "linux-x64-gcc-ccache-config-develhead"),
                  self.cache(2, "linux-x64-gcc-ccache-config-olddevel"),
                  self.cache(3, "linux-x64-gcc-ccache-config-mainhead", "refs/heads/main"),
                  self.cache(4, "linux-x64-gcc-ccache-config-oldmain", "refs/heads/main")]
        self.assertEqual(self.obsolete(caches, heads), [4, 2])
        retained = [c for c in caches if c["id"] not in self.obsolete(caches, heads)]
        self.assertEqual(self.obsolete(retained, heads), [])

    def test_head_protection_is_ref_scoped_and_requires_exact_sha_suffix(self):
        heads = {"refs/heads/devel": "head"}
        caches = [self.cache(1, "tidy-cache-Linux-head", "refs/heads/main"),
                  self.cache(2, "tidy-cache-Linux-old", "refs/heads/main"),
                  self.cache(3, "tidy-cache-Linux-head-extra"),
                  self.cache(4, "tidy-cache-Linux-old")]
        self.assertEqual(self.obsolete(caches, heads), [3, 1])

    def test_budget_reserves_current_heads_before_newer_unmanaged_caches(self):
        caches = [self.cache(1, "linux-x64-gcc-ccache-config-head", size=2_000_000_000),
                  self.cache(2, "package-cache", size=5_000_000_000)]
        removed = self.obsolete(caches, {"refs/heads/devel": "head"})
        self.assertEqual(removed, [2])
        self.assertLessEqual(sum(c["size_in_bytes"] for c in caches if c["id"] not in removed), self.budget)

    def test_all_current_head_versions_survive_and_excess_budget_warns(self):
        caches = [self.cache(1, "tidy-cache-Linux-head"),
                  self.cache(2, "tidy-cache-Linux-head")]
        heads = {"refs/heads/devel": "head"}
        self.assertEqual(self.obsolete(caches, heads), [])
        caches[0]["size_in_bytes"] = self.budget
        caches.append(self.cache(3, "package-cache"))
        with contextlib.redirect_stdout(io.StringIO()) as log:
            self.assertEqual(self.obsolete(caches, heads), [])
        self.assertIn("::warning::Protected and grace-window caches use 6250000000 bytes", log.getvalue())
        self.assertIn("6000000000 byte (6 GB) budget by 250000000 bytes", log.getvalue())

    def test_budget_does_not_fall_back_to_an_older_generation(self):
        caches = [self.cache(1, "tidy-cache-Linux-old"),
                  self.cache(2, "tidy-cache-Linux-new", size=self.budget + 1)]
        self.assertEqual(self.obsolete(caches), [2, 1])

    def test_delayed_cleanup_for_push_a_preserves_a_after_live_head_b(self):
        # workflow_run must protect the completed run's A SHA, even if the
        # workflow itself executes on the default branch at a different SHA.
        workflow = (ROOT / ".github/workflows/cache-cleanup.yml").read_text()
        self.assertIn("TRIGGER_SHA: ${{ github.event_name == 'workflow_run' && "
                      "github.event.workflow_run.head_sha || github.sha }}", workflow)
        caches = [self.cache(1, "tidy-cache-Linux-A"),
                  self.cache(2, "tidy-cache-Linux-B"),
                  self.cache(3, "tidy-cache-Linux-A"),
                  self.cache(4, "tidy-cache-Linux-older")]
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "B"}}])
        for event in ("push", "workflow_run"):
            with self.subTest(event=event):
                with patch.dict(os.environ, {"GH_REPO": "owner/repo", "TRIGGER_SHA": "A"}), \
                        patch("subprocess.check_output", side_effect=[
                            json.dumps([{"actions_caches": caches}]), refs, refs,
                        ]), patch("subprocess.run") as delete:
                    exec(compile(self.body, "cache-cleanup", "exec"), {})
                delete.assert_called_once_with(
                    ["gh", "cache", "delete", "4", "--repo", "owner/repo"], check=True)
                retained = caches[:3]
                self.assertEqual(self.obsolete(retained, {"refs/heads/devel": "B"}, "A"), [])

    def test_head_b_cleanup_preserves_recent_a_for_queued_rerun_or_concurrent_save(self):
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "B"}}])
        for timestamp in ("created_at", "last_accessed_at"):
            with self.subTest(timestamp=timestamp):
                caches = [self.cache(1, "tidy-cache-Linux-A"),
                          self.cache(2, "tidy-cache-Linux-B"),
                          self.cache(3, "tidy-cache-Linux-older")]
                # A is no longer a live/triggering head. Its recent creation or
                # access still protects it, including an A save newer than B's.
                caches[0][timestamp] = (NOW - timedelta(hours=1)).isoformat()
                with patch.dict(os.environ, {"GH_REPO": "owner/repo", "TRIGGER_SHA": "B"}), \
                        patch("subprocess.check_output", side_effect=[
                            json.dumps([{"actions_caches": caches}]), refs, refs,
                        ]), patch("subprocess.run") as delete:
                    exec(compile(self.body, "cache-cleanup", "exec"), {})
                delete.assert_called_once_with(
                    ["gh", "cache", "delete", "3", "--repo", "owner/repo"], check=True)

    def test_combined_head_trigger_and_recent_caches_exceed_budget_without_deletion(self):
        caches = [self.cache(1, "tidy-cache-Linux-B", size=2_000_000_000),
                  self.cache(2, "tidy-cache-Linux-A", size=2_000_000_000),
                  self.cache(3, "package-cache", size=2_000_000_000),
                  self.cache(4, "tidy-cache-Linux-older")]
        caches[0]["created_at"] = NOW.isoformat()  # Count overlapping protection once.
        caches[2]["created_at"] = NOW.isoformat()
        heads = {"refs/heads/devel": "B"}
        self.assertEqual(self.obsolete(caches, heads, "A"), [4])
        caches[2]["size_in_bytes"] += 1
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "B"}}])
        with patch.dict(os.environ, {"GH_REPO": "owner/repo", "TRIGGER_SHA": "A"}), \
                patch("subprocess.check_output", side_effect=[
                    json.dumps([{"actions_caches": caches}]), refs,
                ]), patch("subprocess.run") as delete, \
                contextlib.redirect_stdout(io.StringIO()) as log:
            exec(compile(self.body, "cache-cleanup", "exec"), {})
        delete.assert_not_called()
        self.assertIn("6000000001 bytes", log.getvalue())
        self.assertIn("skipping cleanup", log.getvalue())

    def test_head_advance_between_listing_and_deletion_is_rechecked(self):
        caches = [self.cache(1, "tidy-cache-Linux-nexthead"),
                  self.cache(2, "tidy-cache-Linux-oldhead")]
        def refs(sha):
            return json.dumps([{"ref": "refs/heads/devel", "object": {"sha": sha}}])
        with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                patch("subprocess.check_output", side_effect=[
                    json.dumps([{"actions_caches": caches}]), refs("oldhead"), refs("nexthead"),
                ]) as api, patch("subprocess.run") as delete:
            exec(compile(self.body, "cache-cleanup", "exec"), {})
        delete.assert_not_called()
        self.assertEqual(api.call_count, 3)

    def test_deletes_obsolete_cache_by_id_so_same_key_replacements_survive(self):
        caches = [self.cache(1, "tidy-cache-Linux-old"), self.cache(2, "tidy-cache-Linux-head")]
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "head"}}])
        with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                patch("subprocess.check_output", side_effect=[
                    json.dumps([{"actions_caches": caches}]), refs, refs,
                ]), patch("subprocess.run") as delete:
            exec(compile(self.body, "cache-cleanup", "exec"), {})
        delete.assert_called_once_with(["gh", "cache", "delete", "1", "--repo", "owner/repo"], check=True)

    def test_over_budget_cleanup_skips_deletion_and_succeeds(self):
        caches = [self.cache(1, "tidy-cache-Linux-head", size=self.budget + 1),
                  self.cache(2, "tidy-cache-Linux-old"), self.cache(3, "package-cache")]
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "head"}}])
        with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                patch("subprocess.check_output", side_effect=[
                    json.dumps([{"actions_caches": caches}]), refs,
                ]), patch("subprocess.run") as delete, \
                contextlib.redirect_stdout(io.StringIO()) as log:
            exec(compile(self.body, "cache-cleanup", "exec"), {})
        delete.assert_not_called()
        self.assertIn("::warning::Protected and grace-window caches use 6000000001 bytes", log.getvalue())
        self.assertIn("budget by 1 bytes", log.getvalue())

    def test_deletion_api_and_auth_errors_warn_and_continue_successfully(self):
        for status in (1, 4):
            with self.subTest(status=status):
                caches = [self.cache(1, "tidy-cache-Linux-old"),
                          self.cache(2, "tidy-cache-Linux-older"),
                          self.cache(3, "tidy-cache-Linux-head")]
                refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "head"}}])
                with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                        patch("subprocess.check_output", side_effect=[
                            json.dumps([{"actions_caches": caches}]), refs, refs, refs,
                        ]), patch("subprocess.run", side_effect=[
                            subprocess.CalledProcessError(status, ["gh", "cache", "delete", "2"]),
                            None,
                        ]) as delete, contextlib.redirect_stdout(io.StringIO()) as log:
                    exec(compile(self.body, "cache-cleanup", "exec"), {})
                self.assertEqual([call.args[0][3] for call in delete.call_args_list], ["2", "1"])
                self.assertIn("::warning::Could not delete cache 2", log.getvalue())
                self.assertIn(f"exit status {status}", log.getvalue())

    def test_head_recheck_api_error_warns_and_skips_unsafe_deletion(self):
        caches = [self.cache(1, "tidy-cache-Linux-old"), self.cache(2, "tidy-cache-Linux-head")]
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "head"}}])
        with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                patch("subprocess.check_output", side_effect=[
                    json.dumps([{"actions_caches": caches}]), refs,
                    subprocess.CalledProcessError(1, ["gh", "api"]),
                ]), patch("subprocess.run") as delete, \
                contextlib.redirect_stdout(io.StringIO()) as log:
            exec(compile(self.body, "cache-cleanup", "exec"), {})
        delete.assert_not_called()
        self.assertIn("::warning::Cache cleanup API/auth error", log.getvalue())

    def test_listing_api_error_warns_and_succeeds(self):
        with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                patch("subprocess.check_output", side_effect=subprocess.CalledProcessError(4, ["gh", "api"])), \
                patch("subprocess.run") as delete, contextlib.redirect_stdout(io.StringIO()) as log:
            exec(compile(self.body, "cache-cleanup", "exec"), {})
        delete.assert_not_called()
        self.assertIn("::warning::Cache cleanup API/auth error", log.getvalue())

    def test_malformed_input_is_logged_and_skips_cleanup(self):
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "head"}}])
        malformed_timestamp = self.cache(1, "tidy-cache-Linux-old")
        malformed_timestamp["last_accessed_at"] = "invalid date"
        for listing, error in (("not JSON", "JSONDecodeError"),
                               (json.dumps([{}]), "KeyError"),
                               (json.dumps([{"actions_caches": None}]), "TypeError"),
                               (json.dumps([{"actions_caches": [{"id": 1}]}]), "KeyError"),
                               (json.dumps([{"actions_caches": [malformed_timestamp]}]), "ValueError")):
            with self.subTest(error=error, listing=listing):
                with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                        patch("subprocess.check_output", side_effect=[listing, refs]), \
                        patch("subprocess.run") as delete, contextlib.redirect_stdout(io.StringIO()) as log:
                    exec(compile(self.body, "cache-cleanup", "exec"), {})
                delete.assert_not_called()
                self.assertIn(f"::warning::Malformed cache cleanup input ({error})", log.getvalue())

    def test_cleanup_only_uses_trusted_triggers_and_serialized_deletion_permissions(self):
        cleanup = (ROOT / ".github/workflows/cache-cleanup.yml").read_text()
        triggers = cleanup.split("on:\n", 1)[1].split("\npermissions:", 1)[0]
        self.assertIn("  push:\n    branches: [devel, main]", triggers)
        self.assertNotIn("pull_request", triggers)
        self.assertIn("  workflow_run:", triggers)
        self.assertIn("  group: cache-cleanup\n  cancel-in-progress: false", cleanup)
        self.assertIn("permissions:\n  contents: read\n", cleanup)
        self.assertEqual(cleanup.count("actions: write"), 1)
        self.assertIn("    permissions:\n      contents: read\n      actions: write", cleanup)

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
