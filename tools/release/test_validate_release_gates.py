#!/usr/bin/env python3
"""Offline release prerequisite regressions with isolated Git fixtures."""

from __future__ import annotations

import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import validate_release_gates as gate


ROOT = Path(__file__).resolve().parents[2]
VERSION = "9.8.7"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
RECORD_PATH = f"docs/release-gates/v{VERSION}.json"
EXPECTED_IDS = (
    "required-ci-strict-promotion", "final-revision-native-manual",
    "ucrt64-120-second-guard", "server-load-qualification", "paired-performance-gates",
    "independent-release-verification", "owner-native-windows-smoke",
)


def record(commit: str = COMMIT) -> dict:
    return {"schema_version": 1, "version": VERSION, "qualified_commit": commit,
            "gates": [{"id": name, "status": "passed", "evidence": f"issue #123: {name}"}
                      for name in EXPECTED_IDS]}


def rules(*contexts: str) -> list:
    return [{"type": "required_status_checks", "parameters": {
        "strict_required_status_checks_policy": True,
        "required_status_checks": [{"context": name} for name in contexts],
    }}]


def run(run_id: int = 1, name: str = "Linux", **fields) -> dict:
    return dict({"id": run_id, "name": name, "head_sha": COMMIT,
                 "status": "completed", "conclusion": "success"}, **fields)


def pages(*runs: dict) -> list:
    return [{"total_count": len(runs), "check_runs": list(runs)}]


class RecordTests(unittest.TestCase):
    def test_fixed_gate_order_and_valid_evidence(self):
        self.assertEqual(gate.GATE_IDS, EXPECTED_IDS)
        self.assertEqual(gate.validate_record(record(), VERSION), COMMIT)
        value = record(COMMIT.upper())
        value["gates"][0]["evidence"] = " https://example.org/issues/123 \n"
        self.assertEqual(gate.validate_record(value, VERSION), COMMIT)

    def test_record_shape_is_exact(self):
        for value in (None, True, [], "0", {}, dict(record(), extra=0)):
            with self.subTest(value=value), self.assertRaises(gate.GateError):
                gate.validate_record(value, VERSION)
        for key in record():
            value = record()
            del value[key]
            with self.subTest(key=key), self.assertRaises(gate.GateError):
                gate.validate_record(value, VERSION)

    def test_schema_and_version_types_and_mismatch(self):
        for key, values in (("schema_version", (True, False, "1", 1.0, 0, 2, None)),
                            ("version", (None, 0, True, "v9.8.7", "9.8.6"))):
            for replacement in values:
                value = record()
                value[key] = replacement
                with self.subTest(key=key, value=replacement), self.assertRaises(gate.GateError):
                    gate.validate_record(value, VERSION)

    def test_qualified_commit_requires_full_hex(self):
        for value in (None, 0, True, "0", "a" * 39, "a" * 41, "g" * 40, COMMIT + "\n"):
            with self.subTest(value=value), self.assertRaises(gate.GateError):
                gate.validate_record(record(value), VERSION)

    def test_gate_membership_and_order_are_exact(self):
        valid = record()["gates"]
        for entries in (None, {}, valid[:-1], valid + [valid[0]],
                        [valid[1], valid[0], *valid[2:]], [valid[0], *valid[:-1]],
                        [dict(valid[0], id="unknown"), *valid[1:]],
                        [dict(valid[0], id="signed-publication"), *valid[1:]]):
            value = record()
            value["gates"] = entries
            with self.subTest(entries=entries), self.assertRaises(gate.GateError):
                gate.validate_record(value, VERSION)

    def test_every_gate_requires_passed_status_and_evidence(self):
        for index in range(len(EXPECTED_IDS)):
            for field, replacements in (("status", (None, True, 1, "pending", "failed", "PASSED", "")),
                                        ("evidence", (None, True, 1, [], "", " \t\n"))):
                for replacement in replacements:
                    value = record()
                    value["gates"][index][field] = replacement
                    with self.subTest(index=index, field=field, value=replacement):
                        with self.assertRaises(gate.GateError):
                            gate.validate_record(value, VERSION)

    def test_gate_entry_shape_and_id_type(self):
        first = record()["gates"][0]
        for entry in (None, [], 0, dict(first, extra=True), dict(first, id=1),
                      {key: value for key, value in first.items() if key != "evidence"}):
            value = record()
            value["gates"][0] = entry
            with self.subTest(entry=entry), self.assertRaises(gate.GateError):
                gate.validate_record(value, VERSION)

    def test_json_refuses_malformed_duplicate_nonfinite_and_deep_input(self):
        for data in (b"", b"0x", b"\xff", b'{"a":1,"a":2}',
                     b'{"gates":[{"status":1,"status":2}]}', b"NaN", b"Infinity",
                     b"-Infinity", b"[" * 2000 + b"]" * 2000):
            with self.subTest(data=data[:50]), self.assertRaises(gate.GateError):
                gate.parse_json(data, gate.RECORD_LIMIT)

    def test_json_size_checked_before_decoding_and_inclusive_boundary(self):
        self.assertEqual(gate.parse_json(b"{}", 2), {})
        value = gate.parse_json(b"0", 1)
        self.assertEqual(value, 0)
        with self.assertRaises(gate.GateError):
            gate.validate_record(value, VERSION)
        with mock.patch.object(gate.json, "loads") as loads:
            with self.assertRaises(gate.GateError):
                gate.parse_json(b"{} ", 2)
            loads.assert_not_called()

    def test_json_nesting_bound_ignores_escaped_string_contents(self):
        data = b"[" * gate.DEPTH_LIMIT + b"]" * gate.DEPTH_LIMIT
        self.assertIsInstance(gate.parse_json(data, gate.RECORD_LIMIT), list)
        value = {"brackets": '[{"' * 100 + "\\"}
        self.assertEqual(gate.parse_json(json.dumps(value).encode(), gate.RECORD_LIMIT), value)
        with mock.patch.object(gate.json, "loads", side_effect=RecursionError("nesting")):
            with self.assertRaises(gate.GateError):
                gate.parse_json(b"{}", 2)

    def test_bounded_file_and_stdin_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.json"
            path.write_bytes(b"{}   ")
            with self.assertRaises(gate.GateError):
                gate.read_json(str(path), 2)
        stream = mock.Mock(wraps=io.BytesIO(b"{}   "))
        with mock.patch.object(gate.sys, "stdin", mock.Mock(buffer=stream)):
            with self.assertRaises(gate.GateError):
                gate.read_json("-", 2)
        stream.read.assert_called_once_with(3)


