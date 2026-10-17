#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import os
import re
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
from functools import lru_cache
from pathlib import Path
from typing import Any


MODULE_PATH = Path(__file__).with_name("check_acceptance_matrix.py")
SPEC = importlib.util.spec_from_file_location("check_acceptance_matrix", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

PUBLIC_SPEC = importlib.util.spec_from_file_location(
    "acceptance_public_tree", MODULE_PATH.with_name("validate_public_tree.py"))
assert PUBLIC_SPEC and PUBLIC_SPEC.loader
PUBLIC_TREE = importlib.util.module_from_spec(PUBLIC_SPEC)
sys.modules[PUBLIC_SPEC.name] = PUBLIC_TREE
PUBLIC_SPEC.loader.exec_module(PUBLIC_TREE)


@lru_cache(maxsize=1)
def repository_index() -> Any:
    # Repository sources are read-only throughout this suite; the index is frozen.
    return MODULE.index_tests(MODULE.DEFAULT_ROOT)


@contextlib.contextmanager
def immutable_repository_reads() -> Any:
    # These mutation sweeps alter only in-memory JSON. Cache immutable filesystem
    # answers (at most 256 per operation) for the sweep, then discard them before
    # any other test runs.
    with contextlib.ExitStack() as stack:
        for name in ("_read_text", "_path_problem", "load_required_ids"):
            cached = lru_cache(maxsize=256)(getattr(MODULE, name))
            stack.enter_context(mock.patch.object(MODULE, name, cached))
        yield


def document_read_count(read: Any, document: Path) -> int:
    identity = document.resolve()
    return sum(Path(call.args[0]).resolve() == identity for call in read.call_args_list)


@lru_cache(maxsize=1)
def repository_paths() -> tuple[str, ...]:
    # All partitions use the same immutable discovery, including untracked files.
    return tuple(PUBLIC_TREE.tracked_paths(MODULE.DEFAULT_ROOT))


TEST_CMAKE = """add_test(NAME AlphaContract COMMAND alpha)
add_test(
    NAME
    BetaMultiLine
    COMMAND beta)
"""

TEST_CPP = """#include <gtest/gtest.h>

TEST(Alpha, One) {}
TEST_F(Fixture, Two) {}
TEST_P(
    Param,
    Three) {}
TEST(
    Multi,
    Line) {}
"""

SOURCE = "docs/d.md#section"
NATIVE_COMMAND = "pwsh tools/run.ps1 -Out <dir>"
RECEIPT = "luminumbra.render_qualification.v1"


def baseline_matrix() -> dict[str, Any]:
    return {
        "schema": "luminumbra.acceptance_matrix.v2",
        "description": "synthetic matrix",
        "rows": [
            {
                "id": "alpha.covered",
                "source": SOURCE,
                "statement": "Covered by ctest and script.",
                "status": "covered",
                "requirements": ["dw.synthetic", "ar.synthetic", "dw.mapping.2", "dw.mapping.3",
                                 "dw.mapping.4", "dw.mapping.5"],
                "evidence": [
                    {"kind": "ctest", "ref": "AlphaContract"},
                    {"kind": "ctest", "ref": "Param.*"},
                    {"kind": "script", "ref": "tools/script.py"},
                ],
            },
            {
                "id": "gtest.partial",
                "source": SOURCE,
                "statement": "Partly covered.",
                "status": "partial",
                "evidence": [
                    {"kind": "ctest", "ref": "Alpha.One"},
                    {
                        "kind": "script",
                        "ref": "tools/later.py",
                        "planned": "traversal runner; owner: fw",
                    },
                ],
            },
            {
                "id": "native.run",
                "source": SOURCE,
                "statement": "Needs a native run.",
                "status": "needs-native-run",
                "evidence": [
                    {
                        "kind": "native",
                        "ref": "tools/run.ps1",
                        "command": NATIVE_COMMAND,
                        "receipt_schema": RECEIPT,
                    },
                    {"kind": "doc", "ref": "docs/d.md"},
                ],
            },
            {
                "id": "planned.doc",
                "source": SOURCE,
                "statement": "Not landed yet.",
                "status": "planned",
                "evidence": [
                    {
                        "kind": "doc",
                        "ref": "docs/future.md",
                        "planned": "traversal v2; owner: ac; issue #132",
                    },
                ],
            },
        ],
    }


def row_of(matrix: dict[str, Any], row_id: str) -> dict[str, Any]:
    for row in matrix["rows"]:
        if row["id"] == row_id:
            return row
    raise KeyError(row_id)


class AcceptanceMatrixTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.root = self.tmp / "repo"
        (self.root / "test").mkdir(parents=True)
        (self.root / "tools").mkdir()
        (self.root / "docs").mkdir()
        (self.root / "CMakeLists.txt").write_text(
            "add_test(NAME RootLevelContract COMMAND root)\n", encoding="utf-8"
        )
        (self.root / "test" / "CMakeLists.txt").write_text(TEST_CMAKE, encoding="utf-8")
        (self.root / "test" / "x_test.cpp").write_text(TEST_CPP, encoding="utf-8")
        (self.root / "tools" / "script.py").write_text("print('ok')\n", encoding="utf-8")
        (self.root / "tools" / "run.ps1").write_text("exit 0\n", encoding="utf-8")
        (self.root / "docs" / "d.md").write_text("# Doc\n\n## Section\n", encoding="utf-8")
        (self.root / "tools" / "ci").mkdir()
        requirements = []
        for prefix, path in zip(("dw", "ar"), MODULE.CONTRACT_DOCUMENTS):
            text = "Every requirement has evidence."
            (self.root / path).write_text("# Contract\n\n" + text + "\n", encoding="utf-8")
            requirements.append({"id": prefix + ".synthetic", "source": path + "#contract",
                                 "quote": text, "text": text})
        self.inventory = {"schema": MODULE.INVENTORY_SCHEMA,
                          "documents": list(MODULE.CONTRACT_DOCUMENTS),
                          "requirements": requirements, "exclusions": []}
        self.write_inventory()
        source = self.root / MODULE.REQUIREMENT_SOURCE
        source.write_text(
            source.read_text(encoding="utf-8") + "\n" + MODULE.REQUIREMENT_HEADING + "\n\n"
            + "\n".join(f"- `{row['id']}`: Synthetic requirement." for row in baseline_matrix()["rows"])
            + "\n\n## Deferred\n", encoding="utf-8",
        )
        for source_ref, text, _ in MODULE.contract_statements(
            source.read_text(encoding="utf-8"), MODULE.REQUIREMENT_SOURCE.as_posix()
        ):
            if source_ref.endswith("#acceptance-requirement-ids"):
                self.inventory["requirements"].append({
                    "id": "dw.mapping." + str(len(self.inventory["requirements"])),
                    "source": source_ref, "text": text, "quote": text,
                })
        self.write_inventory()
        self.index = MODULE.index_tests(self.root)
        self.matrix_path = self.tmp / "matrix.json"

    def write_inventory(self) -> None:
        (self.root / "tools/ci/acceptance_requirements.json").write_text(
            json.dumps(self.inventory), encoding="utf-8")

    def errors(self, matrix: dict[str, Any], require_landed: bool = False) -> list[str]:
        return MODULE.validate(matrix, self.index, self.root, require_landed)

    def assertErrorContains(self, matrix: dict[str, Any], fragment: str) -> None:
        errors = self.errors(matrix)
        self.assertTrue(any(fragment in error for error in errors), errors)

    def write_matrix(self, matrix: Any) -> None:
        if isinstance(matrix, str):
            self.matrix_path.write_text(matrix, encoding="utf-8")
        else:
            self.matrix_path.write_text(json.dumps(matrix, indent=2), encoding="utf-8")

    def run_main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = MODULE.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def base_args(self) -> list[str]:
        return ["--root", str(self.root), "--matrix", str(self.matrix_path)]

    def test_baseline_is_valid_and_covers_all_statuses(self) -> None:
        matrix = baseline_matrix()
        self.assertEqual(self.errors(matrix), [])
        self.assertEqual(
            {row["status"] for row in matrix["rows"]},
            {"covered", "partial", "needs-native-run", "planned"},
        )

    def test_duplicate_id_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][1]["id"] = "alpha.covered"
        self.assertErrorContains(matrix, "alpha.covered: duplicate id")

    def test_bad_id_pattern_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["id"] = "Bad ID"
        self.assertErrorContains(matrix, "id does not match")

    def test_unknown_status_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["status"] = "bogus"
        self.assertErrorContains(matrix, "unknown status: bogus")

    def test_empty_or_missing_evidence_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"] = []
        self.assertErrorContains(matrix, "evidence must be a non-empty list")
        matrix = baseline_matrix()
        del matrix["rows"][0]["evidence"]
        self.assertErrorContains(matrix, "evidence must be a non-empty list")

    def test_unknown_kind_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"][2]["kind"] = "manual"
        self.assertErrorContains(matrix, "unknown evidence kind: manual")

    def test_missing_ctest_name_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"][0]["ref"] = "NoSuchTest"
        self.assertErrorContains(matrix, "ctest not found: NoSuchTest")
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"][1]["ref"] = "Alpha.Missing"
        self.assertErrorContains(matrix, "ctest not found: Alpha.Missing")

    def test_wildcard_suite_hit_and_miss(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"][1]["ref"] = "Alpha.*"
        self.assertEqual(self.errors(matrix), [])
        matrix["rows"][0]["evidence"][1]["ref"] = "Nope.*"
        self.assertErrorContains(matrix, "ctest not found: Nope.*")

    def test_missing_script_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"][2]["ref"] = "tools/absent.py"
        self.assertErrorContains(matrix, "file not found: tools/absent.py")

    def test_missing_requirement_row_is_rejected(self) -> None:
        matrix = baseline_matrix()
        del matrix["rows"][0]["requirements"]
        self.assertErrorContains(matrix, "requirement lacks a matrix row: dw.synthetic")

    def test_source_paths_anchors_and_issue_syntax_are_checked_for_all_statuses(self) -> None:
        for source, error in (("docs/missing.md#section", "source file not found"),
                              ("docs/d.md#missing", "source anchor not found"),
                              ("docs/d.md#", "source anchor not found"),
                              ("../escape.md#section", "source path escapes root"),
                              ("issue #0", "invalid issue source"),
                              ("issue #126 trailing", "invalid issue source")):
            for row_id in ("alpha.covered", "planned.doc"):
                with self.subTest(source=source, row=row_id):
                    matrix = baseline_matrix()
                    row_of(matrix, row_id)["source"] = source
                    self.assertErrorContains(matrix, error)
        matrix = baseline_matrix()
        matrix["rows"][0]["source"] = "issue #126"
        self.assertEqual(self.errors(matrix), [])

    def test_github_heading_punctuation_unicode_duplicates_and_fences(self) -> None:
        doc = self.root / "docs/d.md"
        doc.write_text("# Section\n## `Calendar` & Café (v1)\n## Section\n"
                       "```\n## Ghost\n```\n", encoding="utf-8")
        for anchor in ("section", "calendar--café-v1", "section-1"):
            matrix = baseline_matrix()
            matrix["rows"][0]["source"] = "docs/d.md#" + anchor
            self.assertEqual(self.errors(matrix), [])
        matrix["rows"][0]["source"] = "docs/d.md#ghost"
        self.assertErrorContains(matrix, "source anchor not found")

    def test_heading_parses_are_shared_without_stale_results_or_mutable_aliases(self) -> None:
        text = "# Shared source headings\n## Repeated\n## Repeated\n"
        with mock.patch.object(MODULE, "heading_slug", wraps=MODULE.heading_slug) as slug:
            for _ in range(10):
                headings = MODULE._headings(text)
                self.assertEqual(headings, {0: "shared-source-headings", 1: "repeated", 2: "repeated-1"})
                headings.clear()
            self.assertEqual(slug.call_count, 3)
            self.assertEqual(MODULE._headings(text + "## Added\n")[3], "added")
            self.assertEqual(slug.call_count, 7)

    def test_sources_share_document_reads_only_within_one_validation(self) -> None:
        doc = self.root / "docs/d.md"
        doc.write_text("# First\n## Second\n## Third\n## Fourth\n", encoding="utf-8")
        matrix = baseline_matrix()
        for row, anchor in zip(matrix["rows"], ("first", "second", "third", "fourth")):
            row["source"] = "docs/d.md#" + anchor
        with mock.patch.object(MODULE, "_read_text", wraps=MODULE._read_text) as read:
            self.assertEqual(self.errors(matrix), [])
            self.assertEqual(document_read_count(read, doc), 1)
        doc.write_text("# Changed\n", encoding="utf-8")
        self.assertErrorContains(matrix, "source anchor not found: docs/d.md#first")
        doc.unlink()
        self.assertErrorContains(matrix, "source file not found: docs/d.md")

    def test_document_read_counts_accept_platform_root_spellings(self) -> None:
        root = self.root.resolve()
        resolve = Path.resolve
        # Model the resolved-root boundary on any host, without needing a macOS
        # temporary-directory alias or Windows directory-link privileges.
        spellings = (
            (Path("/var/folders/fixture/repo"), Path("/private/var/folders/fixture/repo")),
            (Path(r"c:\FIXTUR~1\repo"), Path("C:/Fixture Directory/repo")),
        )
        for original, canonical in spellings:
            with self.subTest(original=original, canonical=canonical):
                aliases = {alias / suffix: root / suffix
                           for alias in (original, canonical)
                           for suffix in (Path("."), Path("docs/d.md"))}

                def resolve_alias(path: Path, *args: Any, **kwargs: Any) -> Path:
                    return resolve(aliases.get(path, path), *args, **kwargs)

                with mock.patch.object(Path, "resolve", resolve_alias), \
                        mock.patch.object(MODULE, "_read_text", wraps=MODULE._read_text) as read:
                    self.assertEqual(MODULE.validate(baseline_matrix(), self.index, original, False), [])
                    self.assertEqual(document_read_count(read, original / "docs/d.md"), 1)
                    self.assertEqual(document_read_count(read, canonical / "docs/d.md"), 1)
                    self.assertEqual(sum(call.args[0] == root / "docs/d.md"
                                         for call in read.call_args_list), 1)

    def test_immutable_read_cache_is_discarded_after_each_sweep(self) -> None:
        doc = self.root / "docs/d.md"
        with mock.patch.object(MODULE, "_read_text", wraps=MODULE._read_text) as read:
            with immutable_repository_reads():
                for _ in range(3):
                    self.assertEqual(MODULE._read_text(doc), "# Doc\n\n## Section\n")
                    self.assertIsNone(MODULE._path_problem(self.root, "docs/d.md", True))
                self.assertEqual(read.call_count, 1)
            doc.write_text("# Changed\n", encoding="utf-8")
            with immutable_repository_reads():
                self.assertEqual(MODULE._read_text(doc), "# Changed\n")
                self.assertEqual(read.call_count, 2)
            doc.unlink()
            with immutable_repository_reads():
                self.assertEqual(MODULE._path_problem(self.root, "docs/d.md", True),
                                 "file not found: docs/d.md")

    def test_execution_workflow_uses_the_native_producer_condition(self) -> None:
        workflow = (MODULE.DEFAULT_ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        linux = workflow.split("\n  linux:\n", 1)[1].split("\n  gns-compile-probe:\n", 1)[0]
        for name in ("Configure", "Test", "Test over-the-wire networking",
                     "Check acceptance matrix CTest execution", "Upload test evidence"):
            with self.subTest(step=name):
                step = linux.split("      - name: " + name + "\n", 1)[1].split("      - name: ", 1)[0]
                condition = re.search(r"^        if: (.+)$", step, re.MULTILINE)
                self.assertIsNotNone(condition)
                self.assertTrue(condition[1].startswith("${{ needs.changes.outputs.needs_cpp != 'false'"))
                if name != "Upload test evidence":
                    self.assertEqual(condition[1], "${{ needs.changes.outputs.needs_cpp != 'false' }}")

    def test_missing_covered_doc_ref_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"].append({"kind": "doc", "ref": "docs/missing.md"})
        self.assertErrorContains(matrix, "file not found: docs/missing.md")

    def test_needs_native_run_keeps_planned_recording_unevaluated(self) -> None:
        matrix = baseline_matrix()
        row = row_of(matrix, "native.run")
        row["evidence"][0]["planned"] = "record native qualification; owner: ac"
        self.assertEqual(self.errors(matrix), [])
        errors = self.errors(matrix, require_landed=True)
        self.assertEqual(errors, [
            "gtest.partial: planned evidence not landed: tools/later.py",
            "native.run: planned evidence not landed: tools/run.ps1",
            "planned.doc: planned evidence not landed: docs/future.md",
        ])
        discovery, report = self.execution_inputs()
        receipt = self.execution(discovery, report, matrix=matrix)
        self.assertFalse(receipt["full_acceptance_qualified"])
        self.assertIn("native", {entry["kind"] for entry in receipt["unevaluated"]})
        row["evidence"] = [row["evidence"][0]]
        row["status"] = "planned"
        self.assertEqual(self.errors(matrix), [])

    def test_issue_126_covered_requires_mapping_and_inventory_completeness(self) -> None:
        matrix = baseline_matrix()
        row = copy.deepcopy(matrix["rows"][0])
        row.update(id="iss.126", source="issue #126", requirements=[])
        matrix["rows"].append(row)
        self.assertEqual(self.errors(matrix), [])
        matrix["rows"][0]["requirements"].remove("dw.synthetic")
        self.assertErrorContains(matrix, "iss.126: inventory incomplete: status must be planned")
        row["status"] = "planned"
        row["evidence"] = [{"kind": "script", "ref": "tools/future.py", "planned": "supply acceptance evidence; owner: ac"}]
        self.assertFalse(any(error.startswith("iss.126:") for error in self.errors(matrix)))
        # Deleting the requirement itself cannot hide it from the inventory self-check.
        self.inventory["requirements"].pop(0)
        self.write_inventory()
        row["status"] = "covered"
        row["evidence"] = [{"kind": "script", "ref": "tools/script.py"}]
        self.assertErrorContains(matrix, "inventory unaccounted statement")
        self.assertErrorContains(matrix, "iss.126: inventory incomplete")

    def test_inventory_accounts_for_new_behaviour_tables_and_current_exclusions(self) -> None:
        doc = self.root / MODULE.CONTRACT_DOCUMENTS[0]
        doc.write_text("# Contract\n\nEvery requirement has evidence.\n\n"
                       "Rendering never activates regions.\n\n"
                       "| State | Bytes |\n|---|---:|\n| Header | 56 |\n\n"
                       "Current behaviour: the clock restarts. It is descriptive.\n", encoding="utf-8")
        self.assertErrorContains(baseline_matrix(), "inventory unaccounted statement")
        statements = MODULE.contract_statements(doc.read_text(), MODULE.CONTRACT_DOCUMENTS[0])
        self.assertEqual(len(statements), 5)
        for number, (source, text, descriptive) in enumerate(statements[1:]):
            entry = {"id": f"new.{number}", "source": source, "text": text, "quote": text}
            if descriptive:
                entry["reason"] = "Descriptive Current behaviour paragraph."
                entry["category"] = "current-behaviour"
                self.inventory["exclusions"].append(entry)
            else:
                self.inventory["requirements"].append(entry)
        self.inventory["requirements"] = [e for e in self.inventory["requirements"]
                                          if not e["id"].startswith("dw.mapping.")]
        requirements, errors = MODULE.validate_inventory(self.inventory, self.root)
        # New descriptive text cannot extend the reviewed exclusion set.
        self.assertEqual(len(errors), 2)
        self.assertTrue(all("exclusion is not in the closed audited table" in e for e in errors))
        self.assertEqual(len(requirements), 4)
        del self.inventory["exclusions"][0]["reason"]
        self.assertIn("exclusion requires a reason", "\n".join(MODULE.validate_inventory(self.inventory, self.root)[1]))

    def test_scope_requirement_cannot_be_hidden_by_exclusion(self) -> None:
        root = MODULE.DEFAULT_ROOT
        inventory = MODULE.load_matrix(root / "tools/ci/acceptance_requirements.json")
        matrix = MODULE.load_matrix(root / "tools/ci/acceptance_matrix.json")
        entry = next(e for e in inventory["requirements"] if e["id"] == "dw.scope.001")
        inventory["requirements"].remove(entry)
        inventory["exclusions"].append(dict(entry, reason="Not in scope"))
        for row in matrix["rows"]:
            row["requirements"] = [ref for ref in row.get("requirements", []) if ref != entry["id"]]
        with mock.patch.object(MODULE, "load_matrix", return_value=inventory):
            errors = MODULE.validate(matrix, repository_index(), root, False)
        self.assertIn("exclusion requires an audited non-normative category", "\n".join(errors))
        self.assertIn("iss.126: inventory incomplete", "\n".join(errors))

    @immutable_repository_reads()
    def test_behaviour_and_reserved_zero_cannot_be_excluded_under_any_category(self) -> None:
        root = MODULE.DEFAULT_ROOT
        inventory = MODULE.load_matrix(root / "tools/ci/acceptance_requirements.json")
        matrix = MODULE.load_matrix(root / "tools/ci/acceptance_matrix.json")
        index = repository_index()
        for identity in ("dw.scope.001", "dw.scope.009", "ar.file.055"):
            for category in ("rationale", "example", "cross-reference", "current-behaviour", "historical"):
                with self.subTest(identity=identity, category=category):
                    altered = copy.deepcopy(inventory)
                    entry = next(e for e in altered["requirements"] if e["id"] == identity)
                    altered["requirements"].remove(entry)
                    altered["exclusions"].append(dict(entry, category=category, reason="Reviewed context"))
                    unmapped = copy.deepcopy(matrix)
                    for row in unmapped["rows"]:
                        row["requirements"] = [ref for ref in row.get("requirements", []) if ref != identity]
                    with mock.patch.object(MODULE, "load_matrix", return_value=altered):
                        errors = MODULE.validate(unmapped, index, root, False)
                    self.assertIn(f"{identity}: exclusion is not in the closed audited table", "\n".join(errors))
                    self.assertIn("iss.126: inventory incomplete", "\n".join(errors))

    @immutable_repository_reads()
    def test_every_audited_exclusion_is_bound_to_id_source_category_quote_and_text(self) -> None:
        root = MODULE.DEFAULT_ROOT
        inventory = MODULE.load_matrix(root / "tools/ci/acceptance_requirements.json")
        for position, entry in enumerate(inventory["exclusions"]):
            for key, value in (("id", "unreviewed.exclusion"),
                               ("source", "docs/distant-world.md#scope"),
                               ("category", "example"),
                               ("quote", entry["quote"][:-1]),
                               ("text", entry["text"] + " Additional behavior.")):
                with self.subTest(identity=entry["id"], key=key):
                    # Only this exclusion changes; all other fixture entries
                    # remain read-only inputs to the validator.
                    altered = dict(inventory, exclusions=list(inventory["exclusions"]))
                    altered["exclusions"][position] = dict(entry)
                    altered["exclusions"][position][key] = value
                    errors = MODULE.validate_inventory(altered, root)[1]
                    self.assertIn("exclusion is not in the closed audited table", "\n".join(errors))

    def test_exclusion_categories_are_closed_and_required_in_normative_sections(self) -> None:
        entry = self.inventory["requirements"].pop(0)
        self.inventory["exclusions"].append(dict(entry, reason="Descriptive context."))
        for category in (None, "not-in-scope", "", ["rationale"]):
            with self.subTest(category=category):
                self.inventory["exclusions"][0]["category"] = category
                errors = MODULE.validate_inventory(self.inventory, self.root)[1]
                self.assertIn("exclusion requires an audited non-normative category", "\n".join(errors))
        for category, fragment in (("current-behaviour", "not in a Current behaviour paragraph"),
                                   ("historical", "requires a historical heading")):
            with self.subTest(category=category):
                self.inventory["exclusions"][0]["category"] = category
                self.assertIn(fragment, "\n".join(MODULE.validate_inventory(self.inventory, self.root)[1]))

    def test_normative_exclusions_require_reviewed_overrides_even_with_short_quotes(self) -> None:
        entry = self.inventory["requirements"].pop(0)
        entry.update(category="rationale", reason="Not in scope")
        self.inventory["exclusions"].append(entry)
        for marker in ("must", "SHALL", "requires", "must not", "is refused", "is a failure", "never", "**must**"):
            with self.subTest(marker=marker):
                text = f"The session {marker} save."
                (self.root / MODULE.CONTRACT_DOCUMENTS[0]).write_text(
                    "# Contract\n\n" + text + "\n", encoding="utf-8")
                entry.update(text=text, quote="The session")
                errors = MODULE.validate_inventory(self.inventory, self.root)[1]
                self.assertIn("normative exclusion requires an exact reviewed override", "\n".join(errors))

    @immutable_repository_reads()
    def test_reviewed_exclusion_override_is_bound_to_id_source_text_and_category(self) -> None:
        root = MODULE.DEFAULT_ROOT
        inventory = MODULE.load_matrix(root / "tools/ci/acceptance_requirements.json")
        self.assertEqual(MODULE.validate_inventory(inventory, root)[1], [])
        for key, value in (("id", "dw.unreviewed"), ("source", "docs/distant-world.md#scope"),
                           ("text", "The tier must retain edits."), ("category", "rationale")):
            with self.subTest(key=key):
                altered = copy.deepcopy(inventory)
                entry = next(e for e in altered["exclusions"] if e["id"] == "dw.tiers.021")
                entry[key] = value
                if key == "text":
                    entry["quote"] = value
                errors = MODULE.validate_inventory(altered, root)[1]
                self.assertIn("normative exclusion requires an exact reviewed override", "\n".join(errors))

    def test_existing_exclusions_have_audited_categories_and_only_one_override(self) -> None:
        inventory = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_requirements.json")
        exclusions = inventory["exclusions"]
        self.assertEqual(len(exclusions), 39)
        self.assertEqual(len(MODULE.AUDITED_EXCLUSIONS), 39)
        self.assertEqual({e["id"] for e in exclusions},
                         {binding[0] for binding in MODULE.AUDITED_EXCLUSIONS})
        self.assertTrue(all(e["category"] in MODULE.EXCLUSION_CATEGORIES for e in exclusions))
        marked = {e["id"] for e in exclusions if MODULE.NORMATIVE_MARKER_RE.search(e["text"])}
        self.assertEqual(marked, {"dw.tiers.021"})
        self.assertEqual(marked, set(MODULE.REVIEWED_EXCLUSION_OVERRIDES))

    def test_inventory_accepts_root_under_symlinked_directory(self) -> None:
        linked_parent = self.tmp / "linked"
        try:
            linked_parent.symlink_to(self.tmp.resolve(), target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"directory symlinks unavailable: {exc}")
        linked_root = linked_parent / self.root.name
        self.assertNotEqual(linked_root, linked_root.resolve())
        requirements, errors = MODULE.validate_inventory(self.inventory, linked_root)
        self.assertEqual(errors, [])
        self.assertEqual(requirements, {entry["id"]: entry for entry in self.inventory["requirements"]})

    def test_inventory_refuses_bad_sources_quotes_unknown_and_duplicate_ids(self) -> None:
        for key, value, error in (("source", "docs/missing.md#contract", "source file not found"),
                                  ("source", "docs/distant-world.md#wrong", "source anchor not found"),
                                  ("quote", "Not in the document", "quote must be a verbatim excerpt"),
                                  ("id", "ar.synthetic", "duplicate inventory id")):
            with self.subTest(key=key, value=value):
                inventory = copy.deepcopy(self.inventory)
                inventory["requirements"][0][key] = value
                self.assertIn(error, "\n".join(MODULE.validate_inventory(inventory, self.root)[1]))
        matrix = baseline_matrix()
        matrix["rows"][0]["requirements"].append("absent.requirement")
        self.assertErrorContains(matrix, "unknown requirement: absent.requirement")

    def test_missing_inventory_refuses_validation(self) -> None:
        (self.root / "tools/ci/acceptance_requirements.json").unlink()
        self.assertErrorContains(baseline_matrix(), "cannot read")

    def test_current_behaviour_heading_cannot_hide_a_requirement(self) -> None:
        entry = self.inventory["requirements"][0]
        text = "Current behaviour: C4 replaces the fixed fields with world-anchored pages."
        doc = self.root / MODULE.CONTRACT_DOCUMENTS[0]
        doc.write_text(doc.read_text(encoding="utf-8").replace(entry["text"], text), encoding="utf-8")
        entry.update(text=text, quote=text)
        self.write_inventory()
        self.assertEqual(self.errors(baseline_matrix()), [])

    def test_reaudited_obligations_are_requirements_with_planned_rows(self) -> None:
        root = MODULE.DEFAULT_ROOT
        inventory = MODULE.load_matrix(root / "tools/ci/acceptance_requirements.json")
        matrix = MODULE.load_matrix(root / "tools/ci/acceptance_matrix.json")
        requirements = {e["id"] for e in inventory["requirements"]}
        exclusions = {e["id"] for e in inventory["exclusions"]}
        for identity in ("dw.policy.036", "dw.policy.039", "ar.history.020"):
            with self.subTest(identity=identity):
                self.assertIn(identity, requirements)
                self.assertNotIn(identity, exclusions)
                rows = [r for r in matrix["rows"] if identity in r.get("requirements", [])]
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]["status"], "planned")

    def test_inventory_duplicate_text_and_changed_contract_refuse(self) -> None:
        duplicate = dict(self.inventory["requirements"][0], id="dw.duplicate")
        self.inventory["requirements"].append(duplicate)
        self.write_inventory()
        self.assertErrorContains(baseline_matrix(), "inventory stale or duplicate statement")
        self.inventory["requirements"].pop()
        self.write_inventory()
        (self.root / MODULE.CONTRACT_DOCUMENTS[0]).write_text(
            "# Contract\n\nThe amended requirement has evidence.\n", encoding="utf-8")
        self.assertErrorContains(baseline_matrix(), "inventory unaccounted statement")
        self.assertErrorContains(baseline_matrix(), "inventory stale or duplicate statement")

    def test_repository_inventory_completeness_and_generated_doc(self) -> None:
        root = MODULE.DEFAULT_ROOT
        inventory = MODULE.load_matrix(root / "tools/ci/acceptance_requirements.json")
        requirements, errors = MODULE.validate_inventory(inventory, root)
        self.assertEqual(errors, [])
        matrix = MODULE.load_matrix(root / "tools/ci/acceptance_matrix.json")
        self.assertEqual(MODULE.validate(matrix, repository_index(), root, False), [])
        mapped = {ref for row in matrix["rows"] for ref in row.get("requirements", [])}
        self.assertEqual(set(requirements), mapped)
        self.assertTrue(MODULE._doc_matches(root / "docs/acceptance-matrix.md", MODULE.render_doc(matrix)))

    def test_path_escape_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"][2]["ref"] = "../secret.py"
        self.assertErrorContains(matrix, "path escapes root: ../secret.py")

    def test_absolute_path_is_rejected(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"][2]["ref"] = "/etc/passwd"
        self.assertErrorContains(matrix, "path must be repo-relative: /etc/passwd")

    def test_native_missing_command_is_rejected_even_when_planned(self) -> None:
        matrix = baseline_matrix()
        del row_of(matrix, "native.run")["evidence"][0]["command"]
        self.assertErrorContains(matrix, "native command must be a non-empty string")
        matrix = baseline_matrix()
        item = row_of(matrix, "native.run")["evidence"][0]
        del item["command"]
        item["planned"] = "render qualification runner (v0.3.1); owner: ac"
        self.assertErrorContains(matrix, "native command must be a non-empty string")

    def test_native_bad_receipt_schema_is_rejected(self) -> None:
        matrix = baseline_matrix()
        row_of(matrix, "native.run")["evidence"][0]["receipt_schema"] = "render.v1"
        self.assertErrorContains(matrix, "native receipt_schema must match")
        matrix = baseline_matrix()
        del row_of(matrix, "native.run")["evidence"][0]["receipt_schema"]
        self.assertErrorContains(matrix, "native receipt_schema must match")

    def test_native_command_cannot_borrow_an_unrelated_landed_ref(self) -> None:
        matrix = baseline_matrix()
        item = row_of(matrix, "native.run")["evidence"][0]
        item.update(ref="tools/script.py", command="pwsh tools/gates/run-v03-acceptance.ps1")
        self.assertErrorContains(matrix, "native command entry point file not found: tools/gates/run-v03-acceptance.ps1")
        self.assertErrorContains(matrix, "native command entry point must match ref")
        # A missing entry point remains missing even when its evidence ref exists.
        root = MODULE.DEFAULT_ROOT
        matrix = MODULE.load_matrix(root / "tools/ci/acceptance_matrix.json")
        row = row_of(matrix, "iss.68")
        row["status"] = "needs-native-run"
        row["evidence"] = [{"kind": "native", "ref": "tools/ci/check_acceptance_matrix.py",
                            "command": "pwsh tools/gates/missing-qualification.ps1", "receipt_schema": RECEIPT}]
        errors = MODULE.validate(matrix, repository_index(), root, False)
        self.assertIn("native command entry point file not found: tools/gates/missing-qualification.ps1", "\n".join(errors))

    def test_native_command_entry_point_must_match_ref_even_when_both_exist(self) -> None:
        matrix = baseline_matrix()
        item = row_of(matrix, "native.run")["evidence"][0]
        item["ref"] = "tools/script.py"
        self.assertErrorContains(matrix, "native command entry point must match ref: tools/run.ps1 != tools/script.py")

    def test_native_entry_point_handles_interpreters_flags_quotes_and_direct_scripts(self) -> None:
        for command, path in (
            ("pwsh -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1 -Out <dir>", "tools/run.ps1"),
            ('pwsh -File "./tools/run.ps1"', "tools/run.ps1"),
            ("pwsh -NoProfile -File tools/run.ps1", "tools/run.ps1"),
            ("pwsh -ExecutionPolicy Bypass -File tools/run.ps1", "tools/run.ps1"),
            ("python3 tools/script.py --out <dir>", "tools/script.py"),
            ("python './tools/script.py'", "tools/script.py"),
            ("bash tools/run.sh --out <dir>", "tools/run.sh"),
            ('pwsh "tools/path with spaces/run.ps1"', "tools/path with spaces/run.ps1"),
            (r"pwsh tools\run.ps1", "tools/run.ps1"),
            ("./tools/script.py", "tools/script.py"),
        ):
            with self.subTest(command=command):
                self.assertEqual(MODULE._native_entry_point(command), path)
        matrix = baseline_matrix()
        item = row_of(matrix, "native.run")["evidence"][0]
        item.update(ref="tools/script.py", command='python3 "./tools/script.py"')
        self.assertEqual(self.errors(matrix), [])

    def test_native_interpreters_refuse_incompatible_script_types_without_mutation(self) -> None:
        for host in ("python3", "python", "pwsh", "pwsh -File", "bash"):
            compatible = {".py"} if host in {"python3", "python"} else (
                {".ps1"} if host.startswith("pwsh") else {".sh", ".bash"}
            )
            for suffix in (".py", ".ps1", ".sh", ".bash", ".js", ".rb", ".pl", ".cmd", ".bat"):
                path = "tools/entry" + suffix
                (self.root / path).write_text("fixture\n", encoding="utf-8")
                matrix = baseline_matrix()
                item = row_of(matrix, "native.run")["evidence"][0]
                item.update(ref=path, command=host + " " + path)
                before = copy.deepcopy(matrix)
                with self.subTest(host=host, suffix=suffix):
                    if suffix in compatible:
                        self.assertEqual(MODULE._native_entry_point(item["command"]), path)
                        self.assertEqual(self.errors(matrix), [])
                    else:
                        self.assertIsNone(MODULE._native_entry_point(item["command"]))
                        self.assertEqual(self.errors(matrix), [
                            "native.run: native command must invoke a repository script entry point"
                        ])
                    self.assertEqual(matrix, before)

    def test_native_literal_placeholders_and_truncated_options(self) -> None:
        self.assertEqual(MODULE._native_entry_point(
            "python3 tools/script.py sse2=<host.xml> --output=<receipt.json> --override=0"
        ), "tools/script.py")
        for command in ("pwsh -ExecutionPolicy", "pwsh -NoProfile -ExecutionPolicy",
                        "pwsh -File", "python3", "bash", "pwsh -ExecutionPolicy 0",
                        "python3 tools/script.py --out=<bad/path>",
                        "python3 tools/script.py --out=>bad"):
            with self.subTest(command=command):
                self.assertIsNone(MODULE._native_entry_point(command))

    def test_native_long_malformed_arguments_are_refused_without_mutation(self) -> None:
        # This length makes ambiguous repetition impractical even without a
        # timing assertion. Valid literals and placeholders must still parse.
        literal = "a" * 16384
        matrix = baseline_matrix()
        item = row_of(matrix, "native.run")["evidence"][0]
        item.update(ref="tools/script.py")
        prefix = "python3 tools/script.py --out="
        for value in (literal, literal + "<receipt.json>", "<host>" * 4096):
            self.assertEqual(MODULE._native_entry_point(prefix + value), "tools/script.py")
        for suffix in (">", "<", "<bad/path>"):
            with self.subTest(suffix=suffix):
                item["command"] = prefix + literal + suffix
                before = copy.deepcopy(matrix)
                self.assertIsNone(MODULE._native_entry_point(item["command"]))
                self.assertEqual(self.errors(matrix), [
                    "native.run: native command must invoke a repository script entry point"
                ])
                self.assertEqual(matrix, before)

    @immutable_repository_reads()
    def test_native_closed_grammar_rejects_options_shell_syntax_and_multiple_scripts(self) -> None:
        root = MODULE.DEFAULT_ROOT
        matrix = MODULE.load_matrix(root / "tools/ci/acceptance_matrix.json")
        row = row_of(matrix, "iss.68")
        row["status"] = "needs-native-run"
        item = {"kind": "native", "ref": "tools/ci/check_acceptance_matrix.py", "receipt_schema": RECEIPT}
        row["evidence"] = [item]
        index = repository_index()
        for command in (
            'python3 -X "./tools/ci/check_acceptance_matrix.py" -c "pass"',
            'python3 -X "./tools/ci/check_acceptance_matrix.py" "./tools/gates/run-v03-acceptance.ps1"',
            'python3 -c "pass" tools/ci/check_acceptance_matrix.py',
            'python3 -m tools/ci/check_acceptance_matrix.py',
            'python3 -u tools/ci/check_acceptance_matrix.py',
            'python3 -I tools/ci/check_acceptance_matrix.py',
            'python3 -- tools/ci/check_acceptance_matrix.py',
            'pwsh -Command tools/ci/check_acceptance_matrix.py',
            'pwsh -EncodedCommand tools/ci/check_acceptance_matrix.py',
            'pwsh -ExecutionPolicy RemoteSigned -File tools/ci/check_acceptance_matrix.py',
            'pwsh -NoProfile tools/ci/check_acceptance_matrix.py',
            'pwsh -File -NoProfile tools/ci/check_acceptance_matrix.py',
            'pwsh -NoLogo -File tools/ci/check_acceptance_matrix.py',
            'bash -x tools/ci/check_acceptance_matrix.py',
            'sh tools/ci/check_acceptance_matrix.py',
            'powershell.exe -File tools/ci/check_acceptance_matrix.py',
            'python3.12 tools/ci/check_acceptance_matrix.py',
            'env python3 tools/ci/check_acceptance_matrix.py',
            'FLAG=1 python3 tools/ci/check_acceptance_matrix.py',
            'python3 tools/ci/check_acceptance_matrix.py | cat',
            'python3 tools/ci/check_acceptance_matrix.py; echo ok',
            'python3 tools/ci/check_acceptance_matrix.py && echo ok',
            'python3 tools/ci/check_acceptance_matrix.py `echo ok`',
            'python3 tools/ci/check_acceptance_matrix.py $(echo ok)',
            'python3 tools/ci/check_acceptance_matrix.py "$(echo ok)"',
            'python3 tools/ci/check_acceptance_matrix.py > output',
            'python3 tools/ci/check_acceptance_matrix.py\npython3 tools/ci/check_acceptance_matrix.py',
            'python3 tools/ci/check_acceptance_matrix.py tools/gates/run-v03-acceptance.ps1',
            'python3 tools/ci/check_acceptance_matrix.py --script=tools/gates/run-v03-acceptance.ps1',
        ):
            with self.subTest(command=command):
                item["command"] = command
                self.assertIsNone(MODULE._native_entry_point(command))
                errors = MODULE.validate(matrix, index, root, False)
                self.assertIn("native command must invoke a repository script entry point", "\n".join(errors))

    def test_native_entry_point_refuses_missing_absolute_escaping_or_inline_commands(self) -> None:
        matrix = baseline_matrix()
        item = row_of(matrix, "native.run")["evidence"][0]
        for command, fragment in (
            ("pwsh tools/missing.ps1", "native command entry point file not found"),
            ("pwsh ../tools/run.ps1", "native command entry point path escapes root"),
            ("pwsh /absolute/run.ps1", "native command entry point path must be repo-relative"),
            ("pwsh", "must invoke a repository script entry point"),
            ('pwsh "tools/run.ps1', "must invoke a repository script entry point"),
            ("bash -c tools/run.ps1", "must invoke a repository script entry point"),
            ("pwsh -Command tools/run.ps1", "must invoke a repository script entry point"),
            ("python -m tools/script.py", "must invoke a repository script entry point"),
        ):
            with self.subTest(command=command):
                item["command"] = command
                self.assertErrorContains(matrix, fragment)

    def test_repository_timer_and_disc_claims_require_outstanding_evidence(self) -> None:
        matrix = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_matrix.json")
        timer = row_of(matrix, "perf.timer-coverage")
        self.assertEqual(timer["status"], "needs-native-run")
        for metric in ("frame wall", "CPU submit", "present", "GPU"):
            self.assertIn(metric, timer["statement"])
        self.assertIn("fake queries", timer["statement"])
        self.assertIn("outstanding", timer["statement"])
        self.assertTrue(any(item["kind"] == "native" for item in timer["evidence"]))
        attribution = row_of(matrix, "perf.timer-attribution")
        self.assertEqual(attribution["status"], "covered")
        self.assertIn("fake queries", attribution["statement"])
        self.assertIn("delayed result collection", attribution["statement"])
        self.assertIn("frame-distinct GPU-query attribution remains unproven", attribution["statement"])
        rows = [row for row in matrix["rows"] if "ar.activation.003" in row.get("requirements", [])]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["status"], "partial")
        self.assertIn("outside-corner", rows[0]["statement"])
        self.assertTrue(any("planned" in item and "tangencies" in item["planned"]
                            for item in rows[0]["evidence"]))

    def test_repository_boundary_discrepancy_and_watchdog_rearming_evidence(self) -> None:
        matrix = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_matrix.json")
        rows = [row for row in matrix["rows"] if "ar.activation.050" in row.get("requirements", [])]
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row["status"], "planned")
        self.assertEqual(row["source"], "docs/active-regions.md#activation-and-ordering")
        self.assertIn("ActiveRegionLedgerTest.ValidAnchorBoundaries", row["discrepancy"])
        self.assertIn("[-2^24, 2^24)", row["discrepancy"])
        self.assertIn("issue #250", row["discrepancy"])
        self.assertIn("docs/active-regions.md now agrees", row["discrepancy"])
        self.assertIn("test/common/ActiveRegionLedger_test.cpp", row["discrepancy"])
        self.assertIn("asymmetric", row["discrepancy"])
        self.assertIn("docs/active-regions.md", row["discrepancy"])
        self.assertIn("coverage remains planned", row["discrepancy"])
        self.assertIn("owner: ds", row["evidence"][0]["planned"])
        self.assertIn("host-boundary regression", row["evidence"][0]["planned"])
        self.assertEqual(row["evidence"][0]["ref"], "GameSessionHeadlessWorldTest.HalfOpenHostInputBoundaries")
        self.assertIn("rejection counters", row["evidence"][0]["planned"])
        self.assertIn("accept -2^24", row["evidence"][0]["planned"])
        self.assertIn("reject +2^24", row["evidence"][0]["planned"])
        self.assertIn("next representable float below -2^24", row["evidence"][0]["planned"])
        self.assertIn("walking anchors, ground-object edits and pins", row["evidence"][0]["planned"])
        self.assertIn(MODULE._escape_cell("Discrepancy: " + row["discrepancy"]), MODULE.render_doc(matrix))
        watchdog = row_of(matrix, "em.client-hang")
        self.assertIn("HangWatchdog.RearmsAfterHeartbeatResumes", {e["ref"] for e in watchdog["evidence"]})

    def test_planned_item_skips_existence(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][1]["evidence"][1]["ref"] = "tools/absent.py"
        self.assertEqual(self.errors(matrix), [])

    def test_require_landed_flags_planned_items(self) -> None:
        errors = self.errors(baseline_matrix(), require_landed=True)
        self.assertEqual(len(errors), 2, errors)
        self.assertIn("gtest.partial: planned evidence not landed: tools/later.py", errors)
        self.assertIn("planned.doc: planned evidence not landed: docs/future.md", errors)

    def test_status_inconsistencies_are_rejected(self) -> None:
        cases: list[tuple[str, str, Any]] = []

        matrix = baseline_matrix()
        row_of(matrix, "alpha.covered")["evidence"].append(
            {"kind": "script", "ref": "tools/script.py", "planned": "runner; owner: fw"}
        )
        cases.append(("covered planned", "alpha.covered", matrix))

        matrix = baseline_matrix()
        row_of(matrix, "alpha.covered")["evidence"] = [{"kind": "doc", "ref": "docs/d.md"}]
        cases.append(("covered doc only", "alpha.covered", matrix))

        matrix = baseline_matrix()
        row_of(matrix, "native.run")["evidence"] = [{"kind": "doc", "ref": "docs/d.md"}]
        cases.append(("native missing", "native.run", matrix))

        matrix = baseline_matrix()
        row_of(matrix, "planned.doc")["evidence"].append({"kind": "doc", "ref": "docs/d.md"})
        cases.append(("planned with plain", "planned.doc", matrix))

        matrix = baseline_matrix()
        row_of(matrix, "gtest.partial")["evidence"] = [{"kind": "ctest", "ref": "Alpha.One"}]
        cases.append(("partial no planned", "gtest.partial", matrix))

        matrix = baseline_matrix()
        row_of(matrix, "gtest.partial")["evidence"] = [
            {"kind": "script", "ref": "tools/later.py", "planned": "runner; owner: fw"}
        ]
        cases.append(("partial no plain", "gtest.partial", matrix))

        for name, row_id, case in cases:
            with self.subTest(name):
                errors = self.errors(case)
                self.assertTrue(
                    any(error.startswith(f"{row_id}: status ") and "inconsistent with evidence" in error
                        for error in errors),
                    errors,
                )

    def test_malformed_planned_is_rejected(self) -> None:
        for planned in (None, "", "   ", 0, False, [], {"owner": "fw", "note": "runner"}):
            with self.subTest(planned=planned):
                matrix = baseline_matrix()
                row_of(matrix, "gtest.partial")["evidence"][1]["planned"] = planned
                self.assertErrorContains(matrix, "planned must be a non-empty descriptive string")

    def test_planned_description_and_owner_remain_required(self) -> None:
        for planned, expected in (
            ("runner", "planned must include a non-empty '; owner:' workstream"),
            ("runner; owner:   ; issue #132", "planned must include a non-empty '; owner:' workstream"),
            (" ; owner: fw", "planned work description must not be empty"),
        ):
            with self.subTest(planned=planned):
                matrix = baseline_matrix()
                row_of(matrix, "gtest.partial")["evidence"][1]["planned"] = planned
                self.assertErrorContains(matrix, expected)

    def test_requirement_source_is_required(self) -> None:
        (self.root / MODULE.REQUIREMENT_SOURCE).unlink()
        self.assertErrorContains(baseline_matrix(), "cannot read requirement source")

    def test_invalid_requirement_lists_are_rejected(self) -> None:
        for body, expected in (
            ("# Other section\n", "expected one"),
            (MODULE.REQUIREMENT_HEADING + "\n", "requirement list must not be empty"),
            (MODULE.REQUIREMENT_HEADING + "\n- `bad ID`: Invalid.\n", "malformed requirement entry"),
            (MODULE.REQUIREMENT_HEADING + "\n- `alpha.covered`: First.\n- `alpha.covered`: Second.\n",
             "duplicate requirement ID: alpha.covered"),
        ):
            with self.subTest(expected=expected):
                (self.root / MODULE.REQUIREMENT_SOURCE).write_text(body, encoding="utf-8")
                self.assertErrorContains(baseline_matrix(), expected)

    def test_new_contract_requirement_requires_a_mapping(self) -> None:
        source = self.root / MODULE.REQUIREMENT_SOURCE
        source.write_text(
            source.read_text(encoding="utf-8").replace(
                "## Deferred", "- `future.required`: New contract requirement.\n\n## Deferred"
            ), encoding="utf-8",
        )
        errors = self.errors(baseline_matrix())
        self.assertEqual(errors, [
            "<inventory>: inventory unaccounted statement (1): "
            "docs/distant-world.md#acceptance-requirement-ids: "
            "- `future.required`: New contract requirement.",
            "future.required: required mapping missing",
        ])

    def test_test_index_handles_suffixes_multiline_and_add_test_breaks(self) -> None:
        self.assertIn("BetaMultiLine", self.index.ctest_names)
        self.assertIn("RootLevelContract", self.index.ctest_names)
        self.assertIn(("Param", "Three"), self.index.gtest_cases)
        self.assertIn(("Multi", "Line"), self.index.gtest_cases)
        self.assertIn(("Fixture", "Two"), self.index.gtest_cases)

        matrix = baseline_matrix()
        row_of(matrix, "alpha.covered")["evidence"] = [
            {"kind": "ctest", "ref": "BetaMultiLine"},
            {"kind": "ctest", "ref": "RootLevelContract"},
            {"kind": "ctest", "ref": "Param.Three"},
            {"kind": "ctest", "ref": "Multi.Line"},
            {"kind": "ctest", "ref": "Fixture.Two"},
        ]
        self.assertEqual(self.errors(matrix), [])

        row_of(matrix, "alpha.covered")["evidence"] = [
            {"kind": "ctest", "ref": "Multi.Three"},
            {"kind": "script", "ref": "tools/script.py"},
        ]
        self.assertErrorContains(matrix, "ctest not found: Multi.Three")

    def test_commented_add_test_is_not_indexed(self) -> None:
        (self.root / "tools" / "extra.cmake").write_text(
            "# add_test(NAME CommentedLine COMMAND x)\n"
            "#[[ add_test(NAME BlockCommented COMMAND x) ]]\n"
            "#[==[\n"
            "add_test(NAME BracketCommented COMMAND x)\n"
            "]==]\n"
            'message("# not a comment") add_test(NAME QuotedHashLive COMMAND x)\n'
            "add_test(NAME LiveOne COMMAND x) # add_test(NAME TrailingCommented COMMAND x)\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in ("CommentedLine", "BlockCommented", "BracketCommented", "TrailingCommented"):
            self.assertNotIn(ghost, index.ctest_names)
        self.assertIn("QuotedHashLive", index.ctest_names)
        self.assertIn("LiveOne", index.ctest_names)

    def test_commented_test_macros_are_not_indexed(self) -> None:
        (self.root / "test" / "comments_test.cpp").write_text(
            "// TEST(Ghost, LineComment) {}\n"
            "/* TEST(Ghost, BlockComment) {}\n"
            "   TEST_F(Ghost, MultiBlock) {} */\n"
            "TEST(Live, Real) {} // TEST(Ghost, Trailing) {}\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in (("Ghost", "LineComment"), ("Ghost", "BlockComment"),
                      ("Ghost", "MultiBlock"), ("Ghost", "Trailing")):
            self.assertNotIn(ghost, index.gtest_cases)
        self.assertIn(("Live", "Real"), index.gtest_cases)
        self.assertNotIn("Ghost", index.gtest_suites)

        matrix = baseline_matrix()
        row_of(matrix, "alpha.covered")["evidence"] = [
            {"kind": "ctest", "ref": "Ghost.LineComment"},
            {"kind": "script", "ref": "tools/script.py"},
        ]
        self.assertErrorContains(matrix, "ctest not found: Ghost.LineComment")

    def test_add_test_with_cmake_variable_name_is_not_indexed_as_prefix(self) -> None:
        (self.root / "tools" / "suffix.cmake").write_text(
            "add_test(NAME Foo${suffix} COMMAND x)\n"
            'add_test(NAME "Bar${suffix}" COMMAND x)\n',
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertNotIn("Foo", index.ctest_names)
        self.assertNotIn("Bar", index.ctest_names)
        self.assertNotIn("Foo${suffix}", index.ctest_names)

        matrix = baseline_matrix()
        row_of(matrix, "alpha.covered")["evidence"] = [
            {"kind": "ctest", "ref": "Foo"},
            {"kind": "script", "ref": "tools/script.py"},
        ]
        errors = MODULE.validate(matrix, index, self.root, False)
        self.assertEqual(errors, ["alpha.covered: ctest not found: Foo"])

    def test_quoted_add_test_name_is_indexed(self) -> None:
        (self.root / "tools" / "quoted.cmake").write_text(
            'add_test(NAME "QuotedName" COMMAND x)\n',
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertIn("QuotedName", index.ctest_names)

        matrix = baseline_matrix()
        row_of(matrix, "alpha.covered")["evidence"] = [
            {"kind": "ctest", "ref": "QuotedName"},
            {"kind": "script", "ref": "tools/script.py"},
        ]
        self.assertEqual(MODULE.validate(matrix, index, self.root, False), [])

    def test_cmake_if_false_branch_is_not_indexed(self) -> None:
        (self.root / "tools" / "disabled.cmake").write_text(
            "if(FALSE)\n"
            "  add_test(NAME DeadIfFalse COMMAND x)\n"
            "endif()\n"
            "IF ( off )\n"
            "  add_test(NAME DeadIfOff COMMAND x)\n"
            "endif ( )\n"
            "if(0)\n"
            "  add_test(NAME DeadIfZero COMMAND x)\n"
            "endif()\n"
            "add_test(NAME LiveAfter COMMAND x)\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in ("DeadIfFalse", "DeadIfOff", "DeadIfZero"):
            self.assertNotIn(ghost, index.ctest_names)
        self.assertIn("LiveAfter", index.ctest_names)

    def test_cmake_if_false_else_branch_is_indexed(self) -> None:
        (self.root / "tools" / "else.cmake").write_text(
            "if(FALSE)\n"
            "  add_test(NAME ElseDead COMMAND x)\n"
            "else()\n"
            "  add_test(NAME ElseLive COMMAND x)\n"
            "endif()\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertNotIn("ElseDead", index.ctest_names)
        self.assertIn("ElseLive", index.ctest_names)

    def test_cmake_nested_if_inside_disabled_block(self) -> None:
        (self.root / "tools" / "nested.cmake").write_text(
            "if(FALSE)\n"
            "  if(WIN32)\n"
            "    add_test(NAME NestedDeadWin COMMAND x)\n"
            "  else()\n"
            "    add_test(NAME NestedDeadElse COMMAND x)\n"
            "  endif()\n"
            "  add_test(NAME StillDead COMMAND x)\n"
            "else()\n"
            "  if(UNIX)\n"
            "    add_test(NAME ElseNestedLive COMMAND x)\n"
            "  endif()\n"
            "endif()\n"
            "add_test(NAME AfterAll COMMAND x)\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in ("NestedDeadWin", "NestedDeadElse", "StillDead"):
            self.assertNotIn(ghost, index.ctest_names)
        self.assertIn("ElseNestedLive", index.ctest_names)
        self.assertIn("AfterAll", index.ctest_names)

    def test_c_if_zero_block_is_not_indexed(self) -> None:
        (self.root / "test" / "dead_test.cpp").write_text(
            "#if 0\n"
            "TEST(DeadZero, One) {}\n"
            "#endif\n"
            "TEST(LiveZero, Two) {}\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertNotIn(("DeadZero", "One"), index.gtest_cases)
        self.assertNotIn("DeadZero", index.gtest_suites)
        self.assertIn(("LiveZero", "Two"), index.gtest_cases)

    def test_c_if_zero_else_branch_is_indexed(self) -> None:
        (self.root / "test" / "else_test.cpp").write_text(
            "#if 0\n"
            "TEST(ElseDead, One) {}\n"
            "#else\n"
            "TEST(ElseLive, Two) {}\n"
            "#endif\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertNotIn(("ElseDead", "One"), index.gtest_cases)
        self.assertIn(("ElseLive", "Two"), index.gtest_cases)

    def test_c_nested_conditionals_inside_if_zero(self) -> None:
        (self.root / "test" / "nested_test.cpp").write_text(
            "#if 0\n"
            "#if FOO\n"
            "TEST(NestedDeadIf, A) {}\n"
            "#else\n"
            "TEST(NestedDeadElse, B) {}\n"
            "#endif\n"
            "#ifdef BAR\n"
            "TEST(NestedDeadIfdef, C) {}\n"
            "#endif\n"
            "TEST(DeadAfterNested, D) {}\n"
            "#endif\n"
            "TEST(LiveAfterZero, E) {}\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in ("NestedDeadIf", "NestedDeadElse", "NestedDeadIfdef", "DeadAfterNested"):
            self.assertNotIn(ghost, index.gtest_suites)
        self.assertIn(("LiveAfterZero", "E"), index.gtest_cases)

    def test_cmake_if_false_elseif_false_branches_are_not_indexed(self) -> None:
        (self.root / "tools" / "elseif_false.cmake").write_text(
            "if(FALSE)\n"
            "  add_test(NAME ChainDeadIf COMMAND x)\n"
            "elseif(FALSE)\n"
            "  add_test(NAME ChainDeadElseif COMMAND x)\n"
            "endif()\n"
            "if(OFF)\n"
            "  add_test(NAME ChainOffIf COMMAND x)\n"
            "ELSEIF(0)\n"
            "  add_test(NAME ChainOffElseif COMMAND x)\n"
            "endif()\n"
            "add_test(NAME ChainAfter COMMAND x)\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in ("ChainDeadIf", "ChainDeadElseif", "ChainOffIf", "ChainOffElseif"):
            self.assertNotIn(ghost, index.ctest_names)
        self.assertIn("ChainAfter", index.ctest_names)

    def test_cmake_if_false_elseif_unknown_branch_is_indexed(self) -> None:
        (self.root / "tools" / "elseif_var.cmake").write_text(
            "if(FALSE)\n"
            "  add_test(NAME VarDeadIf COMMAND x)\n"
            "elseif(SOMEVAR)\n"
            "  add_test(NAME VarLiveElseif COMMAND x)\n"
            "endif()\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertNotIn("VarDeadIf", index.ctest_names)
        self.assertIn("VarLiveElseif", index.ctest_names)

    def test_cmake_if_false_elseif_false_else_branch_is_indexed(self) -> None:
        (self.root / "tools" / "elseif_else.cmake").write_text(
            "if(FALSE)\n"
            "  add_test(NAME ElseChainDeadIf COMMAND x)\n"
            "elseif(FALSE)\n"
            "  add_test(NAME ElseChainDeadElseif COMMAND x)\n"
            "else()\n"
            "  add_test(NAME ElseChainLive COMMAND x)\n"
            "endif()\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertNotIn("ElseChainDeadIf", index.ctest_names)
        self.assertNotIn("ElseChainDeadElseif", index.ctest_names)
        self.assertIn("ElseChainLive", index.ctest_names)

    def test_cmake_static_false_elseif_after_live_branch_is_dead(self) -> None:
        (self.root / "tools" / "live_chain.cmake").write_text(
            "if(SOMEVAR)\n"
            "  add_test(NAME LiveFirst COMMAND x)\n"
            "elseif(FALSE)\n"
            "  add_test(NAME LiveAfterLive COMMAND x)\n"
            "endif()\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertIn("LiveFirst", index.ctest_names)
        self.assertNotIn("LiveAfterLive", index.ctest_names)

    def test_cmake_nested_elseif_chain_inside_disabled_block(self) -> None:
        (self.root / "tools" / "nested_chain.cmake").write_text(
            "if(FALSE)\n"
            "  if(A)\n"
            "    add_test(NAME NestedChainDeadA COMMAND x)\n"
            "  elseif(FALSE)\n"
            "    add_test(NAME NestedChainDeadB COMMAND x)\n"
            "  else()\n"
            "    add_test(NAME NestedChainDeadC COMMAND x)\n"
            "  endif()\n"
            "  add_test(NAME NestedChainDeadD COMMAND x)\n"
            "elseif(SOMEVAR)\n"
            "  if(FALSE)\n"
            "    add_test(NAME NestedChainDeadE COMMAND x)\n"
            "  elseif(B)\n"
            "    add_test(NAME NestedChainLiveF COMMAND x)\n"
            "  endif()\n"
            "endif()\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in (
            "NestedChainDeadA",
            "NestedChainDeadB",
            "NestedChainDeadC",
            "NestedChainDeadD",
            "NestedChainDeadE",
        ):
            self.assertNotIn(ghost, index.ctest_names)
        self.assertIn("NestedChainLiveF", index.ctest_names)

    def test_c_if_zero_elif_zero_branches_are_not_indexed(self) -> None:
        (self.root / "test" / "elif_zero.cpp").write_text(
            "#if 0\n"
            "TEST(ElifZeroDeadIf, A) {}\n"
            "#elif 0\n"
            "TEST(ElifZeroDeadElif, B) {}\n"
            "#elif (0)\n"
            "TEST(ElifZeroDeadParen, C) {}\n"
            "#endif\n"
            "TEST(ElifZeroAfter, D) {}\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in ("ElifZeroDeadIf", "ElifZeroDeadElif", "ElifZeroDeadParen"):
            self.assertNotIn(ghost, index.gtest_suites)
        self.assertIn(("ElifZeroAfter", "D"), index.gtest_cases)

    def test_c_if_zero_elif_unknown_and_else_branches_are_indexed(self) -> None:
        (self.root / "test" / "elif_foo.cpp").write_text(
            "#if 0\n"
            "TEST(ElifFooDeadIf, A) {}\n"
            "#elif FOO\n"
            "TEST(ElifFooLive, B) {}\n"
            "#else\n"
            "TEST(ElifFooElseLive, C) {}\n"
            "#endif\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertNotIn(("ElifFooDeadIf", "A"), index.gtest_cases)
        self.assertIn(("ElifFooLive", "B"), index.gtest_cases)
        self.assertIn(("ElifFooElseLive", "C"), index.gtest_cases)

    def test_c_if_zero_elif_zero_else_branch_is_indexed(self) -> None:
        (self.root / "test" / "elif_zero_else.cpp").write_text(
            "#if 0\n"
            "TEST(ZeroChainDeadIf, A) {}\n"
            "#elif 0\n"
            "TEST(ZeroChainDeadElif, B) {}\n"
            "#else\n"
            "TEST(ZeroChainElseLive, C) {}\n"
            "#endif\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        self.assertNotIn(("ZeroChainDeadIf", "A"), index.gtest_cases)
        self.assertNotIn(("ZeroChainDeadElif", "B"), index.gtest_cases)
        self.assertIn(("ZeroChainElseLive", "C"), index.gtest_cases)

    def test_c_nested_conditionals_inside_elif_zero(self) -> None:
        (self.root / "test" / "nested_elif.cpp").write_text(
            "#if 0\n"
            "TEST(NestedElifDeadIf, A) {}\n"
            "#elif 0\n"
            "#if FOO\n"
            "TEST(NestedElifDeadInner, B) {}\n"
            "#elif BAR\n"
            "TEST(NestedElifDeadInnerElif, C) {}\n"
            "#endif\n"
            "#elif FOO\n"
            "TEST(NestedElifLive, D) {}\n"
            "#endif\n",
            encoding="utf-8",
        )
        index = MODULE.index_tests(self.root)
        for ghost in ("NestedElifDeadIf", "NestedElifDeadInner", "NestedElifDeadInnerElif"):
            self.assertNotIn(ghost, index.gtest_suites)
        self.assertIn(("NestedElifLive", "D"), index.gtest_cases)

    def test_covered_with_native_evidence_is_rejected(self) -> None:
        matrix = baseline_matrix()
        self.assertEqual(self.errors(matrix), [])
        row_of(matrix, "alpha.covered")["evidence"].append(
            {
                "kind": "native",
                "ref": "tools/run.ps1",
                "command": NATIVE_COMMAND,
                "receipt_schema": RECEIPT,
            }
        )
        self.assertErrorContains(
            matrix,
            "alpha.covered: status covered inconsistent with evidence: "
            "native evidence requires needs-native-run",
        )

    def test_render_doc_is_deterministic_and_complete(self) -> None:
        matrix = baseline_matrix()
        first = MODULE.render_doc(matrix)
        self.assertEqual(first, MODULE.render_doc(copy.deepcopy(matrix)))
        self.assertTrue(first.startswith("# Acceptance matrix\n"))
        self.assertTrue(first.endswith("\n"))
        self.assertFalse(first.endswith("\n\n"))
        self.assertIn("| Row | Status | Source | Requirements | Statement | Evidence |", first)
        self.assertIn(SOURCE, first)
        self.assertIn(f"| `{SOURCE}` |", first)
        self.assertIn("dw.synthetic, ar.synthetic", first)
        self.assertIn("| covered | 1 |", first)
        self.assertIn("| partial | 1 |", first)
        self.assertIn("| needs-native-run | 1 |", first)
        self.assertIn("| planned | 1 |", first)
        self.assertIn("| Total | 4 |", first)
        self.assertIn("(planned: traversal runner; owner: fw)", first)
        self.assertIn("(planned: traversal v2; owner: ac; issue #132)", first)
        self.assertIn(f"-- command: `{NATIVE_COMMAND}`; receipt: `{RECEIPT}`", first)
        self.assertLess(first.index("| alpha.covered |"), first.index("| planned.doc |"))

    def test_render_doc_escapes_pipes(self) -> None:
        matrix = baseline_matrix()
        row_of(matrix, "alpha.covered")["statement"] = "left|right"
        row_of(matrix, "alpha.covered")["evidence"][2]["ref"] = "tools/a|b.py"
        doc = MODULE.render_doc(matrix)
        self.assertIn("left\\|right", doc)
        self.assertNotIn("left|right", doc)
        self.assertIn("script: `tools/a\\|b.py`", doc)

    def test_write_doc_then_check_doc_passes(self) -> None:
        self.write_matrix(baseline_matrix())
        doc = self.root / "docs" / "acceptance-matrix.md"
        code, out, err = self.run_main(*self.base_args(), "--write-doc", str(doc))
        self.assertEqual(code, 0, err)
        self.assertIn("static references ok: 4 rows; execution unevaluated", out)
        self.assertNotIn(b"\r", doc.read_bytes())
        code, _, err = self.run_main(*self.base_args(), "--check-doc", str(doc))
        self.assertEqual(code, 0, err)

    def test_crlf_doc_is_accepted(self) -> None:
        self.write_matrix(baseline_matrix())
        doc = self.root / "docs" / "acceptance-matrix.md"
        self.run_main(*self.base_args(), "--write-doc", str(doc))
        crlf = doc.read_bytes().replace(b"\n", b"\r\n")
        doc.write_bytes(crlf)
        code, _, err = self.run_main(*self.base_args(), "--check-doc", str(doc))
        self.assertEqual(code, 0, err)

    def test_edited_doc_is_stale(self) -> None:
        self.write_matrix(baseline_matrix())
        doc = self.root / "docs" / "acceptance-matrix.md"
        self.run_main(*self.base_args(), "--write-doc", str(doc))
        doc.write_text(doc.read_text(encoding="utf-8") + "hand edit\n", encoding="utf-8")
        code, _, err = self.run_main(*self.base_args(), "--check-doc", str(doc))
        self.assertEqual(code, 1)
        self.assertIn(f"error: {doc} is stale; regenerate with --write-doc", err)

    def test_missing_doc_is_stale(self) -> None:
        self.write_matrix(baseline_matrix())
        doc = self.root / "docs" / "missing.md"
        code, _, err = self.run_main(*self.base_args(), "--check-doc", str(doc))
        self.assertEqual(code, 1)
        self.assertIn("is stale", err)

    def test_write_doc_with_invalid_matrix_writes_nothing(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][1]["id"] = "alpha.covered"
        self.write_matrix(matrix)
        doc = self.root / "docs" / "acceptance-matrix.md"
        code, _, err = self.run_main(*self.base_args(), "--write-doc", str(doc))
        self.assertEqual(code, 1)
        self.assertFalse(doc.exists())
        self.assertIn("error: alpha.covered: duplicate id", err)

    def test_errors_are_sorted_by_row_id(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"][0]["status"] = "bogus"
        row_of(matrix, "planned.doc")["statement"] = ""
        self.write_matrix(matrix)
        code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 1)
        lines = err.splitlines()
        self.assertTrue(lines and all(line.startswith("error: ") for line in lines), lines)
        self.assertEqual(lines, sorted(lines))
        self.assertTrue(lines[0].startswith("error: alpha.covered"), lines)

    def test_missing_matrix_exits_2(self) -> None:
        code, _, err = self.run_main("--root", str(self.root), "--matrix", str(self.tmp / "none.json"))
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error: "), err)

    def test_unparseable_matrix_exits_2(self) -> None:
        self.write_matrix("{not json")
        code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error: "), err)

    def execution_inputs(self, cases: list[dict[str, Any]] | None = None) -> tuple[Path, Path]:
        discovery = self.tmp / "discovery.json"
        discovery.write_text(json.dumps({"kind": "ctestInfo", "tests": [
            {"name": name} for name in ("AlphaContract", "Alpha.One", "Instances/Param.Three/0",
                                        "Instances/Param.Three/1", "Unrelated")
        ]}), encoding="utf-8")
        report = self.tmp / "ctest.junit.xml"
        if cases is None:
            cases = [{"name": name, "status": "run"} for name in
                     ("AlphaContract", "Alpha.One", "Instances/Param.Three/0", "Instances/Param.Three/1")]
        self.write_junit(report, cases)
        return discovery, report

    def write_junit(self, path: Path, cases: list[dict[str, Any]]) -> None:
        suite = ET.Element("testsuite")
        for spec in cases:
            spec = dict(spec)
            adverse = spec.pop("adverse", None)
            case = ET.SubElement(suite, "testcase", spec)
            if adverse:
                ET.SubElement(case, adverse)
        ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)

    def execution(self, discovery: Path, *reports: Path, matrix: dict[str, Any] | None = None) -> dict[str, Any]:
        return MODULE.validate_execution(matrix or baseline_matrix(), self.index, list(reports), discovery)

    def test_execution_uses_every_discovered_parameter_and_scopes_other_evidence(self) -> None:
        discovery, report = self.execution_inputs()
        receipt = self.execution(discovery, report)
        self.assertTrue(receipt["ctest_references_passed"])
        self.assertFalse(receipt["full_acceptance_qualified"])
        parameter = next(ref for ref in receipt["references"] if ref["ref"] == "Param.*")
        self.assertEqual(parameter["expected"], ["Instances/Param.Three/0", "Instances/Param.Three/1"])
        self.assertEqual({entry["kind"] for entry in receipt["unevaluated"]}, {"script", "native", "doc"})
        self.assertEqual({entry["reason"] for entry in receipt["unevaluated"]},
                         {"planned", "separate-evidence-required"})
        self.assertEqual(len(receipt["discovery_sha256"]), 64)
        self.assertEqual(receipt["reports"][0]["name"], report.name)

    def test_execution_cannot_hide_an_omitted_parameter_instance(self) -> None:
        discovery, report = self.execution_inputs()
        tree = ET.parse(report)
        tree.getroot().remove(tree.getroot()[-1])
        tree.write(report, encoding="utf-8")
        receipt = self.execution(discovery, report)
        self.assertFalse(receipt["ctest_references_passed"])
        parameter = next(ref for ref in receipt["references"] if ref["ref"] == "Param.*")
        self.assertEqual(parameter["missing"], ["Instances/Param.Three/1"])

    def test_execution_refuses_adverse_and_nonexecuted_statuses(self) -> None:
        for status, result, adverse in (("run", None, "failure"), ("run", None, "error"),
                                        ("run", None, "skipped"), ("notrun", None, None),
                                        ("disabled", None, None), (None, None, None),
                                        ("run", "suppressed", None)):
            with self.subTest(status=status, result=result, adverse=adverse):
                discovery, report = self.execution_inputs()
                tree = ET.parse(report)
                first = tree.getroot()[0]
                first.attrib.pop("status")
                if status is not None:
                    first.set("status", status)
                if result is not None:
                    first.set("result", result)
                if adverse:
                    ET.SubElement(first, adverse)
                tree.write(report, encoding="utf-8")
                receipt = self.execution(discovery, report)
                self.assertFalse(receipt["ctest_references_passed"])
                self.assertEqual(receipt["references"][0]["adverse"], ["AlphaContract"])

    def test_successful_duplicate_cannot_mask_a_failure_in_another_report(self) -> None:
        discovery, report = self.execution_inputs()
        duplicate = self.tmp / "second.xml"
        self.write_junit(duplicate, [{"name": "AlphaContract", "status": "run", "adverse": "failure"}])
        receipt = self.execution(discovery, report, duplicate)
        self.assertFalse(receipt["ctest_references_passed"])
        self.assertEqual(receipt["references"][0]["adverse"], ["AlphaContract"])

    def test_extra_report_supplies_separately_run_manual_test(self) -> None:
        (self.root / "test" / "manual.cmake").write_text("add_test(NAME ManualSoak COMMAND soak)\n", encoding="utf-8")
        self.index = MODULE.index_tests(self.root)
        matrix = baseline_matrix()
        matrix["rows"][0]["evidence"].append({"kind": "ctest", "ref": "ManualSoak"})
        discovery, report = self.execution_inputs()
        inventory = json.loads(discovery.read_text(encoding="utf-8"))
        inventory["tests"].append({"name": "ManualSoak"})
        discovery.write_text(json.dumps(inventory), encoding="utf-8")
        self.assertFalse(self.execution(discovery, report, matrix=matrix)["ctest_references_passed"])
        soak = self.tmp / "soak.xml"
        self.write_junit(soak, [{"name": "ManualSoak", "status": "run"}])
        self.assertTrue(self.execution(discovery, report, soak, matrix=matrix)["ctest_references_passed"])

    def test_execution_rejects_undiscovered_matching_identity_and_classname_guess(self) -> None:
        discovery, report = self.execution_inputs()
        wrong = self.tmp / "wrong.xml"
        self.write_junit(wrong, [{"name": "Instances/Param.Three/2", "status": "run"}])
        receipt = self.execution(discovery, report, wrong)
        self.assertFalse(receipt["ctest_references_passed"])
        parameter = next(ref for ref in receipt["references"] if ref["ref"] == "Param.*")
        self.assertEqual(parameter["unexpected"], ["Instances/Param.Three/2"])
        self.write_junit(report, [{"name": "One", "classname": "Alpha", "status": "run"}])
        self.assertFalse(self.execution(discovery, report)["ctest_references_passed"])

    def test_execution_rejects_empty_or_invalid_discovery_and_reports(self) -> None:
        for document in ({"kind": "ctestInfo", "tests": []}, {"kind": "wrong", "tests": [{"name": "AlphaContract"}]},
                         {"kind": "ctestInfo", "tests": [{"name": "AlphaContract"}, {"name": "AlphaContract"}]}, []):
            with self.subTest(document=document):
                discovery, report = self.execution_inputs()
                discovery.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(MODULE.MatrixError):
                    self.execution(discovery, report)
        for document in ("<testsuite/>", "<not-junit><testcase name='AlphaContract'/></not-junit>", "<invalid"):
            with self.subTest(document=document):
                discovery, report = self.execution_inputs()
                report.write_text(document, encoding="utf-8")
                with self.assertRaises(MODULE.MatrixError):
                    self.execution(discovery, report)

    def test_script_only_matrix_is_execution_unevaluated(self) -> None:
        matrix = baseline_matrix()
        matrix["rows"] = [matrix["rows"][0]]
        matrix["rows"][0]["evidence"] = [{"kind": "script", "ref": "tools/script.py"}]
        discovery, report = self.execution_inputs()
        receipt = self.execution(discovery, report, matrix=matrix)
        self.assertFalse(receipt["ctest_references_passed"])
        self.assertEqual(receipt["references"], [])
        self.assertFalse(receipt["full_acceptance_qualified"])

    def test_execution_cli_writes_scoped_receipt_and_requires_discovery(self) -> None:
        self.write_matrix(baseline_matrix())
        discovery, report = self.execution_inputs()
        code, _, err = self.run_main(*self.base_args(), "--junit", str(report))
        self.assertEqual(code, 2, err)
        receipt = self.tmp / "output" / "receipt.json"
        code, out, err = self.run_main(*self.base_args(), "--junit", str(report), "--discovery", str(discovery),
                                       "--execution-receipt", str(receipt))
        self.assertEqual(code, 0, err)
        self.assertIn("full acceptance remains unqualified", out)
        data = json.loads(receipt.read_text(encoding="utf-8"))
        self.assertIsNone(data["identity"])
        self.assertEqual(len(data["matrix_sha256"]), 64)

    def test_ci_identity_binds_checkout_and_candidate_and_refuses_bad_context(self) -> None:
        checkout, candidate = "a" * 40, "b" * 40
        event = self.tmp / "event.json"
        event.write_text(json.dumps({"pull_request": {"head": {"sha": candidate}}}), encoding="utf-8")
        environment = {"CI": "true", "GITHUB_ACTIONS": "true", "GITHUB_EVENT_PATH": str(event),
                       "GITHUB_SHA": checkout, "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2",
                       "GITHUB_JOB": "linux-gcc"}
        with mock.patch.dict(os.environ, environment, clear=True), \
                mock.patch.object(MODULE.subprocess, "check_output", return_value=checkout + "\n"):
            identity = MODULE._ci_identity(self.root)
            self.assertEqual((identity["checkout_sha"], identity["candidate_sha"]), (checkout, candidate))
            for key, bad in (("GITHUB_SHA", candidate), ("GITHUB_RUN_ATTEMPT", "0"), ("GITHUB_ACTIONS", "false")):
                with self.subTest(key=key), mock.patch.dict(os.environ, {key: bad}):
                    with self.assertRaises(MODULE.MatrixError):
                        MODULE._ci_identity(self.root)
            for invalid in ([], {"pull_request": {}}, {"pull_request": {"head": {"sha": "short"}}}):
                event.write_text(json.dumps(invalid), encoding="utf-8")
                with self.subTest(event=invalid), self.assertRaises(MODULE.MatrixError):
                    MODULE._ci_identity(self.root)


class ProductionAcceptanceMatrixTests(unittest.TestCase):
    @immutable_repository_reads()
    def test_deleting_any_required_production_mapping_fails_validation(self) -> None:
        root = MODULE.DEFAULT_ROOT
        matrix = MODULE.load_matrix(root / "tools/ci/acceptance_matrix.json")
        required_ids = MODULE.load_required_ids(root / MODULE.REQUIREMENT_SOURCE)
        index = repository_index()
        self.assertIn("perf.floor-p99", required_ids)
        self.assertEqual(MODULE.validate(matrix, index, root, False), [])
        inventory = MODULE.load_matrix(root / "tools/ci/acceptance_requirements.json")
        for row_id in sorted(required_ids):
            with self.subTest(row_id=row_id):
                # Validation reads rows without mutating the shared fixture.
                incomplete = dict(matrix, rows=[row for row in matrix["rows"] if row["id"] != row_id])
                mapped = {ref for row in incomplete["rows"] for ref in row.get("requirements", [])}
                missing = sorted({e["id"] for e in inventory["requirements"]} - mapped)
                expected = [f"<inventory>: requirement lacks a matrix row: {ref}" for ref in missing]
                if missing and row_id != "iss.126":
                    expected.append("iss.126: inventory incomplete: status must be planned")
                expected.append(f"{row_id}: required mapping missing")
                self.assertEqual(MODULE.validate(incomplete, index, root, False), sorted(expected))

    def test_production_planned_notes_render_as_descriptive_text(self) -> None:
        matrix = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_matrix.json")
        planned = [item["planned"] for row in matrix["rows"] for item in row["evidence"]
                   if "planned" in item]
        self.assertTrue(planned)
        rendered = MODULE.render_doc(matrix)
        for note in planned:
            self.assertIsInstance(note, str)
            self.assertTrue(note.strip())
            self.assertIn(f"(planned: {MODULE._escape_cell(note)})", rendered)
        self.assertNotRegex(rendered, r"(?i)\b(?:PR|pull request|change)\s*#?\s*\d+")


class AcceptanceMatrixPublishabilityTests(unittest.TestCase):
    def test_inventory_and_generated_matrix_are_publishable(self) -> None:
        public_documents = ("tools/ci/acceptance_requirements.json", "tools/ci/acceptance_matrix.json",
                            "docs/acceptance-matrix.md", *MODULE.CONTRACT_DOCUMENTS)
        for relative in public_documents:
            with self.subTest(path=relative):
                self.assertNotRegex(
                    (MODULE.DEFAULT_ROOT / relative).read_text(encoding="utf-8"),
                    re.compile(r"/pull/|\bPR\s*#\s*[0-9]+\b", re.IGNORECASE),
                )
        for relative in ("tools/ci/acceptance_requirements.json", "tools/ci/acceptance_matrix.json",
                         "docs/acceptance-matrix.md", "tools/ci/check_acceptance_matrix.py",
                         "tools/ci/test_check_acceptance_matrix.py"):
            with self.subTest(path=relative):
                self.assertEqual(PUBLIC_TREE.inspect_path(relative), [])
                self.assertEqual(PUBLIC_TREE.inspect_text(
                    relative, (MODULE.DEFAULT_ROOT / relative).read_text(encoding="utf-8")), [])

    def test_public_tree_partitions_cover_each_path_once(self) -> None:
        paths = repository_paths()
        partitions = [paths[number::4] for number in range(4)]
        self.assertTrue(all(partitions))
        self.assertEqual(sorted(path for partition in partitions for path in partition), list(paths))
        self.assertEqual(len(paths), len(set(paths)))


class AcceptanceMatrixPublicTreeTests(unittest.TestCase):
    # The complete public tree is much broader than the matrix fixtures. Keep
    # its four disjoint scans in separate CTest entries under the same timeout.
    def check_partition(self, number: int) -> None:
        paths = repository_paths()[number::4]
        self.assertTrue(paths)
        with mock.patch.object(PUBLIC_TREE, "tracked_paths", return_value=paths):
            self.assertEqual(PUBLIC_TREE.validate(MODULE.DEFAULT_ROOT), [])

    def test_partition_0(self) -> None:
        self.check_partition(0)

    def test_partition_1(self) -> None:
        self.check_partition(1)

    def test_partition_2(self) -> None:
        self.check_partition(2)

    def test_partition_3(self) -> None:
        self.check_partition(3)


class PublicTreeScanTests(unittest.TestCase):
    def test_binary_files_only_read_the_classification_prefix(self) -> None:
        reads: list[int] = []

        class BinaryInput(io.BytesIO):
            def read(self, size: int = -1) -> bytes:
                reads.append(size)
                return super().read(size)

        stream = BinaryInput(b"\0" + b"x" * 16384)
        with mock.patch.object(PUBLIC_TREE, "tracked_paths", return_value=["assets/example.bin"]), \
                mock.patch.object(Path, "open", return_value=stream):
            self.assertEqual(PUBLIC_TREE.validate(Path(".")), [])
        self.assertEqual(reads, [8192])
        self.assertTrue(stream.closed)

    def test_excluded_text_still_receives_path_checks_without_file_reads(self) -> None:
        with mock.patch.object(PUBLIC_TREE, "tracked_paths", return_value=["vendor/example.exe"]), \
                mock.patch.object(Path, "open") as opened:
            self.assertEqual([finding.rule for finding in PUBLIC_TREE.validate(Path("."))], ["generated-binary"])
            opened.assert_not_called()

    def test_comment_masks_preserve_all_language_branches_and_offsets(self) -> None:
        cases = (
            ('auto n = 0x1f\'ff; auto s = R"tag(hidden)tag"; // visible\n',
             False, {"cpp": True}, "// visible"),
            ('char *s = "hidden"; /* visible */\n', False, {}, "/* visible */"),
            ('$s = @"\nhidden\n"@\n<# visible <# nested #> #>\n',
             True, {}, "<# visible <# nested #> #>"),
            ('s = """hidden # literal"""\n# visible\n',
             False, {"hash_comments": True, "triple_quotes": True}, "# visible"),
            ('s="hidden # literal"\n# visible\n',
             False, {"hash_comments": True}, "# visible"),
            ('set(s [==[hidden # literal]==]) # visible\n',
             False, {"hash_comments": True, "cmake": True}, "# visible"),
            ('label: owner\'s scalar # visible\nquoted: &value "hidden # literal"\n',
             False, {"hash_comments": True, "yaml": True}, "# visible"),
        )
        for text, powershell, options, comment in cases:
            with self.subTest(options=options, powershell=powershell):
                expected = ["\n" if ch == "\n" else " " for ch in text]
                start = text.index(comment)
                expected[start:start + len(comment)] = comment
                self.assertEqual(PUBLIC_TREE.code_mask(
                    text, powershell, comments_only=True, **options), "".join(expected))
                code = PUBLIC_TREE.code_mask(text, powershell, **options)
                self.assertEqual(len(code), len(text))
                self.assertEqual([i for i, ch in enumerate(code) if ch == "\n"],
                                 [i for i, ch in enumerate(text) if ch == "\n"])
                self.assertNotIn("hidden", code)
                self.assertNotIn("visible", code)

    def test_line_prefilter_preserves_findings_for_all_line_boundaries(self) -> None:
        for separator in ("\n", "\r\n", "\r", "\v", "\f", "\x1c", "\x1d", "\x1e",
                          "\x85", "\u2028", "\u2029"):
            text = separator.join(("Public contract.", "# P1: example", "\x54ODO", "issue #220"))
            expected = [(rule, match[0], line_number)
                        for line_number, full_line in enumerate(text.splitlines(keepends=True), 1)
                        for rule, pattern in PUBLIC_TREE.TEXT_RULES
                        for match in pattern.finditer(full_line.rstrip("\r\n"))]
            with self.subTest(separator=repr(separator)):
                actual = [(finding.rule, finding.detail, finding.line)
                          for finding in PUBLIC_TREE.inspect_text("docs/example.md", text)
                          if finding.rule in {rule for rule, _ in PUBLIC_TREE.TEXT_RULES}]
                self.assertEqual(actual, expected)
                self.assertTrue(actual)

    def test_text_prefilters_preserve_every_rule_and_unicode_case_policy(self) -> None:
        passages = (
            "\x54ODO \x46IXME \x57IP \x54BD \x58XX",
            "\x73pec 12; \u017fpec 12; \x74ask #3; \x57ave 7; \x50R #12",
            "\x46R-RENDER \x54-NET \x41C-foo \x502-T3 \x41-T4 \x41UDIO-FOO",
            "\x494-TEST \x57S-9 \x4fQ-A \x4eFR-x \x4bDD-a \x46FF-3 \x52D-4 \x4bD-a \x54123 \x502.1",
            "\x70hase 2; \u0130tem 2; \u0131tem 2",
            "\x28A2)", "\x28A\u0661)", "# \x501: example",
            "\x50illar A; \x54rack B; pre-\x503; \x49ncrement 3; \x69nc 3",
            "\x73pec.md \x70lan.md \x68andoff.md \x64esign-decisions.md",
            "\x2fhome/example/file \x2fmnt/c/path", "//\x77sl.localhost/distro",
            "Q:\\\x55sers\\Example\\a", "ghp_" + "a" * 30,
            "sk-" + "a" * 24, "AKIA" + "A" * 16,
            "-----BEGIN " + "PRIVATE KEY-----",
        )
        text = "\n".join(passages)
        expected = [(rule, match[0], line_number)
                    for line_number, line in enumerate(text.splitlines(), 1)
                    for rule, pattern in PUBLIC_TREE.TEXT_RULES for match in pattern.finditer(line)]
        self.assertEqual({rule for rule, _, _ in expected}, {rule for rule, _ in PUBLIC_TREE.TEXT_RULES})
        actual = [(finding.rule, finding.detail, finding.line)
                  for finding in PUBLIC_TREE.inspect_text("docs/example.md", text)
                  if finding.rule in {rule for rule, _ in PUBLIC_TREE.TEXT_RULES}]
        self.assertEqual(actual, expected)

    def test_hash_masking_requires_a_possible_hash_and_preserves_wrapped_comments(self) -> None:
        with mock.patch.object(PUBLIC_TREE, "candidate_prose", wraps=PUBLIC_TREE.candidate_prose) as prose:
            self.assertEqual(PUBLIC_TREE.inspect_text("example.cpp", "// Public contract.\n" * 100), [])
            prose.assert_not_called()
        for suffix, leader in ((".cpp", "// "), (".ps1", "# "), (".py", "# "),
                               (".cmake", "# "), (".yaml", "# "), (".yml", "# ")):
            with self.subTest(suffix=suffix):
                text = leader + "Synthetic package\r\n" + leader + "deadbee inspected.\r\n"
                self.assertEqual([(finding.rule, finding.line)
                                  for finding in PUBLIC_TREE.inspect_text("example" + suffix, text)],
                                 [("candidate-provenance", 1)])

    def test_provenance_hints_preserve_wrapped_and_unicode_findings(self) -> None:
        for suffix, leader in ((".cpp", "// "), (".psm1", "# "), (".py", "# "),
                               (".sh", "# "), (".cmake", "# "), (".yaml", "# "), (".md", "")):
            for first, last, rule in (("A \x70rivate", "native fixture.", "private-provenance"),
                                      ("The \x6fperator", "stopped it.", "private-process-provenance"),
                                      ("\x50\u0131D:", "12345", "private-process-provenance")):
                with self.subTest(suffix=suffix, rule=rule):
                    text = leader + first + "\r\n" + leader + last
                    self.assertEqual([(finding.rule, finding.line)
                                      for finding in PUBLIC_TREE.inspect_text("example" + suffix, text)],
                                     [(rule, 1)])

    def test_exemption_scanning_requires_a_possible_code_token(self) -> None:
        with mock.patch.object(PUBLIC_TREE, "code_mask", wraps=PUBLIC_TREE.code_mask) as mask:
            for suffix in (".cpp", ".cc", ".cxx", ".h", ".hpp", ".ps1", ".psm1"):
                self.assertEqual(PUBLIC_TREE.code_token_exemptions("example" + suffix,
                                                                 "ordinary code\n" * 100), set())
            mask.assert_not_called()
        text = "auto n = counts[\x70hase - 1]; // counts[\x70hase - 1]\n"
        spans = PUBLIC_TREE.code_token_exemptions("example.cpp", text)
        self.assertEqual(len(spans), 1)
        self.assertEqual(text[slice(*next(iter(spans)))], "\x70hase - 1")
        for path, text in (("example.cpp", "auto n = counts[/* gap */\x70hase - 1];"),
                           ("example.ps1", "$x. <# gap #> \x49tem1")):
            with self.subTest(path=path):
                self.assertEqual(len(PUBLIC_TREE.code_token_exemptions(path, text)), 1)

    def test_name_prefilter_preserves_exact_matcher_findings(self) -> None:
        token = "AttributionToken"
        named_pattern = next(pattern for rule, pattern in PUBLIC_TREE.PROVENANCE_TEXT_RULES
                             if rule == "internal-reviewer-provenance" and pattern.groups)
        passages = (token + " reviewed.", token + "-review.", "prefix-" + token + " review.",
                    "review by " + token + ".", token.upper() + "/low review.",
                    token.replace("k", "\u212a") + " reviewed.",
                    "one-two-three-four-five review.", "Ordinary public language.")
        passages += tuple(token + " " + context for context in (
            "audit", "verification", "analysis", "model", "agent", "signed-off", "authoritative",
            "finding", "critique", "audited", "verified", "accepted", "approved", "rejected", "found",
            "corrected", "#123", "r4", "adapters"))
        passages += tuple(token + "/" + tier for tier in ("low", "medium", "high", "xhigh", "max", "ultra"))
        for name in (token, "prefix-" + token):
            with mock.patch.object(PUBLIC_TREE, "AGENT_NAME_DIGESTS", {PUBLIC_TREE.name_digest(name)}):
                for passage in passages:
                    expected = [match[0] for match in named_pattern.finditer(passage)
                                if any(PUBLIC_TREE.name_digest(group) in PUBLIC_TREE.AGENT_NAME_DIGESTS
                                       for group in match.groups() if group is not None)]
                    with self.subTest(name=name, passage=passage):
                        actual = [finding.detail for finding in PUBLIC_TREE.inspect_text("docs/example.md", passage)
                                  if finding.rule == "internal-reviewer-provenance"]
                        self.assertEqual(actual, expected)
                        if expected:
                            self.assertTrue(PUBLIC_TREE._may_name_attribution(passage))
                self.assertFalse(PUBLIC_TREE._may_name_attribution("Ordinary public language."))


if __name__ == "__main__":
    unittest.main()
