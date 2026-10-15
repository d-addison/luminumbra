#!/usr/bin/env python3
"""Offline release prerequisite regressions with isolated Git fixtures."""

from __future__ import annotations

import copy
import io
import json
import os
from pathlib import Path
import re
import shlex
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
    "owner-native-windows-smoke",
)


def record(commit: str = COMMIT) -> dict:
    return {"schema_version": 1, "version": VERSION, "qualified_commit": commit,
            "gates": [{"id": name, "status": "passed",
                       "evidence": f"{commit} https://github.com/owner/repository/actions/runs/123"}
                      for name in EXPECTED_IDS]}


def rules(*contexts: str) -> list:
    return [{"ruleset_id": 1, "type": "required_status_checks", "parameters": {
        "strict_required_status_checks_policy": True,
        "required_status_checks": [{"context": name} for name in contexts],
    }}]


def requirements(*names: str) -> tuple:
    return tuple((name, None) for name in names)


def branch(*contexts: str, enabled: bool = True, level: str = "everyone",
           checks: list | None = None) -> dict:
    return {"protected": True, "protection": {"enabled": enabled,
            "required_status_checks": {"enforcement_level": level,
                                       "contexts": list(contexts),
                                       "checks": [] if checks is None else checks}}}


def rule_stream(*response_pages: list, links: tuple[str, ...] = (), newline: str = "\n") -> bytes:
    responses = []
    for index, page in enumerate(response_pages):
        headers = ["HTTP/2.0 200 OK", "Content-Type: application/json"]
        if links and links[index]:
            headers.append(f"Link: {links[index]}")
        responses.append(newline.join(headers) + newline * 2 + json.dumps(page))
    return newline.join(responses).encode()


def rule_link(page: int, relation: str) -> str:
    return (f'<https://api.github.com/repos/owner/repository/rules/branches/main?per_page=100&page={page}>'
            f'; rel="{relation}"')


def run(run_id: int = 1, name: str = "Linux", **fields) -> dict:
    return dict({"id": run_id, "name": name, "head_sha": COMMIT,
                 "status": "completed", "conclusion": "success"}, **fields)


def pages(*runs: dict) -> list:
    return [{"total_count": len(runs), "check_runs": list(runs)}]


def status(status_id: int = 1, context: str = "Linux", **fields) -> dict:
    return dict({"id": status_id, "context": context, "state": "success",
                 "created_at": "2000-01-01T00:00:00Z", "creator": {"id": 99}}, **fields)


def status_link(page: int, relation: str, commit: str = COMMIT) -> str:
    return (f'<https://api.github.com/repos/owner/repository/commits/{commit}/statuses?per_page=100&page={page}>'
            f'; rel="{relation}"')


