#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


MODULE_PATH = Path(__file__).with_name("check_acceptance_matrix.py")
SPEC = importlib.util.spec_from_file_location("check_acceptance_matrix", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


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
        "schema": "luminumbra.acceptance_matrix.v1",
        "description": "synthetic matrix",
        "rows": [
            {
                "id": "alpha.covered",
                "source": SOURCE,
                "statement": "Covered by ctest and script.",
                "status": "covered",
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
                        "planned": {"owner": "fw", "pr": 7},
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
                        "planned": {"owner": "ac", "issue": 132, "note": "traversal v2"},
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
        (self.root / "docs" / "d.md").write_text("# Doc\n", encoding="utf-8")
        self.index = MODULE.index_tests(self.root)
        self.matrix_path = self.tmp / "matrix.json"

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
        item["planned"] = {"owner": "ac", "pr": 218}
        self.assertErrorContains(matrix, "native command must be a non-empty string")

    def test_native_bad_receipt_schema_is_rejected(self) -> None:
        matrix = baseline_matrix()
        row_of(matrix, "native.run")["evidence"][0]["receipt_schema"] = "render.v1"
        self.assertErrorContains(matrix, "native receipt_schema must match")
        matrix = baseline_matrix()
        del row_of(matrix, "native.run")["evidence"][0]["receipt_schema"]
        self.assertErrorContains(matrix, "native receipt_schema must match")

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
            {"kind": "script", "ref": "tools/script.py", "planned": {"owner": "fw"}}
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
            {"kind": "script", "ref": "tools/later.py", "planned": {"owner": "fw"}}
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
        cases = {
            "planned.owner must be a non-empty string": {"pr": 7},
            "planned.pr must be a positive integer": {"owner": "fw", "pr": 0},
            "planned.issue must be a positive integer": {"owner": "fw", "issue": "132"},
        }
        for fragment, planned in cases.items():
            with self.subTest(fragment):
                matrix = baseline_matrix()
                row_of(matrix, "gtest.partial")["evidence"][1]["planned"] = planned
                self.assertErrorContains(matrix, fragment)

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
        self.assertIn("| Row | Status | Statement | Evidence |", first)
        self.assertIn("| covered | 1 |", first)
        self.assertIn("| partial | 1 |", first)
        self.assertIn("| needs-native-run | 1 |", first)
        self.assertIn("| planned | 1 |", first)
        self.assertIn("| Total | 4 |", first)
        self.assertIn("(planned: fw, change 7)", first)
        self.assertIn("(planned: ac, issue #132 - traversal v2)", first)
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
        self.assertIn("acceptance matrix ok: 4 rows", out)
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


if __name__ == "__main__":
    unittest.main()