class ApiTests(unittest.TestCase):
    def test_concatenated_page_objects_and_single_array_are_equivalent(self):
        response = [{"total_count": 2, "check_runs": [run(1, conclusion="failure")]},
                    {"total_count": 2, "check_runs": [run(2)]}]
        for separator in (b"", b"\n", b" \r\n\t"):
            data = separator.join(json.dumps(page, indent=2).encode() for page in response)
            parsed = gate.parse_check_pages(b" \n" + data + b"\t ")
            self.assertEqual(parsed, response)
            gate.validate_check_runs(parsed, ("Linux",), COMMIT)
        self.assertEqual(gate.parse_check_pages(json.dumps(response).encode()), response)

    def test_page_stream_refuses_invalid_trailing_duplicate_and_nonobject_json(self):
        valid = json.dumps(pages(run())[0]).encode()
        for data in (b"", b" \n", b"0", b"[]", b"null", b"true", b"\xff",
                     valid + b"x", valid + b'{"check_runs":', valid + b"[]",
                     valid + b"0", b'{"a":1,"a":2}',
                     b'{"check_runs":[{"id":1,"id":2}]}',
                     b'{"a":NaN}', b'{"a":Infinity}', b'{"a":-Infinity}'):
            with self.subTest(data=data[:80]), self.assertRaises(gate.GateError):
                gate.parse_check_pages(data)

    def test_page_stream_size_and_nesting_bounds_apply_before_decoding(self):
        self.assertEqual(gate.parse_check_pages(b"{}", 2), [{}])
        with mock.patch.object(gate.json, "JSONDecoder") as decoder:
            with self.assertRaises(gate.GateError):
                gate.parse_check_pages(b"{}{}", 3)
            with self.assertRaises(gate.GateError):
                gate.parse_check_pages(b"[" * (gate.DEPTH_LIMIT + 1) + b"]" * (gate.DEPTH_LIMIT + 1))
            decoder.assert_not_called()
        value = {"a": '[{"' * 100 + "\\", "b": []}
        self.assertEqual(gate.parse_check_pages(json.dumps(value).encode()), [value])
        boundary = b'{"a":' + b"[" * (gate.DEPTH_LIMIT - 1) + b"]" * (gate.DEPTH_LIMIT - 1) + b"}"
        self.assertEqual(len(gate.parse_check_pages(boundary)), 1)
        with mock.patch.object(gate.json.JSONDecoder, "raw_decode", side_effect=RecursionError("nesting")):
            with self.assertRaises(gate.GateError):
                gate.parse_check_pages(b"{}")

    def test_stream_pages_still_enforce_pagination_consistency_and_unique_ids(self):
        responses = ([*pages(run()), *pages(run())],
                     [{"total_count": 2, "check_runs": [run()]}],
                     [*pages(run()), {"total_count": 2, "check_runs": [run(2)]}])
        for response in responses:
            data = b"\n".join(json.dumps(page).encode() for page in response)
            with self.subTest(response=response), self.assertRaises(gate.GateError):
                gate.validate_check_runs(gate.parse_check_pages(data), ("Linux",), COMMIT)

    def test_contexts_from_all_rules_preserve_order_and_deduplicate(self):
        response = [{"type": "required_signatures"}, *rules("Windows", "Linux"),
                    *rules("Linux", "Tidy")]
        original = copy.deepcopy(response)
        self.assertEqual(gate.required_contexts(response), ("Windows", "Linux", "Tidy"))
        self.assertEqual(response, original)

    def test_missing_or_malformed_required_rules_refuse(self):
        for response in (None, {}, [], [None], [{}], [{"type": 1}],
                         [{"type": "required_signatures"}], [{"type": "required_status_checks"}],
                         rules(), [{"type": "required_status_checks", "parameters": []}]):
            with self.subTest(response=response), self.assertRaises(gate.GateError):
                gate.required_contexts(response)

    def test_strict_policy_must_be_boolean_true_on_every_rule(self):
        for policy in (None, False, 1, "true"):
            response = rules("Linux")
            response[0]["parameters"]["strict_required_status_checks_policy"] = policy
            with self.subTest(policy=policy), self.assertRaises(gate.GateError):
                gate.required_contexts([*rules("Windows"), *response])
        response = rules("Linux")
        del response[0]["parameters"]["strict_required_status_checks_policy"]
        with self.assertRaises(gate.GateError):
            gate.required_contexts(response)

    def test_required_context_types(self):
        for checks in (None, {}, [None], [{}], [{"context": True}], [{"context": " \n"}]):
            response = rules("Linux")
            response[0]["parameters"]["required_status_checks"] = checks
            with self.subTest(checks=checks), self.assertRaises(gate.GateError):
                gate.required_contexts(response)

    def test_context_names_are_compared_exactly(self):
        context = "CI / Linux / GCC"
        contexts = gate.required_contexts(rules(context))
        gate.validate_check_runs(pages(run(name=context)), contexts, COMMIT)
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run(name="Linux / GCC")), contexts, COMMIT)

    def test_paginated_latest_run_by_id_independent_of_order(self):
        response = [{"total_count": 4, "check_runs": [run(40), run(10, conclusion="failure")]},
                    {"total_count": 4, "check_runs": [run(30, "Windows"), run(20, "Other")]}]
        original = copy.deepcopy(response)
        gate.validate_check_runs(response, ("Linux", "Windows"), COMMIT)
        gate.validate_check_runs(list(reversed(response)), ("Windows", "Linux"), COMMIT.upper())
        self.assertEqual(response, original)

    def test_newer_unsuccessful_or_incomplete_run_blocks_older_success(self):
        for status, conclusion in (("completed", "neutral"), ("completed", "skipped"),
                                   ("completed", "failure"), ("completed", "cancelled"),
                                   ("completed", "timed_out"), ("completed", "action_required"),
                                   ("completed", "stale"), ("completed", None),
                                   ("completed", "SUCCESS"), ("in_progress", "success"),
                                   ("queued", None), ("waiting", None)):
            with self.subTest(status=status, conclusion=conclusion), self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(run(), run(2, status=status, conclusion=conclusion)),
                                         ("Linux",), COMMIT)

    def test_missing_context_cannot_be_replaced_by_other_success(self):
        for response in (pages(), pages(run(name="Other")), pages(run())):
            with self.subTest(response=response), self.assertRaises(gate.GateError):
                gate.validate_check_runs(response, ("Linux", "Windows"), COMMIT)

    def test_required_rerun_after_first_hundred_runs_is_checked(self):
        first_page = [run(index, f"Other {index}") for index in range(1, 101)]
        response = [{"total_count": 101, "check_runs": first_page},
                    {"total_count": 101, "check_runs": [run(101)]}]
        gate.validate_check_runs(response, ("Linux",), COMMIT)
        first_page[0] = run()
        response[1]["check_runs"][0]["conclusion"] = "failure"
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(response, ("Linux",), COMMIT)

    def test_pagination_missing_changed_counts_and_duplicate_runs(self):
        for response in (None, {}, [], [None], [{}],
                         [{"total_count": True, "check_runs": []}],
                         [{"total_count": -1, "check_runs": []}],
                         [{"total_count": 1, "check_runs": {}}],
                         [{"total_count": 2, "check_runs": [run()]}],
                         [{"total_count": 0, "check_runs": [run()]}],
                         [*pages(run()), {"total_count": 2, "check_runs": [run(2)]}],
                         pages(run(), run())):
            with self.subTest(response=response), self.assertRaises(gate.GateError):
                gate.validate_check_runs(response, ("Linux",), COMMIT)

    def test_malformed_runs_and_wrong_commit_refuse(self):
        invalid = [None, [], {}]
        for key, values in (("id", (True, "1", 0, -1)), ("name", (None, 0, " \n")),
                            ("head_sha", (None, "0", "f" * 40)), ("status", (None, True)),
                            ("conclusion", (True, []))):
            invalid.extend(dict(run(), **{key: value}) for value in values)
        for key in run():
            value = run()
            del value[key]
            invalid.append(value)
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(value), ("Linux",), COMMIT)
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run(), run(2, "Other", head_sha="f" * 40)),
                                     ("Linux",), COMMIT)