class RecordTests(unittest.TestCase):
    def test_revision_bound_evidence_supports_runs_artifacts_and_comments(self):
        for name in EXPECTED_IDS:
            for location in ("https://github.com/owner/repository/actions/runs/123",
                             "https://github.com/owner/repository/actions/runs/123/artifacts/456",
                             "https://github.com/owner/repository/issues/123#issuecomment-456"):
                value = record()
                index = EXPECTED_IDS.index(name)
                value["gates"][index]["evidence"] = f"{COMMIT} {location}"
                with self.subTest(name=name, location=location):
                    self.assertEqual(gate.validate_record(value, VERSION), COMMIT)

    def test_every_gate_refuses_local_evidence_paths_with_clear_message(self):
        for index, name in enumerate(EXPECTED_IDS):
            for extension in ("json", "md", "txt", "log"):
                value = record()
                location = f"docs/release-evidence/{COMMIT}/{name}.{extension}"
                value["gates"][index]["evidence"] = f"{COMMIT} {location}"
                with self.subTest(name=name, extension=extension):
                    with self.assertRaisesRegex(
                            gate.GateError,
                            f"evidence record location must be a supported HTTPS URL: {name}"):
                        gate.validate_record(value, VERSION)

    def test_every_gate_refuses_placeholders_wrong_revision_and_nonconcrete_locations(self):
        for index, name in enumerate(EXPECTED_IDS):
            location = "https://github.com/owner/repository/actions/runs/123"
            for evidence in ("issue #123", "TODO", "pending", "passed", COMMIT,
                             f"{'f' * 40} {location}", f"{COMMIT[:8]} {location}",
                             f"{COMMIT} docs/release-evidence/{'f' * 40}/{name}.json",
                             f"{COMMIT} docs/release-evidence/{COMMIT}/other.json",
                             f"{COMMIT} docs/release-evidence/{COMMIT}/../{name}.json",
                             f"{COMMIT} issue #123", f"{COMMIT} <artifact URL>",
                             f"{COMMIT} https://github.com/owner/repository/issues/123",
                             f"{COMMIT} https://github.com/owner/repository/actions/runs/0",
                             f"{COMMIT} https://github.com/owner/repository/actions/runs/TODO",
                             f"{COMMIT} http://github.com/owner/repository/actions/runs/123",
                             f"{COMMIT} https://github.com/owner/repository/actions/runs/123 extra"):
                value = record()
                value["gates"][index]["evidence"] = evidence
                with self.subTest(name=name, evidence=evidence), self.assertRaises(gate.GateError):
                    gate.validate_record(value, VERSION)

    def test_post_publication_verification_cannot_enter_record(self):
        self.assertNotIn("independent-release-verification", gate.GATE_IDS)
        value = record()
        value["gates"].insert(5, {"id": "independent-release-verification", "status": "passed",
                                  "evidence": f"{COMMIT} https://github.com/owner/repository/actions/runs/123"})
        with self.assertRaises(gate.GateError):
            gate.validate_record(value, VERSION)

    def test_fixed_gate_order_and_valid_evidence(self):
        self.assertEqual(gate.GATE_IDS, EXPECTED_IDS)
        self.assertEqual(gate.validate_record(record(), VERSION), COMMIT)
        value = record(COMMIT.upper())
        value["gates"][0]["evidence"] = f" {COMMIT.upper()} https://github.com/owner/repository/actions/runs/123 \n"
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
    def test_classic_contexts_merge_with_rules_and_preserve_all_app_bindings(self):
        response = branch("Linux", "Windows", "Legacy", "Legacy", checks=[
            {"context": "Windows", "app_id": 42},
            {"context": "Windows", "app_id": 99},
            {"context": "Windows", "app_id": 42},
            {"context": "Other", "app_id": None},
        ])
        original = copy.deepcopy(response)
        contexts = gate.required_contexts(rules("Linux"), gate.classic_required_contexts(response))
        self.assertEqual(contexts, (("Linux", None), ("Windows", 42), ("Windows", 99),
                                    ("Other", None), ("Legacy", None)))
        self.assertEqual(response, original)
        runs = [run(), run(2, "Windows", app={"id": 42}),
                run(3, "Windows", app={"id": 99})]
        gate.validate_check_runs(pages(*runs), contexts, COMMIT,
                                 [status(context="Other"), status(2, "Legacy")])
        for missing in range(len(runs)):
            with self.subTest(missing=missing), self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(*(runs[:missing] + runs[missing + 1:])),
                                         contexts, COMMIT,
                                         [status(context="Other"), status(2, "Legacy")])

    def test_disabled_classic_protection_is_ignored_including_rules_only_live_shape(self):
        for response in ({"protected": False}, branch(enabled=False, level="off"),
                         branch("Windows", enabled=False), branch("Windows", level="off")):
            with self.subTest(response=response):
                self.assertEqual(gate.classic_required_contexts(response), ())
                contexts = gate.required_contexts(rules("Linux"), gate.classic_required_contexts(response))
                self.assertEqual(contexts, requirements("Linux"))
                gate.validate_check_runs(pages(run()), contexts, COMMIT, [])

    def test_classic_protected_response_refuses_missing_and_malformed_fields(self):
        for response in (None, [], {}, {"protected": 1}, {"protected": "true"},
                         {"protected": True}, {"protected": True, "protection": None}):
            with self.subTest(response=response), self.assertRaises(gate.GateError):
                gate.classic_required_contexts(response)
        for enabled in (True, False):
            for path, invalid_values in (
                    (("protection", "enabled"), (None, 0, "true", {})),
                    (("protection", "required_status_checks"), (None, [], True)),
                    (("protection", "required_status_checks", "enforcement_level"),
                     (None, True, "unknown", "")),
                    (("protection", "required_status_checks", "contexts"),
                     (None, {}, [True], [""], [" \n"])),
                    (("protection", "required_status_checks", "checks"),
                     (None, {}, [None], [{}],
                      [{"context": "Linux", "app_id": True}],
                      [{"context": "Linux", "app_id": False}],
                      [{"context": "Linux", "app_id": 0}],
                      [{"context": "Linux", "app_id": -2}],
                      [{"context": "Linux", "app_id": -1.0}],
                      [{"context": "Linux", "app_id": "42"}],
                      [{"context": "Linux", "app_id": 42.0}],
                      [{"context": "Linux", "app_id": {}}],
                      [{"context": "Linux", "app_id": []}],
                      [{"context": "", "app_id": 42}]))):
                for value in (*invalid_values, "missing"):
                    response = branch("Linux", enabled=enabled)
                    target = response
                    for key in path[:-1]:
                        target = target[key]
                    if value == "missing":
                        del target[path[-1]]
                    else:
                        target[path[-1]] = value
                    with self.subTest(enabled=enabled, path=path, value=value):
                        with self.assertRaises(gate.GateError):
                            gate.classic_required_contexts(response)

    def test_classic_any_app_null_and_absent_bindings_are_unpinned(self):
        for check in ({"context": "Windows", "app_id": -1},
                      {"context": "Windows", "app_id": None},
                      {"context": "Windows"}):
            response = branch("Windows", checks=[check])
            original = copy.deepcopy(response)
            classic = gate.classic_required_contexts(response)
            with self.subTest(check=check):
                self.assertEqual(classic, requirements("Windows"))
                self.assertEqual(response, original)
                contexts = gate.required_contexts(rules("Linux"), classic)
                self.assertEqual(contexts, requirements("Linux", "Windows"))
                for app_id in (42, 99):
                    with self.subTest(app_id=app_id):
                        gate.validate_check_runs(
                            pages(run(), run(2, "Windows", app={"id": app_id})),
                            contexts, COMMIT, [])
                with self.assertRaises(gate.GateError):
                    gate.validate_check_runs(pages(run()), contexts, COMMIT, [])
                with self.assertRaises(gate.GateError):
                    gate.validate_check_runs(
                        pages(run(), run(2, "Windows", conclusion="failure")),
                        contexts, COMMIT, [])

    def test_classic_app_binding_cannot_be_satisfied_by_other_app_or_status_creator(self):
        for level in ("non_admins", "everyone"):
            response = branch("Windows", level=level, checks=[{"context": "Windows", "app_id": 42}])
            contexts = gate.required_contexts(rules("Linux"), gate.classic_required_contexts(response))
            self.assertEqual(contexts, (("Linux", None), ("Windows", 42)))
            for runs in ([], [run(2, "Windows", app={"id": 99})],
                         [run(2, "Windows", app={"id": 42}, conclusion="failure"),
                          run(3, "Windows", app={"id": 99})]):
                with self.subTest(level=level, runs=runs), self.assertRaises(gate.GateError):
                    gate.validate_check_runs(pages(run(), *runs), contexts, COMMIT,
                                             [status(context="Windows", creator={"id": 42})])
            gate.validate_check_runs(pages(run(), run(2, "Windows", app={"id": 42})),
                                     contexts, COMMIT, [])

    def test_classic_contexts_do_not_replace_strict_rules_requirement(self):
        classic = gate.classic_required_contexts(branch("Windows"))
        for response in ([], rules()):
            with self.subTest(response=response), self.assertRaises(gate.GateError):
                gate.required_contexts(response, classic)

    def test_required_app_binding_from_rules_and_classic_protection(self):
        for source, field in ((rules("Linux"), "integration_id"),
                              ({"strict": True, "checks": [{"context": "Linux"}]}, "app_id")):
            checks = (source[0]["parameters"]["required_status_checks"]
                      if type(source) is list else source["checks"])
            for app_id in ((None, -1, 42) if field == "app_id" else (None, 42)):
                checks[0][field] = app_id
                expected = (("Linux", None if app_id == -1 else app_id),)
                self.assertEqual(gate.required_contexts(source), expected)
                gate.validate_check_runs(pages(run(app={"id": 42})), expected, COMMIT, [])
            for app_id in (True, False, 0, -2, -1.0, "42", 42.0, {}, []):
                checks[0][field] = app_id
                with self.subTest(field=field, app_id=app_id), self.assertRaises(gate.GateError):
                    gate.required_contexts(source)
            if field == "integration_id":
                checks[0][field] = -1
                with self.assertRaises(gate.GateError):
                    gate.required_contexts(source)
            del checks[0][field]
            self.assertEqual(gate.required_contexts(source), requirements("Linux"))
        for source in ({"strict": False, "checks": [{"context": "Linux", "app_id": 42}]},
                       {"strict": True, "contexts": ["Linux"]}):
            with self.assertRaises(gate.GateError):
                gate.required_contexts(source)

    def test_pinned_app_filters_before_latest_selection_across_pages(self):
        contexts = (("Linux", 42),)
        response = [{"total_count": 3, "check_runs": [run(1, app={"id": 42}, conclusion="failure")]},
                    {"total_count": 3, "check_runs": [run(2, app={"id": 42}),
                                                       run(3, app={"id": 99}, conclusion="failure")]}]
        original = copy.deepcopy(response)
        for ordered in (response, list(reversed(response))):
            gate.validate_check_runs(ordered, contexts, COMMIT, [])
        self.assertEqual(response, original)

    def test_same_name_wrong_app_success_cannot_replace_failed_or_missing_pinned_run(self):
        for runs in ((run(2, app={"id": 99}),),
                     (run(1, app={"id": 42}, conclusion="failure"), run(2, app={"id": 99})),
                     (run(1, app={"id": 42}), run(2, app={"id": 42}, conclusion="failure"),
                      run(3, app={"id": 99}))):
            with self.subTest(runs=runs), self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(*runs), (("Linux", 42),), COMMIT, [])

    def test_pinned_app_refuses_missing_malformed_and_boolean_identity(self):
        for app in (None, True, [], {}, {"id": None}, {"id": "42"}, {"id": True}, {"id": 42.0}):
            with self.subTest(app=app), self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(run(app=app)), (("Linux", 42),), COMMIT, [])
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run()), (("Linux", 42),), COMMIT, [])
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run(app={"id": True})), (("Linux", 1),), COMMIT, [])

    def test_unpinned_requirements_keep_latest_by_name_from_any_app(self):
        for app in (None, {"id": 42}, {"id": 99}):
            response = pages(run(1, app={"id": 42}, conclusion="failure"), run(2, app=app))
            gate.validate_check_runs(response, requirements("Linux"), COMMIT, [])
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run(1, app={"id": 42}),
                                          run(2, app={"id": 99}, conclusion="failure")),
                                     requirements("Linux"), COMMIT, [])

    def test_same_name_distinct_bindings_are_all_preserved(self):
        response = rules("Linux")
        checks = response[0]["parameters"]["required_status_checks"]
        checks.extend([{"context": "Linux", "integration_id": app} for app in (42, 99, 42)])
        contexts = gate.required_contexts(response)
        self.assertEqual(contexts, (("Linux", None), ("Linux", 42), ("Linux", 99)))
        gate.validate_check_runs(pages(run(1, app={"id": 42}), run(2, app={"id": 99})), contexts, COMMIT, [])
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run(app={"id": 42})), contexts, COMMIT, [])

    def test_multi_page_rules_require_checks_from_later_pages(self):
        first = [{"ruleset_id": index, "type": "required_signatures"} for index in range(1, 101)]
        later = rules("Windows")
        links = (rule_link(2, "next") + ", " + rule_link(2, "last"), rule_link(1, "prev"))
        for newline in ("\n", "\r\n"):
            original = copy.deepcopy(first)
            parsed = gate.parse_rules_pages(rule_stream(first, later, links=links, newline=newline))
            self.assertEqual(parsed, first + later)
            self.assertEqual(first, original)
            contexts = gate.required_contexts(parsed)
            self.assertEqual(contexts, requirements("Windows"))
            gate.validate_check_runs(pages(run(name="Windows")), contexts, COMMIT, [])
            with self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(run()), contexts, COMMIT, [])

    def test_rules_pagination_refuses_missing_extra_repeated_and_changed_pages(self):
        first = [{"ruleset_id": index, "type": "required_signatures"} for index in range(1, 101)]
        next_link = rule_link(2, "next") + ", " + rule_link(2, "last")
        for data in (rule_stream(first, links=(next_link,)),
                     rule_stream(rules("Linux"), links=(next_link,)),
                     rule_stream(first, first, links=(next_link, rule_link(1, "prev"))),
                     rule_stream(first, rules("Linux"), links=(next_link, "")),
                     rule_stream(first, rules("Linux"), links=(next_link, rule_link(1, "prev") + ", " + rule_link(3, "last"))),
                     rule_stream(first, links=(rule_link(3, "next"),)),
                     rule_stream(first, links=(rule_link(2, "last"),)),
                     rule_stream(rules("Linux"), rules("Windows")),
                     rule_stream(first, rules("Linux"), links=(next_link, rule_link(1, "prev").replace("/owner/", "/other/")))):
            with self.subTest(data=data[-150:]), self.assertRaises(gate.GateError):
                gate.parse_rules_pages(data)

    def test_rules_page_parser_refuses_malformed_status_headers_links_and_bodies(self):
        valid = rule_stream(rules("Linux"))
        for data in (b"", b"0", b"[]", json.dumps(rules("Linux")).encode(),
                     valid.replace(b"200 OK", b"403 Forbidden"),
                     valid.replace(b"Content-Type:", b"Content-Type"),
                     valid + b"trailing", valid[:-1],
                     rule_stream([{"ruleset_id": True, "type": "required_signatures"}]),
                     rule_stream([{"type": "required_signatures"}]),
                     valid.replace(b'"ruleset_id": 1', b'"ruleset_id": 1, "ruleset_id": 2'),
                     valid.replace(b'"ruleset_id": 1', b'"ruleset_id": NaN'),
                     rule_stream(rules("Linux"), links=("invalid",)),
                     rule_stream(rules("Linux"), links=(rule_link(2, "next") + ", " + rule_link(2, "next"),)),
                     valid.replace(b"Content-Type:", b'Link: invalid\nLink: invalid\nContent-Type:')):
            with self.subTest(data=data[:90]), self.assertRaises(gate.GateError):
                gate.parse_rules_pages(data)

    def test_rules_size_depth_and_exact_full_final_page(self):
        full = [{"ruleset_id": index, "type": "required_signatures"} for index in range(1, 101)]
        self.assertEqual(gate.parse_rules_pages(rule_stream(full)), full)
        self.assertEqual(gate.parse_rules_pages(rule_stream([])), [])
        value = rules('CI / "HTTP/2" [Linux]')
        self.assertEqual(gate.parse_rules_pages(rule_stream(value)), value)
        for data in (rule_stream(full + [full[0]]), b" " * (gate.RULES_LIMIT + 1),
                     b"HTTP/2.0 200 OK\n\n" + b"[" * (gate.DEPTH_LIMIT + 1),
                     b"HTTP/2.0 200 OK\n\n\xff"):
            with self.subTest(data=data[:80]), self.assertRaises(gate.GateError):
                gate.parse_rules_pages(data)

    def test_three_rules_pages_and_full_or_empty_terminal_pages(self):
        first = [{"ruleset_id": index, "type": "required_signatures"} for index in range(1, 101)]
        second = [{"ruleset_id": index, "type": "required_signatures"} for index in range(101, 201)]
        final = rules("Linux")
        links = (rule_link(2, "next") + ", " + rule_link(3, "last"),
                 rule_link(1, "prev") + ", " + rule_link(1, "first") + ", "
                 + rule_link(3, "next") + ", " + rule_link(3, "last"),
                 rule_link(2, "prev") + ", " + rule_link(1, "first"))
        self.assertEqual(gate.parse_rules_pages(rule_stream(first, second, final, links=links)),
                         first + second + final)
        for final in ([], second):
            data = rule_stream(first, final, links=(rule_link(2, "next"), rule_link(1, "prev")))
            self.assertEqual(gate.parse_rules_pages(data), first + final)
        first[0] = rules("Linux")[0]
        with self.assertRaises(gate.GateError):
            gate.parse_rules_pages(rule_stream(first, rules("Windows"),
                                               links=(rule_link(2, "next"), rule_link(1, "prev"))))

    def test_rules_link_validation_and_inclusive_size_bound(self):
        link = rule_link(2, "next")
        for invalid in (link.replace("https:", "http:"),
                        link.replace("api.github.com", "other.invalid"),
                        link.replace("branches/main", "branches/devel"),
                        link.replace("per_page=100", "per_page=30"),
                        link.replace("page=2>", "page=0>"),
                        link.replace("page=2>", "page=true>"),
                        link.replace("page=2>", "page=2&page=3>"),
                        link.replace("page=2>", "page=2&extra=1>"),
                        link.replace("page=2>", "page=2#fragment>"),
                        rule_link(1, "prev"), rule_link(2, "first")):
            with self.subTest(link=invalid), self.assertRaises(gate.GateError):
                gate.rules_links(invalid, 1)
        data = rule_stream(rules("Linux"))
        with mock.patch.object(gate, "RULES_LIMIT", len(data)):
            self.assertEqual(gate.parse_rules_pages(data), rules("Linux"))
            with self.assertRaises(gate.GateError):
                gate.parse_rules_pages(data + b" ")

    def test_concatenated_page_objects_and_single_array_are_equivalent(self):
        response = [{"total_count": 2, "check_runs": [run(1, conclusion="failure")]},
                    {"total_count": 2, "check_runs": [run(2)]}]
        for separator in (b"", b"\n", b" \r\n\t"):
            data = separator.join(json.dumps(page, indent=2).encode() for page in response)
            parsed = gate.parse_check_pages(b" \n" + data + b"\t ")
            self.assertEqual(parsed, response)
            gate.validate_check_runs(parsed, requirements("Linux"), COMMIT, [])
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
                gate.validate_check_runs(gate.parse_check_pages(data), requirements("Linux"), COMMIT, [])

    def test_contexts_from_all_rules_preserve_order_and_deduplicate(self):
        response = [{"type": "required_signatures"}, *rules("Windows", "Linux"),
                    *rules("Linux", "Tidy")]
        original = copy.deepcopy(response)
        self.assertEqual(gate.required_contexts(response), requirements("Windows", "Linux", "Tidy"))
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
        gate.validate_check_runs(pages(run(name=context)), contexts, COMMIT, [])
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run(name="Linux / GCC")), contexts, COMMIT, [])

    def test_paginated_latest_run_by_id_independent_of_order(self):
        response = [{"total_count": 4, "check_runs": [run(40), run(10, conclusion="failure")]},
                    {"total_count": 4, "check_runs": [run(30, "Windows"), run(20, "Other")]}]
        original = copy.deepcopy(response)
        gate.validate_check_runs(response, requirements("Linux", "Windows"), COMMIT, [])
        gate.validate_check_runs(list(reversed(response)), requirements("Windows", "Linux"), COMMIT.upper(), [])
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
                                         requirements("Linux"), COMMIT, [])

    def test_missing_context_cannot_be_replaced_by_other_success(self):
        for response in (pages(), pages(run(name="Other")), pages(run())):
            with self.subTest(response=response), self.assertRaises(gate.GateError):
                gate.validate_check_runs(response, requirements("Linux", "Windows"), COMMIT, [])

    def test_required_rerun_after_first_hundred_runs_is_checked(self):
        first_page = [run(index, f"Other {index}") for index in range(1, 101)]
        response = [{"total_count": 101, "check_runs": first_page},
                    {"total_count": 101, "check_runs": [run(101)]}]
        gate.validate_check_runs(response, requirements("Linux"), COMMIT, [])
        first_page[0] = run()
        response[1]["check_runs"][0]["conclusion"] = "failure"
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(response, requirements("Linux"), COMMIT, [])

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
                gate.validate_check_runs(response, requirements("Linux"), COMMIT, [])

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
                gate.validate_check_runs(pages(value), requirements("Linux"), COMMIT, [])
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run(), run(2, "Other", head_sha="f" * 40)),
                                     requirements("Linux"), COMMIT, [])


