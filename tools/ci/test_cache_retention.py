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
        workflow = (ROOT / ".github/workflows/cache-cleanup.yml").read_text(encoding="utf-8")
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

    def test_each_tidy_shard_keeps_its_newest_generation_after_grace(self):
        caches = [
            self.cache(shard, f"tidy-cache-Linux-shard{shard}of3-old")
            for shard in range(1, 4)
        ] + [
            self.cache(shard + 3, f"tidy-cache-Linux-shard{shard}of3-new")
            for shard in range(1, 4)
        ]
        self.assertTrue(all(datetime.fromisoformat(c["created_at"].replace("Z", "+00:00"))
                            < NOW - self.grace for c in caches))
        removed = self.obsolete(caches)
        self.assertEqual(removed, [3, 2, 1])
        self.assertEqual([c["id"] for c in caches if c["id"] not in removed], [4, 5, 6])

    def test_older_head_protected_cache_keeps_newer_expired_generation_in_same_shard(self):
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-head"),
                  self.cache(2, "tidy-cache-Linux-shard1of3-new")]
        self.assertTrue(all(not self.is_within_grace(c["created_at"]) for c in caches))
        self.assertEqual(self.obsolete(caches, {"refs/heads/devel": "head"}), [])

    def test_older_recently_accessed_cache_keeps_newer_expired_generation_in_same_shard(self):
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-old"),
                  self.cache(2, "tidy-cache-Linux-shard1of3-new")]
        caches[0]["last_accessed_at"] = (NOW - timedelta(hours=1)).isoformat()
        self.assertFalse(self.is_within_grace(caches[1]["created_at"]))
        self.assertEqual(self.obsolete(caches), [])

    def test_budget_pressure_keeps_single_generation_groups(self):
        for size, unmanaged_size in ((1_000_000_000, 5_000_000_000),
                                     (3_000_000_000, 0)):
            with self.subTest(size=size, unmanaged_size=unmanaged_size):
                caches = [self.cache(shard, f"tidy-cache-Linux-shard{shard}of3-only", size=size)
                          for shard in range(1, 4)]
                if unmanaged_size:
                    caches.append(self.cache(4, "package-cache", size=unmanaged_size))
                with contextlib.redirect_stdout(io.StringIO()):
                    removed = self.obsolete(caches)
                self.assertEqual(removed, [4] if unmanaged_size else [])

    def is_within_grace(self, timestamp):
        return datetime.fromisoformat(timestamp.replace("Z", "+00:00")) >= NOW - self.grace

    def test_protected_tidy_shard_does_not_replace_other_shards(self):
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-head"),
                  self.cache(2, "tidy-cache-Linux-shard2of3-old"),
                  self.cache(3, "tidy-cache-Linux-shard3of3-old"),
                  self.cache(4, "tidy-cache-Linux-shard1of3-older")]
        caches[3]["created_at"] = "2026-10-12T00:00:00Z"
        self.assertEqual(self.obsolete(caches, {"refs/heads/devel": "head"}), [4])

    def test_unsharded_tidy_keys_keep_separate_groups(self):
        caches = [self.cache(1, "tidy-cache-Linux-a"),
                  self.cache(2, "tidy-cache-Linux-b"),
                  self.cache(3, "tidy-cache-Linux-a"),
                  self.cache(4, "tidy-cache-Linux-shard1of3-new")]
        self.assertEqual(self.obsolete(caches), [1])

    def test_total_budget_includes_unmanaged_and_package_caches(self):
        caches = [self.cache(1, "package-old", size=4_000_000_000),
                  self.cache(2, "package-new", size=4_000_000_000),
                  self.cache(3, "perf-linux-x64-gcc-ccache-sha")]
        removed = self.obsolete(caches)
        self.assertEqual(removed, [1])
        self.assertLessEqual(sum(c["size_in_bytes"] for c in caches if c["id"] not in removed), self.budget)

    def test_oversized_cache_and_equal_timestamp_ties(self):
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-a"), self.cache(2, "tidy-cache-Linux-shard1of3-b"),
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
        caches = [self.cache(i, "tidy-cache-Linux-shard1of3-old") for i in range(1, 7)]
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

    def test_recent_unmanaged_and_newest_managed_caches_survive_excess_budget(self):
        caches = [self.cache(1, "package-cache", size=5_000_000_000),
                  self.cache(2, "tidy-cache-Linux-shard1of3-old", size=2_000_000_000)]
        caches[0]["last_accessed_at"] = NOW.isoformat()
        with contextlib.redirect_stdout(io.StringIO()) as log:
            self.assertEqual(self.obsolete(caches), [])
        self.assertIn("7000000000 bytes", log.getvalue())
        self.assertIn("skipping cleanup", log.getvalue())

    def test_live_heads_survive_later_saves_from_older_builds(self):
        heads = {"refs/heads/devel": "develhead", "refs/heads/main": "mainhead"}
        caches = [self.cache(1, "linux-x64-gcc-ccache-config-develhead"),
                  self.cache(2, "linux-x64-gcc-ccache-config-olddevel"),
                  self.cache(3, "linux-x64-gcc-ccache-config-mainhead", "refs/heads/main"),
                  self.cache(4, "linux-x64-gcc-ccache-config-oldmain", "refs/heads/main")]
        self.assertEqual(self.obsolete(caches, heads), [])
        retained = [c for c in caches if c["id"] not in self.obsolete(caches, heads)]
        self.assertEqual(self.obsolete(retained, heads), [])

    def test_head_protection_is_ref_scoped_and_requires_exact_sha_suffix(self):
        heads = {"refs/heads/devel": "head"}
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-head", "refs/heads/main"),
                  self.cache(2, "tidy-cache-Linux-shard1of3-old", "refs/heads/main"),
                  self.cache(3, "tidy-cache-Linux-shard1of3-head-extra"),
                  self.cache(4, "tidy-cache-Linux-shard1of3-old")]
        self.assertEqual(self.obsolete(caches, heads), [3, 1])

    def test_budget_reserves_current_heads_before_newer_unmanaged_caches(self):
        caches = [self.cache(1, "linux-x64-gcc-ccache-config-head", size=2_000_000_000),
                  self.cache(2, "package-cache", size=5_000_000_000)]
        removed = self.obsolete(caches, {"refs/heads/devel": "head"})
        self.assertEqual(removed, [2])
        self.assertLessEqual(sum(c["size_in_bytes"] for c in caches if c["id"] not in removed), self.budget)

    def test_all_current_head_versions_survive_and_excess_budget_warns(self):
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-head"),
                  self.cache(2, "tidy-cache-Linux-shard1of3-head")]
        heads = {"refs/heads/devel": "head"}
        self.assertEqual(self.obsolete(caches, heads), [])
        caches[0]["size_in_bytes"] = self.budget
        caches.append(self.cache(3, "package-cache"))
        with contextlib.redirect_stdout(io.StringIO()) as log:
            self.assertEqual(self.obsolete(caches, heads), [])
        self.assertIn("::warning::Newest, protected and grace-window caches use 6250000000 bytes", log.getvalue())
        self.assertIn("6000000000 byte (6 GB) budget by 250000000 bytes", log.getvalue())

    def test_oversized_newest_generation_survives_and_excess_budget_warns(self):
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-old"),
                  self.cache(2, "tidy-cache-Linux-shard1of3-new", size=self.budget + 1)]
        with contextlib.redirect_stdout(io.StringIO()) as log:
            self.assertEqual(self.obsolete(caches), [])
        self.assertIn("budget by 1 bytes", log.getvalue())
        self.assertIn("skipping cleanup", log.getvalue())

    def test_delayed_cleanup_for_push_a_preserves_a_after_live_head_b(self):
        # workflow_run must protect the completed run's A SHA, even if the
        # workflow itself executes on the default branch at a different SHA.
        workflow = (ROOT / ".github/workflows/cache-cleanup.yml").read_text(encoding="utf-8")
        self.assertIn("TRIGGER_SHA: ${{ github.event.workflow_run.head_sha }}", workflow)
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-A"),
                  self.cache(2, "tidy-cache-Linux-shard1of3-B"),
                  self.cache(3, "tidy-cache-Linux-shard1of3-A"),
                  self.cache(4, "tidy-cache-Linux-shard1of3-older")]
        caches[3]["created_at"] = "2026-10-12T00:00:00Z"
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
                caches = [self.cache(1, "tidy-cache-Linux-shard1of3-A"),
                          self.cache(2, "tidy-cache-Linux-shard1of3-B"),
                          self.cache(3, "tidy-cache-Linux-shard1of3-older")]
                caches[2]["created_at"] = "2026-10-12T00:00:00Z"
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
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-B", size=2_000_000_000),
                  self.cache(2, "tidy-cache-Linux-shard1of3-A", size=2_000_000_000),
                  self.cache(3, "package-cache", size=2_000_000_000),
                  self.cache(4, "tidy-cache-Linux-shard1of3-older")]
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
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-nexthead"),
                  self.cache(2, "tidy-cache-Linux-shard1of3-oldhead")]
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
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-old"), self.cache(2, "tidy-cache-Linux-shard1of3-head")]
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "head"}}])
        with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                patch("subprocess.check_output", side_effect=[
                    json.dumps([{"actions_caches": caches}]), refs, refs,
                ]), patch("subprocess.run") as delete:
            exec(compile(self.body, "cache-cleanup", "exec"), {})
        delete.assert_called_once_with(["gh", "cache", "delete", "1", "--repo", "owner/repo"], check=True)

    def test_over_budget_cleanup_skips_deletion_and_succeeds(self):
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-head", size=self.budget + 1),
                  self.cache(2, "tidy-cache-Linux-shard1of3-old"), self.cache(3, "package-cache")]
        caches[1]["created_at"] = "2026-10-12T00:00:00Z"
        refs = json.dumps([{"ref": "refs/heads/devel", "object": {"sha": "head"}}])
        with patch.dict(os.environ, {"GH_REPO": "owner/repo"}), \
                patch("subprocess.check_output", side_effect=[
                    json.dumps([{"actions_caches": caches}]), refs,
                ]), patch("subprocess.run") as delete, \
                contextlib.redirect_stdout(io.StringIO()) as log:
            exec(compile(self.body, "cache-cleanup", "exec"), {})
        delete.assert_not_called()
        self.assertIn("::warning::Newest, protected and grace-window caches use 6000000001 bytes", log.getvalue())
        self.assertIn("budget by 1 bytes", log.getvalue())

    def test_deletion_api_and_auth_errors_warn_and_continue_successfully(self):
        for status in (1, 4):
            with self.subTest(status=status):
                caches = [self.cache(1, "tidy-cache-Linux-shard1of3-old"),
                          self.cache(2, "tidy-cache-Linux-shard1of3-older"),
                          self.cache(3, "tidy-cache-Linux-shard1of3-head")]
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
        caches = [self.cache(1, "tidy-cache-Linux-shard1of3-old"), self.cache(2, "tidy-cache-Linux-shard1of3-head")]
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
        malformed_timestamp = self.cache(1, "tidy-cache-Linux-shard1of3-old")
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
        cleanup = (ROOT / ".github/workflows/cache-cleanup.yml").read_text(encoding="utf-8")
        triggers = cleanup.split("on:\n", 1)[1].split("\npermissions:", 1)[0]
        self.assertNotIn("  push:", triggers)
        self.assertNotIn("pull_request", triggers)
        self.assertIn("  workflow_run:", triggers)
        self.assertIn("  group: cache-cleanup\n  cancel-in-progress: false", cleanup)
        self.assertIn("permissions:\n  contents: read\n", cleanup)
        self.assertEqual(cleanup.count("actions: write"), 1)
        self.assertIn("    permissions:\n      contents: read\n      actions: write", cleanup)

    def test_cleanup_tracks_actual_workflow_names_and_never_checks_out_code(self):
        cleanup = (ROOT / ".github/workflows/cache-cleanup.yml").read_text(encoding="utf-8")
        self.assertNotIn("actions/checkout", cleanup)
        triggers = cleanup.split("    workflows: [", 1)[1].split("]", 1)[0].split(", ")
        for name in ("ci", "asan", "performance"):
            workflow = (ROOT / f".github/workflows/{name}.yml").read_text(encoding="utf-8")
            self.assertIn(workflow.splitlines()[0].removeprefix("name: "), triggers)

    def test_compiler_cache_caps_match_each_lane(self):
        for workflow, job, cap in (
            ("ci", "linux", "400M"),
            ("asan", "asan-shard", "600M"),
            ("ci", "diligent-rhi", "250M"),
            ("ci", "windows-msvc", "1000M"),
            ("ci", "windows-ucrt64", "1000M"),
        ):
            with self.subTest(workflow=workflow, job=job):
                workflow = (ROOT / f".github/workflows/{workflow}.yml").read_text(encoding="utf-8")
                body = workflow.split(f"  {job}:\n", 1)[1].split("    steps:\n", 1)[0]
                self.assertEqual(body.split("CCACHE_MAXSIZE: ", 1)[1].splitlines()[0], cap)

    def test_two_branches_at_compiler_caps_fit_budget_and_retire_old_ucrt_generations(self):
        # The Windows lanes write their generation on devel only; main holds the
        # Linux lanes. One generation per branch and lane must fit the budget.
        shared = {
            "linux-x64-gcc-ccache-": 400_000_000,
            "linux-x64-gcc-asan-ccache-": 600_000_000,
            "diligent-rhi-linux-x64-gcc-ccache-": 250_000_000,
            "perf-linux-x64-gcc-ccache-": 250_000_000,
            "perf-paired-linux-x64-gcc-": 250_000_000,
        }
        windows = {
            "windows-x64-msvc-ccache-": 1_000_000_000,
            "windows-x64-ucrt64-gcc-ccache-": 1_000_000_000,
        }
        sizes = {"refs/heads/main": shared, "refs/heads/devel": {**shared, **windows}}
        self.assertEqual(self.budget, 6_000_000_000)
        self.assertEqual(sum(sum(lanes.values()) for lanes in sizes.values()), 5_500_000_000)
        # A devel push saves a second generation before cleanup runs; the transient
        # total must stay below GitHub's 10 GB cap without relying on eviction.
        self.assertLess(sum(sizes["refs/heads/devel"].values()) + 5_500_000_000, 10_000_000_000)
        caches = []
        old_ids = []
        for ref, lanes in sizes.items():
            for prefix, size in lanes.items():
                identity = len(caches) + 1
                old_ids.append(identity)
                caches.extend([self.cache(identity, prefix + "old", ref, size),
                               self.cache(identity + 1, prefix + "new", ref, size)])
        removed = self.obsolete(caches)
        self.assertEqual(removed, sorted(old_ids, reverse=True))
        retained_size = sum(c["size_in_bytes"] for c in caches if c["id"] not in removed)
        self.assertEqual(self.budget - retained_size, 500_000_000)

    def test_preview_key_refreshes_with_candidate_and_retains_restore_prefix(self):
        workflow = (ROOT / ".github/workflows/performance.yml").read_text(encoding="utf-8")
        preview = workflow.split("  preview-frame:\n", 1)[1].split("  server-smoke-comparison:\n", 1)[0]
        keys = [line.strip().removeprefix("key: ") for line in preview.splitlines()
                if line.strip().startswith("key: ")]
        self.assertEqual(len(keys), 2)
        self.assertEqual(keys[0], keys[1])
        self.assertIn("${{ github.sha }}", keys[0])
        self.assertIn("restore-keys: |", preview)
        self.assertIn("            perf-linux-x64-gcc-ccache-\n", preview)
        self.assertEqual(workflow.split("CCACHE_MAXSIZE: ", 1)[1].splitlines()[0], "250M")
        self.assertIn("run: ccache --cleanup", preview)

    def test_asan_shards_share_the_existing_cache_with_one_writer(self):
        workflow = (ROOT / ".github/workflows/asan.yml").read_text(encoding="utf-8")
        shard = workflow.split("  asan-shard:\n", 1)[1].split("  asan:\n", 1)[0]
        aggregate = workflow.split("  asan:\n", 1)[1]
        keys = [line.strip().removeprefix("key: ") for line in shard.splitlines()
                if line.strip().startswith("key: ")]
        self.assertEqual(len(keys), 2)
        expected = "linux-x64-gcc-asan-ccache-${{ hashFiles('CMakeLists.txt', 'CMakePresets.json', 'cmake/**/*.cmake', 'src/**/*.cmake', 'test/CMakeLists.txt', 'tools/CMakeLists.txt', 'vendor/CMakeLists.txt') }}-${{ github.sha }}"
        self.assertEqual(keys, [expected, expected])
        self.assertEqual(workflow.count("actions/cache/save@"), 1)
        self.assertNotIn("actions/cache", aggregate)
        restore = shard.split("      - name: Restore compiler cache\n", 1)[1].split("      - name:", 1)[0]
        self.assertNotIn("matrix.shard", restore)
        self.assertIn("            linux-x64-gcc-asan-ccache-\n", restore)
        self.assertIn("CCACHE_MAXSIZE: 600M", shard)
        for name in ("Clean compiler cache before saving", "Save compiler cache"):
            step = shard.split(f"      - name: {name}\n", 1)[1].split("      - name:", 1)[0]
            condition = step.splitlines()[0].strip()
            self.assertTrue(condition.startswith("if: ${{ matrix.shard == 1 && always() && "))
            self.assertIn("github.event_name == 'push'", condition)
            self.assertIn("(github.ref == 'refs/heads/main' || github.ref == 'refs/heads/devel')", condition)
            self.assertIn("steps.compiler-cache.outputs.cache-hit != 'true'", condition)
            self.assertIn("continue-on-error: true", step)
        # Both jobs restore one lane generation; cleanup's budget and grouping stay fixed.
        self.assertEqual(self.budget, 6_000_000_000)
        self.assertEqual(self.obsolete([
            self.cache(1, "linux-x64-gcc-asan-ccache-oldconfig-oldsha"),
            self.cache(2, "linux-x64-gcc-asan-ccache-newconfig-newsha"),
        ]), [1])


if __name__ == "__main__":
    unittest.main()