class GitCommandTests(unittest.TestCase):
    def test_git_errors_include_bounded_stderr_with_replacement_decoding(self):
        for stderr in (b"fatal: broken configuration\n", b"fatal: \xff\n",
                       b"x" * 4096, b"x" * 4096 + b"hidden suffix", b""):
            result = subprocess.CompletedProcess([], 128, stdout=b"", stderr=stderr)
            with self.subTest(stderr=stderr[:40]), mock.patch.object(
                    gate.subprocess, "run", return_value=result):
                with self.assertRaises(gate.GateError) as caught:
                    gate.git(ROOT, "rev-parse", "HEAD^{commit}")
                message = str(caught.exception)
                self.assertIn("Git prerequisite failed: rev-parse HEAD^{commit}", message)
                self.assertIn("exit 128", message)
                self.assertIn(stderr[:4096].decode("utf-8", errors="replace").strip(), message)
                self.assertLessEqual(len(message), 4300)
                self.assertNotIn("hidden suffix", message)
                self.assertEqual("[stderr truncated]" in message, len(stderr) > 4096)

    def test_git_child_configuration_is_isolated_without_mutating_parent(self):
        overrides = {"GIT_CONFIG_NOSYSTEM": "0", "GIT_CONFIG_GLOBAL": "broken-global",
                     "GIT_CONFIG_SYSTEM": "broken-system", "GIT_CONFIG_COUNT": "1",
                     "GIT_CONFIG_KEY_0": "core.bare", "GIT_CONFIG_VALUE_0": "true",
                     "GIT_DIR": "other", "GIT_WORK_TREE": "other",
                     "GIT_OBJECT_DIRECTORY": "other", "GIT_REPLACE_REF_BASE": "refs/other/",
                     "Git_Dir": "mixed-dir", "git_work_tree": "mixed-work-tree",
                     "GIT_config_global": "mixed-global"}
        result = subprocess.CompletedProcess([], 0, stdout=b"result\n", stderr=b"warning")
        # A plain mapping preserves mixed-case keys even on Windows.
        with mock.patch.object(gate.os, "environ", dict(os.environ, **overrides)):
            before = dict(os.environ)
            with mock.patch.object(gate.subprocess, "run", return_value=result) as command:
                self.assertEqual(gate.git(ROOT, "rev-parse", "HEAD^{commit}"), b"result\n")
            self.assertEqual(dict(os.environ), before)
        child = command.call_args.kwargs["env"]
        self.assertEqual({key: value for key, value in child.items() if key.upper().startswith("GIT_")},
                         {"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull})
        self.assertEqual(command.call_args.args[0],
                         ["git", "--no-replace-objects", "-C", str(ROOT),
                          "rev-parse", "HEAD^{commit}"])


class GitTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        self.env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1",
                        GIT_AUTHOR_NAME="Fixture", GIT_AUTHOR_EMAIL="fixture@example.org",
                        GIT_COMMITTER_NAME="Fixture", GIT_COMMITTER_EMAIL="fixture@example.org",
                        GIT_AUTHOR_DATE="2000-01-01T00:00:00+00:00",
                        GIT_COMMITTER_DATE="2000-01-01T00:00:00+00:00")
        self.git("init", "-q", "--template=")
        self.base_files = {"shipping.txt": ("100644", b"qualified content\n")}
        self.base = self.commit(self.base_files)
        self.tag = self.candidate(record(self.base))
        self.promote(self.tag)

    def git(self, *args: str, data: bytes = b"") -> str:
        result = subprocess.run(["git", "-C", str(self.root), *args], env=self.env, input=data,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        return result.stdout.decode().strip()

    def tree(self, files: dict) -> str:
        children = {}
        for path, value in files.items():
            head, separator, tail = path.partition("/")
            if separator:
                children.setdefault(head, {})[tail] = value
            else:
                children[head] = value
        entries = []
        for name, value in sorted(children.items()):
            if type(value) is dict:
                mode, kind, object_id = "040000", "tree", self.tree(value)
            else:
                mode, content = value
                kind, object_id = "blob", self.git("hash-object", "-w", "--stdin", data=content)
            entries.append(f"{mode} {kind} {object_id}\t{name}\0".encode())
        return self.git("mktree", "-z", data=b"".join(entries))

    def commit(self, files: dict, parent: str | None = None,
               second_parent: str | None = None) -> str:
        args = [argument for value in (parent, second_parent) if value for argument in ("-p", value)]
        return self.git("commit-tree", self.tree(files), *args, data=b"fixture\n")

    def candidate(self, value: dict, **extra: tuple[str, bytes]) -> str:
        return self.commit(dict(self.base_files, **extra, **{
            RECORD_PATH: ("100644", json.dumps(value).encode()),
        }), self.base)

    def promote(self, commit: str) -> None:
        self.git("update-ref", "HEAD", commit)
        self.git("update-ref", "refs/remotes/origin/main", commit)

    def validate(self, commit: str | None = None, version: str = VERSION) -> None:
        gate.validate_git_record(self.root, version, commit or self.tag)

    def test_record_only_commit_promoted_at_tip_or_ancestor(self):
        self.validate()
        future = self.commit({"future.txt": ("100644", b"main advances")}, self.tag)
        self.git("update-ref", "refs/remotes/origin/main", future)
        self.validate()

    def test_tag_on_merged_second_parent_omitting_promotion_content_refuses(self):
        first_parent = self.commit({"shipping.txt": ("100644", b"promotion content\n")}, self.base)
        files = {"shipping.txt": ("100644", b"promotion content\n"),
                 RECORD_PATH: ("100644", json.dumps(record(self.base)).encode())}
        promotion = self.commit(files, first_parent, self.tag)
        self.git("update-ref", "refs/remotes/origin/main", promotion)
        self.assertTrue(self.git("diff", "--name-only", self.tag, promotion))
        self.git("merge-base", "--is-ancestor", self.tag, promotion)
        before = self.git("show-ref")
        with self.assertRaisesRegex(gate.GateError, "tag commit.*first-parent"):
            self.validate()
        self.assertEqual(self.git("show-ref"), before)
        self.assertFalse((self.root / ".git/index").exists())

    def test_tag_on_first_parent_promotion_merge_adding_only_record_is_allowed(self):
        files = dict(self.base_files, **{RECORD_PATH: ("100644", json.dumps(record(self.base)).encode())})
        promotion = self.commit(files, self.base, self.tag)
        self.promote(promotion)
        self.assertEqual(self.git("diff", "--name-only", self.base, promotion), RECORD_PATH)
        self.validate(promotion)

    def test_qualified_commit_on_merged_second_parent_refuses(self):
        qualified = self.commit(self.base_files, self.base)
        files = dict(self.base_files, **{RECORD_PATH: ("100644", json.dumps(record(qualified)).encode())})
        branch = self.commit(files, qualified)
        promotion = self.commit(files, self.base, branch)
        self.promote(promotion)
        self.git("merge-base", "--is-ancestor", qualified, promotion)
        with self.assertRaisesRegex(gate.GateError, "qualified_commit.*first-parent"):
            self.validate(promotion)

    def test_first_parent_scan_is_bounded_and_refuses_even_tip_match_on_overflow(self):
        with mock.patch.object(gate, "MAIN_HISTORY_LIMIT", 2):
            self.validate()
            future = self.commit(self.base_files, self.tag)
            self.promote(future)
            with self.assertRaisesRegex(gate.GateError, "history exceeds bound"):
                self.validate(future)
        with mock.patch.object(gate, "git", return_value=b"\n") as command:
            with self.assertRaises(gate.GateError):
                gate.main_first_parent_history(self.root)
            command.assert_called_once_with(self.root, "rev-list", "--first-parent",
                                            f"--max-count={gate.MAIN_HISTORY_LIMIT + 1}",
                                            "refs/remotes/origin/main")
        for output in (b"", b"0\n", f"{COMMIT}\n{COMMIT}\n".encode()):
            with self.subTest(output=output), mock.patch.object(gate, "git", return_value=output):
                with self.assertRaises(gate.GateError):
                    gate.main_first_parent_history(self.root)

    def test_absent_record_even_with_worktree_file_refuses(self):
        self.promote(self.base)
        path = self.root / RECORD_PATH
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(record(self.base)), encoding="utf-8")
        with self.assertRaisesRegex(gate.GateError, "missing committed"):
            self.validate(self.base)

    def test_worktree_cannot_override_committed_pending_record(self):
        pending = record(self.base)
        pending["gates"][0]["status"] = "pending"
        candidate = self.candidate(pending)
        self.promote(candidate)
        path = self.root / RECORD_PATH
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(record(self.base)), encoding="utf-8")
        with self.assertRaisesRegex(gate.GateError, "has not passed"):
            self.validate(candidate)

    def test_tag_not_reachable_from_main_or_missing_main_refuses(self):
        self.git("update-ref", "refs/remotes/origin/main", self.base)
        with self.assertRaises(gate.GateError):
            self.validate()
        self.git("update-ref", "-d", "refs/remotes/origin/main")
        with self.assertRaises(gate.GateError):
            self.validate()

    def test_checkout_must_match_tag_and_inputs_cannot_be_revisions(self):
        self.git("update-ref", "HEAD", self.base)
        with self.assertRaisesRegex(gate.GateError, "checkout"):
            self.validate()
        for value in ("HEAD", "0", "a" * 39, "g" * 40):
            with self.subTest(value=value), self.assertRaises(gate.GateError):
                self.validate(value)
        for version in ("../9.8.7", "v9.8.7", "0", "9.8.7\n"):
            with self.subTest(version=version), self.assertRaises(gate.GateError):
                self.validate(version=version)

    def test_unrelated_descendant_and_missing_qualified_commits_refuse(self):
        unrelated = self.commit({"other.txt": ("100644", b"other")})
        descendant = self.commit(self.base_files, self.tag)
        for qualified in (unrelated, descendant, "f" * 40):
            candidate = self.candidate(record(qualified))
            self.promote(candidate)
            with self.subTest(qualified=qualified), self.assertRaises(gate.GateError):
                self.validate(candidate)

    def test_changes_additions_deletions_and_renames_outside_record_refuse(self):
        for files in (
            {"shipping.txt": ("100644", b"changed")},
            {"new.txt": ("100644", b"new")},
            {"docs/release-gates/v1.2.3.json": ("100644", b"{}")},
            {"shipping.txt": ("100755", b"qualified content\n")},
        ):
            candidate = self.candidate(record(self.base), **files)
            self.promote(candidate)
            with self.subTest(files=files), self.assertRaisesRegex(gate.GateError, "outside"):
                self.validate(candidate)
        for files in ({}, {"renamed.txt": self.base_files["shipping.txt"]}):
            files[RECORD_PATH] = ("100644", json.dumps(record(self.base)).encode())
            candidate = self.commit(files, self.base)
            self.promote(candidate)
            with self.subTest(files=files), self.assertRaisesRegex(gate.GateError, "outside"):
                self.validate(candidate)

    def test_symlink_oversized_and_malformed_committed_records_refuse(self):
        for mode, data in (("120000", b"shipping.txt"),
                           ("100644", b" " * (gate.RECORD_LIMIT + 1)),
                           ("100644", b"{")):
            candidate = self.commit(dict(self.base_files, **{RECORD_PATH: (mode, data)}), self.base)
            self.promote(candidate)
            with self.subTest(mode=mode, size=len(data)), self.assertRaises(gate.GateError):
                self.validate(candidate)

    def test_equal_qualified_commit_is_allowed(self):
        gate.validate_qualified_tree(self.root, self.tag, self.tag, RECORD_PATH)

    def test_rewriting_only_existing_record_is_allowed(self):
        self.base_files[RECORD_PATH] = ("100644", b"{}")
        self.base = self.commit(self.base_files, self.base)
        self.tag = self.candidate(record(self.base))
        self.promote(self.tag)
        self.validate()

    def test_submodule_changes_cannot_be_hidden_by_git_configuration(self):
        nested = self.git("mktree", data=b"")
        first = self.git("commit-tree", nested, data=b"first\n")
        second = self.git("commit-tree", nested, "-p", first, data=b"second\n")
        first_tree = self.git("mktree", data=f"160000 commit {first}\tdependency\n".encode())
        second_tree = self.git("mktree", data=f"160000 commit {second}\tdependency\n".encode())
        qualified = self.git("commit-tree", first_tree, data=b"qualified\n")
        changed = self.git("commit-tree", second_tree, "-p", qualified, data=b"changed\n")
        self.git("config", "diff.ignoreSubmodules", "all")
        with self.assertRaisesRegex(gate.GateError, "outside"):
            gate.validate_qualified_tree(self.root, qualified, changed, RECORD_PATH)

    def test_inherited_git_location_and_replace_objects_cannot_bypass_history(self):
        with mock.patch.dict(os.environ, {"GIT_DIR": str(self.root / "missing"),
                                          "GIT_WORK_TREE": str(self.root / "other")}):
            self.validate()
        candidate = self.candidate(record(self.base), **{"shipping.txt": ("100644", b"changed")})
        self.promote(candidate)
        self.git("replace", candidate, self.tag)
        with self.assertRaises(gate.GateError):
            self.validate(candidate)

    def test_broken_ambient_global_and_system_configuration_cannot_change_validation(self):
        home = self.root / "ambient home"
        xdg = home / ".config"
        (xdg / "git").mkdir(parents=True)
        broken = home / ".gitconfig"
        broken.write_text("[invalid configuration\n", encoding="utf-8")
        (xdg / "git/config").write_bytes(broken.read_bytes())
        before = self.git("show-ref")
        overrides = {"HOME": str(home), "USERPROFILE": str(home), "XDG_CONFIG_HOME": str(xdg),
                     "GIT_CONFIG_NOSYSTEM": "0", "GIT_CONFIG_SYSTEM": str(broken)}
        with mock.patch.dict(os.environ, overrides):
            # Show that Git really encounters the bad file without isolation.
            ambient = {key: value for key, value in os.environ.items()
                       if not key.startswith("GIT_")}
            result = subprocess.run(["git", "-C", str(self.root), "rev-parse", "HEAD^{commit}"],
                                    env=ambient, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    check=False)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertTrue(result.stderr)
            self.validate()
            # Exercise the publication caller's default root and stdin interface.
            script = self.root / "tools/release/validate_release_gates.py"
            script.parent.mkdir(parents=True)
            script.write_bytes(Path(gate.__file__).read_bytes())
            rules_path = self.root / "rules.json"
            rules_path.write_text(json.dumps(rules("Linux")), encoding="utf-8")
            result = subprocess.run([
                sys.executable, "-I", "-B", str(script), "--version", VERSION,
                "--tag-commit", self.tag, "--rules", str(rules_path),
            ], input=json.dumps(pages(run(head_sha=self.tag))).encode(), cwd=home,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"PASS", result.stdout)
            # Valid configuration must not hide changes to qualified content either.
            broken.write_text("[diff]\n\tignoreSubmodules = all\n", encoding="utf-8")
            (xdg / "git/config").write_bytes(broken.read_bytes())
            candidate = self.candidate(record(self.base), **{"shipping.txt": ("100644", b"changed")})
            with self.assertRaisesRegex(gate.GateError, "outside"):
                gate.validate_qualified_tree(self.root, self.base, candidate, RECORD_PATH)
        self.assertEqual(self.git("show-ref"), before)
        self.assertFalse((self.root / ".git/index").exists())

    def test_cli_success_and_refusals_are_read_only(self):
        rules_path, checks_path = self.root / "rules.json", self.root / "checks.json"
        rules_path.write_text(json.dumps(rules("Linux")), encoding="utf-8")
        checks_path.write_text(json.dumps({"total_count": 2, "check_runs": [
            run(1, head_sha=self.tag, conclusion="failure")
        ]}) + "\n" + json.dumps({"total_count": 2, "check_runs": [
            run(2, head_sha=self.tag)
        ]}), encoding="utf-8")
        args = [sys.executable, "-I", "-B", str(Path(gate.__file__)), "--repo-root", str(self.root),
                "--version", VERSION, "--tag-commit", self.tag, "--rules", str(rules_path)]
        before = self.git("show-ref")
        result = subprocess.run(args, input=checks_path.read_bytes(), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=self.env, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"PASS", result.stdout)
        result = subprocess.run([*args, "--checks", str(checks_path)], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=self.env, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        for checks in ("missing.json", str(checks_path)):
            if checks == str(checks_path):
                checks_path.write_text("[]", encoding="utf-8")
            result = subprocess.run([*args, "--checks", checks], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, env=self.env, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"FAIL", result.stderr)
        for rules_input in ("-", str(self.root / "missing-rules.json")):
            result = subprocess.run([*args, "--rules", rules_input], input=b"",
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    env=self.env, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"FAIL", result.stderr)
        self.assertEqual(self.git("show-ref"), before)
        self.assertFalse((self.root / ".git/index").exists())


class WiringTests(unittest.TestCase):
    def test_ctest_and_python_plan_registration(self):
        sys.path.insert(0, str(ROOT / "tools/ci"))
        import ci_paths
        path = "tools/release/test_validate_release_gates.py"
        self.assertIn(path, ci_paths.PYTHON_TESTS)
        self.assertIn((path,), ci_paths.python_test_commands(ROOT))
        self.assertIn("tools/release/validate_release_gates.py", ci_paths.PYTHON_MODULES)
        self.assertTrue(ci_paths.needs_cpp("tools/release/validate_release_gates.py"))
        cmake = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('NAME ReleaseGatesScriptContract\n', cmake)
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/release/test_validate_release_gates.py"', cmake)
        self.assertIn('set_tests_properties(ReleaseGatesScriptContract\n'
                      '        PROPERTIES LABELS "release;tools" TIMEOUT 60)', cmake)

    def test_workflow_requires_gates_before_archive_and_publication(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        identity = workflow.index("      - name: Verify release identity and public tree")
        verification = workflow.index("      - name: Verify release gates")
        archive = workflow.index("      - name: Build source archive, SBOM, and checksums")
        publication = workflow.index("      - name: Create source-only GitHub release")
        self.assertLess(identity, verification)
        self.assertLess(verification, archive)
        self.assertLess(archive, publication)
        step = workflow[verification:archive]
        self.assertNotIn("continue-on-error", step)
        self.assertNotIn("if:", step)
        for text in ("set -euo pipefail", "git fetch --no-tags origin +refs/heads/main:refs/remotes/origin/main",
                     'git rev-parse "${GITHUB_REF_NAME}^{commit}"',
                     'gh api "repos/${GITHUB_REPOSITORY}/rules/branches/main"',
                     'gh api --paginate "repos/${GITHUB_REPOSITORY}/commits/${tag_commit}/check-runs?filter=all&per_page=100" |',
                     "python3 -I -B tools/release/validate_release_gates.py",
                     '--version "$RELEASE_VERSION" --tag-commit "$tag_commit"',
                     '--rules "${RUNNER_TEMP}/release-rules.json"'):
            self.assertIn(text, step)
        self.assertNotIn("--slurp", step)
        self.assertIn("fetch-depth: 0", workflow[:verification])

    def test_source_job_has_only_required_permissions_including_checks_read(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn("\npermissions:\n  contents: read\n", workflow)
        source = workflow.split("\n  source:\n", 1)[1].split("\n  binary-linux:\n", 1)[0]
        permissions = source.split("    permissions:\n", 1)[1].split("    steps:\n", 1)[0]
        self.assertEqual(permissions, "      contents: write\n      checks: read\n"
                         "      id-token: write\n      attestations: write\n")

    def test_documented_ids_match_validator_and_acceptance_link(self):
        documentation = (ROOT / "tools/release/README.md").read_text(encoding="utf-8")
        positions = [documentation.index(f'"id": "{name}"') for name in EXPECTED_IDS]
        self.assertEqual(positions, sorted(positions))
        acceptance = (ROOT / "docs/v0.3-acceptance.md").read_text(encoding="utf-8")
        self.assertIn('`tools/release/README.md` (section "Publication gate record")', acceptance)
        self.assertIn("## Publication gate record", documentation)


if __name__ == "__main__":
    unittest.main()