class StatusTests(unittest.TestCase):
    def test_unsuccessful_status_refuses_even_with_passing_check(self):
        for state in ("failure", "pending", "error"):
            for checks in (pages(), pages(run())):
                with self.subTest(state=state, checks=checks), self.assertRaises(gate.GateError):
                    gate.validate_check_runs(checks, requirements("Linux"), COMMIT,
                                             [status(state=state)])

    def test_passing_status_only_satisfies_unpinned_context(self):
        gate.validate_check_runs(pages(), requirements("Linux"), COMMIT, [status()])
        gate.validate_check_runs(pages(run()), requirements("Linux"), COMMIT, [status()])
        for statuses in ([], [status(context="Other")], [status(context="linux")]):
            with self.subTest(statuses=statuses), self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(), requirements("Linux"), COMMIT, statuses)

    def test_successful_status_cannot_replace_failed_or_pending_check(self):
        for fields in ({"conclusion": "failure"}, {"conclusion": "neutral"},
                       {"status": "in_progress", "conclusion": None}):
            with self.subTest(fields=fields), self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(run(**fields)), requirements("Linux"), COMMIT,
                                         [status()])

    def test_latest_status_by_id_supersedes_older_result_independent_of_order(self):
        for newer_state in ("success", "failure", "pending"):
            statuses = [status(1, state="failure"), status(2, state=newer_state),
                        status(3, context="Windows"), status(4, context="Other", state="failure")]
            original = copy.deepcopy(statuses)
            for ordered in (statuses, list(reversed(statuses)), statuses[2:] + statuses[:2]):
                with self.subTest(state=newer_state, ids=[item["id"] for item in ordered]):
                    if newer_state == "success":
                        gate.validate_check_runs(pages(), requirements("Linux", "Windows"), COMMIT, ordered)
                    else:
                        with self.assertRaises(gate.GateError):
                            gate.validate_check_runs(pages(run(), run(2, "Windows")),
                                                     requirements("Linux", "Windows"), COMMIT, ordered)
            self.assertEqual(statuses, original)

    def test_status_creator_cannot_satisfy_pinned_app_binding(self):
        for source, field in ((rules("Linux"), "integration_id"),
                              ({"strict": True, "checks": [{"context": "Linux"}]}, "app_id")):
            checks = (source[0]["parameters"]["required_status_checks"]
                      if type(source) is list else source["checks"])
            checks[0][field] = 42
            contexts = gate.required_contexts(source)
            for creator in (None, {}, {"id": 42}, {"id": 99}):
                statuses = [status(creator=creator)]
                for response in (pages(), pages(run(app={"id": 99}))):
                    with self.subTest(field=field, creator=creator), self.assertRaises(gate.GateError):
                        gate.validate_check_runs(response, contexts, COMMIT, statuses)
                gate.validate_check_runs(pages(run(app={"id": 42})), contexts, COMMIT, statuses)

    def test_latest_failing_status_from_other_creator_blocks_pinned_success(self):
        response = pages(run(app={"id": 42}))
        contexts = (("Linux", 42),)
        for state in ("failure", "pending", "error"):
            for ordered in ([status(1, creator={"id": 42}), status(2, state=state)],
                            [status(2, state=state), status(1, creator={"id": 42})]):
                with self.subTest(state=state), self.assertRaises(gate.GateError):
                    gate.validate_check_runs(response, contexts, COMMIT, ordered)

    def test_status_does_not_replace_any_of_multiple_app_bindings(self):
        contexts = (("Linux", None), ("Linux", 42), ("Linux", 99))
        gate.validate_check_runs(pages(run(1, app={"id": 42}), run(2, app={"id": 99})),
                                 contexts, COMMIT, [status()])
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run(app={"id": 42})), contexts, COMMIT, [status()])

    def test_status_fields_and_duplicate_id_validation(self):
        invalid = [None, {}, True, [None], [status(), status()]]
        for field, values in (("id", (None, True, "1", 0, -1, 1.0)),
                              ("context", (None, True, "", " \n")),
                              ("state", (None, True, "SUCCESS", "", [], {}))):
            invalid.extend([dict(status(), **{field: value})] for value in values)
            value = status()
            del value[field]
            invalid.append([value])
        for statuses in invalid:
            with self.subTest(statuses=statuses), self.assertRaises(gate.GateError):
                gate.validate_check_runs(pages(run()), requirements("Linux"), COMMIT, statuses)

    def test_complete_status_pages_and_result_selection(self):
        first = [status(index, context=f"Other {index}") for index in range(1, 101)]
        first[0] = status(1, state="failure")
        later = [status(101)]
        links = (status_link(2, "next") + ", " + status_link(2, "last"), status_link(1, "prev"))
        for newline in ("\n", "\r\n"):
            parsed = gate.parse_status_pages(rule_stream(first, later, links=links, newline=newline), COMMIT)
            self.assertEqual(parsed, first + later)
            for ordered in (parsed, list(reversed(parsed))):
                gate.validate_check_runs(pages(), requirements("Linux"), COMMIT, ordered)
        later[0]["state"] = "failure"
        parsed = gate.parse_status_pages(rule_stream(first, later, links=links), COMMIT)
        with self.assertRaises(gate.GateError):
            gate.validate_check_runs(pages(run()), requirements("Linux"), COMMIT, parsed)

    def test_incomplete_extra_changed_and_duplicate_status_pages_refuse(self):
        first = [status(index, context=f"Other {index}") for index in range(1, 101)]
        next_link = status_link(2, "next") + ", " + status_link(2, "last")
        for data in (rule_stream(first, links=(next_link,)),
                     rule_stream([status()], links=(next_link,)),
                     rule_stream(first, [status(101)], links=(next_link, "")),
                     rule_stream(first, [status(101)], links=(next_link, status_link(1, "prev") + ", " + status_link(3, "last"))),
                     rule_stream(first, links=(status_link(3, "next"),)),
                     rule_stream(first, links=(status_link(2, "last"),)),
                     rule_stream([status()], [status(2)]),
                     rule_stream(first, [status(101)], links=(next_link, status_link(1, "prev").replace("/owner/", "/other/"))),
                     rule_stream(first, first, links=(next_link, status_link(1, "prev")))):
            with self.subTest(data=data[-150:]), self.assertRaises(gate.GateError):
                parsed = gate.parse_status_pages(data, COMMIT)
                gate.validate_check_runs(pages(run()), requirements("Linux"), COMMIT, parsed)

    def test_status_parser_refuses_api_errors_and_malformed_headers_or_json(self):
        valid = rule_stream([status()])
        for data in (b"", b"[]", json.dumps([status()]).encode(), valid[:-1], valid + b"trailing",
                     valid.replace(b"200 OK", b"403 Forbidden"),
                     valid.replace(b"200 OK", b"500 Internal Server Error"),
                     valid.replace(b"Content-Type:", b"Content-Type"),
                     valid.replace(b'"id": 1', b'"id": 1, "id": 2', 1),
                     valid.replace(b'"id": 1', b'"id": NaN', 1),
                     valid.replace(b"Content-Type:", b"Link: invalid\nLink: invalid\nContent-Type:"),
                     rule_stream([status()], links=("invalid",))):
            with self.subTest(data=data[:100]), self.assertRaises(gate.GateError):
                gate.parse_status_pages(data, COMMIT)

    def test_status_links_must_reference_tagged_commit_and_expected_endpoint(self):
        first = [status(index) for index in range(1, 101)]
        link = status_link(2, "next")
        for invalid in (link.replace(COMMIT, "f" * 40), link.replace("/statuses", "/status"),
                        link.replace("https:", "http:"), link.replace("api.github.com", "other.invalid"),
                        link.replace("per_page=100", "per_page=30"),
                        link.replace("page=2>", "page=2&page=3>"),
                        link.replace("page=2>", "page=2&extra=1>"),
                        link.replace("page=2>", "page=2#fragment>"),
                        status_link(2, "first"), status_link(1, "prev")):
            with self.subTest(link=invalid), self.assertRaises(gate.GateError):
                gate.parse_status_pages(rule_stream(first, links=(invalid,)), COMMIT)

    def test_status_size_depth_and_full_or_empty_terminal_page(self):
        full = [status(index) for index in range(1, 101)]
        for page in (full, [], [status(context='CI / "HTTP/2" [Linux]')]):
            self.assertEqual(gate.parse_status_pages(rule_stream(page), COMMIT), page)
        for data in (rule_stream(full + [status(101)]),
                     b"HTTP/2.0 200 OK\n\n" + b"[" * (gate.DEPTH_LIMIT + 1),
                     b"HTTP/2.0 200 OK\n\n\xff"):
            with self.subTest(data=data[:80]), self.assertRaises(gate.GateError):
                gate.parse_status_pages(data, COMMIT)
        data = rule_stream([status()])
        with mock.patch.object(gate, "CHECKS_LIMIT", len(data)):
            self.assertEqual(gate.parse_status_pages(data, COMMIT), [status()])
            with self.assertRaises(gate.GateError):
                gate.parse_status_pages(data + b" ", COMMIT)


class GitCommandTests(unittest.TestCase):
    def test_git_errors_include_bounded_stderr_with_replacement_decoding(self):
        for stderr in (b"fatal: broken configuration\n", b"fatal: \xff\n",
                       b"x" * 4096, b"x" * 4096 + b"hidden suffix", b""):
            result = subprocess.CompletedProcess([], 128, stdout=b"", stderr=stderr)
            with self.subTest(stderr=stderr[:40]), mock.patch.object(
                    gate.subprocess, "run", return_value=result):
                with self.assertRaises(gate.GateError) as caught:
                    gate.git(ROOT, "rev-parse", "--verify", "HEAD^0")
                message = str(caught.exception)
                self.assertIn("Git prerequisite failed: rev-parse --verify HEAD^0", message)
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
                     "GIT_OBJECT_DIRECTORY": "other", "GIT_REPLACE_REF_BASE": "refs/other/"}
        result = subprocess.CompletedProcess([], 0, stdout=b"result\n", stderr=b"warning")
        with mock.patch.dict(os.environ, overrides):
            before = dict(os.environ)
            with mock.patch.object(gate.subprocess, "run", return_value=result) as command:
                self.assertEqual(gate.git(ROOT, "rev-parse", "--verify", "HEAD^0"), b"result\n")
            self.assertEqual(dict(os.environ), before)
        child = command.call_args.kwargs["env"]
        self.assertEqual({key: value for key, value in child.items() if key.startswith("GIT_")},
                         {"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull})
        self.assertEqual(command.call_args.args[0],
                         ["git", "--no-replace-objects", "-C", str(ROOT),
                          "rev-parse", "--verify", "HEAD^0"])


class GitTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.statuses_path = self.root / "statuses.json"
        self.statuses_path.write_bytes(rule_stream([]))
        self.branch_path = self.root / "branch.json"
        self.branch_path.write_text(json.dumps(branch(enabled=False, level="off")), encoding="utf-8")
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

    def test_git_arguments_survive_msys_brace_expansion(self):
        # MSYS2 runtimes brace-expand argv, so validator Git arguments must contain no braces.
        with mock.patch.object(gate.subprocess, "run", wraps=subprocess.run) as command:
            self.validate()
        self.assertTrue(command.call_args_list)
        for call in command.call_args_list:
            for argument in call.args[0]:
                self.assertNotRegex(argument, r"[{}]")

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
            result = subprocess.run(["git", "-C", str(self.root), "rev-parse", "HEAD^0"],
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
            rules_path.write_bytes(rule_stream(rules("Linux")))
            result = subprocess.run([
                sys.executable, "-I", "-B", str(script), "--version", VERSION,
                "--tag-commit", self.tag, "--rules", str(rules_path), "--statuses", str(self.statuses_path),
                "--branch", str(self.branch_path),
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
        rules_path.write_bytes(rule_stream(rules("Linux")))
        checks_path.write_text(json.dumps({"total_count": 2, "check_runs": [
            run(1, head_sha=self.tag, conclusion="failure")
        ]}) + "\n" + json.dumps({"total_count": 2, "check_runs": [
            run(2, head_sha=self.tag)
        ]}), encoding="utf-8")
        args = [sys.executable, "-I", "-B", str(Path(gate.__file__)), "--repo-root", str(self.root),
                "--branch", str(self.branch_path),
                "--version", VERSION, "--tag-commit", self.tag, "--rules", str(rules_path), "--statuses", str(self.statuses_path)]
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

    def test_cli_pinned_app_and_later_rules_page_with_read_only_refusals(self):
        rules_path = self.root / "rules.json"
        first = [{"ruleset_id": index, "type": "required_signatures"} for index in range(1, 101)]
        later = rules("Windows")
        later[0]["parameters"]["required_status_checks"][0]["integration_id"] = 42
        next_link = rule_link(2, "next") + ", " + rule_link(2, "last")
        complete = rule_stream(first, later, links=(next_link, rule_link(1, "prev")))
        args = [sys.executable, "-I", "-B", str(Path(gate.__file__)), "--repo-root", str(self.root),
                "--branch", str(self.branch_path),
                "--version", VERSION, "--tag-commit", self.tag, "--rules", str(rules_path), "--statuses", str(self.statuses_path)]
        before = self.git("show-ref")
        for rules_data, runs, expected in (
                (complete, [run(name="Windows", head_sha=self.tag, app={"id": 42})], 0),
                (complete, [run(name="Linux", head_sha=self.tag, app={"id": 42})], 1),
                (complete, [run(name="Windows", head_sha=self.tag, app={"id": 99})], 1),
                (complete, [run(name="Windows", head_sha=self.tag, app={"id": 42}, conclusion="failure"),
                            run(2, name="Windows", head_sha=self.tag, app={"id": 99})], 1),
                (rule_stream(first, links=(next_link,)), [run(head_sha=self.tag)], 1)):
            rules_path.write_bytes(rules_data)
            result = subprocess.run(args, input=json.dumps(pages(*runs)).encode(),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, check=False)
            with self.subTest(expected=expected, runs=runs):
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertIn(b"PASS" if expected == 0 else b"FAIL",
                              result.stdout if expected == 0 else result.stderr)
                self.assertEqual(self.git("show-ref"), before)
                self.assertFalse((self.root / ".git/index").exists())

    def test_cli_status_results_and_incomplete_inputs_are_read_only(self):
        rules_path = self.root / "rules.json"
        rules_path.write_bytes(rule_stream(rules("Linux")))
        args = [sys.executable, "-I", "-B", str(Path(gate.__file__)), "--repo-root", str(self.root),
                "--branch", str(self.branch_path),
                "--version", VERSION, "--tag-commit", self.tag, "--rules", str(rules_path),
                "--statuses", str(self.statuses_path)]
        first = [status(index, context=f"Other {index}") for index in range(1, 101)]
        next_link = status_link(2, "next", self.tag) + ", " + status_link(2, "last", self.tag)
        incomplete = rule_stream(first, links=(next_link,))
        complete = rule_stream(first, [status(101, state="failure")],
                               links=(next_link, status_link(1, "prev", self.tag)))
        before = self.git("show-ref")
        for statuses_data, runs, expected in (
                (rule_stream([status(state="failure")]), [run(head_sha=self.tag)], 1),
                (rule_stream([status()]), [], 0),
                (rule_stream([status(state="pending")]), [run(head_sha=self.tag)], 1),
                (rule_stream([status(1, state="failure"), status(2)]), [run(head_sha=self.tag)], 0),
                (rule_stream([status(2), status(1, state="failure")]), [run(head_sha=self.tag)], 0),
                (rule_stream([status()]), [run(head_sha=self.tag, conclusion="failure")], 1),
                (incomplete, [run(head_sha=self.tag)], 1),
                (complete, [run(head_sha=self.tag)], 1),
                (b"[]", [run(head_sha=self.tag)], 1),
                (rule_stream([]).replace(b"200 OK", b"403 Forbidden"), [run(head_sha=self.tag)], 1)):
            self.statuses_path.write_bytes(statuses_data)
            result = subprocess.run(args, input=json.dumps(pages(*runs)).encode(),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, check=False)
            with self.subTest(statuses=statuses_data[-150:], runs=runs):
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertIn(b"PASS" if expected == 0 else b"FAIL",
                              result.stdout if expected == 0 else result.stderr)
                self.assertEqual(self.git("show-ref"), before)
                self.assertFalse((self.root / ".git/index").exists())
        for statuses_input in ("-", str(self.root / "missing-statuses.json")):
            result = subprocess.run([*args, "--statuses", statuses_input],
                                    input=json.dumps(pages(run(head_sha=self.tag))).encode(),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"FAIL", result.stderr)
        result = subprocess.run(args[:-2], input=json.dumps(pages(run(head_sha=self.tag))).encode(),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"--statuses", result.stderr)
        self.assertEqual(self.git("show-ref"), before)
        self.assertFalse((self.root / ".git/index").exists())

    def test_cli_classic_protection_merge_bindings_and_malformed_inputs(self):
        rules_path = self.root / "rules.json"
        rules_path.write_bytes(rule_stream(rules("Linux")))
        args = [sys.executable, "-I", "-B", str(Path(gate.__file__)), "--repo-root", str(self.root),
                "--version", VERSION, "--tag-commit", self.tag, "--rules", str(rules_path),
                "--statuses", str(self.statuses_path)]
        bound = branch("Windows", checks=[{"context": "Windows", "app_id": 42}])
        before = self.git("show-ref")
        for response, extra_runs, expected in (
                (branch(enabled=False, level="off"), [], 0),
                (branch("Windows", enabled=False), [], 0),
                (branch("Windows", level="off"), [], 0),
                (branch("Windows"), [], 1),
                (branch("Windows"), [run(2, "Windows")], 0),
                (bound, [run(2, "Windows", app={"id": 99})], 1),
                (bound, [run(2, "Windows", app={"id": 42})], 0),
                (None, [], 1), ({"protected": True}, [], 1),
                ({"protected": True, "protection": {"enabled": False}}, [], 1)):
            self.branch_path.write_text(json.dumps(response), encoding="utf-8")
            result = subprocess.run([*args, "--branch", str(self.branch_path)],
                                    input=json.dumps(pages(run(head_sha=self.tag), *[
                                        dict(item, head_sha=self.tag) for item in extra_runs])).encode(),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, check=False)
            with self.subTest(response=response, runs=extra_runs):
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertIn(b"PASS" if expected == 0 else b"FAIL",
                              result.stdout if expected == 0 else result.stderr)
        for data in (b"", b"{", b'{"protected":false,"protected":false}',
                     b" " * (gate.RULES_LIMIT + 1)):
            self.branch_path.write_bytes(data)
            result = subprocess.run([*args, "--branch", str(self.branch_path)],
                                    input=json.dumps(pages(run(head_sha=self.tag))).encode(),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, check=False)
            with self.subTest(data=data[:50]):
                self.assertEqual(result.returncode, 1)
                self.assertIn(b"FAIL", result.stderr)
        for branch_input in ("-", str(self.root / "missing-branch.json")):
            result = subprocess.run([*args, "--branch", branch_input], input=b"",
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"FAIL", result.stderr)
        result = subprocess.run(args, input=b"", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env=self.env, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"--branch", result.stderr)
        self.assertEqual(self.git("show-ref"), before)
        self.assertFalse((self.root / ".git/index").exists())

    def test_committed_native_evidence_refusals_are_read_only(self):
        for index in range(1, len(EXPECTED_IDS)):
            for evidence in ("issue #123", f"{'f' * 40} https://github.com/owner/repository/actions/runs/123"):
                value = record(self.base)
                value["gates"][index]["evidence"] = evidence
                candidate = self.candidate(value)
                self.promote(candidate)
                before = self.git("show-ref")
                with self.subTest(index=index, evidence=evidence), self.assertRaises(gate.GateError):
                    self.validate(candidate)
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
                     'gh api --include --paginate "repos/${GITHUB_REPOSITORY}/rules/branches/main?per_page=100"',
                     'gh api "repos/${GITHUB_REPOSITORY}/branches/main" > "${RUNNER_TEMP}/release-branch.json"',
                     'gh api --include --paginate "repos/${GITHUB_REPOSITORY}/commits/${tag_commit}/statuses?per_page=100" > "${RUNNER_TEMP}/release-statuses.json"',
                     'gh api --paginate "repos/${GITHUB_REPOSITORY}/commits/${tag_commit}/check-runs?filter=all&per_page=100" |',
                     "python3 -I -B tools/release/validate_release_gates.py",
                     '--version "$RELEASE_VERSION" --tag-commit "$tag_commit"',
                     '--rules "${RUNNER_TEMP}/release-rules.json"',
                     '--branch "${RUNNER_TEMP}/release-branch.json"',
                     '--statuses "${RUNNER_TEMP}/release-statuses.json"'):
            self.assertIn(text, step)
        self.assertNotIn("--slurp", step)
        self.assertIn("fetch-depth: 0", workflow[:verification])

    def test_every_release_create_path_is_draft_and_workflow_never_publishes(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        commands = [shlex.split(line.strip()) for line in workflow.splitlines()
                    if "gh release create " in line]
        self.assertEqual(len(commands), 2)
        for command in commands:
            with self.subTest(command=command):
                self.assertEqual(command[:3], ["gh", "release", "create"])
                self.assertIn("--draft", command)
                self.assertIn("--verify-tag", command)
                self.assertNotIn("--draft=false", command)
        self.assertIn("--notes-file", commands[0])
        self.assertIn("--generate-notes", commands[1])
        self.assertNotRegex(workflow, r"--draft(?:=|\s+)false\b")
        self.assertNotRegex(workflow, r"--latest\b")
        self.assertNotRegex(workflow, r"gh\s+release\s+edit\b")
        self.assertNotRegex(workflow, r"gh\s+api[^\n]*(?:/releases|draft.*false)")
        release_commands = re.findall(r"gh\s+release\s+(\S+)", workflow)
        self.assertEqual(release_commands, ["create", "create", "upload"])

    def test_artifacts_and_attestations_support_draft_until_owner_publication(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        source, binaries = workflow.split("\n  binary-linux:\n", 1)
        creation = source.index("gh release create ")
        for action in ("actions/attest-build-provenance@", "actions/attest-sbom@"):
            self.assertLess(source.index(action), creation)
            self.assertEqual(binaries.count(action), 2)
        upload = workflow.split("\n  publish-binaries:\n", 1)[1]
        self.assertIn("needs: [source, binary-linux, binary-windows]", upload)
        self.assertIn("    permissions:\n      contents: write\n", upload)
        upload_step = upload.split("      - name: Attach binary packages\n", 1)[1]
        self.assertIn("        env:\n          GH_TOKEN: ${{ github.token }}\n"
                      "          GH_REPO: ${{ github.repository }}\n", upload_step)
        self.assertIn('run: gh release upload "${GITHUB_REF_NAME}" dist/* --clobber', upload)
        self.assertNotIn("verify_release.py", workflow)

    def test_source_job_has_only_required_permissions_including_checks_read(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn("\npermissions:\n  contents: read\n", workflow)
        source = workflow.split("\n  source:\n", 1)[1].split("\n  binary-linux:\n", 1)[0]
        permissions = source.split("    permissions:\n", 1)[1].split("    steps:\n", 1)[0]
        self.assertEqual(permissions, "      contents: write\n      checks: read\n"
                         "      statuses: read\n"
                         "      id-token: write\n      attestations: write\n")

    def test_documented_ids_match_validator_and_acceptance_link(self):
        documentation = (ROOT / "tools/release/README.md").read_text(encoding="utf-8")
        positions = [documentation.index(f'"id": "{name}"') for name in EXPECTED_IDS]
        self.assertEqual(positions, sorted(positions))
        acceptance = (ROOT / "docs/v0.3-acceptance.md").read_text(encoding="utf-8")
        self.assertIn('`tools/release/README.md` (section "Publication gate record")', acceptance)
        self.assertIn("## Publication gate record", documentation)
        self.assertNotIn('"id": "independent-release-verification"', documentation)
        self.assertNotIn("- independent release verification;", acceptance)
        self.assertIn("six pre-publication gates", documentation)
        self.assertIn("post-publication", documentation)
        self.assertIn("post-publication", acceptance)
        self.assertIn("gh api --include --paginate", documentation)
        self.assertIn("qualified_commit", documentation)
        self.assertIn("concrete record URL", documentation)
        self.assertIn("local paths", documentation)
        self.assertNotIn("docs/release-evidence/", documentation)
        for path in ("tools/release/README.md", "docs/v0.3-acceptance.md", "README.md"):
            text = (ROOT / path).read_text(encoding="utf-8")
            with self.subTest(path=path):
                self.assertIn("--branch", text)
                self.assertIn("classic", text)
                self.assertIn("draft", text)
                self.assertIn("owner", text)
                self.assertIn("evidence", text)
                self.assertIn("manually", text)
                self.assertIn("post-publication", text)


if __name__ == "__main__":
    unittest.main()
