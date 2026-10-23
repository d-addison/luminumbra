#!/usr/bin/env python3

from __future__ import annotations

import sys


_CHILD_TIMEOUT = 12
_LOCAL_PARTITIONS = 64
_CONTRACT_PARTITIONS = 225
_EXPECTED_CASES = 479
_PHASE_LIMIT = 768
_phase_records = 0


def _phase(token: str, *, selector: int | None = None, case: str = "") -> None:
    # At most 768 attempted records per process, each below 256 ASCII bytes.
    # Slice before encoding/filtering; no paths, payloads, retries or stack scans.
    global _phase_records
    if _phase_records >= _PHASE_LIMIT or token not in (
            "entry", "imports-start", "imports-finish", "checker-import-start", "checker-import-finish",
            "public-import-start", "public-import-finish", "module-ready", "selection-start",
            "selection-finish", "case-start", "case-finish"):
        return
    record = "acceptance-phase=" + token
    if type(selector) is int and 0 <= selector < _CONTRACT_PARTITIONS:
        record += " selector=" + str(selector)
    if case:
        record += " case=" + "".join(char if char.isascii() and (char.isalnum() or char in "._")
                                   else "?" for char in case[:128])
    record += "\n"
    _phase_records += 1
    try:
        if sys.stderr.write(record) != len(record):
            _phase_records = _PHASE_LIMIT
            return
        sys.stderr.flush()
    except (OSError, ValueError, AttributeError):
        # A closed/absent/broken capture stream must not change test outcomes.
        _phase_records = _PHASE_LIMIT


_phase("entry")
_phase("imports-start")

import contextlib
import copy
import ctypes
import errno
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import stat
import subprocess
import tempfile
import time
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
from functools import lru_cache
from pathlib import Path
from typing import Any


_NATIVE_FSYNC = os.fsync


_phase("imports-finish")
_phase("checker-import-start")
MODULE_PATH = Path(__file__).with_name("check_acceptance_matrix.py")
SPEC = importlib.util.spec_from_file_location("check_acceptance_matrix", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)
_phase("checker-import-finish")

_phase("public-import-start")
PUBLIC_SPEC = importlib.util.spec_from_file_location(
    "acceptance_public_tree", MODULE_PATH.with_name("validate_public_tree.py"))
assert PUBLIC_SPEC and PUBLIC_SPEC.loader
PUBLIC_TREE = importlib.util.module_from_spec(PUBLIC_SPEC)
sys.modules[PUBLIC_SPEC.name] = PUBLIC_TREE
PUBLIC_SPEC.loader.exec_module(PUBLIC_TREE)
_phase("public-import-finish")


class _PhaseCase(unittest.TestCase):
    def run(self, result: Any = None) -> Any:
        # Keep the original result, setup/teardown, assertions and exit behavior.
        identity = (type(self).__name__[:128] + "." + self._testMethodName[:128])[:128]
        _phase("case-start", case=identity)
        # One reservation unit is a child launch or an outer CLI/transaction
        # operation on slow storage (up to one second). Count actual helper
        # calls, including failed calls and nested loops, rather than test names.
        work = {"count": 0, "depth": 0}
        self._storage_work = work
        def counted(function):
            def invoke(*args, **kwargs):
                if work["depth"] == 0:
                    work["count"] += 1
                work["depth"] += 1
                try:
                    return function(*args, **kwargs)
                finally:
                    work["depth"] -= 1
            return invoke
        def check_reservation():
            self.assertLessEqual(work["count"], _STORAGE_ALLOWANCES.get(type(self).__name__ + "." + self._testMethodName, 1), identity)
        self.addCleanup(check_reservation)
        try:
            with contextlib.ExitStack() as stack:
                for owner, name in ((MODULE, "main"), (MODULE, "_replace_outputs"),
                                    (MODULE, "_recover_outputs"), (MODULE._PreparationCustody, "create")):
                    stack.enter_context(mock.patch.object(owner, name, counted(getattr(owner, name))))
                return super().run(result)
        finally:
            _phase("case-finish", case=identity)


class _PhaseProgram(unittest.TestProgram):
    def createTests(self, *args: Any, **kwargs: Any) -> None:
        _phase("selection-start")
        super().createTests(*args, **kwargs)
        _phase("selection-finish")


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
        # Strict contract/JSON readers also use Path.read_text directly. Their
        # bytes are immutable during these in-memory mutation sweeps too.
        stack.enter_context(mock.patch.object(Path, "read_text", lru_cache(maxsize=256)(Path.read_text)))
        for name in ("_read_text", "_path_problem", "load_required_ids"):
            cached = lru_cache(maxsize=256)(getattr(MODULE, name))
            stack.enter_context(mock.patch.object(MODULE, name, cached))
        source_problem = MODULE._source_problem
        @lru_cache(maxsize=256)
        def source_result(base: Path, source: str) -> str | None:
            # Only filesystem-derived answers are shared. Every mutated JSON
            # binding is still checked by validate/validate_inventory itself.
            return source_problem(base, source, {})
        stack.enter_context(mock.patch.object(MODULE, "_source_problem",
            lambda base, source, documents: source_result(base, source)))
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


class AcceptanceFixture(_PhaseCase):
    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.real_fsync = staticmethod(os.fsync)
        # Build and index the immutable synthetic repository once per class.
        seed = cls("runTest")
        try:
            seed._create_fixture()
            cls.fixture_files = tuple((path.relative_to(seed.root), path.read_bytes())
                                      for path in seed.root.rglob("*") if path.is_file())
            cls.fixture_directories = tuple(sorted(
                {path.parent for path, _ in cls.fixture_files if path.parent != Path(".")},
                key=lambda path: (len(path.parts), path.as_posix())))
            cls.fixture_inventory = seed.inventory
            cls.fixture_index = seed.index
        finally:
            seed.doCleanups()

    def setUp(self) -> None:
        # Parser and repository-only checks need no mutable synthetic tree.
        # Create it on first use; every filesystem test still owns fresh files.
        self._fixture_pending = True
        self.index = self.fixture_index
        # Logical fault/interleaving tests exercise flush ordering and injected
        # sync failures. Physical disk barriers are qualified separately below
        # and in every actual process-termination case, not repeated per fault.
        sync_patch = mock.patch.object(os, "fsync", autospec=_NATIVE_FSYNC)
        self.sync = sync_patch.start()
        self.addCleanup(sync_patch.stop)

    def __getattr__(self, name: str) -> Any:
        if self.__dict__.get("_fixture_pending", False):
            if name in {"tmp", "matrix_path"}:
                self._temporary_fixture()
                return getattr(self, name)
            if name in {"root", "inventory"}:
                self._fixture_pending = False
                self._materialize_fixture()
                return getattr(self, name)
        raise AttributeError(name)

    def _temporary_fixture(self) -> None:
        if "tmp" not in self.__dict__:
            tmp = tempfile.TemporaryDirectory()
            self.addCleanup(tmp.cleanup)
            self.tmp = Path(tmp.name).resolve()
            self.matrix_path = self.tmp / "matrix.json"

    def _materialize_fixture(self) -> None:
        self._temporary_fixture()
        self.root = self.tmp / "repo"
        self.root.mkdir()
        for relative in self.fixture_directories:
            (self.root / relative).mkdir()
        for relative, data in self.fixture_files:
            (self.root / relative).write_bytes(data)
        self.inventory = copy.deepcopy(self.fixture_inventory)
        self.index = self.fixture_index
        self.matrix_path = self.tmp / "matrix.json"

    def _create_fixture(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        # Match the CLI's resolved root, including directory aliases on macOS
        # and Windows short temporary-directory names, so fault injections hit.
        self.tmp = Path(tmp.name).resolve()
        self.root = self.tmp / "repo"
        (self.root / "test").mkdir(parents=True)
        (self.root / ".git").mkdir()
        (self.root / ".git/acceptance-refresh.lock").touch()
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

    def write_matrix(self, matrix: Any) -> None:
        # CLI fault tests must receive their complete repository before they
        # inject failures into publication and filesystem primitives.
        _ = self.root
        if isinstance(matrix, str):
            self.matrix_path.write_text(matrix, encoding="utf-8")
        else:
            self.matrix_path.write_text(json.dumps(matrix, indent=2), encoding="utf-8")

    def run_main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = MODULE.main(list(args))
        self.remember_custody(err.getvalue())
        return code, out.getvalue(), err.getvalue()

    def remember_custody(self, diagnostics: str) -> None:
        # Exercise the real caller receipt transport across invocations, rather
        # than reconstructing authority from a recovery filename or its bytes.
        for line in diagnostics.splitlines():
            if line.startswith("preparation custody receipt: "):
                self.caller_custody = line.removeprefix("preparation custody receipt: ")

    def base_args(self) -> list[str]:
        if "caller_directory" not in self.__dict__:
            directory = tempfile.TemporaryDirectory(prefix="acceptance-caller-")
            self.addCleanup(directory.cleanup)
            self.caller_directory = Path(directory.name).resolve()
        self.receipt_number = self.__dict__.get("receipt_number", 0) + 1
        args = ["--root", str(self.root), "--matrix", str(self.matrix_path)]
        receipt = self.__dict__.get("caller_custody")
        if receipt is not None and MODULE._refresh_lock_path(self.root).with_suffix(".preparation").exists():
            args.extend(("--preparation-custody", receipt))
        args.extend(("--preparation-receipt-file", str(self.caller_directory / (str(self.receipt_number) + ".json"))))
        return args

    def prepare_refresh(self, *, generated_evidence: bool = True) -> dict[str, Any]:
        matrix = baseline_matrix()
        matrix["rows"][0]["requirements"].remove("ar.synthetic")
        matrix["rows"].append({
            "id": "iss.126", "source": MODULE.CONTRACT_DOCUMENTS[1] + "#contract",
            "statement": "Every requirement has evidence.", "status": "covered",
            "requirements": ["ar.synthetic"],
            "evidence": [{"kind": "script", "ref": "tools/script.py"}],
        })
        if generated_evidence:
            for row in (matrix["rows"][0], matrix["rows"][-1]):
                row["evidence"].append({"kind": "doc", "ref": "docs/acceptance-matrix.md"})
        self.write_matrix(matrix)
        doc = self.root / "docs/acceptance-matrix.md"
        doc.write_bytes(MODULE.render_doc(matrix).encode("utf-8"))
        return matrix

    def prepare_rollback(self, *, generated_evidence: bool = True) -> tuple[tuple[Path, ...], dict[Path, bytes]]:
        self.write_inventory()
        self.prepare_refresh(generated_evidence=generated_evidence)
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        paths = (self.root / "docs/acceptance-matrix.md", self.matrix_path,
                 self.root / "tools/ci/acceptance_requirements.json")
        return paths, {path: path.read_bytes() for path in paths}

    def crash_publication(self, count: int, options: list[str]) -> None:
        program = """
import os, pathlib, importlib.util, sys
spec = importlib.util.spec_from_file_location("acceptance_child", sys.argv[1])
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
module = checker.__dict__
replace = pathlib.Path.replace
remaining = int(sys.argv[2])
def publish(path, target):
    global remaining
    result = replace(path, target)
    remaining -= 1
    if remaining == 0:
        os._exit(73)
    return result
pathlib.Path.replace = publish
raise SystemExit(module["main"](sys.argv[3:]))
"""
        child = subprocess.run([sys.executable, "-I", "-B", "-c", program,
                                str(MODULE_PATH), str(count), *self.base_args(), *options],
                               capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
        self.remember_custody(child.stderr)
        self.assertEqual(child.returncode, 73, (child.stdout, child.stderr))

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

    @contextlib.contextmanager
    def retargetable_path(self, alias: Path, *, denied: bool = False) -> Any:
        """Use real links where permitted, and equivalent resolution otherwise."""
        first = alias.with_name("original-" + alias.name)
        second = alias.with_name("retargeted-" + alias.name)
        directory = alias.is_dir()
        alias.rename(first)
        duplicate = shutil.copytree if directory else shutil.copyfile
        duplicate(first, second)
        current = first
        linked = False
        with contextlib.ExitStack() as stack:
            try:
                if denied:
                    stack.enter_context(mock.patch.object(Path, "symlink_to", side_effect=PermissionError("denied")))
                try:
                    alias.symlink_to(first, target_is_directory=directory)
                    linked = True
                except OSError:
                    duplicate(first, alias)
                    # This model changes link topology only through retarget;
                    # ordinary publication replaces regular files. Cache only
                    # resolution (256 names), never bytes, status or identities.
                    # The cache belongs to this one synthetic link context.
                    resolve = lru_cache(maxsize=256)(Path.resolve)
                    def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                        # Resolve parent components after following the alias,
                        # as a real directory link or Windows short name does.
                        if path == alias:
                            path = current
                        elif directory and path.is_relative_to(alias):
                            path = current / path.relative_to(alias)
                        return resolve(path, *args, **kwargs)
                    stack.enter_context(mock.patch.object(Path, "resolve", resolved))
                def retarget() -> None:
                    nonlocal current
                    current = second
                    if linked:
                        alias.unlink()
                        alias.symlink_to(second, target_is_directory=directory)
                    else:
                        resolve.cache_clear()
                yield first, second, retarget
            finally:
                if linked or not directory:
                    alias.unlink()
                else:
                    shutil.rmtree(alias)
                first.rename(alias)
                if directory:
                    shutil.rmtree(second)
                else:
                    second.unlink()

    def write_junit(self, path: Path, cases: list[dict[str, Any]]) -> None:
        suite = ET.Element("testsuite")
        for spec in cases:
            spec = dict(spec)
            adverse = spec.pop("adverse", None)
            case = ET.SubElement(suite, "testcase", spec)
            if adverse:
                ET.SubElement(case, adverse)
        ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


class AcceptanceMatrixTests(AcceptanceFixture):
    def test_native_publication_then_interrupt_restores_every_staged_destination(self) -> None:
        for journaled in (False, True):
            for existing in (False, True):
                for boundary in (1, 2, 3):
                    with self.subTest(journaled=journaled, existing=existing, boundary=boundary), \
                            tempfile.TemporaryDirectory() as directory:
                        base = Path(directory).resolve()
                        paths = [base / str(index) for index in range(3)]
                        before = {path: (b"original " + path.name.encode()) if existing else None for path in paths}
                        for path, data in before.items():
                            if data is not None:
                                path.write_bytes(data)
                                path.chmod(0o640)
                        modes = {path: stat.S_IMODE(path.stat().st_mode) for path in paths if path.exists()}
                        journal = base / "refresh.transaction" if journaled else None
                        replace = Path.replace
                        count = 0
                        def interrupted(source: Path, target: Path) -> Path:
                            nonlocal count
                            result = replace(source, target)
                            count += 1
                            if count == boundary:
                                raise KeyboardInterrupt("publication completed")
                            return result
                        with mock.patch.object(Path, "replace", interrupted), self.assertRaises(KeyboardInterrupt):
                            MODULE._replace_outputs([(path, "candidate " + path.name) for path in paths],
                                                    journal=journal, private_output=paths[0])
                        self.assertEqual({path: path.read_bytes() if path.exists() else None for path in paths}, before)
                        self.assertEqual({path: stat.S_IMODE(path.stat().st_mode) for path in modes}, modes)
                        self.assertEqual(list(base.glob(".acceptance-*")), [])
                        if journal is not None:
                            self.assertFalse(journal.exists())

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            output, journal = base / "output", base / "refresh.transaction"
            output.write_bytes(b"original")
            metadata = output.stat()
            with mock.patch.object(Path, "replace") as publish:
                MODULE._replace_outputs([], journal=journal)
                MODULE._replace_outputs([(output, "original")] * 2, journal=journal)
                publish.assert_not_called()
            self.assertTrue(os.path.samestat(metadata, output.stat()))
            self.assertEqual(metadata.st_mtime_ns, output.stat().st_mtime_ns)
            replace = Path.replace
            with mock.patch.object(Path, "replace", autospec=True, side_effect=replace) as publish:
                MODULE._replace_outputs([(output, "candidate")] * 2, journal=journal)
                self.assertEqual(publish.call_count, 1)
            self.assertEqual(output.read_bytes(), b"candidate")
            self.assertFalse(journal.exists())
            self.assertEqual(list(base.glob(".acceptance-*")), [])

    def test_interrupt_reconciliation_preserves_external_output_and_source_occupants(self) -> None:
        for journaled in (False, True):
            for occupant in ("output", "source"):
                for contents in (b"candidate", b"original", b"external bytes"):
                    with self.subTest(journaled=journaled, occupant=occupant, contents=contents), \
                            tempfile.TemporaryDirectory() as directory:
                        base = Path(directory).resolve()
                        first, second, displaced = (base / name for name in ("first", "second", "displaced"))
                        first.write_bytes(b"first original")
                        second.write_bytes(b"original")
                        journal = base / "refresh.transaction" if journaled else None
                        replace = Path.replace
                        def interrupted(source: Path, target: Path) -> Path:
                            if target != second:
                                return replace(source, target)
                            # Keep the old inode allocated. A same-byte external
                            # occupant must not reuse the just-unlinked original's
                            # inode and accidentally satisfy restoration identity.
                            target.rename(base / "retained-original")
                            if occupant == "source":
                                source.rename(displaced)
                                source.write_bytes(contents)
                            replace(source, target)
                            if occupant == "output":
                                target.rename(displaced)
                                target.write_bytes(contents)
                            self.assertFalse(os.path.samestat(target.stat(), (base / "retained-original").stat()))
                            raise KeyboardInterrupt("external occupant after native publication")
                        with mock.patch.object(Path, "replace", interrupted), \
                                self.assertRaisesRegex(MODULE.MatrixError, "rollback incomplete"):
                            MODULE._replace_outputs([(first, "first candidate"), (second, "candidate")], journal=journal)
                        self.assertEqual(first.read_bytes(), b"first original")
                        self.assertEqual(second.read_bytes(), contents)
                        self.assertCountEqual([path.read_bytes() for path in base.glob(".acceptance-*")],
                                              [b"first original", b"original"])
                        if journal is not None:
                            self.assertTrue(journal.exists())

    def test_interrupted_native_restore_and_removal_are_reconciled_before_cleanup(self) -> None:
        for journaled in (False, True):
            for existing in (False, True):
                with self.subTest(journaled=journaled, existing=existing), tempfile.TemporaryDirectory() as directory:
                    base = Path(directory).resolve()
                    paths = [base / str(index) for index in range(3)]
                    if existing:
                        for path in paths:
                            path.write_bytes(b"original " + path.name.encode())
                    journal = base / "refresh.transaction" if journaled else None
                    replace, restore, unlink = Path.replace, os.replace, Path.unlink
                    restores = []
                    active = False
                    def publish(source: Path, target: Path) -> Path:
                        nonlocal active
                        result = replace(source, target)
                        if target == paths[-1]:
                            active = True
                            raise KeyboardInterrupt("publication completed")
                        return result
                    def interrupted_restore(source: Any, target: Any) -> None:
                        restore(source, target)
                        if active and Path(target) in paths:
                            restores.append(Path(target))
                            raise KeyboardInterrupt("restore completed")
                    def interrupted_removal(path: Path, *args: Any, **kwargs: Any) -> None:
                        unlink(path, *args, **kwargs)
                        if active and path in paths:
                            restores.append(path)
                            raise KeyboardInterrupt("removal completed")
                    with mock.patch.object(Path, "replace", publish), \
                            mock.patch.object(os, "replace", interrupted_restore), \
                            mock.patch.object(Path, "unlink", interrupted_removal), self.assertRaises(KeyboardInterrupt):
                        MODULE._replace_outputs([(path, "candidate") for path in paths], journal=journal)
                    self.assertEqual(restores, list(reversed(paths)))
                    self.assertEqual([path.read_bytes() if path.exists() else None for path in paths],
                                     [b"original " + path.name.encode() if existing else None for path in paths])
                    self.assertEqual(list(base.glob(".acceptance-*")), [])
                    if journal is not None:
                        self.assertFalse(journal.exists())

    def test_restore_interrupt_before_native_operation_retains_copies_for_recovery(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            first, second, journal = (base / name for name in ("first", "second", "refresh.transaction"))
            for path in (first, second):
                path.write_bytes(b"original " + path.name.encode())
            replace, restore = Path.replace, os.replace
            active = False
            def publish(source: Path, target: Path) -> Path:
                nonlocal active
                result = replace(source, target)
                if target == second:
                    active = True
                    raise KeyboardInterrupt("publication completed")
                return result
            def interrupted(source: Any, target: Any) -> None:
                if active and Path(target) == second:
                    raise KeyboardInterrupt("restore not performed")
                restore(source, target)
            with mock.patch.object(Path, "replace", publish), mock.patch.object(os, "replace", interrupted), \
                    self.assertRaisesRegex(MODULE.MatrixError, "rollback incomplete"):
                MODULE._replace_outputs([(first, "candidate"), (second, "candidate")], journal=journal)
            self.assertEqual(first.read_bytes(), b"original first")
            self.assertEqual(second.read_bytes(), b"candidate")
            self.assertCountEqual([path.read_bytes() for path in base.glob(".acceptance-*")],
                                  [b"original first", b"original second"])
            self.assertTrue(journal.exists())
            MODULE._recover_outputs(journal)
            self.assertEqual(second.read_bytes(), b"original second")
            self.assertFalse(journal.exists())
            self.assertEqual(list(base.glob(".acceptance-*")), [])

    def test_restored_set_is_reverified_before_discarding_any_recovery_copy(self) -> None:
        for replacement in (False, True):
            with self.subTest(replacement=replacement), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                paths = [base / str(index) for index in range(3)]
                for path in paths:
                    path.write_bytes(b"original " + path.name.encode())
                replace, restore = Path.replace, os.replace
                active = False
                def publish(source: Path, target: Path) -> Path:
                    nonlocal active
                    result = replace(source, target)
                    if target == paths[-1]:
                        active = True
                        raise KeyboardInterrupt("publication completed")
                    return result
                def change_restored(source: Any, target: Any) -> None:
                    restore(source, target)
                    if active and Path(target) == paths[1]:
                        if replacement:
                            paths[-1].rename(base / "displaced")
                        paths[-1].write_bytes(b"external edit")
                with mock.patch.object(Path, "replace", publish), mock.patch.object(os, "replace", change_restored), \
                        self.assertRaisesRegex(MODULE.MatrixError, "rollback incomplete"):
                    MODULE._replace_outputs([(path, "candidate") for path in paths])
                self.assertEqual(paths[-1].read_bytes(), b"external edit")
                self.assertCountEqual([path.read_bytes() for path in base.glob(".acceptance-*")],
                                      [b"original " + path.name.encode() for path in paths])

    def test_restore_candidate_substitution_is_never_adopted_after_native_success(self) -> None:
        for contents in (b"original", b"external"):
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                output, displaced = base / "output", base / "displaced"
                output.write_bytes(b"original")
                replace, restore = Path.replace, os.replace
                active = False
                def publish(source: Path, target: Path) -> Path:
                    nonlocal active
                    replace(source, target)
                    active = True
                    raise KeyboardInterrupt("publication completed")
                def substitute(source: Any, target: Any) -> None:
                    if not active:
                        restore(source, target)
                        return
                    Path(source).rename(displaced)
                    Path(source).write_bytes(contents)
                    restore(source, target)
                    raise KeyboardInterrupt("foreign restore completed")
                with mock.patch.object(Path, "replace", publish), mock.patch.object(os, "replace", substitute), \
                        self.assertRaisesRegex(MODULE.MatrixError, "rollback incomplete"):
                    MODULE._replace_outputs([(output, "candidate")])
                self.assertEqual(output.read_bytes(), contents)
                self.assertEqual(displaced.read_bytes(), b"original")
                self.assertEqual([path.read_bytes() for path in base.glob(".acceptance-*")], [b"original"])

    def test_journal_native_publication_interrupt_covers_prepare_commit_and_rollback_marker(self) -> None:
        for boundary, completed in ((1, True), (2, True), (3, False), (3, True)):
            with self.subTest(boundary=boundary, completed=completed), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                output, journal = base / "output", base / "refresh.transaction"
                output.write_bytes(b"original")
                replace = os.replace
                count = 0
                def interrupted(source: Any, target: Any) -> None:
                    nonlocal count
                    if Path(target) == journal:
                        count += 1
                        if count == boundary and not completed:
                            raise KeyboardInterrupt("journal not published")
                    replace(source, target)
                    if Path(target) == journal and (count == boundary or (boundary == 3 and count == 2)):
                        raise KeyboardInterrupt("journal publication completed")
                with mock.patch.object(os, "replace", interrupted):
                    if boundary == 3:
                        with self.assertRaisesRegex(MODULE.MatrixError, "rollback journal unavailable"):
                            MODULE._replace_outputs([(output, "candidate")], journal=journal)
                    else:
                        with self.assertRaises(KeyboardInterrupt):
                            MODULE._replace_outputs([(output, "candidate")], journal=journal)
                if boundary == 3:
                    self.assertEqual(output.read_bytes(), b"candidate")
                    self.assertTrue(journal.exists())
                    self.assertEqual([path.read_bytes() for path in base.glob(".acceptance-*")], [b"original"])
                    MODULE._recover_outputs(journal)
                self.assertEqual(output.read_bytes(), b"candidate" if boundary == 3 and not completed else b"original")
                self.assertFalse(journal.exists())
                self.assertEqual(list(base.glob(".acceptance-*")), [])

        for committed in (False, True):
            with self.subTest(committed=committed), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                first, second, journal = (base / name for name in ("first", "second", "refresh.transaction"))
                first.write_bytes(b"first original")
                second.write_bytes(b"second original")
                replace, unlink = Path.replace, Path.unlink
                removed = False
                def publish(source: Path, target: Path) -> Path:
                    result = replace(source, target)
                    if target == second and not committed:
                        raise KeyboardInterrupt("publication completed")
                    return result
                def interrupted_cleanup(path: Path, *args: Any, **kwargs: Any) -> None:
                    nonlocal removed
                    unlink(path, *args, **kwargs)
                    if path.name.startswith(".acceptance-") and not removed:
                        removed = True
                        raise KeyboardInterrupt("cleanup completed")
                with mock.patch.object(Path, "replace", publish), mock.patch.object(Path, "unlink", interrupted_cleanup), \
                        self.assertRaises(KeyboardInterrupt):
                    MODULE._replace_outputs([(first, "first candidate"), (second, "second candidate")], journal=journal)
                self.assertTrue(removed)
                self.assertTrue(journal.exists())
                MODULE._recover_outputs(journal)
                self.assertEqual(first.read_bytes(), b"first candidate" if committed else b"first original")
                self.assertEqual(second.read_bytes(), b"second candidate" if committed else b"second original")
                self.assertFalse(journal.exists())
                self.assertEqual(list(base.glob(".acceptance-*")), [])

    def test_in_place_candidate_edits_are_preserved_during_refusal_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            output = base / "output"
            output.write_bytes(b"original")
            changed = []
            def refuse(published: Any) -> None:
                for path in base.glob(".acceptance-*"):
                    if path.read_bytes() == b"candidate":
                        path.write_bytes(b"external candidate edit")
                        changed.append(path)
                raise OSError("input changed")
            with self.assertRaisesRegex(MODULE.MatrixError, "identity or content changed"):
                MODULE._replace_outputs([(output, "candidate")], verify=refuse)
            self.assertEqual(output.read_bytes(), b"original")
            self.assertEqual(len(changed), 1)
            self.assertEqual(changed[0].read_bytes(), b"external candidate edit")

        for operation in ("write", "flush", "sync"):
            for stage in (1, 2):
                with self.subTest(operation=operation, stage=stage), tempfile.TemporaryDirectory() as directory:
                    base = Path(directory).resolve()
                    output = base / "output"
                    output.write_bytes(b"original")
                    opened = 0
                    create, sync = open, os.fsync
                    def interrupted_open(*args: Any, **kwargs: Any) -> Any:
                        nonlocal opened
                        handle = create(*args, **kwargs)
                        opened += 1
                        if opened == stage and operation != "sync":
                            original = getattr(handle, operation)
                            def interrupted(*values: Any) -> Any:
                                original(*values)
                                raise KeyboardInterrupt("staging operation completed")
                            setattr(handle, operation, interrupted)
                        return handle
                    def interrupted_sync(descriptor: int) -> None:
                        sync(descriptor)
                        if opened == stage and operation == "sync":
                            raise KeyboardInterrupt("sync completed")
                    with mock.patch.object(MODULE, "open", interrupted_open, create=True), \
                            mock.patch.object(os, "fsync", interrupted_sync), self.assertRaises(KeyboardInterrupt):
                        MODULE._replace_outputs([(output, "candidate")])
                    self.assertEqual(output.read_bytes(), b"original")
                    self.assertEqual(list(base.glob(".acceptance-*")), [])

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            output, journal = base / "output", base / "refresh.transaction"
            output.write_bytes(b"original")
            candidate = journal.with_suffix(".pending")
            replace = os.replace
            def edited_pending(source: Any, target: Any) -> None:
                if Path(target) == journal:
                    Path(source).write_bytes(b"external journal candidate edit")
                    raise KeyboardInterrupt("journal publication refused")
                replace(source, target)
            with mock.patch.object(os, "replace", edited_pending), \
                    self.assertRaisesRegex(MODULE.MatrixError, "identity or content changed"):
                MODULE._replace_outputs([(output, "candidate")], journal=journal)
            self.assertEqual(candidate.read_bytes(), b"external journal candidate edit")
            self.assertEqual(output.read_bytes(), b"original")
            self.assertFalse(journal.exists())
            self.assertEqual(list(base.glob(".acceptance-*")), [])

    def test_direct_publication_refuses_external_destination_changes_before_replace(self) -> None:
        for existing in (False, True):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                output = base / "output"
                if existing:
                    output.write_bytes(b"original")
                changed = False
                def edit(published: Any) -> None:
                    nonlocal changed
                    if not changed:
                        changed = True
                        if output.exists():
                            output.rename(base / "displaced")
                        output.write_bytes(b"external bytes")
                with mock.patch.object(Path, "replace") as publish, \
                        self.assertRaisesRegex(MODULE.MatrixError, "rollback incomplete"):
                    MODULE._replace_outputs([(output, "candidate")], verify=edit)
                publish.assert_not_called()
                self.assertEqual(output.read_bytes(), b"external bytes")
                self.assertEqual([path.read_bytes() for path in base.glob(".acceptance-*")],
                                 [b"original"] if existing else [])

    @contextlib.contextmanager
    def recovery_transaction(self) -> Any:
        # Two tiny files exercise the production journal reader and restorer
        # without materializing or indexing a synthetic source repository.
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            paths = [base / str(index) for index in range(2)]
            entries, backups = [], []
            for index, path in enumerate(paths):
                path.write_bytes(b"original " + path.name.encode())
                backup, candidate = [base / (".acceptance-" + f"{2 * index + offset:032x}")
                                     for offset in (0, 1)]
                backup.write_bytes(path.read_bytes())
                candidate.write_bytes(b"candidate " + path.name.encode())
                backups.append(backup)
                entries.append({"path": str(path), "old": MODULE._transaction_record(backup)["sha256"],
                                "backup": MODULE._transaction_record(backup),
                                "new": MODULE._transaction_record(candidate)})
            journal = base / "refresh.transaction"
            MODULE._write_transaction(journal, entries, committed=False)
            for entry in entries:
                Path(entry["new"]["path"]).replace(entry["path"])
            yield base, paths, backups, journal

    def test_native_recovery_interrupt_keeps_independent_original_copies_until_verification(self) -> None:
        with self.recovery_transaction() as (base, paths, backups, journal):
            replace = os.replace
            def interrupted(source: Any, target: Any) -> None:
                replace(source, target)
                if Path(target) in paths:
                    self.assertTrue(all(backup.exists() for backup in backups))
                    raise KeyboardInterrupt("recovery restore completed")
            with mock.patch.object(os, "replace", interrupted), self.assertRaises(KeyboardInterrupt):
                MODULE._recover_outputs(journal)
            self.assertEqual(json.loads(journal.read_bytes())["version"], 2)
            self.assertEqual([backup.read_bytes() for backup in backups], [b"original 0", b"original 1"])
            # A missing already-used source is expected after native success.
            MODULE._recover_outputs(journal)
            self.assertEqual([path.read_bytes() for path in paths], [b"original 0", b"original 1"])
            self.assertFalse(journal.exists())
            self.assertEqual(list(base.glob(".acceptance-*")), [])

        for completed in (False, True):
            with self.subTest(journal_completed=completed), self.recovery_transaction() as (base, paths, backups, journal):
                replace = os.replace
                def interrupted_marker(source: Any, target: Any) -> None:
                    if Path(target) == journal and not completed:
                        raise KeyboardInterrupt("recovery journal not published")
                    replace(source, target)
                    if Path(target) == journal:
                        raise KeyboardInterrupt("recovery journal completed")
                with mock.patch.object(os, "replace", interrupted_marker), self.assertRaises(KeyboardInterrupt):
                    MODULE._recover_outputs(journal)
                self.assertEqual([path.read_bytes() for path in paths], [b"candidate 0", b"candidate 1"])
                self.assertEqual([backup.read_bytes() for backup in backups], [b"original 0", b"original 1"])
                self.assertEqual(len(list(base.glob(".acceptance-*"))), 3 if completed else 2)
                MODULE._recover_outputs(journal)
                self.assertEqual([path.read_bytes() for path in paths], [b"original 0", b"original 1"])
                self.assertFalse(journal.exists())
                self.assertEqual(list(base.glob(".acceptance-*")), [])

    def test_restart_recovery_verifies_the_entire_restored_set_before_cleanup(self) -> None:
        with self.recovery_transaction() as (_, paths, backups, journal):
            replace = os.replace
            def external_edit(source: Any, target: Any) -> None:
                replace(source, target)
                if Path(target) == paths[0]:
                    paths[-1].write_bytes(b"external edit after restore")
            with mock.patch.object(os, "replace", external_edit), \
                    self.assertRaisesRegex(MODULE.MatrixError, "recovery copies retained"):
                MODULE._recover_outputs(journal)
            self.assertEqual(paths[-1].read_bytes(), b"external edit after restore")
            self.assertEqual([backup.read_bytes() for backup in backups], [b"original 0", b"original 1"])
            self.assertTrue(journal.exists())

    def test_restoration_journal_rejects_invalid_records_and_unreadable_or_partial_candidates(self) -> None:
        with self.recovery_transaction() as (base, paths, backups, journal):
            write = MODULE._write_transaction
            def interrupted(*args: Any, **kwargs: Any) -> None:
                write(*args, **kwargs)
                raise KeyboardInterrupt("recovery journal completed")
            with mock.patch.object(MODULE, "_write_transaction", interrupted), self.assertRaises(KeyboardInterrupt):
                MODULE._recover_outputs(journal)
            baseline = json.loads(journal.read_bytes())
            self.assertEqual(baseline["version"], 2)
            unchanged = journal.read_bytes()
            with self.assertRaisesRegex(MODULE.MatrixError, "invalid restoration count"):
                MODULE._write_transaction(journal, baseline["outputs"], committed=False, restorations=[])
            self.assertEqual(journal.read_bytes(), unchanged)
            changes = (
                lambda value: value.update(restorations=[]),
                lambda value: value.update(restorations={}),
                lambda value: value["restorations"].__setitem__(1, value["outputs"][1]["backup"]),
                lambda value: value["restorations"][1].update(sha256="0" * 64),
                lambda value: value["restorations"][1].update(inode=0),
                lambda value: value["restorations"][1].update(size=MODULE._TRANSACTION_BYTES + 1),
            )
            for index, change in enumerate(changes):
                with self.subTest(mutation=index):
                    value = copy.deepcopy(baseline)
                    change(value)
                    raw = json.dumps(value).encode()
                    journal.write_bytes(raw)
                    with self.assertRaises(MODULE.MatrixError), mock.patch.object(os, "replace") as replace:
                        MODULE._recover_outputs(journal)
                    replace.assert_not_called()
                    self.assertEqual(journal.read_bytes(), raw)
                    self.assertEqual([path.read_bytes() for path in paths], [b"candidate 0", b"candidate 1"])
                    self.assertTrue(all(backup.exists() for backup in backups))
            journal.write_text(json.dumps(baseline), encoding="utf-8")
            candidate = Path(baseline["restorations"][1]["path"])
            read = MODULE._read_transaction_file
            for failure in ("unreadable", "partial", "unknown"):
                with self.subTest(failure=failure):
                    def damaged(path: Path, limit: int) -> Any:
                        if path == candidate and failure == "unreadable":
                            raise PermissionError("restore candidate unreadable")
                        data, metadata = read(path, limit)
                        if path == candidate:
                            if failure == "partial":
                                data = data[:1]
                            elif failure == "unknown":
                                fields = list(metadata)
                                fields[1] = 0
                                metadata = os.stat_result(fields)
                        return data, metadata
                    with mock.patch.object(MODULE, "_read_transaction_file", damaged), \
                            self.assertRaises((MODULE.MatrixError, OSError)), mock.patch.object(os, "replace") as replace:
                        MODULE._recover_outputs(journal)
                    replace.assert_not_called()
                    self.assertTrue(journal.exists())
                    self.assertTrue(all(backup.exists() for backup in backups))
            MODULE._recover_outputs(journal)
            self.assertEqual([path.read_bytes() for path in paths], [b"original 0", b"original 1"])
            self.assertEqual(list(base.glob(".acceptance-*")), [])

    def test_private_receipt_native_publication_interrupt_rolls_back_before_accounting(self) -> None:
        self.prepare_refresh()
        discovery, report = self.execution_inputs()
        receipt = self.tmp / "receipt.json"
        replace = Path.replace
        observed = []
        def interrupted(source: Path, target: Path) -> Path:
            result = replace(source, target)
            if target == receipt:
                observed.append(stat.S_IMODE(target.stat().st_mode))
                raise KeyboardInterrupt("receipt publication completed")
            return result
        with mock.patch.object(Path, "replace", interrupted), self.assertRaises(KeyboardInterrupt):
            self.run_main(*self.base_args(), "--junit", str(report), "--discovery", str(discovery),
                          "--execution-receipt", str(receipt))
        self.assertEqual(len(observed), 1)
        if os.name != "nt":
            self.assertEqual(observed, [0o600])
        self.assertFalse(receipt.exists())
        self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])
        self.assertFalse(MODULE._refresh_lock_path(self.root).with_suffix(".transaction").exists())

    def test_recovery_candidate_failures_keep_original_copies_and_publication_unchanged(self) -> None:
        for failure in ("create", "partial", "flush", "sync", "mode", "unknown"):
            with self.subTest(failure=failure), self.recovery_transaction() as (base, paths, backups, journal):
                create, sync, chmod = open, os.fsync, Path.chmod
                metadata = MODULE._transaction_metadata
                def opened(*args: Any, **kwargs: Any) -> Any:
                    if failure == "create":
                        raise PermissionError("recovery creation denied")
                    handle = create(*args, **kwargs)
                    if failure == "partial":
                        write = handle.write
                        def partial(data: bytes) -> int:
                            write(data[:1])
                            return 1
                        handle.write = partial
                    elif failure == "flush":
                        handle.flush = mock.Mock(side_effect=OSError("recovery flush denied"))
                    return handle
                def denied_sync(descriptor: int) -> None:
                    if failure == "sync":
                        raise OSError("recovery sync denied")
                    sync(descriptor)
                def interrupted_mode(path: Path, mode: int) -> None:
                    chmod(path, mode)
                    if failure == "mode" and path.name.startswith(".acceptance-"):
                        raise OSError("recovery mode copy interrupted")
                def unknown(handle: Any) -> Any:
                    result = metadata(handle)
                    if failure == "unknown" and isinstance(handle.name, str):
                        fields = list(result)
                        fields[1] = 0
                        return os.stat_result(fields)
                    return result
                with mock.patch.object(MODULE, "open", opened, create=True), \
                        mock.patch.object(os, "fsync", denied_sync), \
                        mock.patch.object(Path, "chmod", interrupted_mode), \
                        mock.patch.object(MODULE, "_transaction_metadata", unknown), \
                        self.assertRaises((MODULE.MatrixError, OSError)):
                    MODULE._recover_outputs(journal)
                self.assertEqual([path.read_bytes() for path in paths], [b"candidate 0", b"candidate 1"])
                self.assertEqual([backup.read_bytes() for backup in backups], [b"original 0", b"original 1"])
                self.assertTrue(journal.exists())
                extras = set(base.glob(".acceptance-*")) - set(backups)
                self.assertEqual(len(extras), 1 if failure == "unknown" else 0)
                for extra in extras:
                    extra.unlink()
                if failure == "unknown":
                    marker = journal.with_suffix(".preparation")
                    retained = marker.read_bytes()
                    with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                        MODULE._recover_outputs(journal)
                    self.assertEqual(marker.read_bytes(), retained)
                    self.assertEqual([path.read_bytes() for path in paths], [b"candidate 0", b"candidate 1"])
                    self.assertEqual([backup.read_bytes() for backup in backups], [b"original 0", b"original 1"])
                    marker.unlink()  # Explicit fixture teardown after resolving its injected uncertainty.
                MODULE._recover_outputs(journal)
                self.assertEqual([path.read_bytes() for path in paths], [b"original 0", b"original 1"])
                self.assertFalse(journal.exists())
                self.assertEqual(list(base.glob(".acceptance-*")), [])

    def test_cmake_scans_visit_tokens_and_preserve_mutated_source_branches(self) -> None:
        path = self.root / "tools/token-scan.cmake"
        original = ("set(padding " + "x" * 70000 + ")\n"
                    'message("if(FALSE) # quoted")\n'
                    '#[=[ add_test(NAME CommentGhost COMMAND x) ]=]\n'
                    'if(FALSE)\nadd_test(NAME TokenBranch COMMAND x)\nendif()\n'
                    'add_test(NAME TokenLive COMMAND x) # trailing\n')
        reads = []
        class ObservedText(str):
            def __getitem__(self, key: Any) -> Any:
                if isinstance(key, int):
                    reads.append(key)
                return super().__getitem__(key)
        read, flow = MODULE._read_text, MODULE.CMAKE_FLOW_RE
        def observed(file: Path) -> str:
            text = read(file)
            return ObservedText(text) if file == path else text
        path.write_bytes(original.encode("utf-8"))
        try:
            for active in (False, True):
                with self.subTest(active=active):
                    text = original.replace("\nif(FALSE)", "\nif(TRUE )") if active else original
                    path.write_bytes(text.encode("utf-8"))
                    reads.clear()
                    with mock.patch.object(MODULE, "_read_text", observed), \
                            mock.patch.object(MODULE, "CMAKE_FLOW_RE", wraps=flow) as matched:
                        index = MODULE.index_tests(self.root)
                    self.assertIn("TokenLive", index.ctest_names)
                    self.assertEqual("TokenBranch" in index.ctest_names, active)
                    self.assertNotIn("CommentGhost", index.ctest_names)
                    self.assertLessEqual(matched.match.call_count, 8)
                    self.assertLess(len(reads), 256)
            for text in ("", '#[=[ unterminated\nadd_test(NAME CommentGhost COMMAND x)',
                         'message("unterminated\n'):
                path.write_bytes(text.encode("utf-8"))
                index = MODULE.index_tests(self.root)
                self.assertNotIn("CommentGhost", index.ctest_names)
                self.assertNotIn("TokenLive", index.ctest_names)
        finally:
            path.write_bytes(original.encode("utf-8"))
        self.assertEqual(path.read_bytes(), original.encode("utf-8"))

    def test_source_index_shares_only_immutable_parses_and_reads_current_bytes(self) -> None:
        for relative, parser, original, changed, first, second in (
            ("tools/parsed.cmake", "_strip_cmake",
             "add_test(NAME ParseAlpha COMMAND x)\n", "add_test(NAME ParseBravo COMMAND x)\n",
             "ParseAlpha", "ParseBravo"),
            ("test/parsed.cpp", "_strip_c",
             "TEST(ParseAlpha, Case) {}\n", "TEST(ParseBravo, Case) {}\n",
             ("ParseAlpha", "Case"), ("ParseBravo", "Case")),
        ):
            with self.subTest(source=relative):
                path = self.root / relative
                path.write_bytes(original.encode("utf-8"))
                metadata = path.stat()
                parse, read = getattr(MODULE, parser), MODULE._read_text
                def names() -> Any:
                    index = MODULE.index_tests(self.root)
                    return index.ctest_names if relative.endswith("cmake") else index.gtest_cases
                with mock.patch.object(MODULE, parser, wraps=parse) as parsed, \
                        mock.patch.object(MODULE, "_read_text", wraps=read) as reads:
                    for _ in range(3):
                        self.assertIn(first, names())
                    self.assertEqual(sum(call.args == (original,) for call in parsed.call_args_list), 1)
                    try:
                        # Size and timestamps alone cannot bind a parse result.
                        self.assertEqual(len(original), len(changed))
                        path.write_bytes(changed.encode("utf-8"))
                        os.utime(path, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
                        self.assertIn(second, names())
                        self.assertNotIn(first, names())
                        # Same-byte rebinding must still be read, without link privileges.
                        replacement = self.tmp / "replacement-source"
                        replacement.write_bytes(original.encode("utf-8"))
                        replacement.replace(path)
                        self.assertIn(first, names())
                        self.assertNotIn(second, names())
                    finally:
                        path.write_bytes(original.encode("utf-8"))
                    self.assertEqual(path.read_bytes(), original.encode("utf-8"))
                    self.assertEqual(sum(call.args == (changed,) for call in parsed.call_args_list), 1)
                    self.assertEqual(sum(call.args == (path,) for call in reads.call_args_list), 7)

    def test_source_parse_cache_bounds_and_grammar_survive_file_mutations(self) -> None:
        cmake = self.root / "tools/cache-bound.cmake"
        cpp = self.root / "test/cache-bound.cpp"
        text = ("add_test(NAME CacheBound COMMAND x)\n// TEST(CacheBound, Case) {}\n"
                "TEST(CacheLive, Case) {}\n")
        cmake.write_bytes(text.encode("utf-8"))
        cpp.write_bytes(text.encode("utf-8"))
        parse = MODULE._strip_cmake
        with mock.patch.object(MODULE, "_strip_cmake", wraps=parse) as parsed:
            self.assertIn("CacheBound", MODULE.index_tests(self.root).ctest_names)
            self.assertIn(("CacheLive", "Case"), MODULE.index_tests(self.root).gtest_cases)
            self.assertNotIn(("CacheBound", "Case"), MODULE.index_tests(self.root).gtest_cases)
            self.assertEqual(sum(call.args == (text,) for call in parsed.call_args_list), 1)
            # Oversized sources still receive every check, without retaining keys.
            large = text + "# padding\n" * 7000
            try:
                cmake.write_bytes(large.encode("utf-8"))
                for _ in range(2):
                    self.assertIn("CacheBound", MODULE.index_tests(self.root).ctest_names)
                self.assertEqual(sum(call.args == (large,) for call in parsed.call_args_list), 2)
                cmake.write_bytes(b"")
                self.assertNotIn("CacheBound", MODULE.index_tests(self.root).ctest_names)
                cmake.unlink()
                self.assertNotIn("CacheBound", MODULE.index_tests(self.root).ctest_names)
                cmake.write_bytes(text.encode("utf-8"))
                read = MODULE._read_text
                def denied(path: Path) -> str:
                    if path == cmake:
                        raise PermissionError("source parse denied")
                    return read(path)
                with mock.patch.object(MODULE, "_read_text", denied):
                    with self.assertRaisesRegex(PermissionError, "source parse denied"):
                        MODULE.index_tests(self.root)
            finally:
                cmake.write_bytes(text.encode("utf-8"))
            self.assertEqual(cmake.read_bytes(), text.encode("utf-8"))
        large_cpp = text + "// padding\n" * 7000 + "TEST(CacheLarge, Case) {}\n"
        try:
            cpp.write_bytes(large_cpp.encode("utf-8"))
            with mock.patch.object(MODULE, "_strip_c", wraps=MODULE._strip_c) as parsed:
                for _ in range(2):
                    self.assertIn(("CacheLarge", "Case"), MODULE.index_tests(self.root).gtest_cases)
                self.assertEqual(sum(call.args == (large_cpp,) for call in parsed.call_args_list), 2)
        finally:
            cpp.write_bytes(text.encode("utf-8"))
        self.assertEqual(cpp.read_bytes(), text.encode("utf-8"))
        try:
            # Feed actual source versions through discovery, exceeding the entry cap.
            for number in range(257):
                cmake.write_bytes(f"add_test(NAME CacheVersion{number} COMMAND x)\n".encode("utf-8"))
                self.assertIn(f"CacheVersion{number}", MODULE.index_tests(self.root).ctest_names)
            cmake.write_bytes(text.encode("utf-8"))
            with mock.patch.object(MODULE, "_strip_cmake", wraps=parse) as parsed:
                self.assertIn("CacheBound", MODULE.index_tests(self.root).ctest_names)
                self.assertEqual(sum(call.args == (text,) for call in parsed.call_args_list), 1)
        finally:
            cmake.write_bytes(text.encode("utf-8"))
        self.assertEqual(cmake.read_bytes(), text.encode("utf-8"))

    def test_evidence_checks_resolve_the_validation_root_once_and_recheck_edits(self) -> None:
        root = self.root.resolve()
        matrix = baseline_matrix()
        resolve = Path.resolve
        roots = []
        def resolution(path: Path, *values: Any, **options: Any) -> Path:
            if path == root:
                roots.append(path)
            return resolve(path, *values, **options)
        # Each extra, distinct reference needs a target check, not another root check.
        for number in range(24):
            matrix["rows"][0]["evidence"].append({"kind": "script", "ref": f"tools/entry{number}.py",
                "planned": "Supply evidence; owner: fw"})
        matrix["rows"][0]["status"] = "partial"
        with mock.patch.object(Path, "resolve", resolution):
            self.assertEqual(MODULE.validate(matrix, self.index, self.root, False), [])
        self.assertLess(len(roots), 12)
        script = self.root / "tools/script.py"
        original = script.read_bytes()
        try:
            script.unlink()
            self.assertIn("file not found: tools/script.py",
                          "\n".join(MODULE.validate(matrix, self.index, self.root, False)))
        finally:
            script.write_bytes(original)
        self.assertEqual(MODULE.validate(matrix, self.index, self.root, False), [])

    def test_inventory_and_input_scans_reuse_only_the_current_root(self) -> None:
        root, resolve = self.root.resolve(), Path.resolve
        calls = []
        def resolution(path: Path, *values: Any, **options: Any) -> Path:
            if path == root:
                calls.append(path)
            return resolve(path, *values, **options)
        matrix = baseline_matrix()
        for operation, bound in ((lambda: MODULE.validate_inventory(self.inventory, root), 3),
                                 (lambda: MODULE._inventory_units(root), 1),
                                 (lambda: MODULE._matrix_input_paths(matrix, root), 1)):
            with self.subTest(bound=bound):
                calls.clear()
                with mock.patch.object(Path, "resolve", resolution):
                    operation()
                self.assertLessEqual(len(calls), bound)
        source = root / MODULE.CONTRACT_DOCUMENTS[1]
        original = source.read_bytes()
        try:
            source.write_bytes(b"# Changed\nActual new statement.\n")
            self.assertIn("inventory unaccounted statement",
                          "\n".join(MODULE.validate_inventory(self.inventory, root)[1]))
            self.assertIn((MODULE.CONTRACT_DOCUMENTS[1] + "#changed", "Actual new statement."),
                          MODULE._inventory_units(root))
            source.unlink()
            self.assertIn("file not found", "\n".join(MODULE.validate_inventory(self.inventory, root)[1]))
            with self.assertRaisesRegex(MODULE.MatrixError, "file not found"):
                MODULE._inventory_units(root)
        finally:
            source.write_bytes(original)
        self.assertEqual(source.read_bytes(), original)
        script = root / "tools/script.py"
        original_script = script.read_bytes()
        try:
            self.assertIn(script, MODULE._matrix_input_paths(matrix, root)[0])
            script.unlink()
            script.mkdir()
            self.assertNotIn(script, MODULE._matrix_input_paths(matrix, root)[0])
        finally:
            if script.is_dir():
                script.rmdir()
            script.write_bytes(original_script)
        self.assertEqual(script.read_bytes(), original_script)
        self.assertEqual(MODULE.validate_inventory(self.inventory, root)[1], [])
        self.assertIn(script, MODULE._matrix_input_paths(matrix, root)[0])

    def test_cli_parser_setup_is_shared_without_sharing_invocation_inputs(self) -> None:
        self.prepare_refresh()
        discovery, report = self.execution_inputs()
        cached_parser = getattr(MODULE, "_argument_parser", None)
        if cached_parser is not None:
            cached_parser.cache_clear()
            self.addCleanup(cached_parser.cache_clear)
        with mock.patch.object(MODULE.argparse.ArgumentParser, "__init__", autospec=True,
                               side_effect=MODULE.argparse.ArgumentParser.__init__) as parser:
            self.assertEqual(self.run_main(*self.base_args(), "--junit", str(report),
                             "--discovery", str(discovery))[0], 0)
            self.assertEqual(self.run_main(*self.base_args(), "--invalid-option")[0], 2)
            self.assertEqual(self.run_main(*self.base_args())[0], 0)
            original = self.matrix_path.read_bytes()
            try:
                self.matrix_path.write_bytes(b"{")
                code, _, err = self.run_main(*self.base_args())
                self.assertEqual(code, 2, err)
                self.assertIn("cannot parse", err)
            finally:
                self.matrix_path.write_bytes(original)
            code, out, err = self.run_main(*self.base_args())
            self.assertEqual(code, 0, err)
            self.assertIn("static references ok", out)
            parser.assert_called_once()

    def test_mutable_fixture_is_created_once_on_first_use(self) -> None:
        self.assertNotIn("root", self.__dict__)
        self.assertNotIn("tmp", self.__dict__)
        temporary = tempfile.TemporaryDirectory
        with mock.patch.object(tempfile, "TemporaryDirectory", wraps=temporary) as created:
            self.assertIs(self.index, self.fixture_index)
            self.assertNotIn("tmp", self.__dict__)
            self.assertTrue(self.tmp.is_dir())
            self.assertFalse((self.tmp / "repo").exists())
            self.assertEqual(self.matrix_path.parent, self.tmp)
            self.assertEqual(self.inventory, self.fixture_inventory)
            self.assertIsNot(self.inventory, self.fixture_inventory)
            self.assertEqual({path: (self.root / path).read_bytes() for path, _ in self.fixture_files},
                             dict(self.fixture_files))
            self.assertTrue(self.tmp.is_dir())
            self.assertEqual(self.matrix_path.parent, self.tmp)
            created.assert_called_once()
        second = type(self)("runTest")
        second._fixture_pending = True
        try:
            self.assertNotEqual(second.root, self.root)
            second.inventory["requirements"].clear()
            self.assertTrue(self.inventory["requirements"])
            self.assertTrue(self.fixture_inventory["requirements"])
            target = self.fixture_files[0][0]
            (second.root / target).write_bytes(b"changed fixture")
            self.assertEqual((self.root / target).read_bytes(), self.fixture_files[0][1])
            with self.assertRaises(AttributeError):
                getattr(second, "unavailable_fixture_attribute")
        finally:
            second.doCleanups()

    def test_failed_lazy_fixture_creation_closes_and_removes_partial_tree(self) -> None:
        seed = type(self)("runTest")
        seed._fixture_pending = True
        try:
            with mock.patch.object(Path, "write_bytes", side_effect=PermissionError("fixture denied")):
                with self.assertRaisesRegex(PermissionError, "fixture denied"):
                    getattr(seed, "root")
            temporary = seed.tmp
            self.assertTrue(temporary.exists())
        finally:
            seed.doCleanups()
        self.assertFalse(temporary.exists())

    def errors(self, matrix: dict[str, Any], require_landed: bool = False) -> list[str]:
        return MODULE.validate(matrix, self.index, self.root, require_landed)

    def assertErrorContains(self, matrix: dict[str, Any], fragment: str) -> None:
        errors = self.errors(matrix)
        self.assertTrue(any(fragment in error for error in errors), errors)

    def test_baseline_is_valid_and_covers_all_statuses(self) -> None:
        matrix = baseline_matrix()
        self.assertEqual(self.errors(matrix), [])
        self.assertEqual(
            {row["status"] for row in matrix["rows"]},
            {"covered", "partial", "needs-native-run", "planned"},
        )

    def refresh_decisions(self, output: str) -> list[dict[str, Any]]:
        return [json.loads(line.removeprefix("inventory decision: "))
                for line in output.splitlines() if line.startswith("inventory decision: ")]

    def start_paused_refresh(self, *, during_publication: bool = False) -> subprocess.Popen[str]:
        ready = self.tmp / "refresh-ready"
        ready.unlink(missing_ok=True)
        program = """
import pathlib, importlib.util, sys
spec = importlib.util.spec_from_file_location("acceptance_child", sys.argv[1])
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
module = checker.__dict__
original = module["refresh_inventory"]
replace = pathlib.Path.replace
def pause():
    pathlib.Path(sys.argv[4]).write_bytes(b"ready")
    sys.stdin.read(1)
def paused(*args):
    result = original(*args)
    pause()
    return result
def published(path, target):
    result = replace(path, target)
    pathlib.Path.replace = replace
    pause()
    return result
if sys.argv[5] == "publish":
    pathlib.Path.replace = published
else:
    module["refresh_inventory"] = paused
raise SystemExit(module["main"](["--root", sys.argv[2], "--matrix", sys.argv[3], "--refresh-inventory", "--preparation-receipt-file", sys.argv[6]]))
"""
        child = subprocess.Popen([sys.executable, "-I", "-B", "-c", program, str(MODULE_PATH),
                                  str(self.root), str(self.matrix_path), str(ready),
                                  "publish" if during_publication else "prepare", self.base_args()[-1]],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        def cleanup() -> None:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=_CHILD_TIMEOUT)
        self.addCleanup(cleanup)
        child._acceptance_deadline = time.monotonic() + _CHILD_TIMEOUT
        deadline = child._acceptance_deadline
        while not ready.exists():
            if child.poll() is not None:
                self.fail(f"refresh exited before synchronization: {child.communicate()}")
            if time.monotonic() >= deadline:
                self.fail("refresh did not reach synchronization")
            time.sleep(0.01)
        return child

    def test_readers_refuse_mixed_publication_then_accept_completed_refresh(self) -> None:
        discovery, report = self.execution_inputs()
        doc = self.root / "docs/acceptance-matrix.md"
        replace = Path.replace
        for options in ([], ["--check-doc", str(doc)],
                        ["--junit", str(report), "--discovery", str(discovery)]):
            with self.subTest(options=options):
                self.prepare_rollback()
                checked = False
                def interleave(path: Path, target: Path) -> Path:
                    nonlocal checked
                    result = replace(path, target)
                    if not checked:
                        checked = True
                        with mock.patch.object(MODULE.time, "monotonic", side_effect=[0.0, 3.0]), \
                                mock.patch.object(MODULE, "load_matrix") as load:
                            code, _, err = self.run_main(*self.base_args(), *options)
                        self.assertEqual(code, 2, err)
                        self.assertIn("cannot acquire inventory refresh lock within deadline", err)
                        load.assert_not_called()
                    return result
                with mock.patch.object(Path, "replace", interleave):
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 0, err)
                self.assertTrue(checked)
                code, _, err = self.run_main(*self.base_args(), *options)
                self.assertEqual(code, 0, err)

    def test_read_lock_failures_do_not_read_inputs(self) -> None:
        self.prepare_refresh()
        for error in (errno.EACCES, errno.EAGAIN, errno.EDEADLK, errno.EIO):
            with self.subTest(error=error), \
                    mock.patch.object(MODULE, "_try_refresh_lock", side_effect=OSError(error, "unavailable")), \
                    mock.patch.object(MODULE.time, "monotonic", side_effect=[0.0, 3.0]), \
                    mock.patch.object(MODULE, "load_matrix") as load:
                code, _, err = self.run_main(*self.base_args())
                self.assertEqual(code, 2, err)
                self.assertIn("unavailable" if error == errno.EIO else "within deadline", err)
                load.assert_not_called()
        with mock.patch.object(Path, "open", side_effect=PermissionError("lock open denied")), \
                mock.patch.object(MODULE, "load_matrix") as load:
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("lock open denied", err)
        load.assert_not_called()

    def test_reader_retries_after_publication_finishes_before_loading_inputs(self) -> None:
        self.prepare_rollback()
        with contextlib.ExitStack() as holder:
            holder.enter_context(MODULE._refresh_lock(self.root))
            with mock.patch.object(MODULE, "load_matrix", wraps=MODULE.load_matrix) as load:
                def finish_publication(delay: float) -> None:
                    self.assertGreater(delay, 0)
                    load.assert_not_called()
                    holder.close()
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                    self.assertEqual(code, 0, err)
                with mock.patch.object(MODULE.time, "sleep", side_effect=finish_publication) as wait:
                    code, _, err = self.run_main(*self.base_args(), "--check-doc",
                                                 str(self.root / "docs/acceptance-matrix.md"))
        self.assertEqual(code, 0, err)
        wait.assert_called_once()
        self.assertTrue(load.called)

    def test_readers_share_the_lock_and_writers_wait(self) -> None:
        self.prepare_refresh()
        with MODULE._refresh_lock(self.root, shared=True):
            code, _, err = self.run_main(*self.base_args())
            self.assertEqual(code, 0, err)
            with mock.patch.object(MODULE.time, "monotonic", side_effect=[0.0, 3.0]):
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
            self.assertEqual(code, 2, err)
            self.assertIn("within deadline", err)

    def test_first_reader_holds_lock_before_first_writer_can_publish(self) -> None:
        self.prepare_refresh()
        lock = MODULE._refresh_lock_path(self.root)
        doc = self.root / "docs/acceptance-matrix.md"
        discovery, report = self.execution_inputs()
        load = MODULE.load_matrix
        for options in ([], ["--check-doc", str(doc)],
                        ["--junit", str(report), "--discovery", str(discovery)]):
            with self.subTest(options=options):
                lock.unlink()
                destination = self.tmp / "first-writer.md"
                checked = False
                def interleave(path: Path) -> Any:
                    nonlocal checked
                    if not checked:
                        checked = True
                        with mock.patch.object(MODULE.time, "monotonic", side_effect=[0.0, 3.0]):
                            code, _, err = self.run_main(*self.base_args(), "--write-doc", str(destination))
                        self.assertEqual(code, 2, err)
                        self.assertIn("within deadline", err)
                        self.assertFalse(destination.exists())
                    return load(path)
                with mock.patch.object(MODULE, "load_matrix", interleave):
                    code, _, err = self.run_main(*self.base_args(), *options)
                self.assertEqual(code, 0, err)
                self.assertTrue(checked)
                self.assertEqual(lock.read_bytes(), b"")

    def test_first_writer_can_create_lock_between_reader_open_and_create(self) -> None:
        self.prepare_rollback()
        lock = MODULE._refresh_lock_path(self.root)
        lock.unlink()
        system_open = os.open
        interleaved = False
        def create(path: Any, flags: int, *args: Any, **kwargs: Any) -> int:
            nonlocal interleaved
            if Path(path) == lock and not interleaved:
                interleaved = True
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 0, err)
            return system_open(path, flags, *args, **kwargs)
        with mock.patch.object(os, "open", create):
            code, _, err = self.run_main(*self.base_args(), "--check-doc",
                                         str(self.root / "docs/acceptance-matrix.md"))
        self.assertEqual(code, 0, err)
        self.assertTrue(interleaved)
        self.assertEqual(lock.read_bytes(), b"")

    def test_readonly_checkout_detects_first_writer_and_discards_speculative_results(self) -> None:
        doc = self.root / "docs/acceptance-matrix.md"
        discovery, report = self.execution_inputs()
        lock = MODULE._refresh_lock_path(self.root)
        system_open, load = os.open, MODULE.load_matrix
        for failure in (None, "partial", "unreadable"):
            for options in ([], ["--check-doc", str(doc)],
                            ["--junit", str(report), "--discovery", str(discovery)]):
                with self.subTest(failure=failure, options=options):
                    self.prepare_rollback()
                    lock.unlink()
                    interleaved = False
                    def readonly(path: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                        if Path(path) == lock and flags & os.O_CREAT:
                            raise OSError(errno.EROFS, "read-only metadata")
                        return system_open(path, flags, *args, **kwargs)
                    def first_writer(path: Path) -> Any:
                        nonlocal interleaved
                        old = load(path)
                        if not interleaved:
                            interleaved = True
                            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                            self.assertEqual(code, 0, err)
                            if failure == "partial":
                                raise MODULE.MatrixError("partial input")
                            if failure == "unreadable":
                                raise PermissionError("input unreadable")
                        return old
                    with mock.patch.object(os, "open", readonly), \
                            mock.patch.object(MODULE, "load_matrix", first_writer):
                        code, out, err = self.run_main(*self.base_args(), *options)
                    self.assertEqual(code, 0, err)
                    self.assertTrue(interleaved)
                    self.assertIn("detect-and-retry", err)
                    self.assertNotIn("error:", err)
                    self.assertEqual(len(out.splitlines()), 1)

    def test_missing_lock_creation_failures_use_fallback_only_for_readonly_metadata(self) -> None:
        self.prepare_refresh()
        lock = MODULE._refresh_lock_path(self.root)
        lock.unlink()
        system_open = os.open
        for error in (errno.EACCES, errno.EPERM, errno.EROFS, errno.EIO):
            with self.subTest(error=error):
                def denied(path: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                    if Path(path) == lock:
                        raise OSError(error, "creation denied")
                    return system_open(path, flags, *args, **kwargs)
                with mock.patch.object(os, "open", denied):
                    code, out, err = self.run_main(*self.base_args())
                self.assertEqual(code, 2 if error == errno.EIO else 0, err)
                self.assertIn("creation denied", err)
                self.assertEqual("detect-and-retry" in err, error != errno.EIO)
                self.assertEqual("static references ok" in out, error != errno.EIO)
                self.assertFalse(lock.exists())

    def test_repeated_loss_of_synchronization_refuses_speculative_success(self) -> None:
        self.prepare_refresh()
        @contextlib.contextmanager
        def unlocked(*args: Any, **kwargs: Any) -> Any:
            yield False
        with mock.patch.object(MODULE, "_refresh_lock", unlocked):
            code, out, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("lock changed", err)
        self.assertEqual(out, "")

    def test_reader_creation_closes_descriptor_when_handle_construction_fails(self) -> None:
        lock = MODULE._refresh_lock_path(self.root)
        lock.unlink()
        with mock.patch.object(os, "open", wraps=os.open) as opened, \
                mock.patch.object(os, "fdopen", side_effect=OSError(errno.EIO, "handle unavailable")), \
                mock.patch.object(os, "close", wraps=os.close) as closed, \
                mock.patch.object(MODULE, "load_matrix") as load:
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("handle unavailable", err)
        opened.assert_called_once_with(lock, os.O_RDONLY | os.O_CREAT, 0o600)
        closed.assert_called_once()
        load.assert_not_called()
        self.assertEqual(lock.read_bytes(), b"")
        lock.unlink()
        with mock.patch.object(MODULE.time, "monotonic", side_effect=OSError("timer unavailable")), \
                mock.patch.object(MODULE, "load_matrix") as load:
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("timer unavailable", err)
        load.assert_not_called()
        # The persistent file remains, but the failed acquisition owns no handle.
        renamed = lock.with_suffix(".closed")
        lock.rename(renamed)
        renamed.rename(lock)

    def test_speculative_diagnostics_are_bounded_and_overflow_refuses_success(self) -> None:
        capture = MODULE._ReadDiagnostics()
        self.assertEqual(capture.write(""), 0)
        self.assertEqual(capture.write("x" * capture.LIMIT), capture.LIMIT)
        self.assertFalse(capture.overflow)
        self.assertEqual(len(capture.getvalue()), capture.LIMIT)
        self.assertEqual(capture.write("overflow"), 8)
        self.assertTrue(capture.overflow)
        capture.write("ignored")
        self.assertEqual(len(capture.getvalue()), capture.LIMIT)
        lock = MODULE._refresh_lock_path(self.root)
        lock.unlink()
        for stream in ("stdout", "stderr"):
            with self.subTest(stream=stream):
                def verbose(*args: Any) -> int:
                    getattr(sys, stream).write("x" * (capture.LIMIT + 1))
                    return 0
                with mock.patch.object(os, "open", side_effect=OSError(errno.EROFS, "read-only metadata")), \
                        mock.patch.object(MODULE, "_run_locked", verbose):
                    code, out, err = self.run_main(*self.base_args())
                self.assertEqual(code, 2, err)
                self.assertEqual(out, "")
                self.assertIn("diagnostics exceed capture bound", err)

    def test_validation_creates_missing_lock_and_uses_existing_lock_read_only(self) -> None:
        self.prepare_refresh()
        lock = MODULE._refresh_lock_path(self.root)
        lock.unlink()
        doc = self.root / "docs/acceptance-matrix.md"
        for options in ([], ["--check-doc", str(doc)]):
            code, out, err = self.run_main(*self.base_args(), *options)
            self.assertEqual(code, 0, err)
            self.assertIn("static references ok", out)
            self.assertTrue(lock.exists())
        self.assertEqual(lock.read_bytes(), b"")
        identity, opened = lock.stat(), []
        open_path = Path.open
        def read_only(path: Path, mode: str = "r", *args: Any, **kwargs: Any) -> Any:
            if path == lock:
                opened.append(mode)
                if mode != "rb":
                    raise PermissionError("metadata is read-only")
            return open_path(path, mode, *args, **kwargs)
        with mock.patch.object(Path, "open", read_only):
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 0, err)
        self.assertEqual(opened, ["rb"])
        self.assertTrue(os.path.samestat(identity, lock.stat()))

    def test_overlapping_refresh_is_refused_without_stealing_live_lock(self) -> None:
        self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        paths = (self.matrix_path, self.root / "tools/ci/acceptance_requirements.json",
                 self.root / "docs/acceptance-matrix.md")
        before = [path.read_bytes() for path in paths]
        child = self.start_paused_refresh()
        acquire = MODULE._refresh_lock
        with mock.patch.object(MODULE, "_refresh_lock",
                               side_effect=lambda root, **kwargs: acquire(root, timeout=0.05, **kwargs)):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("cannot acquire inventory refresh lock within deadline", err)
        self.assertIsNone(child.poll())
        self.assertEqual([path.read_bytes() for path in paths], before)
        out, err = child.communicate("x", timeout=max(0, child._acceptance_deadline - time.monotonic()))
        self.assertEqual(child.returncode, 0, (out, err))
        after = [path.read_bytes() for path in paths]
        self.assertTrue(all(old != new for old, new in zip(before, after)))
        self.assertEqual(self.run_main(*self.base_args(), "--refresh-inventory")[0], 0)
        self.assertEqual([path.read_bytes() for path in paths], after)

    def test_crashed_holder_releases_lock_without_removing_persistent_file(self) -> None:
        self.prepare_refresh()
        child = self.start_paused_refresh()
        lock = MODULE._refresh_lock_path(self.root)
        identity = os.stat(lock)
        child.kill()
        child.communicate(timeout=_CHILD_TIMEOUT)
        self.assertTrue(lock.exists())
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        self.assertTrue(os.path.samestat(identity, os.stat(lock)))
        self.assertEqual(lock.read_bytes(), b"")

    def test_refresh_lock_location_and_failures_are_private_and_clear(self) -> None:
        self.prepare_refresh()
        metadata = self.root / ".git"
        linked = self.tmp / "metadata"
        metadata.rename(linked)
        metadata.write_text("gitdir: ../metadata\n", encoding="utf-8")
        self.assertEqual(MODULE._refresh_lock_path(self.root), linked / "acceptance-refresh.lock")
        self.assertEqual(self.run_main(*self.base_args(), "--refresh-inventory")[0], 0)
        for locator in ("", "gitdir: ", "unknown", "gitdir: ../absent", "gitdir: tools"):
            with self.subTest(locator=locator):
                metadata.write_text(locator, encoding="utf-8")
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 2, err)
                self.assertIn("Git", err)
        metadata.write_bytes(b"gitdir: \xff")
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("decode", err)
        metadata.unlink()
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("Git metadata", err)
        linked.rename(metadata)
        for error in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
            with self.subTest(error=error), \
                    mock.patch.object(MODULE, "_try_refresh_lock", side_effect=OSError(error, "busy")):
                with self.assertRaisesRegex(MODULE.MatrixError, "within deadline"):
                    with MODULE._refresh_lock(self.root, timeout=0):
                        self.fail("contended lock was acquired")
        for failure in (PermissionError("lock unreadable"), OSError(errno.EIO, "lock unavailable")):
            with self.subTest(failure=failure), mock.patch.object(MODULE, "_try_refresh_lock", side_effect=failure):
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 2, err)
                self.assertIn(str(failure), err)
        with mock.patch.object(Path, "open", side_effect=PermissionError("lock open denied")):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("lock open denied", err)
        # A real Git listing excludes private metadata even with untracked files.
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True, capture_output=True,
                       timeout=_CHILD_TIMEOUT)
        self.assertNotIn(".git/acceptance-refresh.lock", PUBLIC_TREE.tracked_paths(self.root))
        self.assertEqual(PUBLIC_TREE.validate(self.root), [])

    def test_refresh_lock_uses_both_platform_primitives(self) -> None:
        for name, module_name, shared in ((name, module_name, shared)
                                         for name, module_name in (("nt", "msvcrt"), ("posix", "fcntl"))
                                         for shared in (False, True)):
            with self.subTest(platform=name, shared=shared):
                primitive = mock.Mock()
                primitive.LOCK_EX = 2
                primitive.LOCK_NB = 4
                primitive.LOCK_SH = 1
                handle = mock.Mock()
                kernel = mock.Mock()
                with mock.patch.object(MODULE, "os", mock.Mock(name=name)), \
                        mock.patch.dict(sys.modules, {module_name: primitive}), \
                        mock.patch.object(ctypes, "WinDLL", return_value=kernel, create=True):
                    MODULE.os.name = name
                    MODULE._try_refresh_lock(handle, shared=shared)
                if name == "nt":
                    primitive.get_osfhandle.assert_called_once_with(handle.fileno())
                    args = kernel.LockFileEx.call_args.args
                    self.assertEqual(args[:5], (primitive.get_osfhandle.return_value,
                                               1 if shared else 3, 0, 1, 0))
                    overlapped = args[5]._obj
                    self.assertEqual((overlapped.Internal, overlapped.InternalHigh,
                                      overlapped.Offset, overlapped.OffsetHigh, overlapped.hEvent),
                                     (0, 0, 0, 0, None))
                    self.assertEqual(len(kernel.LockFileEx.argtypes), 6)
                    primitive.locking.assert_not_called()
                    handle.seek.assert_not_called()
                    handle.read.assert_not_called()
                    handle.write.assert_not_called()
                else:
                    primitive.flock.assert_called_once_with(
                        handle.fileno(), (primitive.LOCK_SH if shared else primitive.LOCK_EX) | primitive.LOCK_NB)

    def test_windows_read_and_write_lock_errors_preserve_deadline_policy(self) -> None:
        for shared in (False, True):
            for error in (33, 6):
                with self.subTest(shared=shared, error=error):
                    kernel = mock.Mock()
                    kernel.LockFileEx.return_value = 0
                    with mock.patch.object(MODULE, "os", mock.Mock(name="nt")), \
                            mock.patch.dict(sys.modules, {"msvcrt": mock.Mock()}), \
                            mock.patch.object(ctypes, "WinDLL", return_value=kernel, create=True), \
                            mock.patch.object(ctypes, "get_last_error", return_value=error, create=True), \
                            mock.patch.object(ctypes, "WinError", return_value=OSError(errno.EIO, "invalid handle"),
                                              create=True) as failure:
                        MODULE.os.name = "nt"
                        with self.assertRaises(OSError) as caught:
                            MODULE._try_refresh_lock(mock.Mock(), shared=shared)
                    self.assertEqual(caught.exception.errno, errno.EACCES if error == 33 else errno.EIO)
                    if error == 33:
                        failure.assert_not_called()
                    else:
                        failure.assert_called_once_with(error)

    def test_refresh_source_changes_before_or_during_publication_are_rolled_back(self) -> None:
        for during in (False, True):
            for source in (*MODULE.CONTRACT_DOCUMENTS, "docs/d.md", "test/x_test.cpp",
                           "test/CMakeLists.txt", "tools/script.py", "test/added.cpp",
                           "tools/ci/acceptance_requirements.json", "matrix"):
                with self.subTest(during=during, source=source):
                    self.write_inventory()
                    self.prepare_refresh()
                    (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
                        "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
                    paths = (self.matrix_path, self.root / "tools/ci/acceptance_requirements.json",
                             self.root / "docs/acceptance-matrix.md")
                    before = {path: path.read_bytes() for path in paths}
                    changed = self.matrix_path if source == "matrix" else self.root / source
                    original = changed.read_bytes() if changed.exists() else None
                    mutated = False
                    def mutate() -> None:
                        nonlocal mutated
                        if mutated:
                            return
                        mutated = True
                        changed.write_bytes((original or b"") + b"\n ")
                        if changed in before:
                            before[changed] = changed.read_bytes()
                    replace = Path.replace
                    publish = MODULE._replace_outputs
                    def replace_then_edit(file: Path, target: Path) -> Path:
                        result = replace(file, target)
                        mutate()
                        return result
                    def edit_then_publish(*args: Any, **kwargs: Any) -> None:
                        mutate()
                        publish(*args, **kwargs)
                    try:
                        with mock.patch.object(Path, "replace", replace_then_edit if during else replace), \
                                mock.patch.object(MODULE, "_replace_outputs", publish if during else edit_then_publish):
                            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                        self.assertEqual(code, 2, err)
                        self.assertIn("input snapshot changed", err)
                        self.assertEqual({path: path.read_bytes() for path in paths}, before)
                    finally:
                        if original is None:
                            changed.unlink(missing_ok=True)
                        else:
                            changed.write_bytes(original)

    def _check_optional_outputs_cannot_overwrite_inputs_or_each_other(self, refresh) -> None:
        self.prepare_refresh()
        discovery, report = self.execution_inputs()
        for selected_refresh in (refresh,):
            for target in (self.root / MODULE.CONTRACT_DOCUMENTS[0], self.root / "docs/d.md",
                           self.root / "test/x_test.cpp", self.root / "tools/script.py",
                           self.root / "CMakeLists.txt", discovery, report,
                           MODULE._refresh_lock_path(self.root)):
                for option in ("--write-doc", "--execution-receipt"):
                    with self.subTest(refresh=refresh, target=target, option=option):
                        before = target.read_bytes()
                        args = [*self.base_args(), "--junit", str(report), "--discovery", str(discovery)]
                        if refresh:
                            args.append("--refresh-inventory")
                        with mock.patch.object(Path, "replace") as replace:
                            code, _, err = self.run_main(*args, option, str(target))
                        self.assertEqual(code, 2, err)
                        self.assertIn("output path overlaps an input", err)
                        replace.assert_not_called()
                        self.assertEqual(target.read_bytes(), before)
            target = self.tmp / "additional.md"
            args = [*self.base_args(), "--junit", str(report), "--discovery", str(discovery),
                    "--write-doc", str(target), "--execution-receipt", str(target)]
            if refresh:
                args.append("--refresh-inventory")
            code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
            self.assertEqual(code, 2, err)
            self.assertIn("output paths must be distinct", err)
            self.assertFalse(target.exists())
        doc = self.root / "docs/acceptance-matrix.md"
        before = doc.read_bytes()
        code, _, err = self.run_main(*self.base_args(), "--junit", str(report),
                                     "--discovery", str(discovery), "--execution-receipt", str(doc))
        self.assertEqual(code, 2, err)
        self.assertIn("output path overlaps an input", err)
        self.assertEqual(doc.read_bytes(), before)

    def test_empty_snapshot_and_added_or_removed_discovery_sources(self) -> None:
        root = self.tmp / "empty"
        root.mkdir()
        snapshot = MODULE._RefreshSnapshot(root, [])
        snapshot.verify({})
        source = root / "CMakeLists.txt"
        source.write_bytes(b"add_test(NAME New COMMAND new)\n")
        with self.assertRaisesRegex(MODULE.MatrixError, "input snapshot changed"):
            snapshot.verify({})
        snapshot = MODULE._RefreshSnapshot(root, [])
        source.unlink()
        with self.assertRaisesRegex(MODULE.MatrixError, "input snapshot changed"):
            snapshot.verify({})

    def test_discovery_stats_only_candidate_names_in_both_source_scans(self) -> None:
        noise = {self.root / directory / f"unrelated-{number}.txt"
                 for directory in ("test", "tools") for number in range(12)}
        for path in noise:
            path.touch()
        # Candidate-looking directories must still be filtered by file type.
        for name in ("directory.cpp", "directory.h", "directory.cmake", "CMakeLists.txt"):
            (self.root / "tools" / name).mkdir()
        (self.root / "test/directory.cpp").mkdir()
        is_file = Path.is_file
        checked: list[Path] = []
        def check(path: Path) -> bool:
            checked.append(path)
            return is_file(path)
        with mock.patch.object(Path, "is_file", check):
            cmake = MODULE._cmake_files(self.root)
            cpp = MODULE._gtest_sources(self.root)
        self.assertEqual(cmake, [self.root / "CMakeLists.txt", self.root / "test/CMakeLists.txt"])
        self.assertEqual(cpp, [self.root / "test/x_test.cpp"])
        self.assertTrue(noise.isdisjoint(checked))
        with mock.patch.object(Path, "rglob", autospec=True, side_effect=Path.rglob) as scan:
            self.assertEqual(MODULE._discovery_files(self.root), (cmake, cpp))
        self.assertEqual([call.args[0] for call in scan.call_args_list],
                         [self.root / "test", self.root / "tools"])
        self.assertEqual(MODULE._discovery_files(self.tmp / "missing"), ([], []))

    def test_matrix_inputs_deduplicate_exact_references_without_losing_roles_or_aliases(self) -> None:
        matrix = self.prepare_refresh()
        alias = self.root / "tools/alias.py"
        alias.hardlink_to(self.root / "tools/script.py")
        matrix["rows"][0]["evidence"].append({"kind": "script", "ref": "tools/alias.py"})
        matrix["rows"].append(dict(matrix["rows"][0], source="docs/acceptance-matrix.md"))
        expected = MODULE._matrix_input_paths(matrix, self.root)
        matrix["rows"] *= 12
        with mock.patch.object(MODULE, "_path_problem", wraps=MODULE._path_problem) as check:
            actual = MODULE._matrix_input_paths(matrix, self.root)
        self.assertEqual(actual, expected)
        self.assertLessEqual(check.call_count, len(expected[0]))
        doc = self.root / "docs/acceptance-matrix.md"
        self.assertEqual(actual[0].count(doc), 2)  # Source and generated evidence.
        self.assertIn(doc, actual[1])
        for path in (alias, self.root / "tools/script.py"):
            self.assertIn(path, actual[0])
            self.assertIn(path, actual[1])
        self.assertEqual(MODULE._matrix_input_paths({"rows": []}, self.root), ([], []))

    def overlap_args(self) -> Any:
        return MODULE.argparse.Namespace(
            inventory=self.root / "tools/ci/acceptance_requirements.json",
            generated_doc=self.root / "docs/acceptance-matrix.md", refresh_inventory=True,
            write_doc=self.tmp / "export.md", execution_receipt=self.tmp / "receipt.json",
            junit=[], discovery=None)

    def test_overlap_metadata_work_is_bounded_by_distinct_paths(self) -> None:
        self.prepare_refresh()
        args = self.overlap_args()
        inputs = [self.root / "tools/script.py", self.root / "docs/d.md"] * 24
        resolve = Path.resolve
        resolved: list[Path] = []
        def record(path: Path, *args: Any, **kwargs: Any) -> Path:
            resolved.append(path)
            return resolve(path, *args, **kwargs)
        with mock.patch.object(Path, "resolve", record):
            MODULE._check_output_paths(args, self.root, self.matrix_path, inputs)
        self.assertEqual(len(resolved), len(set(resolved)))
        # Real hard links must use the same comparison path as resolved names.
        args.write_doc.hardlink_to(inputs[0])
        before = inputs[0].read_bytes()
        with self.assertRaisesRegex(MODULE.MatrixError, "overlaps an input"):
            MODULE._check_output_paths(args, self.root, self.matrix_path, inputs)
        self.assertEqual(inputs[0].read_bytes(), before)
        args.write_doc.unlink()
        with mock.patch.object(Path, "stat", side_effect=PermissionError("metadata denied")):
            with self.assertRaisesRegex(PermissionError, "metadata denied"):
                MODULE._same_path(args.write_doc, inputs[0])

    def test_cli_lock_overlap_metadata_is_shared_only_for_one_check(self) -> None:
        self.prepare_refresh()
        lock = MODULE._refresh_lock_path(self.root)
        export = self.tmp / "export.md"
        export.write_bytes(b"previous export")
        args = MODULE.argparse.Namespace(root=self.root, matrix=self.matrix_path, refresh_inventory=False,
            check_doc=export, write_doc=export, discovery=None, execution_receipt=None,
            junit=[], require_landed=False, ci_context=False)
        resolve, metadata = Path.resolve, Path.stat
        resolved, stated = [], []
        def resolution(path: Path, *values: Any, **options: Any) -> Path:
            resolved.append(path)
            return resolve(path, *values, **options)
        def information(path: Path, *values: Any, **options: Any) -> Any:
            stated.append(path)
            return metadata(path, *values, **options)
        with mock.patch.object(Path, "resolve", resolution), \
                mock.patch.object(Path, "stat", information), \
                mock.patch.object(MODULE, "_run", return_value=0):
            self.assertEqual(MODULE._run_locked(args, self.root), 0)
        # One initial lock resolution and one in the comparison cache; every
        # argument shares the same lock metadata within this overlap check.
        self.assertEqual(resolved.count(lock), 2)
        # POSIX resolve itself may stat its result; Windows resolves through a
        # handle. Only one additional identity stat is needed on either host.
        self.assertGreaterEqual(stated.count(lock), 1)
        self.assertLessEqual(stated.count(lock), resolved.count(lock) + 1)
        export.unlink()
        export.hardlink_to(lock)
        with self.assertRaisesRegex(MODULE.MatrixError, "overlaps an input: refresh lock"):
            MODULE._run_locked(args, self.root)
        export.unlink()
        export.write_bytes(b"independent export")
        with mock.patch.object(MODULE, "_run", return_value=0):
            self.assertEqual(MODULE._run_locked(args, self.root), 0)
        # Metadata failures are not cached across calls or treated as absence.
        def denied(path: Path, *values: Any, **options: Any) -> Any:
            if path == lock:
                raise PermissionError("lock metadata denied")
            return metadata(path, *values, **options)
        with mock.patch.object(Path, "stat", denied):
            with self.assertRaisesRegex(PermissionError, "lock metadata denied"):
                MODULE._run_locked(args, self.root)

    def test_readonly_modes_do_not_expand_publication_inputs(self) -> None:
        matrix = self.prepare_refresh()
        discovery, report = self.execution_inputs()
        doc = self.root / "docs/acceptance-matrix.md"
        for options in ([], ["--check-doc", str(doc)],
                        ["--discovery", str(discovery), "--junit", str(report)]):
            with self.subTest(options=options), \
                    mock.patch.object(MODULE, "_matrix_input_paths", wraps=MODULE._matrix_input_paths) as inputs, \
                    mock.patch.object(MODULE, "_check_output_paths", wraps=MODULE._check_output_paths) as overlaps:
                code, _, err = self.run_main(*self.base_args(), *options)
                self.assertEqual(code, 0, err)
                inputs.assert_not_called()
                overlaps.assert_not_called()
        for bad, message in (({}, "rows must be a non-empty list"),
                             (dict(matrix, rows=[]), "rows must be a non-empty list")):
            self.write_matrix(bad)
            code, _, err = self.run_main(*self.base_args())
            self.assertEqual(code, 1, err)
            self.assertIn(message, err)
        self.write_matrix(matrix)
        (self.root / "tools/script.py").unlink()
        code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 1, err)
        self.assertIn("file not found: tools/script.py", err)
        self.matrix_path.write_bytes(b"{")
        code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("cannot parse", err)

    def test_immutable_source_answers_are_shared_and_discarded_after_each_sweep(self) -> None:
        doc = self.root / "docs/d.md"
        source = "docs/d.md#section"
        with mock.patch.object(MODULE, "_source_problem", wraps=MODULE._source_problem) as read:
            with immutable_repository_reads():
                for _ in range(3):
                    self.assertIsNone(MODULE._source_problem(self.root, source, {}))
                    self.assertEqual(MODULE._source_problem(self.root, "docs/d.md#absent", {}),
                                     "source anchor not found: docs/d.md#absent")
                self.assertEqual(read.call_count, 2)
            doc.write_text("# Changed\n", encoding="utf-8")
            with immutable_repository_reads():
                self.assertEqual(MODULE._source_problem(self.root, source, {}),
                                 "source anchor not found: docs/d.md#section")
            doc.unlink()
            with immutable_repository_reads():
                self.assertEqual(MODULE._source_problem(self.root, source, {}),
                                 "source file not found: docs/d.md")
        doc.write_bytes(b"# Section\n")
        with mock.patch.object(MODULE, "_read_text", side_effect=PermissionError("source denied")):
            with immutable_repository_reads():
                self.assertIn("source denied", MODULE._source_problem(self.root, source, {}))
        # Exception paths unwind every patch as well as successful sweeps.
        original = MODULE._source_problem
        with self.assertRaisesRegex(RuntimeError, "interrupted sweep"):
            with immutable_repository_reads():
                raise RuntimeError("interrupted sweep")
        self.assertIs(MODULE._source_problem, original)

    def test_denied_file_alias_resolution_does_not_walk_unrelated_parents(self) -> None:
        alias = self.root / "tools/script.py"
        with self.retargetable_path(alias, denied=True) as (first, second, retarget):
            with mock.patch.object(Path, "is_relative_to", side_effect=AssertionError("unexpected parent walk")):
                self.assertEqual(alias.resolve(), first)
                self.assertEqual(self.matrix_path.resolve(), self.matrix_path)
                retarget()
                self.assertEqual(alias.resolve(), second)
            self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_modeled_resolution_cache_is_local_and_retargeted_without_caching_bytes(self) -> None:
        alias = self.root / "tools/script.py"
        original = Path.resolve
        resolved = []
        def resolution(path: Path, *values: Any, **options: Any) -> Path:
            resolved.append(path)
            return original(path, *values, **options)
        with mock.patch.object(Path, "resolve", resolution):
            with self.retargetable_path(alias, denied=True) as (first, second, retarget):
                for _ in range(3):
                    self.assertEqual(alias.resolve(), first)
                self.assertEqual(resolved.count(first), 1)
                first.write_bytes(b"updated bytes")
                self.assertEqual(MODULE._RefreshSnapshot._fingerprint(first),
                                 MODULE.hashlib.sha256(b"updated bytes").digest())
                retarget()
                for _ in range(3):
                    self.assertEqual(alias.resolve(), second)
                self.assertEqual(resolved.count(second), 1)
                self.assertNotEqual(MODULE._RefreshSnapshot._fingerprint(first),
                                    MODULE._RefreshSnapshot._fingerprint(second))
            count = len(resolved)
            alias.resolve()
            alias.resolve()
            self.assertEqual(len(resolved), count + 2)

    def test_external_edits_invalidate_path_discovery_and_evidence_answers(self) -> None:
        self.prepare_refresh()
        args = self.overlap_args()
        script = self.root / "tools/script.py"
        snapshot = MODULE._RefreshSnapshot(self.root, [script])
        MODULE._check_output_paths(args, self.root, self.matrix_path, [script])
        self.assertEqual(self.errors(baseline_matrix()), [])
        self.assertNotIn("ExternalContract", MODULE.index_tests(self.root).ctest_names)
        # A real child changes the actual paths read by the parent; no mock is
        # expected to cross the process boundary. Hard links need no symlink privilege.
        program = """
import pathlib, sys
root, export = map(pathlib.Path, sys.argv[1:])
script = root / 'tools/script.py'
export.hardlink_to(script)
script.write_text('changed input', encoding='utf-8')
(root / 'test/new.cmake').write_text('add_test(NAME ExternalContract COMMAND external)', encoding='utf-8')
(root / 'tools/run.ps1').unlink()
"""
        child = subprocess.run([sys.executable, "-I", "-B", "-c", program,
                                str(self.root), str(args.write_doc)],
                               capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
        self.assertEqual(child.returncode, 0, (child.stdout, child.stderr))
        self.assertIn("ExternalContract", MODULE.index_tests(self.root).ctest_names)
        self.assertErrorContains(baseline_matrix(), "file not found: tools/run.ps1")
        with self.assertRaisesRegex(MODULE.MatrixError, "input snapshot changed"):
            snapshot.verify({})
        with self.assertRaisesRegex(MODULE.MatrixError, "overlaps an input"):
            MODULE._check_output_paths(args, self.root, self.matrix_path, [script])
        args.write_doc.unlink()
        MODULE._check_output_paths(args, self.root, self.matrix_path, [script])

    def _check_cli_alias_retargeting_refuses_before_and_after_every_publication(self, boundary) -> None:
        for name in ("inventory", "matrix", "generated", "check", "write", "discovery", "receipt", "junit"):
            for selected_boundary in (boundary,):
                for denied in (False, True):
                    with self.subTest(alias=name, boundary=boundary, denied=denied):
                        paths, before = self.prepare_rollback()
                        discovery, report = self.execution_inputs()
                        check, export, receipt = (self.tmp / filename for filename in
                                                   ("check.md", "export.md", "receipt.json"))
                        updated = MODULE.refresh_inventory(self.inventory, MODULE.load_matrix(self.matrix_path),
                                                           self.root)[1]
                        check.write_bytes(MODULE.render_doc(updated).encode("utf-8"))
                        export.write_bytes(b"previous export\n")
                        receipt.write_bytes(b"previous receipt\n")
                        aliases = dict(zip(("generated", "matrix", "inventory"), paths))
                        aliases.update(check=check, write=export, discovery=discovery, receipt=receipt, junit=report)
                        alias = aliases[name]
                        with self.retargetable_path(alias, denied=denied) as (first, second, retarget):
                            originals = {path.resolve(): data for path, data in before.items()}
                            originals.update({path.resolve(): path.read_bytes() for path in
                                              (check, export, receipt, discovery, report)})
                            originals[second] = second.read_bytes()
                            load, publish, replace = MODULE.load_matrix, MODULE._replace_outputs, Path.replace
                            changed = False
                            published = []
                            def change() -> None:
                                nonlocal changed
                                self.assertFalse(changed)
                                retarget()
                                changed = True
                            def after_read(path: Path) -> Any:
                                result = load(path)
                                if boundary == "loaded" and path == paths[2].resolve():
                                    change()
                                return result
                            def before_stage(*args: Any, **kwargs: Any) -> None:
                                if boundary == "staging":
                                    change()
                                publish(*args, **kwargs)
                            def after_replace(path: Path, target: Path) -> Path:
                                result = replace(path, target)
                                published.append(target)
                                if boundary == len(published):
                                    change()
                                return result
                            with mock.patch.object(MODULE, "load_matrix", after_read), \
                                    mock.patch.object(MODULE, "_replace_outputs", before_stage), \
                                    mock.patch.object(Path, "replace", after_replace):
                                code, out, err = self.run_main(*self.base_args(), "--refresh-inventory",
                                    "--check-doc", str(check), "--write-doc", str(export),
                                    "--discovery", str(discovery), "--junit", str(report),
                                    "--execution-receipt", str(receipt))
                            self.assertTrue(changed)
                            self.assertEqual(code, 2, err)
                            self.assertIn("input snapshot changed", err)
                            self.assertNotIn("static references ok", out)
                            self.assertNotIn("rollback incomplete", err)
                            self.assertEqual(len(published), boundary if isinstance(boundary, int) else 0)
                            self.assertEqual(alias.resolve(), second)
                            self.assertEqual({path: path.read_bytes() for path in originals}, originals)
                            self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])
                            self.assertFalse(MODULE._refresh_lock_path(self.root).with_suffix(".transaction").exists())

    def test_parent_and_checkout_alias_retargeting_rolls_back_actual_destinations(self) -> None:
        for parent in (self.root / "tools/ci", self.root):
            for denied in (False, True):
                with self.subTest(parent=parent.name, denied=denied):
                    paths, before = self.prepare_rollback(generated_evidence=False)
                    with self.retargetable_path(parent, denied=denied) as (first, second, retarget):
                        originals = {path.resolve(): data for path, data in before.items()}
                        originals.update({path: path.read_bytes() for path in second.rglob("*")
                                          if path.is_file() and ".git" not in path.parts})
                        replace = Path.replace
                        def publish(path: Path, target: Path) -> Path:
                            result = replace(path, target)
                            retarget()
                            return result
                        with mock.patch.object(Path, "replace", publish):
                            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                        self.assertEqual(code, 2, err)
                        self.assertIn("input snapshot changed", err)
                        self.assertNotIn("rollback incomplete", err)
                        self.assertEqual(parent.resolve(), second)
                        self.assertEqual({path: path.read_bytes() for path in originals}, originals)
                        self.assertEqual(list(first.rglob(".acceptance-*")), [])

    def test_nonrefresh_writers_reject_cli_and_referenced_input_alias_retargeting(self) -> None:
        for name in ("matrix", "inventory", "generated", "check", "discovery", "junit", "write", "receipt",
                     "reference"):
            for during in (False, True):
                with self.subTest(alias=name, during=during):
                    self.prepare_refresh()
                    discovery, report = self.execution_inputs()
                    doc = self.root / "docs/acceptance-matrix.md"
                    export, receipt = self.tmp / "export.md", self.tmp / "receipt.json"
                    export.write_bytes(b"old export")
                    receipt.write_bytes(b"old receipt")
                    aliases = dict(matrix=self.matrix_path, inventory=self.root / "tools/ci/acceptance_requirements.json",
                                   generated=doc, check=doc, discovery=discovery, junit=report,
                                   write=export, receipt=receipt, reference=self.root / "tools/script.py")
                    with self.retargetable_path(aliases[name]) as (first, second, retarget):
                        before = {path.resolve(): path.read_bytes() for path in aliases.values()}
                        before[second] = second.read_bytes()
                        replace, publish = Path.replace, MODULE._replace_outputs
                        changed = False
                        def before_stage(*args: Any, **kwargs: Any) -> None:
                            if not during:
                                retarget()
                            publish(*args, **kwargs)
                        def after_replace(path: Path, target: Path) -> Path:
                            nonlocal changed
                            result = replace(path, target)
                            if not changed:
                                changed = True
                                retarget()
                            return result
                        with mock.patch.object(MODULE, "_replace_outputs", before_stage), \
                                mock.patch.object(Path, "replace", after_replace if during else replace):
                            code, out, err = self.run_main(*self.base_args(), "--write-doc", str(export),
                                "--check-doc", str(doc), "--discovery", str(discovery), "--junit", str(report),
                                "--execution-receipt", str(receipt))
                        self.assertEqual(code, 2, err)
                        self.assertIn("input snapshot changed", err)
                        self.assertNotIn("references passed", out)
                        self.assertNotIn("rollback incomplete", err)
                        self.assertEqual({path: path.read_bytes() for path in before}, before)
                        self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def test_same_byte_input_hardlink_rebindings_are_refused(self) -> None:
        for name in ("inventory", "matrix", "source", "script", "cmake", "gtest", "discovery", "junit"):
            with self.subTest(input=name):
                paths, before = self.prepare_rollback()
                discovery, report = self.execution_inputs()
                sources = dict(inventory=paths[2], matrix=paths[1], source=self.root / "docs/d.md",
                               script=self.root / "tools/script.py", cmake=self.root / "test/CMakeLists.txt",
                               gtest=self.root / "test/x_test.cpp", discovery=discovery, junit=report)
                source = sources[name]
                saved, replacement = self.tmp / "saved-input", self.tmp / "replacement-input"
                saved.hardlink_to(source)
                replacement.write_bytes(source.read_bytes())
                publish = MODULE._replace_outputs
                def rebind(*args: Any, **kwargs: Any) -> None:
                    source.unlink()
                    source.hardlink_to(replacement)
                    publish(*args, **kwargs)
                try:
                    with mock.patch.object(MODULE, "_replace_outputs", rebind), \
                            mock.patch.object(Path, "replace", autospec=True, side_effect=Path.replace) as replace:
                        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory",
                            "--discovery", str(discovery), "--junit", str(report))
                    self.assertEqual(code, 2, err)
                    self.assertIn("input snapshot changed", err)
                    replace.assert_not_called()
                    self.assertEqual({path: path.read_bytes() for path in paths}, before)
                    self.assertEqual(source.read_bytes(), saved.read_bytes())
                    self.assertEqual(source.read_bytes(), replacement.read_bytes())
                finally:
                    saved.unlink()
                    replacement.unlink()

    def test_alias_change_before_snapshot_never_reads_the_new_target(self) -> None:
        paths, before = self.prepare_rollback()
        alias = paths[2]
        lock = MODULE._refresh_lock_path(self.root)
        for failure in ("lock", "missing", "partial", "unreadable"):
            with self.subTest(target=failure):
                with self.retargetable_path(alias, denied=True) as (first, second, retarget):
                    if failure == "partial":
                        second.write_bytes(b"{")
                    elif failure == "missing":
                        second.unlink()
                    snapshot, fingerprint, resolve = MODULE._RefreshSnapshot.__init__, \
                        MODULE._RefreshSnapshot._fingerprint, Path.resolve
                    def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                        result = resolve(path, *args, **kwargs)
                        return lock if failure == "lock" and result == second else result
                    def create(snapshot_self: Any, *args: Any, **kwargs: Any) -> None:
                        retarget()
                        snapshot(snapshot_self, *args, **kwargs)
                    def read(path: Path) -> Any:
                        self.assertNotIn(path, (second, lock))
                        return fingerprint(path)
                    try:
                        with mock.patch.object(Path, "resolve", resolved), \
                                mock.patch.object(MODULE._RefreshSnapshot, "__init__", create), \
                                mock.patch.object(MODULE._RefreshSnapshot, "_fingerprint", staticmethod(read)), \
                                mock.patch.object(Path, "replace") as replace:
                            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                        self.assertEqual(code, 2, err)
                        self.assertIn("input snapshot changed", err)
                        replace.assert_not_called()
                        self.assertEqual(first.read_bytes(), before[alias])
                    finally:
                        if not second.exists():
                            second.touch()
        self.assertEqual({path: path.read_bytes() for path in paths}, before)

    def test_snapshot_unknown_or_unreadable_name_identity_refuses_without_publication(self) -> None:
        paths, before = self.prepare_rollback()
        lstat = Path.lstat
        for failure in ("unknown", "unreadable"):
            with self.subTest(failure=failure):
                def metadata(path: Path) -> Any:
                    result = lstat(path)
                    if path == paths[2]:
                        if failure == "unreadable":
                            raise PermissionError("input identity unreadable")
                        values = list(result)
                        values[1] = 0
                        return os.stat_result(values)
                    return result
                with mock.patch.object(Path, "lstat", metadata), mock.patch.object(Path, "replace") as replace:
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 2, err)
                self.assertIn("identity unavailable" if failure == "unknown" else "identity unreadable", err)
                replace.assert_not_called()
                self.assertEqual({path: path.read_bytes() for path in paths}, before)

    def test_snapshot_inputs_and_outputs_use_the_bounded_regular_file_reader(self) -> None:
        for role in ("input", "output"):
            for failure in ("special", "oversize", "partial", "changed"):
                with self.subTest(role=role, failure=failure):
                    self.prepare_refresh()
                    export = self.tmp / "export.md"
                    export.write_bytes(b"previous export")
                    blocked = self.matrix_path if role == "input" else export
                    identity = MODULE._read_transaction_file(blocked, MODULE._TRANSACTION_BYTES)[1]
                    before = {path: path.read_bytes() for path in (self.matrix_path, export)}
                    metadata = MODULE._transaction_metadata
                    reads = 0
                    def information(handle: Any) -> Any:
                        nonlocal reads
                        result = metadata(handle)
                        if not os.path.samestat(result, identity):
                            return result
                        reads += 1
                        fields = list(result)
                        if failure == "special":
                            fields[0] = stat.S_IFIFO | 0o600
                        elif failure == "oversize":
                            fields[6] = MODULE._TRANSACTION_BYTES + 1
                        elif failure == "partial":
                            fields[6] += 1
                        elif reads > 1:
                            fields[8] += 1
                        else:
                            return result
                        return os.stat_result(fields)
                    with mock.patch.object(MODULE, "_transaction_metadata", information), \
                            mock.patch.object(MODULE, "load_matrix", wraps=MODULE.load_matrix) as load, \
                            mock.patch.object(Path, "replace") as replace:
                        code, _, err = self.run_main(*self.base_args(), "--write-doc", str(export))
                    self.assertEqual(code, 2, err)
                    self.assertIn("invalid recovery file or size" if failure in {"special", "oversize"}
                                  else "recovery file changed", err)
                    self.assertGreater(reads, 0)
                    load.assert_not_called()
                    replace.assert_not_called()
                    self.assertEqual({path: path.read_bytes() for path in before}, before)

    def test_repeated_cli_alias_uses_the_first_selected_destination(self) -> None:
        paths, before = self.prepare_rollback(generated_evidence=False)
        doc = paths[0]
        with self.retargetable_path(doc, denied=True) as (first, second, retarget):
            resolve = Path.resolve
            changed = False
            def selected(path: Path, *args: Any, **kwargs: Any) -> Path:
                nonlocal changed
                result = resolve(path, *args, **kwargs)
                if path == doc and not changed:
                    changed = True
                    retarget()
                return result
            with mock.patch.object(Path, "resolve", selected), \
                    mock.patch.object(Path, "replace", autospec=True, side_effect=Path.replace) as replace:
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory",
                                             "--check-doc", str(doc), "--write-doc", str(doc))
            self.assertTrue(changed)
            self.assertEqual(code, 2, err)
            self.assertIn("input snapshot changed", err)
            replace.assert_not_called()
            self.assertEqual(first.read_bytes(), before[doc])
            self.assertEqual(second.read_bytes(), before[doc])
            self.assertEqual({path: path.read_bytes() for path in paths[1:]},
                             {path: before[path] for path in paths[1:]})

    def test_locked_retries_keep_original_cli_names(self) -> None:
        self.prepare_refresh()
        with self.retargetable_path(self.matrix_path) as (first, second, retarget):
            args = MODULE.argparse.Namespace(root=self.root, matrix=self.matrix_path, refresh_inventory=False,
                check_doc=None, write_doc=None, discovery=None, execution_receipt=None,
                junit=[], require_landed=False, ci_context=False)
            original = vars(args).copy()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(MODULE._run_locked(args, self.root), 0)
                retarget()
                second.write_bytes(b"{")
                self.assertEqual(MODULE._run_locked(args, self.root), 2)
            self.assertEqual(vars(args), original)
            self.assertNotEqual(first.read_bytes(), second.read_bytes())

    def test_refresh_aliased_inputs_and_standard_document_share_one_identity(self) -> None:
        self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        doc = self.root / "docs/acceptance-matrix.md"
        args = ["--root", str(self.root / "test/.."),
                "--matrix", str(self.root / ".." / self.matrix_path.name),
                "--write-doc", str(self.root / "docs/../docs/acceptance-matrix.md"),
                "--refresh-inventory", "--check-doc", str(doc),
                "--preparation-receipt-file", self.base_args()[-1]]
        code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
        self.assertEqual(code, 0, err)
        matrix = MODULE.load_matrix(self.matrix_path)
        self.assertEqual(row_of(matrix, "iss.126")["source"], MODULE.CONTRACT_DOCUMENTS[1] + "#moved")
        self.assertEqual(self.errors(matrix), [])
        self.assertEqual(doc.read_text(encoding="utf-8"), MODULE.render_doc(matrix))
        before = doc.read_bytes(), doc.stat().st_mtime_ns
        self.assertEqual(self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])[0], 0)
        self.assertEqual((doc.read_bytes(), doc.stat().st_mtime_ns), before)

    def test_generated_document_evidence_allows_both_writers_and_separate_exports(self) -> None:
        doc = self.root / "docs/acceptance-matrix.md"
        export = self.tmp / "export.md"
        for options in (["--refresh-inventory"], ["--write-doc", str(doc)],
                        ["--write-doc", str(export)],
                        ["--refresh-inventory", "--write-doc", str(export)]):
            with self.subTest(options=options):
                matrix = self.prepare_refresh()
                expected = MODULE.render_doc(matrix).encode("utf-8")
                doc.write_bytes(b"Stale generated view.\n")
                export.unlink(missing_ok=True)
                code, out, err = self.run_main(*self.base_args(), *options)
                self.assertEqual(code, 0, err)
                self.assertIn("static references ok", out)
                if "--write-doc" in options and str(export) in options:
                    self.assertEqual(export.read_bytes(), expected)
                if "--refresh-inventory" in options or str(doc) in options:
                    self.assertEqual(doc.read_bytes(), expected)

    def test_generated_document_as_source_or_other_evidence_remains_protected(self) -> None:
        doc = self.root / "docs/acceptance-matrix.md"
        for reference in ("source", "script", "native", "unknown", "alias-spelling", "other-document"):
            for options in (["--refresh-inventory"], ["--write-doc", str(doc)]):
                with self.subTest(reference=reference, options=options):
                    matrix = self.prepare_refresh()
                    row = matrix["rows"][0]
                    if reference == "source":
                        row["source"] = "docs/acceptance-matrix.md#acceptance-matrix"
                    else:
                        row["evidence"].append({
                            "kind": "doc" if reference in {"alias-spelling", "other-document"} else reference,
                            "ref": "./docs/acceptance-matrix.md" if reference == "alias-spelling"
                                   else "docs/other.md" if reference == "other-document"
                                   else "docs/acceptance-matrix.md",
                        })
                    if reference == "other-document":
                        # A nonstandard document reference cannot become its own
                        # output just because the standard view is also cited.
                        options = ["--write-doc", str(self.root / "docs/other.md"),
                                   *(["--refresh-inventory"] if "--refresh-inventory" in options else [])]
                    self.write_matrix(matrix)
                    before = {path: path.read_bytes() for path in self.tmp.rglob("*") if path.is_file()}
                    with mock.patch.object(Path, "replace") as publish:
                        code, _, err = self.run_main(*self.base_args(), *options)
                    self.assertEqual(code, 2, err)
                    self.assertIn("output path overlaps an input", err)
                    publish.assert_not_called()
                    self.assertEqual({path: path.read_bytes() for path in self.tmp.rglob("*") if path.is_file()},
                                     before)

    def test_generated_document_evidence_aliases_remain_real_inputs(self) -> None:
        doc = self.root / "docs/acceptance-matrix.md"
        alias = self.root / "docs/alias.md"
        for kind in ("hardlink", "symlink", "symlink-denied"):
            for options in (["--refresh-inventory"], ["--write-doc", str(doc)]):
                with self.subTest(kind=kind, options=options), contextlib.ExitStack() as stack:
                    matrix = self.prepare_refresh()
                    matrix["rows"][0]["evidence"].append({"kind": "doc", "ref": "docs/alias.md"})
                    self.write_matrix(matrix)
                    if kind == "hardlink":
                        os.link(doc, alias)
                    else:
                        if kind == "symlink-denied":
                            stack.enter_context(mock.patch.object(Path, "symlink_to",
                                                                  side_effect=PermissionError("denied")))
                        try:
                            alias.symlink_to(doc)
                        except OSError:
                            alias.write_bytes(doc.read_bytes())
                            resolve = Path.resolve
                            def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                                result = resolve(path, *args, **kwargs)
                                return doc if result == alias else result
                            stack.enter_context(mock.patch.object(Path, "resolve", resolved))
                    stack.callback(alias.unlink)
                    before = doc.read_bytes()
                    with mock.patch.object(Path, "replace") as publish:
                        code, _, err = self.run_main(*self.base_args(), *options)
                    self.assertEqual(code, 2, err)
                    self.assertIn("output path overlaps an input", err)
                    publish.assert_not_called()
                    self.assertEqual(doc.read_bytes(), before)

    def test_nonstandard_output_cannot_alias_generated_document(self) -> None:
        self.prepare_refresh()
        doc = self.root / "docs/acceptance-matrix.md"
        alias = self.tmp / "export.md"
        os.link(doc, alias)
        before = doc.read_bytes()
        discovery, report = self.execution_inputs()
        for option in ("--write-doc", "--execution-receipt"):
            for refresh in (False, True):
                with self.subTest(option=option, refresh=refresh):
                    with mock.patch.object(Path, "replace") as publish:
                        code, _, err = self.run_main(*self.base_args(), option, str(alias),
                                                     "--junit", str(report), "--discovery", str(discovery),
                                                     *(["--refresh-inventory"] if refresh else []))
                    self.assertEqual(code, 2, err)
                    self.assertIn("output paths must be distinct" if refresh else "output path overlaps an input", err)
                    publish.assert_not_called()
                    self.assertEqual(doc.read_bytes(), before)
                    self.assertTrue(doc.samefile(alias))

    def test_generated_document_resolution_and_validation_precede_publication(self) -> None:
        doc = self.root / "docs/acceptance-matrix.md"
        resolve = Path.resolve
        for outside in (False, True):
            for fail in (False, True):
                with self.subTest(outside=outside, fail=fail):
                    paths, before = self.prepare_rollback()
                    destination = (self.tmp if outside else self.root) / "resolved-view.md"
                    destination.write_bytes(before[doc])
                    def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                        result = resolve(path, *args, **kwargs)
                        return destination if result == doc else result
                    read = Path.read_bytes
                    def read_resolved(path: Path) -> bytes:
                        return read(destination if path == doc else path)
                    replace = Path.replace
                    published = []
                    def publish(path: Path, target: Path) -> Path:
                        published.append(target)
                        if fail and target == paths[-1]:
                            raise PermissionError("last output unavailable")
                        return replace(path, target)
                    # Models reparse/short-name resolution without requiring
                    # Windows symlink privileges; the identity checks stay real.
                    with mock.patch.object(Path, "resolve", resolved), \
                            mock.patch.object(Path, "read_bytes", read_resolved), \
                            mock.patch.object(Path, "replace", publish):
                        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                    if outside:
                        self.assertEqual(code, 1, err)
                        self.assertIn("path escapes root: docs/acceptance-matrix.md", err)
                        self.assertEqual(published, [])
                    else:
                        self.assertEqual(code, 2 if fail else 0, err)
                        self.assertEqual(published, [destination, *paths[1:]])
                    if outside or fail:
                        self.assertEqual(destination.read_bytes(), before[doc])
                        self.assertEqual({path: path.read_bytes() for path in paths}, before)
                        self.assertNotIn("rollback incomplete", err)
                    else:
                        self.assertEqual(destination.read_bytes(), MODULE.render_doc(
                            MODULE.load_matrix(self.matrix_path)).encode("utf-8"))
                    self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def test_generated_document_evidence_stays_in_refresh_snapshot(self) -> None:
        doc = self.root / "docs/acceptance-matrix.md"
        validate = MODULE.validate
        read = MODULE._read_transaction_file
        for failure in ("changed", "unreadable", "removed"):
            with self.subTest(failure=failure):
                paths, before = self.prepare_rollback()
                checked = False
                def validate_then_change(*args: Any, **kwargs: Any) -> Any:
                    nonlocal checked
                    result = validate(*args, **kwargs)
                    self.assertEqual(result, [])
                    checked = True
                    if failure == "removed":
                        doc.unlink()
                    elif failure == "changed":
                        doc.write_bytes(b"External document edit.\n")
                    return result
                def read_after_validation(path: Path, limit: int, **kwargs: Any) -> Any:
                    if checked and path == doc and failure == "unreadable":
                        raise PermissionError("document unreadable")
                    return read(path, limit, **kwargs)
                with mock.patch.object(MODULE, "validate", validate_then_change), \
                        mock.patch.object(MODULE, "_read_transaction_file", read_after_validation), \
                        mock.patch.object(Path, "replace") as publish:
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertTrue(checked)
                self.assertEqual(code, 2, err)
                self.assertIn("document unreadable" if failure == "unreadable" else "input snapshot changed", err)
                publish.assert_not_called()
                self.assertEqual({path: path.read_bytes() for path in paths[1:]},
                                 {path: before[path] for path in paths[1:]})
                if failure == "removed":
                    self.assertFalse(doc.exists())
                else:
                    self.assertEqual(doc.read_bytes(), b"External document edit.\n" if failure == "changed" else before[doc])
                self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def test_generated_document_alias_retargeting_is_still_snapshotted(self) -> None:
        paths, before = self.prepare_rollback()
        doc = paths[0]
        first, second = self.root / "first-view.md", self.root / "second-view.md"
        for path in (first, second):
            path.write_bytes(before[doc])
        current = first
        resolve, read, validate = Path.resolve, Path.read_bytes, MODULE.validate
        def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
            result = resolve(path, *args, **kwargs)
            return current if result == doc else result
        def read_resolved(path: Path) -> bytes:
            return read(current if path == doc else path)
        def validate_then_retarget(*args: Any, **kwargs: Any) -> Any:
            nonlocal current
            result = validate(*args, **kwargs)
            self.assertEqual(result, [])
            current = second  # Identical bytes, but a different alias target.
            return result
        with mock.patch.object(Path, "resolve", resolved), \
                mock.patch.object(Path, "read_bytes", read_resolved), \
                mock.patch.object(MODULE, "validate", validate_then_retarget), \
                mock.patch.object(Path, "replace") as publish:
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(current, second)
        self.assertEqual(code, 2, err)
        self.assertIn("input snapshot changed", err)
        publish.assert_not_called()
        self.assertEqual({path: path.read_bytes() for path in paths}, before)
        self.assertEqual(first.read_bytes(), before[doc])
        self.assertEqual(second.read_bytes(), before[doc])
        self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def test_default_output_destinations_use_resolved_paths_for_publish_and_rollback(self) -> None:
        self.matrix_path = self.root / "tools/ci/acceptance_matrix.json"
        resolve, replace = Path.resolve, Path.replace
        for name in ("document", "matrix", "inventory"):
            for fail in (False, True):
                with self.subTest(output=name, fail=fail):
                    paths, before = self.prepare_rollback(generated_evidence=False)
                    alias = dict(zip(("document", "matrix", "inventory"), paths))[name]
                    destination = self.tmp / ("resolved-" + alias.name)
                    destination.write_bytes(before[alias])
                    # Model resolution on Windows without symlink privileges;
                    # POSIX also exercises actual symlink replacement semantics.
                    def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                        result = resolve(path, *args, **kwargs)
                        return destination if result == alias else result
                    published = []
                    def publish(path: Path, target: Path) -> Path:
                        published.append(target)
                        if fail and len(published) == 3:
                            raise PermissionError("last output unavailable")
                        return replace(path, target)
                    expected = [destination if path == alias else path for path in paths]
                    with contextlib.ExitStack() as stack:
                        if os.name == "nt":
                            stack.enter_context(mock.patch.object(Path, "resolve", resolved))
                        else:
                            alias.unlink()
                            alias.symlink_to(destination)
                            def restore_alias() -> None:
                                alias.unlink()
                                alias.write_bytes(before[alias])
                            stack.callback(restore_alias)
                        identity = alias.lstat()
                        with mock.patch.object(Path, "replace", publish):
                            code, _, err = self.run_main("--root", str(self.root), "--refresh-inventory",
                                                       "--preparation-receipt-file", self.base_args()[-1])
                        self.assertEqual(code, 2 if fail else 0, err)
                        self.assertEqual(published, expected)
                        self.assertTrue(os.path.samestat(identity, alias.lstat()))
                        if fail:
                            self.assertIn("last output unavailable", err)
                            self.assertNotIn("rollback incomplete", err)
                            self.assertEqual(destination.read_bytes(), before[alias])
                            self.assertEqual({path: path.read_bytes() for path in paths}, before)
                        else:
                            self.assertNotEqual(destination.read_bytes(), before[alias])
                        self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def test_default_input_destinations_are_resolved_for_readers(self) -> None:
        self.matrix_path = self.root / "tools/ci/acceptance_matrix.json"
        self.prepare_refresh()
        inventory = self.root / "tools/ci/acceptance_requirements.json"
        resolve = Path.resolve
        for alias in (self.matrix_path, inventory):
            with self.subTest(input=alias):
                destination = self.tmp / ("resolved-" + alias.name)
                destination.write_bytes(alias.read_bytes())
                original = alias.read_bytes()
                alias.write_bytes(b"partial input")
                def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                    result = resolve(path, *args, **kwargs)
                    return destination if result == alias else result
                try:
                    with mock.patch.object(Path, "resolve", resolved):
                        code, _, err = self.run_main("--root", str(self.root))
                    self.assertEqual(code, 0, err)
                    for data in (b"", b"{", b'{"schema": "unknown"}'):
                        destination.write_bytes(data)
                        with mock.patch.object(Path, "resolve", resolved):
                            code, _, err = self.run_main("--root", str(self.root))
                        self.assertNotEqual(code, 0, err)
                    read = Path.read_text
                    def unreadable(path: Path, *args: Any, **kwargs: Any) -> str:
                        if path == destination:
                            raise PermissionError("resolved input unreadable")
                        return read(path, *args, **kwargs)
                    with mock.patch.object(Path, "resolve", resolved), \
                            mock.patch.object(Path, "read_text", unreadable):
                        code, _, err = self.run_main("--root", str(self.root))
                    self.assertNotEqual(code, 0, err)
                    self.assertIn("resolved input unreadable", err)
                    destination.unlink()
                    with mock.patch.object(Path, "resolve", resolved):
                        code, _, err = self.run_main("--root", str(self.root))
                    self.assertNotEqual(code, 0, err)
                finally:
                    alias.write_bytes(original)

    def test_default_destinations_cannot_read_the_mandatory_lock_range(self) -> None:
        self.matrix_path = self.root / "tools/ci/acceptance_matrix.json"
        self.prepare_refresh()
        lock, resolve = MODULE._refresh_lock_path(self.root), Path.resolve
        for alias in (self.matrix_path, self.root / "tools/ci/acceptance_requirements.json",
                      self.root / "docs/acceptance-matrix.md"):
            for refresh in (False, True):
                with self.subTest(alias=alias, refresh=refresh):
                    def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                        result = resolve(path, *args, **kwargs)
                        return lock if result == alias else result
                    with mock.patch.object(Path, "resolve", resolved), \
                            mock.patch.object(MODULE, "load_matrix") as load:
                        code, _, err = self.run_main("--root", str(self.root),
                                                     *(["--refresh-inventory"] if refresh else []))
                    self.assertEqual(code, 2, err)
                    self.assertIn("refresh lock cannot be used as an input", err)
                    load.assert_not_called()

    def test_changed_or_unreadable_commit_input_restores_old_outputs(self) -> None:
        for failure in ("unreadable", "removed", "partial"):
            with self.subTest(failure=failure):
                self.write_inventory()
                self.prepare_refresh()
                source = self.root / MODULE.CONTRACT_DOCUMENTS[1]
                source.write_text("# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
                original = source.read_bytes()
                paths = (self.matrix_path, self.root / "tools/ci/acceptance_requirements.json",
                         self.root / "docs/acceptance-matrix.md")
                before = [path.read_bytes() for path in paths]
                replace = Path.replace
                read = MODULE._read_transaction_file
                replaced = False
                def replace_then_change(file: Path, target: Path) -> Path:
                    nonlocal replaced
                    result = replace(file, target)
                    replaced = True
                    if failure == "removed":
                        source.unlink()
                    elif failure == "partial":
                        source.write_bytes(b"# Part")
                    return result
                def unreadable(file: Path, limit: int, **kwargs: Any) -> Any:
                    if replaced and file == source and failure == "unreadable":
                        raise PermissionError("commit input unreadable")
                    return read(file, limit, **kwargs)
                try:
                    with mock.patch.object(Path, "replace", replace_then_change), \
                            mock.patch.object(MODULE, "_read_transaction_file", unreadable):
                        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                    self.assertEqual(code, 2, err)
                    self.assertIn("commit input unreadable" if failure == "unreadable"
                                  else "input snapshot changed", err)
                    self.assertEqual([path.read_bytes() for path in paths], before)
                finally:
                    source.write_bytes(original)

    def assert_concurrent_output_edits_survive_rollback(self, changed_indices: tuple[int, ...]) -> None:
        paths, before = self.prepare_rollback()
        changed = {paths[index]: b"editor save: " + paths[index].name.encode("utf-8")
                   for index in changed_indices}
        replace = Path.replace
        copies: dict[Path, bytes] = {}
        def edit_after_publication(file: Path, target: Path) -> Path:
            result = replace(file, target)
            if target == paths[-1]:
                copies.update({path: path.read_bytes() for path in self.tmp.rglob(".acceptance-*")})
                for path, data in changed.items():
                    path.write_bytes(data)
            return result
        with mock.patch.object(Path, "replace", edit_after_publication):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        for path in paths:
            self.assertEqual(path.read_bytes(), changed.get(path, before[path]))
        self.assertIn("rollback incomplete", err)
        self.assertEqual(len(copies), len(paths))
        for path in changed:
            self.assertIn(f"rollback conflict for {path}", err)
        for copy, data in copies.items():
            self.assertEqual(copy.read_bytes(), data)
            self.assertIn(str(copy), err)
        self.assertCountEqual(copies.values(), before.values())
        self.assertEqual(set(self.tmp.rglob(".acceptance-*")), set(copies))

    def test_rollback_preserves_concurrent_matrix_edit(self) -> None:
        self.assert_concurrent_output_edits_survive_rollback((1,))

    def test_rollback_preserves_concurrent_inventory_edit(self) -> None:
        self.assert_concurrent_output_edits_survive_rollback((2,))

    def test_rollback_preserves_concurrent_matrix_and_inventory_edits(self) -> None:
        self.assert_concurrent_output_edits_survive_rollback((1, 2))

    def test_normal_rollback_retains_all_copies_until_every_restore_completes(self) -> None:
        paths, before = self.prepare_rollback()
        replace, restore = Path.replace, os.replace
        copies: dict[Path, bytes] = {}
        recoverable: list[bool] = []
        restored: list[Path] = []
        def fail_after_publication(file: Path, target: Path) -> Path:
            result = replace(file, target)
            if target == paths[-1]:
                copies.update({path: path.read_bytes() for path in self.tmp.rglob(".acceptance-*")})
                (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_bytes(b"external source edit\n")
            return result
        def record_recovery(file: Path, target: Path) -> None:
            if copies:
                recoverable.append(all(copy.exists() and copy.read_bytes() == data
                                       for copy, data in copies.items()))
                restored.append(target)
            restore(file, target)
        with mock.patch.object(Path, "replace", fail_after_publication), \
                mock.patch.object(os, "replace", record_recovery):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("input snapshot changed", err)
        self.assertNotIn("rollback incomplete", err)
        self.assertEqual(restored, list(reversed(paths)))
        self.assertEqual(recoverable, [True] * len(paths))
        self.assertEqual({path: path.read_bytes() for path in paths}, before)
        self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def test_rollback_faults_retain_all_copies_and_continue_other_restores(self) -> None:
        failures = ("restore", "restore-and-cleanup", "unreadable-output", "partial-output", "unreadable-final-output",
                    "unreadable-backup", "partial-backup", "create-candidate", "partial-candidate",
                    "candidate-mode", "unknown-identity", "removed-output", "replaced-output",
                    "directory-output", "changed-resolution", "changed-during-read",
                    "replaced-during-read", "changed-before-replace")
        for failure in failures:
            with self.subTest(failure=failure):
                paths, before = self.prepare_rollback()
                matrix = self.matrix_path
                source = self.root / MODULE.CONTRACT_DOCUMENTS[1]
                displaced = self.tmp / "displaced-matrix"
                replace, restore, read = Path.replace, os.replace, Path.read_bytes
                metadata, resolve, chmod, create = Path.stat, Path.resolve, Path.chmod, open
                unlink = Path.unlink
                verify = MODULE._RefreshSnapshot.verify
                bounded_read = MODULE._read_transaction_file
                copies: dict[Path, bytes] = {}
                published: dict[Path, bytes] = {}
                active = False
                reads = 0
                def fail_after_publication(file: Path, target: Path) -> Path:
                    result = replace(file, target)
                    if target == paths[-1]:
                        copies.update({path: read(path) for path in self.tmp.rglob(".acceptance-*")})
                        published.update({path: read(path) for path in paths})
                        if failure == "removed-output":
                            matrix.unlink()
                        elif failure in ("replaced-output", "directory-output"):
                            matrix.rename(displaced)
                            if failure == "directory-output":
                                matrix.mkdir()
                            else:
                                matrix.write_bytes(published[matrix])
                        source.write_bytes(b"external source edit\n")
                    return result
                def begin_rollback(snapshot: Any, published_outputs: dict[Path, bytes]) -> None:
                    nonlocal active
                    try:
                        verify(snapshot, published_outputs)
                    except (MODULE.MatrixError, OSError):
                        active = True
                        raise
                def locked_restore(file: Path, target: Path) -> None:
                    if active and target == matrix and failure in ("restore", "restore-and-cleanup"):
                        raise PermissionError("restore denied")
                    restore(file, target)
                def failed_cleanup(file: Path, *args: Any, **kwargs: Any) -> None:
                    if active and failure == "restore-and-cleanup" and file.parent == matrix.parent \
                            and file.name.startswith(".acceptance-") and file not in copies and file.exists():
                        raise PermissionError("restore candidate cleanup denied")
                    unlink(file, *args, **kwargs)
                def faulted_read(file: Path) -> bytes:
                    nonlocal reads
                    if active and file == matrix:
                        reads += 1
                        if failure == "unreadable-output" or (failure == "unreadable-final-output" and reads == 2):
                            raise PermissionError("published output unreadable")
                        if failure == "partial-output":
                            return read(file)[:8]
                        data = read(file)
                        if failure == "changed-during-read":
                            file.write_bytes(b"editor save during verification\n")
                        elif failure == "replaced-during-read":
                            file.rename(displaced)
                            file.write_bytes(data)
                        return data
                    if active and file in copies and copies[file] == before[matrix]:
                        if failure == "unreadable-backup":
                            raise PermissionError("recovery copy unreadable")
                        if failure == "partial-backup":
                            return read(file)[:8]
                        if failure == "changed-before-replace":
                            matrix.write_bytes(b"editor save while preparing restore\n")
                    return read(file)
                def faulted_bounded_read(file: Path, limit: int, **kwargs: Any) -> Any:
                    data, file_metadata = bounded_read(file, limit, **kwargs)
                    if active and (file == matrix or file in copies):
                        data = faulted_read(file)
                    return data, file_metadata
                def unknown_identity(file: Path, *args: Any, **kwargs: Any) -> os.stat_result:
                    result = metadata(file, *args, **kwargs)
                    if failure == "unknown-identity" and kwargs.get("follow_symlinks", True) \
                            and file.parent == matrix.parent and file.name.startswith(".acceptance-"):
                        values = list(result)
                        values[1] = 0
                        return os.stat_result(values)
                    return result
                def changed_resolution(file: Path, *args: Any, **kwargs: Any) -> Path:
                    if active and file == matrix and failure == "changed-resolution":
                        return displaced
                    return resolve(file, *args, **kwargs)
                def failed_mode(file: Path, mode: int, *args: Any, **kwargs: Any) -> None:
                    if active and failure == "candidate-mode" and file.parent == matrix.parent:
                        raise PermissionError("restore permission copy denied")
                    chmod(file, mode, *args, **kwargs)
                def faulted_candidate(file: Path, *args: Any, **kwargs: Any) -> Any:
                    if active and file.parent == matrix.parent and failure == "create-candidate":
                        raise PermissionError("restore creation denied")
                    handle = create(file, *args, **kwargs)
                    if active and file.parent == matrix.parent and failure == "partial-candidate":
                        write = handle.write
                        def partial(data: bytes) -> None:
                            write(data[:8])
                            raise OSError("partial restore candidate")
                        handle.write = partial
                    return handle
                with mock.patch.object(Path, "replace", fail_after_publication), \
                        mock.patch.object(MODULE._RefreshSnapshot, "verify", begin_rollback), \
                        mock.patch.object(os, "replace", locked_restore), \
                        mock.patch.object(Path, "read_bytes", faulted_read), \
                        mock.patch.object(MODULE, "_read_transaction_file", faulted_bounded_read), \
                        mock.patch.object(Path, "stat", unknown_identity), \
                        mock.patch.object(Path, "resolve", changed_resolution), \
                        mock.patch.object(Path, "chmod", failed_mode), \
                        mock.patch.object(Path, "unlink", failed_cleanup), \
                        mock.patch.object(MODULE, "open", faulted_candidate, create=True):
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 2, err)
                self.assertIn(f"rollback conflict for {matrix}", err)
                self.assertIn("rollback incomplete", err)
                for path in (paths[0], paths[2]):
                    self.assertEqual(path.read_bytes(), before[path])
                if failure == "removed-output":
                    self.assertFalse(matrix.exists())
                elif failure == "directory-output":
                    self.assertTrue(matrix.is_dir())
                    matrix.rmdir()
                elif failure in ("changed-during-read", "changed-before-replace"):
                    self.assertTrue(matrix.read_bytes().startswith(b"editor save"))
                else:
                    self.assertEqual(matrix.read_bytes(), published[matrix])
                self.assertEqual(len(copies), len(paths))
                extra_candidates = set(self.tmp.rglob(".acceptance-*")) - set(copies)
                self.assertEqual(len(extra_candidates), 1 if failure == "restore-and-cleanup" else 0)
                if failure == "restore-and-cleanup":
                    self.assertIn("cannot remove temporary output", err)
                    self.assertIn(str(next(iter(extra_candidates))), err)
                preparation = MODULE._refresh_lock_path(self.root).with_suffix(".preparation")
                if extra_candidates:
                    retained = preparation.read_bytes()
                    with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                        MODULE._recover_outputs(MODULE._refresh_lock_path(self.root).with_suffix(".transaction"))
                    self.assertEqual(preparation.read_bytes(), retained)
                    preparation.unlink()  # The fixture explicitly owns teardown of this injected conflict.
                for candidate in extra_candidates:
                    candidate.unlink()
                for copy, data in copies.items():
                    self.assertEqual(copy.read_bytes(), data)
                    self.assertIn(str(copy), err)
                    copy.unlink()
                displaced.unlink(missing_ok=True)
                MODULE._refresh_lock_path(self.root).with_suffix(".transaction").unlink(missing_ok=True)

    def test_receipt_rollback_guards_existing_and_created_outputs_for_every_writer(self) -> None:
        for refresh, existing, failure in ((refresh, existing, failure)
                                           for refresh in (False, True) for existing in (False, True)
                                           for failure in (("edited", "removed", "unchanged") if existing
                                                           else ("edited", "removed", "unchanged", "unlink-denied"))):
            with self.subTest(refresh=refresh, existing=existing, failure=failure):
                paths, originals = self.prepare_rollback()
                if not refresh:
                    (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
                        "# Contract\nEvery requirement has evidence.\n", encoding="utf-8")
                discovery, report = self.execution_inputs()
                receipt = self.tmp / "receipt.json"
                document = self.tmp / "additional.md"
                receipt.unlink(missing_ok=True)
                if existing:
                    receipt.write_bytes(b"previous execution receipt\n")
                document.write_bytes(b"previous additional document\n")
                before = {document: document.read_bytes(), **(originals if refresh else {})}
                if existing:
                    before[receipt] = receipt.read_bytes()
                replace, unlink = Path.replace, Path.unlink
                copies: dict[Path, bytes] = {}
                published = b""
                def fail_after_receipt(file: Path, target: Path) -> Path:
                    nonlocal published
                    if target == document:
                        raise PermissionError("additional document locked")
                    result = replace(file, target)
                    if target == receipt:
                        published = target.read_bytes()
                        copies.update({copy: data for copy in self.tmp.rglob(".acceptance-*")
                                       if (data := copy.read_bytes()) in before.values()})
                        if failure == "edited":
                            receipt.write_bytes(b"editor receipt save\n")
                        elif failure == "removed":
                            receipt.unlink()
                    return result
                def denied_unlink(file: Path, *args: Any, **kwargs: Any) -> None:
                    if published and file == receipt and failure == "unlink-denied":
                        raise PermissionError("receipt removal denied")
                    unlink(file, *args, **kwargs)
                args = [*self.base_args(), "--write-doc", str(document), "--junit", str(report),
                        "--discovery", str(discovery), "--execution-receipt", str(receipt)]
                if refresh:
                    args.append("--refresh-inventory")
                with mock.patch.object(Path, "replace", fail_after_receipt), \
                        mock.patch.object(Path, "unlink", denied_unlink):
                    code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
                self.assertEqual(code, 2, err)
                for path in (*paths, document):
                    self.assertEqual(path.read_bytes(), (originals if path in originals else before)[path])
                if failure == "unchanged":
                    self.assertNotIn("rollback incomplete", err)
                    if existing:
                        self.assertEqual(receipt.read_bytes(), before[receipt])
                    else:
                        self.assertFalse(receipt.exists())
                    self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])
                else:
                    self.assertIn(f"rollback conflict for {receipt}", err)
                    self.assertIn("rollback incomplete", err)
                    if failure == "removed":
                        self.assertFalse(receipt.exists())
                    else:
                        self.assertEqual(receipt.read_bytes(), b"editor receipt save\n"
                                         if failure == "edited" else published)
                    self.assertEqual(len(copies), len(before))
                    self.assertEqual(set(self.tmp.rglob(".acceptance-*")), set(copies))
                    for copy, data in copies.items():
                        self.assertEqual(copy.read_bytes(), data)
                        self.assertIn(str(copy), err)
                        copy.unlink()
                MODULE._refresh_lock_path(self.root).with_suffix(".transaction").unlink(missing_ok=True)

    def test_refresh_moves_statement_and_clears_only_derived_inventory_error(self) -> None:
        matrix = self.prepare_refresh()
        document = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        document.write_text("# Moved\n\nEvery requirement has evidence.\n", encoding="utf-8")
        self.assertErrorContains(matrix, "iss.126: inventory incomplete")
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.refresh_decisions(out), [])
        refreshed = MODULE.load_matrix(self.root / "tools/ci/acceptance_requirements.json")
        expected = copy.deepcopy(self.inventory)
        next(entry for entry in expected["requirements"] if entry["id"] == "ar.synthetic")["source"] = (
            MODULE.CONTRACT_DOCUMENTS[1] + "#moved")
        expected["requirements"].sort(key=lambda entry: entry["id"])
        self.assertEqual(refreshed, expected)
        updated = MODULE.load_matrix(self.matrix_path)
        self.assertEqual(row_of(updated, "iss.126"), dict(row_of(matrix, "iss.126"),
                         source=MODULE.CONTRACT_DOCUMENTS[1] + "#moved"))
        self.assertEqual(self.errors(updated), [])
        self.assertEqual((self.root / "docs/acceptance-matrix.md").read_text(encoding="utf-8"),
                         MODULE.render_doc(updated))
        document.write_text(document.read_text(encoding="utf-8") + "\nA new obligation.\n", encoding="utf-8")
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        self.assertIn("iss.126: inventory incomplete", err)
        self.assertEqual(self.refresh_decisions(out)[0]["entry"]["text"], "A new obligation.")

    def test_refresh_rewrapped_statement_preserves_exact_accounting(self) -> None:
        self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_bytes(
            b"# Contract\r\n\r\nEvery requirement\r\n  has evidence.\r\n")
        before = (self.root / "tools/ci/acceptance_requirements.json").read_bytes()
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.refresh_decisions(out), [])
        self.assertEqual((self.root / "tools/ci/acceptance_requirements.json").read_bytes(), before)

    def test_refresh_existing_unmapped_requirement_gets_fragment_and_remains_incomplete(self) -> None:
        matrix = self.prepare_refresh()
        row_of(matrix, "iss.126")["requirements"] = []
        self.write_matrix(matrix)
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        self.assertIn("iss.126: inventory incomplete", err)
        decisions = self.refresh_decisions(out)
        self.assertEqual(len(decisions), 1)
        self.assertEqual(decisions[0]["decision"], "unmapped-requirement")
        self.assertEqual(decisions[0]["entry"], self.inventory["requirements"][1])
        self.assertIn("iss.126", [row["id"] for row in decisions[0]["candidate_rows"]])
        self.assertEqual(MODULE.load_matrix(self.matrix_path), matrix)

    def test_refresh_new_reworded_removed_and_punctuation_changes_need_decisions(self) -> None:
        matrix = self.prepare_refresh()
        path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        original = path.read_text(encoding="utf-8")
        for document, new_text in (
            (original + "\nNew statements require evidence.\n", "New statements require evidence."),
            (original.replace("Every requirement", "Each requirement"), "Each requirement has evidence."),
            (original.replace("evidence.", "evidence!"), "Every requirement has evidence!"),
            ("# Contract\n", None),
        ):
            with self.subTest(document=document):
                path.write_text(document, encoding="utf-8")
                before = (self.root / "tools/ci/acceptance_requirements.json").read_bytes()
                code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 1, err)
                self.assertIn("iss.126: inventory incomplete", err)
                self.assertEqual((self.root / "tools/ci/acceptance_requirements.json").read_bytes(), before)
                decisions = self.refresh_decisions(out)
                missing = [item for item in decisions if item["decision"] == "unaccounted"]
                if new_text is not None:
                    self.assertEqual(len(missing), 1)
                    stub = missing[0]
                    self.assertEqual(stub["entry"]["text"], new_text)
                    self.assertEqual(stub["entry"]["quote"], new_text)
                    self.assertEqual(stub["entry"]["source"], MODULE.CONTRACT_DOCUMENTS[1] + "#contract")
                    self.assertIn("iss.126", [row["id"] for row in stub["candidate_rows"]])
                    self.assertRegex(stub["entry"]["id"], MODULE.ROW_ID_RE)
                else:
                    self.assertEqual(missing, [])
                    self.assertIn("inventory stale or duplicate statement", err)
                if document != original + "\nNew statements require evidence.\n":
                    retained = [item for item in decisions if item["decision"] != "unaccounted"]
                    self.assertEqual([item["entry"]["id"] for item in retained], ["ar.synthetic"])
                self.assertEqual(MODULE.load_matrix(self.matrix_path), matrix)

    def test_refresh_is_idempotent_and_consistent_tree_is_byte_and_mtime_noop(self) -> None:
        self.prepare_refresh()
        paths = [self.matrix_path, self.root / "tools/ci/acceptance_requirements.json",
                 self.root / "docs/acceptance-matrix.md"]
        for move in (False, True):
            if move:
                path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
                path.write_text(path.read_text(encoding="utf-8").replace("# Contract", "# Moved"), encoding="utf-8")
                self.assertEqual(self.run_main(*self.base_args(), "--refresh-inventory")[0], 0)
            before = [(path.read_bytes(), path.stat().st_mtime_ns) for path in paths]
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
            self.assertEqual(code, 0, err)
            self.assertEqual([(path.read_bytes(), path.stat().st_mtime_ns) for path in paths], before)

    def test_refresh_bytes_do_not_depend_on_entry_row_or_object_key_order(self) -> None:
        matrix = self.prepare_refresh()
        path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        path.write_text(path.read_text(encoding="utf-8").replace("# Contract", "# Moved")
                        + "\nA new obligation.\n", encoding="utf-8")
        outputs = []
        for reverse in (False, True):
            inventory = copy.deepcopy(self.inventory)
            reordered = copy.deepcopy(matrix)
            if reverse:
                inventory["requirements"].reverse()
                inventory = dict(reversed(list(inventory.items())))
                inventory["requirements"] = [dict(reversed(list(entry.items())))
                                             for entry in inventory["requirements"]]
                reordered["rows"].reverse()
                for row in reordered["rows"]:
                    row.get("requirements", []).reverse()
            (self.root / "tools/ci/acceptance_requirements.json").write_text(json.dumps(inventory), encoding="utf-8")
            self.write_matrix(reordered)
            paths = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                     self.root / "docs/acceptance-matrix.md")
            before = [file.read_bytes() for file in paths]
            code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
            self.assertEqual(code, 1, err)
            self.assertEqual([file.read_bytes() for file in paths], before)
            outputs.append(out)
        self.assertEqual(outputs[0], outputs[1])

    def test_refresh_inventory_only_move_canonicalizes_matrix_object_order(self) -> None:
        matrix = self.prepare_refresh()
        row_of(matrix, "iss.126")["source"] = "issue #126"
        matrix["rows"].sort(key=lambda row: row["id"])
        for row in matrix["rows"]:
            row.get("requirements", []).sort()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        outputs = []
        for sort_keys in (False, True):
            self.write_inventory()
            self.matrix_path.write_text(json.dumps(matrix, sort_keys=sort_keys), encoding="utf-8")
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
            self.assertEqual(code, 0, err)
            outputs.append(self.matrix_path.read_bytes())
        self.assertEqual(outputs[0], outputs[1])

    def test_refresh_duplicate_new_occurrences_get_distinct_editable_fragments(self) -> None:
        self.prepare_refresh()
        path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        path.write_text(path.read_text(encoding="utf-8")
                        + "\nNew evidence is required.\n\nNew evidence is required.\n", encoding="utf-8")
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        stubs = self.refresh_decisions(out)
        self.assertEqual(len(stubs), 2)
        self.assertEqual(len({stub["entry"]["id"] for stub in stubs}), 2)
        self.assertTrue(all(stub["entry"]["text"] == "New evidence is required." for stub in stubs))
        self.inventory["requirements"].append(stubs[0]["entry"])
        self.write_inventory()
        matrix = MODULE.load_matrix(self.matrix_path)
        row_of(matrix, "iss.126")["requirements"].append(stubs[0]["entry"]["id"])
        self.write_matrix(matrix)
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        remaining = self.refresh_decisions(out)
        self.assertEqual([stub["entry"]["id"] for stub in remaining], [stubs[1]["entry"]["id"]])
        self.inventory["requirements"].append(remaining[0]["entry"])
        self.write_inventory()
        row_of(matrix, "iss.126")["requirements"].append(remaining[0]["entry"]["id"])
        self.write_matrix(matrix)
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.refresh_decisions(out), [])

    def test_refresh_unicode_fragments_preserve_text_in_ascii_pipes(self) -> None:
        self.prepare_refresh()
        path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        statement = "Caf\u00e9 \u2264 \u03bb requires evidence."
        path.write_text(path.read_text(encoding="utf-8") + "\n" + statement + "\n", encoding="utf-8")
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        self.assertTrue(out.isascii())
        self.assertEqual(self.refresh_decisions(out)[0]["entry"]["text"], statement)

    def test_refresh_empty_inventory_or_documents_remains_incomplete(self) -> None:
        self.prepare_refresh()
        self.inventory["requirements"] = []
        self.write_inventory()
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        self.assertEqual(len(self.refresh_decisions(out)), 6)
        for document in MODULE.CONTRACT_DOCUMENTS:
            (self.root / document).write_text("# Empty\n", encoding="utf-8")
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        self.assertIn("inventory requirements must not be empty", err)
        self.assertEqual(self.refresh_decisions(out), [])

    def test_refresh_cross_document_move_and_aggregate_source_decision(self) -> None:
        matrix = self.prepare_refresh()
        source = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        target = self.root / MODULE.CONTRACT_DOCUMENTS[0]
        source.write_text("# Contract\n", encoding="utf-8")
        target.write_text(target.read_text(encoding="utf-8")
                          + "\n## Relocated\nEvery requirement has evidence.\n", encoding="utf-8")
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        self.assertEqual(row_of(MODULE.load_matrix(self.matrix_path), "iss.126")["source"],
                         MODULE.CONTRACT_DOCUMENTS[0] + "#relocated")
        self.write_inventory()
        row_of(matrix, "iss.126")["requirements"].append("dw.synthetic")
        self.write_matrix(matrix)
        paths = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                 self.root / "docs/acceptance-matrix.md")
        before = [(path.read_bytes(), path.stat().st_mtime_ns) for path in paths]
        first = self.run_main(*self.base_args(), "--refresh-inventory")
        code, out, err = first
        self.assertEqual(code, 1, err)
        decision = self.refresh_decisions(out)[0]
        self.assertEqual(decision["decision"], "row-source")
        self.assertEqual(decision["entry"], row_of(matrix, "iss.126"))
        self.assertEqual(decision["candidate_sources"], [MODULE.CONTRACT_DOCUMENTS[0] + "#contract",
                                                       MODULE.CONTRACT_DOCUMENTS[0] + "#relocated"])
        self.assertEqual(self.run_main(*self.base_args(), "--refresh-inventory"), first)
        self.assertEqual([(path.read_bytes(), path.stat().st_mtime_ns) for path in paths], before)

    def test_refresh_decisions_persist_without_any_writes_alongside_a_move(self) -> None:
        for decision_kind in ("row-source", "unaccounted", "removed-or-reworded-or-ambiguous",
                              "unmapped-requirement"):
            with self.subTest(decision=decision_kind):
                matrix = self.prepare_refresh()
                self.write_inventory()
                path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
                # The original heading remains valid, so ordinary validation alone
                # cannot detect an unresolved aggregate row-source decision.
                path.write_text("# Contract\n\n# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
                if decision_kind == "row-source":
                    row_of(matrix, "iss.126")["requirements"].append("dw.synthetic")
                elif decision_kind == "unaccounted":
                    path.write_text(path.read_text(encoding="utf-8") + "\nA new obligation.\n", encoding="utf-8")
                elif decision_kind == "removed-or-reworded-or-ambiguous":
                    document = self.root / MODULE.CONTRACT_DOCUMENTS[0]
                    original = document.read_text(encoding="utf-8")
                    document.write_text(original.replace("Every requirement has evidence.",
                                                         "Every requirement needs evidence."), encoding="utf-8")
                else:
                    matrix["rows"][0]["requirements"].remove("dw.synthetic")
                self.write_matrix(matrix)
                paths = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                         self.root / "docs/acceptance-matrix.md")
                before = [(file.read_bytes(), file.stat().st_mtime_ns) for file in paths]
                extra_doc = self.tmp / "additional.md"
                first = self.run_main(*self.base_args(), "--refresh-inventory", "--write-doc", str(extra_doc))
                self.assertEqual(first[0], 1, first[2])
                self.assertIn(decision_kind, {item["decision"] for item in self.refresh_decisions(first[1])})
                self.assertEqual(self.run_main(*self.base_args(), "--refresh-inventory", "--write-doc", str(extra_doc)),
                                 first)
                self.assertEqual([(file.read_bytes(), file.stat().st_mtime_ns) for file in paths], before)
                self.assertFalse(extra_doc.exists())
                if decision_kind == "removed-or-reworded-or-ambiguous":
                    document.write_text(original, encoding="utf-8")

    def _check_refresh_invalid_sources_use_validation_messages_before_any_relocation(self, row_id) -> None:
        matrix = self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        for selected_row in (row_id,):
            for invalid in ([], {}, 17, None, False, "", " "):
                for aggregate in (False, True):
                    with self.subTest(row=row_id, source=invalid, aggregate=aggregate):
                        malformed = copy.deepcopy(matrix)
                        row_of(malformed, row_id)["source"] = invalid
                        if aggregate:
                            row_of(malformed, "iss.126")["requirements"].append("dw.synthetic")
                        self.write_matrix(malformed)
                        paths = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                                 self.root / "docs/acceptance-matrix.md")
                        before = [path.read_bytes() for path in paths]
                        message = f"{row_id}: source must be a non-empty string"
                        self.assertIn(message, self.errors(malformed))
                        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                        self.assertEqual(code, 2, err)
                        self.assertIn(message, err)
                        self.assertEqual([path.read_bytes() for path in paths], before)

    def test_refresh_and_validation_refuse_unhashable_status_and_evidence_kind(self) -> None:
        matrix = self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        for key in ("status", "kind"):
            for invalid in ([], {}, 17, None, False):
                with self.subTest(key=key, value=invalid):
                    malformed = copy.deepcopy(matrix)
                    row = row_of(malformed, "iss.126")
                    (row if key == "status" else row["evidence"][0])[key] = invalid
                    self.write_matrix(malformed)
                    paths = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                             self.root / "docs/acceptance-matrix.md")
                    before = [path.read_bytes() for path in paths]
                    message = "unknown status:" if key == "status" else "unknown evidence kind:"
                    self.assertErrorContains(malformed, message)
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                    self.assertEqual(code, 1, err)
                    self.assertIn(message, err)
                    self.assertEqual([path.read_bytes() for path in paths], before)

    def test_refresh_ambiguous_and_duplicate_accounting_is_never_guessed(self) -> None:
        self.prepare_refresh()
        path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        path.write_text("# First\nEvery requirement has evidence.\n\n"
                        "# Second\nEvery requirement has evidence.\n", encoding="utf-8")
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        self.assertEqual(len(self.refresh_decisions(out)), 3)
        self.assertEqual(MODULE.load_matrix(self.root / "tools/ci/acceptance_requirements.json"), self.inventory)
        self.inventory["requirements"].append(dict(self.inventory["requirements"][1], id="ar.duplicate"))
        self.write_inventory()
        path.write_text("# Contract\nEvery requirement has evidence.\n\n"
                        "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        decisions = self.refresh_decisions(out)
        self.assertEqual(len(decisions), 3)
        self.assertEqual([item["entry"]["id"] for item in decisions
                          if item["decision"] == "unmapped-requirement"], ["ar.duplicate"])
        self.assertEqual([item["entry"]["id"] for item in decisions
                          if item["decision"] == "removed-or-reworded-or-ambiguous"], ["ar.synthetic"])
        self.assertEqual([item["entry"]["source"] for item in decisions
                          if item["decision"] == "unaccounted"], [MODULE.CONTRACT_DOCUMENTS[1] + "#moved"])
        self.assertEqual(MODULE.load_matrix(self.root / "tools/ci/acceptance_requirements.json"), self.inventory)

    def test_refresh_missing_unreadable_or_partial_contract_never_writes(self) -> None:
        self.prepare_refresh()
        path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        original = path.read_bytes()
        outputs = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                   self.root / "docs/acceptance-matrix.md")
        before = [output.read_bytes() for output in outputs]
        for content in (None, b"# Contract\n\xff", "directory"):
            with self.subTest(content=content):
                path.unlink(missing_ok=True)
                if content == "directory":
                    path.mkdir()
                elif content is not None:
                    path.write_bytes(content)
                try:
                    with mock.patch.object(Path, "replace") as replace:
                        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                    self.assertEqual(code, 2, err)
                    replace.assert_not_called()
                    self.assertEqual([output.read_bytes() for output in outputs], before)
                finally:
                    if path.is_dir():
                        path.rmdir()
                    path.write_bytes(original)
        read = Path.read_text
        def denied(file: Path, *args: Any, **kwargs: Any) -> str:
            if file == path:
                raise PermissionError("contract denied")
            return read(file, *args, **kwargs)
        with mock.patch.object(Path, "read_text", denied), mock.patch.object(Path, "replace") as replace:
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("contract denied", err)
        replace.assert_not_called()
        self.assertEqual([output.read_bytes() for output in outputs], before)

    def test_refresh_preserves_unknown_entry_metadata_and_rejects_invalid_row_lists(self) -> None:
        matrix = self.prepare_refresh()
        self.inventory["requirements"][1]["review_note"] = "Retained metadata."
        self.write_inventory()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        refreshed = MODULE.load_matrix(self.root / "tools/ci/acceptance_requirements.json")
        self.assertEqual(next(item for item in refreshed["requirements"] if item["id"] == "ar.synthetic")["review_note"],
                         "Retained metadata.")
        for rows in (None, [None], [dict(matrix["rows"][0], requirements=None)],
                     [dict(matrix["rows"][0], requirements=[{}])],
                     [dict(matrix["rows"][0], id="Invalid ID")],
                     [matrix["rows"][0], matrix["rows"][0]]):
            with self.subTest(rows=rows):
                self.write_matrix(dict(matrix, rows=rows))
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 2, err)
                self.assertIn("valid matrix row", err)

    def test_refresh_malformed_metadata_is_rejected_without_writes(self) -> None:
        self.prepare_refresh()
        for mutate in (
            lambda item: item.update(schema="unknown"),
            lambda item: item.update(documents=[]),
            lambda item: item.update(requirements=None),
            lambda item: item["requirements"].append(None),
            lambda item: item["requirements"][0].update(quote="absent quote"),
            lambda item: item["requirements"].append(item["requirements"][0]),
        ):
            inventory = copy.deepcopy(self.inventory)
            mutate(inventory)
            path = self.root / "tools/ci/acceptance_requirements.json"
            path.write_text(json.dumps(inventory), encoding="utf-8")
            before = path.read_bytes()
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
            self.assertEqual(code, 2, err)
            self.assertEqual(path.read_bytes(), before)

    def test_refresh_write_failure_keeps_old_bytes_and_removes_closed_temporary_file(self) -> None:
        self.prepare_refresh()
        path = self.root / "tools/ci/acceptance_requirements.json"
        document = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        document.write_text("# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        before = path.read_bytes()
        with mock.patch.object(Path, "replace", side_effect=PermissionError("replace denied")):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("replace denied", err)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list(path.parent.iterdir()), [path])

    def test_refresh_rolls_back_every_failed_replacement_including_windows_locks(self) -> None:
        for failed_name in ("document", "matrix", "inventory"):
            with self.subTest(failure=failed_name):
                self.prepare_refresh()
                self.write_inventory()
                (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
                    "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
                inventory = self.root / "tools/ci/acceptance_requirements.json"
                doc = self.root / "docs/acceptance-matrix.md"
                paths = {"document": doc, "matrix": self.matrix_path, "inventory": inventory}
                before = {key: path.read_bytes() for key, path in paths.items()}
                replaced = []
                replace = Path.replace
                def locked(file: Path, target: Path) -> Path:
                    if target == paths[failed_name]:
                        raise PermissionError("destination locked")
                    replaced.append(target)
                    return replace(file, target)
                with mock.patch.object(Path, "replace", locked):
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 2, err)
                self.assertIn("destination locked", err)
                self.assertEqual(inventory.read_bytes(), before["inventory"])
                expected_replaced = {"document": [], "matrix": [doc],
                                     "inventory": [doc, self.matrix_path]}
                self.assertEqual(replaced, expected_replaced[failed_name])
                for name, path in paths.items():
                    self.assertEqual(path.read_bytes(), before[name])
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory",
                                              "--check-doc", str(doc))
                self.assertEqual(code, 0, err)
                matrix = MODULE.load_matrix(self.matrix_path)
                self.assertEqual(self.errors(matrix), [])
                self.assertEqual(row_of(matrix, "iss.126")["source"], MODULE.CONTRACT_DOCUMENTS[1] + "#moved")
                self.assertEqual(doc.read_text(encoding="utf-8"), MODULE.render_doc(matrix))
                after = [(path.read_bytes(), path.stat().st_mtime_ns) for path in paths.values()]
                self.assertEqual(self.run_main(*self.base_args(), "--refresh-inventory")[0], 0)
                self.assertEqual([(path.read_bytes(), path.stat().st_mtime_ns) for path in paths.values()], after)
                self.assertEqual(set(inventory.parent.iterdir()), {inventory})

    def _check_refresh_stages_all_outputs_before_replacement_and_retries_partial_data(self, failure) -> None:
        create = open
        for fail_at in range(1, 9):
            with self.subTest(failure=failure, stage=fail_at):
                self.prepare_refresh()
                self.write_inventory()
                (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
                    "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
                paths = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                         self.root / "docs/acceptance-matrix.md")
                before = [path.read_bytes() for path in paths]
                extra_doc = self.tmp / "additional.md"
                receipt = self.tmp / "receipt.json"
                extra_doc.unlink(missing_ok=True)
                receipt.unlink(missing_ok=True)
                discovery, report = self.execution_inputs()
                args = [*self.base_args(), "--refresh-inventory", "--write-doc", str(extra_doc),
                        "--junit", str(report), "--discovery", str(discovery),
                        "--execution-receipt", str(receipt)]
                before_paths = set(self.tmp.rglob("*"))
                calls = 0
                def partial_file(*args: Any, **kwargs: Any) -> Any:
                    nonlocal calls
                    calls += 1
                    if calls == fail_at and failure == "create":
                        raise PermissionError("staged output creation denied")
                    handle = create(*args, **kwargs)
                    if calls == fail_at:
                        write = handle.write
                        def partial(data: bytes) -> None:
                            write(data[:8])
                            raise OSError("partial staged output")
                        handle.write = partial
                    return handle
                with mock.patch.object(MODULE, "open", side_effect=partial_file, create=True), \
                        mock.patch.object(Path, "replace") as replace:
                    code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
                self.assertEqual(code, 2, err)
                self.assertIn("partial staged output" if failure == "partial"
                              else "staged output creation denied", err)
                replace.assert_not_called()
                self.assertEqual([path.read_bytes() for path in paths], before)
                self.assertFalse(extra_doc.exists())
                self.assertFalse(receipt.exists())
                self.assertEqual(set(self.tmp.rglob("*")), before_paths)
                code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
                self.assertEqual(code, 0, err)
                self.assertEqual(extra_doc.read_bytes(), paths[2].read_bytes())
                self.assertEqual(json.loads(receipt.read_bytes())["matrix_sha256"], MODULE._sha256(self.matrix_path))

    def test_refresh_refuses_colliding_output_paths_without_writes(self) -> None:
        self.prepare_refresh()
        inventory = self.root / "tools/ci/acceptance_requirements.json"
        doc = self.root / "docs/acceptance-matrix.md"
        for target in (inventory, self.matrix_path):
            before = [path.read_bytes() for path in (inventory, self.matrix_path, doc)]
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory", "--write-doc", str(target))
            self.assertEqual(code, 2, err)
            self.assertIn("output paths must be distinct", err)
            self.assertEqual([path.read_bytes() for path in (inventory, self.matrix_path, doc)], before)
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory", "--write-doc", str(doc))
        self.assertEqual(code, 0, err)

    def test_refresh_unreadable_output_prevents_every_replacement(self) -> None:
        self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        extra_doc, receipt = self.tmp / "additional.md", self.tmp / "receipt.json"
        extra_doc.write_bytes(b"stale document\n")
        receipt.write_bytes(b"stale receipt\n")
        discovery, report = self.execution_inputs()
        paths = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                 self.root / "docs/acceptance-matrix.md", extra_doc, receipt)
        before = [path.read_bytes() for path in paths]
        read = MODULE._read_transaction_file
        for blocked in paths:
            with self.subTest(path=blocked):
                def denied(file: Path, limit: int, **kwargs: Any) -> Any:
                    if file == blocked:
                        raise PermissionError("output read denied")
                    return read(file, limit, **kwargs)
                with mock.patch.object(MODULE, "_read_transaction_file", denied), \
                        mock.patch.object(Path, "replace") as replace:
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory",
                                                  "--write-doc", str(extra_doc), "--junit", str(report),
                                                  "--discovery", str(discovery), "--execution-receipt", str(receipt))
                self.assertEqual(code, 2, err)
                self.assertIn("output read denied", err)
                replace.assert_not_called()
                self.assertEqual([path.read_bytes() for path in paths], before)

    def test_refresh_prepares_optional_doc_and_receipt_before_writes_and_retries(self) -> None:
        self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        paths = (self.root / "tools/ci/acceptance_requirements.json", self.matrix_path,
                 self.root / "docs/acceptance-matrix.md")
        before = [path.read_bytes() for path in paths]
        discovery, report = self.execution_inputs()
        original = discovery.read_bytes()
        receipt = self.tmp / "receipt.json"
        extra_doc = self.tmp / "additional.md"
        args = [*self.base_args(), "--refresh-inventory", "--write-doc", str(extra_doc),
                "--junit", str(report), "--discovery", str(discovery), "--execution-receipt", str(receipt)]
        for content in (b"{", b"[]", b'{"kind":"ctestInfo","tests":[]}', b"\xff"):
            discovery.write_bytes(content)
            code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
            self.assertEqual(code, 2, err)
            self.assertFalse(receipt.exists())
            self.assertFalse(extra_doc.exists())
            self.assertEqual([path.read_bytes() for path in paths], before)
        discovery.write_bytes(original)
        replace = Path.replace
        def locked(file: Path, target: Path) -> Path:
            if target == self.matrix_path:
                raise PermissionError("matrix locked")
            return replace(file, target)
        with mock.patch.object(Path, "replace", locked):
            code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
        self.assertEqual(code, 2, err)
        self.assertEqual(paths[0].read_bytes(), before[0])
        self.assertFalse(receipt.exists())
        self.assertFalse(extra_doc.exists())
        self.assertEqual([path.read_bytes() for path in paths], before)
        code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(receipt.read_text(encoding="utf-8"))["matrix_sha256"],
                         MODULE._sha256(self.matrix_path))
        self.assertEqual(self.errors(MODULE.load_matrix(self.matrix_path)), [])
        self.assertEqual(extra_doc.read_bytes(), paths[2].read_bytes())
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved again\nEvery requirement has evidence.\n", encoding="utf-8")
        all_paths = (*paths, extra_doc, receipt)
        old = [path.read_bytes() for path in all_paths]
        def inventory_locked(file: Path, target: Path) -> Path:
            if target == paths[0]:
                raise PermissionError("inventory locked")
            return replace(file, target)
        with mock.patch.object(Path, "replace", inventory_locked):
            code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
        self.assertEqual(code, 2, err)
        self.assertIn("inventory locked", err)
        self.assertEqual([path.read_bytes() for path in all_paths], old)
        for target in (*paths, extra_doc):
            with self.subTest(collision=target):
                code, _, err = self.run_main(*args, "--execution-receipt", str(target))
                self.assertEqual(code, 2, err)
                self.assertIn("output paths must be distinct", err)

    def test_refresh_partial_write_does_not_publish_partial_inventory(self) -> None:
        self.prepare_refresh()
        path = self.root / "tools/ci/acceptance_requirements.json"
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        before = path.read_bytes()
        create = open
        def partial_file(*args: Any, **kwargs: Any) -> Any:
            handle = create(*args, **kwargs)
            write = handle.write
            def partial(data: bytes) -> None:
                write(data[:8])
                raise OSError("partial write refused")
            handle.write = partial
            return handle
        with mock.patch.object(MODULE, "open", side_effect=partial_file, create=True):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("partial write refused", err)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list(path.parent.iterdir()), [path])

    def test_refresh_preserves_file_permissions_and_cleans_readonly_replacement_failure(self) -> None:
        self.prepare_refresh()
        path = self.root / "tools/ci/acceptance_requirements.json"
        path.chmod(0o640)
        permissions = path.stat().st_mode
        document = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        document.write_text("# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        self.assertEqual(path.stat().st_mode, permissions)
        document.write_text("# Moved again\nEvery requirement has evidence.\n", encoding="utf-8")
        path.chmod(0o440)
        readonly_mode = path.stat().st_mode
        before = path.read_bytes()
        unlink = Path.unlink
        denied = []
        replace = Path.replace
        def locked(file: Path, target: Path) -> Path:
            if target == path:
                self.assertEqual(file.stat().st_mode, readonly_mode)
                raise PermissionError("replace denied")
            return replace(file, target)
        def readonly(file: Path, *args: Any, **kwargs: Any) -> None:
            if file.parent == path.parent and file != path and not denied:
                denied.append(file)
                raise PermissionError("read-only candidate")
            unlink(file, *args, **kwargs)
        try:
            with mock.patch.object(Path, "replace", locked), mock.patch.object(Path, "unlink", readonly):
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        finally:
            path.chmod(0o640)
        self.assertEqual(code, 2, err)
        self.assertIn("replace denied", err)
        self.assertEqual(len(denied), 1)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list(path.parent.iterdir()), [path])

    def test_output_staging_collision_never_changes_or_removes_existing_files(self) -> None:
        matrix = self.prepare_refresh()
        doc = self.tmp / "additional.md"
        collision = self.tmp / (".acceptance-" + "a" * 32)
        collision.write_bytes(b"unrelated file\r\n")
        before = {path: path.read_bytes() for path in self.tmp.rglob("*") if path.is_file()}
        with mock.patch.object(MODULE.secrets, "token_hex", side_effect=lambda size: "a" * (2 * size)), \
                mock.patch.object(Path, "replace") as replace:
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory", "--write-doc", str(doc))
        self.assertEqual(code, 2, err)
        self.assertIn("cannot replace output", err)
        replace.assert_not_called()
        self.assertFalse(doc.exists())
        self.assertEqual({path: path.read_bytes() for path in self.tmp.rglob("*") if path.is_file()}, before)
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory", "--write-doc", str(doc))
        self.assertEqual(code, 0, err)
        self.assertEqual(doc.read_bytes(), MODULE.render_doc(matrix).encode("utf-8"))
        self.assertEqual(collision.read_bytes(), b"unrelated file\r\n")

    def test_new_document_modes_follow_umask_and_receipts_stay_private(self) -> None:
        for refresh in (False, True):
            self.prepare_refresh()
            discovery, report = self.execution_inputs()
            doc = self.tmp / "additional.md"
            receipt = self.tmp / "receipt.json"
            reference = self.tmp / "ordinary.md"
            for mask in (0o022, 0o027, 0o077):
                with self.subTest(refresh=refresh, umask=oct(mask)):
                    doc.unlink(missing_ok=True)
                    receipt.unlink(missing_ok=True)
                    reference.unlink(missing_ok=True)
                    previous = os.umask(mask)
                    try:
                        reference.write_bytes(b"ordinary creation\n")
                        with tempfile.NamedTemporaryFile(dir=self.tmp) as private:
                            private_mode = stat.S_IMODE(os.fstat(private.fileno()).st_mode)
                        args = [*self.base_args(), "--write-doc", str(doc), "--junit", str(report),
                                "--discovery", str(discovery), "--execution-receipt", str(receipt)]
                        if refresh:
                            args.append("--refresh-inventory")
                        code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
                    finally:
                        os.umask(previous)
                    self.assertEqual(code, 0, err)
                    self.assertEqual(stat.S_IMODE(doc.stat().st_mode),
                                     stat.S_IMODE(reference.stat().st_mode))
                    self.assertEqual(stat.S_IMODE(receipt.stat().st_mode), private_mode)
                    for output in (doc, receipt):
                        output.write_bytes(b"stale output\n")
                        output.chmod(0o640)
                    modes = [stat.S_IMODE(output.stat().st_mode) for output in (doc, receipt)]
                    code, _, err = self.run_main(*args, "--preparation-receipt-file", self.base_args()[-1])
                    self.assertEqual(code, 0, err)
                    self.assertEqual([stat.S_IMODE(output.stat().st_mode) for output in (doc, receipt)], modes)
                    self.assertEqual(doc.read_bytes(), MODULE.render_doc(MODULE.load_matrix(self.matrix_path)).encode("utf-8"))
                    self.assertIsInstance(json.loads(receipt.read_bytes()), dict)

    def test_refresh_failed_permission_copy_does_not_replace_inventory(self) -> None:
        self.prepare_refresh()
        path = self.root / "tools/ci/acceptance_requirements.json"
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        before = path.read_bytes()
        with mock.patch.object(Path, "chmod", side_effect=PermissionError("permission copy denied")):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("permission copy denied", err)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list(path.parent.iterdir()), [path])

    def test_refresh_consistent_crlf_doc_and_failed_doc_replacement(self) -> None:
        matrix = self.prepare_refresh()
        doc = self.root / "docs/acceptance-matrix.md"
        crlf = MODULE.render_doc(matrix).replace("\n", "\r\n").encode("utf-8")
        doc.write_bytes(crlf)
        self.assertNotIn(b"\r\r\n", crlf)
        before = doc.read_bytes(), doc.stat().st_mtime_ns
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        self.assertEqual((doc.read_bytes(), doc.stat().st_mtime_ns), before)
        doc.write_bytes(b"Stale generated view.\r\n")
        with mock.patch.object(Path, "replace", side_effect=PermissionError("doc replace denied")):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("cannot replace output", err)
        self.assertEqual(doc.read_bytes(), b"Stale generated view.\r\n")

    def test_unreadable_source_anchor_is_a_validation_error(self) -> None:
        original = MODULE._read_text
        def denied(path: Path) -> str:
            if path == self.root / "docs/d.md":
                raise PermissionError("source denied")
            return original(path)
        with mock.patch.object(MODULE, "_read_text", side_effect=denied):
            self.assertErrorContains(baseline_matrix(), "source cannot read docs/d.md")

    def test_refresh_audited_historical_exclusion_follows_only_unique_exact_relocation(self) -> None:
        self.prepare_refresh()
        audited = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_requirements.json")
        entry = next(item for item in audited["exclusions"] if item["id"] == "ar.history.001")
        self.inventory["exclusions"].append(entry)
        self.write_inventory()
        path = self.root / MODULE.CONTRACT_DOCUMENTS[1]
        original = path.read_text(encoding="utf-8")
        heading = "Historical ambient hash portability investigation"
        path.write_text(original + "\n## " + heading + "\n\n" + entry["text"] + "\n", encoding="utf-8")
        self.assertEqual(self.run_main(*self.base_args())[0], 0)
        path.write_text(original + "\n## Historical moved\n\n" + entry["text"] + "\n", encoding="utf-8")
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        refreshed = MODULE.load_matrix(self.root / "tools/ci/acceptance_requirements.json")
        self.assertEqual(refreshed["exclusions"], [dict(entry, source=MODULE.CONTRACT_DOCUMENTS[1] + "#historical-moved")])
        self.assertEqual(self.run_main(*self.base_args())[0], 0)
        for heading, changed_entry, copy_old in (
            ("Ordinary section", entry, False),
            ("Historical moved", dict(entry, category="example"), False),
            ("Historical moved", dict(entry, id="unreviewed.exclusion"), False),
            ("Historical moved", entry, True),
        ):
            with self.subTest(heading=heading, changed_entry=changed_entry, copy_old=copy_old):
                self.inventory["exclusions"] = [changed_entry]
                self.write_inventory()
                text = original + "\n## " + heading + "\n\n" + entry["text"] + "\n"
                if copy_old:
                    text += "\n## Historical ambient hash portability investigation\n\n" + entry["text"] + "\n"
                path.write_text(text, encoding="utf-8")
                code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 1, err)
                self.assertTrue(self.refresh_decisions(out))
                self.assertEqual(MODULE.load_matrix(self.root / "tools/ci/acceptance_requirements.json"), self.inventory)

    def test_refresh_reviewed_normative_override_retains_current_behaviour_context(self) -> None:
        self.prepare_refresh()
        audited = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_requirements.json")
        entries = [item for item in audited["exclusions"] if item["id"] in
                   {"dw.tiers.020", "dw.tiers.021", "dw.tiers.022"}]
        self.inventory["exclusions"] = entries
        self.write_inventory()
        path = self.root / MODULE.CONTRACT_DOCUMENTS[0]
        original = path.read_text(encoding="utf-8")
        paragraph = " ".join(entry["text"] for entry in entries)
        path.write_text(original + "\n## Moved tiers\n\n" + paragraph + "\n", encoding="utf-8")
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        refreshed = MODULE.load_matrix(self.root / "tools/ci/acceptance_requirements.json")
        self.assertEqual({entry["source"] for entry in refreshed["exclusions"]},
                         {MODULE.CONTRACT_DOCUMENTS[0] + "#moved-tiers"})
        self.write_inventory()
        path.write_text(original + "\n## Moved tiers\n\n" + paragraph.replace("Current behaviour:", "Observed behaviour:")
                        + "\n", encoding="utf-8")
        code, out, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 1, err)
        self.assertIn("not in a Current behaviour paragraph", err)
        self.assertTrue(self.refresh_decisions(out))
        self.assertEqual(MODULE.load_matrix(self.root / "tools/ci/acceptance_requirements.json"), self.inventory)

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

    def test_evidence_path_checks_are_shared_only_within_one_validation(self) -> None:
        matrix = baseline_matrix()
        evidence = matrix["rows"][0]["evidence"]
        evidence.extend([{"kind": "script", "ref": "tools/script.py"}] * 24)
        with mock.patch.object(MODULE, "_path_problem", wraps=MODULE._path_problem) as check:
            self.assertEqual(self.errors(matrix), [])
        self.assertEqual(sum(call.args[1] == "tools/script.py" for call in check.call_args_list), 1)
        self.assertEqual(sum(call.args[1] == "tools/run.ps1" for call in check.call_args_list), 1)
        (self.root / "tools/script.py").unlink()
        self.assertErrorContains(matrix, "file not found: tools/script.py")
        # A planned reference must not seed an existence answer for a landed
        # reference, in either order, including native command entry points.
        for kind, ref in (("script", "tools/script.py"), ("native", "tools/run.ps1")):
            (self.root / ref).unlink(missing_ok=True)
            item = {"kind": kind, "ref": ref}
            if kind == "native":
                item.update(command=NATIVE_COMMAND, receipt_schema=RECEIPT)
            for reverse in (False, True):
                items = [dict(item, planned="Future check; owner: fw"), item]
                if reverse:
                    items.reverse()
                matrix["rows"][0]["evidence"] = items
                with self.subTest(kind=kind, reverse=reverse):
                    self.assertErrorContains(matrix, f"file not found: {ref}")

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
        with mock.patch.object(MODULE, "_read_text", wraps=MODULE._read_text) as read, \
                mock.patch.object(Path, "read_text", autospec=True, side_effect=Path.read_text) as strict_read:
            with immutable_repository_reads():
                for _ in range(3):
                    self.assertEqual(MODULE._read_text(doc), "# Doc\n\n## Section\n")
                    self.assertIsNone(MODULE._path_problem(self.root, "docs/d.md", True))
                    self.assertEqual(doc.read_text(encoding="utf-8"), "# Doc\n\n## Section\n")
                self.assertEqual(read.call_count, 1)
                # _read_text's errors='replace' read and the strict UTF-8 read
                # each touch disk once despite repeated mutation validations.
                self.assertEqual(strict_read.call_count, 2)
            doc.write_text("# Changed\n", encoding="utf-8")
            with immutable_repository_reads():
                self.assertEqual(MODULE._read_text(doc), "# Changed\n")
                self.assertEqual(read.call_count, 2)
            doc.write_bytes(b"\xff")
            with immutable_repository_reads():
                with self.assertRaises(UnicodeDecodeError):
                    doc.read_text(encoding="utf-8")
            doc.unlink()
            with immutable_repository_reads():
                self.assertEqual(MODULE._path_problem(self.root, "docs/d.md", True),
                                 "file not found: docs/d.md")

    def test_execution_workflow_uses_the_native_producer_condition(self) -> None:
        workflow = (MODULE.DEFAULT_ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        linux = workflow.split("\n  linux:\n", 1)[1].split("\n  gns-compile-probe:\n", 1)[0]
        for name in ("Configure", "Test", "Test over-the-wire networking", "Upload test evidence"):
            with self.subTest(step=name):
                step = linux.split("      - name: " + name + "\n", 1)[1].split("      - name: ", 1)[0]
                condition = re.search(r"^        if: (.+)$", step, re.MULTILINE)
                self.assertIsNotNone(condition)
                self.assertTrue(condition[1].startswith("${{ needs.changes.outputs.needs_cpp != 'false'"))
                if name == "Test over-the-wire networking":
                    self.assertEqual(condition[1], "${{ needs.changes.outputs.needs_cpp != 'false' && matrix.shard != 2 }}")
                elif name != "Upload test evidence":
                    self.assertEqual(condition[1], "${{ needs.changes.outputs.needs_cpp != 'false' }}")
        aggregate = workflow.split("\n  linux-result:\n", 1)[1].split("\n  windows-msvc-result:\n", 1)[0]
        step = aggregate.split("      - name: Check acceptance matrix CTest execution\n", 1)[1].split("      - name: ", 1)[0]
        condition = re.search(r"^        if: (.+)$", step, re.MULTILINE)
        self.assertIsNotNone(condition)
        self.assertEqual(condition[1], "needs.linux.result == 'success' && needs.changes.outputs.needs_cpp != 'false'")
        self.assertLess(aggregate.index("name: Merge shard JUnit and check inventory\n"),
                        aggregate.index("name: Check acceptance matrix CTest execution\n"))

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

    def test_station_numeric_evidence_includes_floating_point_refusal_controls(self) -> None:
        matrix = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_matrix.json")
        row = row_of(matrix, "station.numeric-inputs")
        self.assertEqual(row["status"], "covered")
        self.assertIn("independent MXCSR rounding, FTZ and DAZ states", row["statement"])
        self.assertIn({"kind": "ctest", "ref": "StationDiscoveryNumericRefusals"}, row["evidence"])
        self.assertIn("missing FMA, disabled OS AVX state", row["statement"])
        self.assertIn({"kind": "ctest", "ref": "CommonLibraryIsaContract"}, row["evidence"])
        self.assertIn({"kind": "ctest", "ref": "StationDiscoveryCliSmoke"}, row["evidence"])

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

    def test_repository_boundary_contract_and_watchdog_rearming_evidence(self) -> None:
        matrix = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_matrix.json")
        rows = [row for row in matrix["rows"] if "ar.activation.050" in row.get("requirements", [])]
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row["status"], "partial")
        self.assertIn("outside -2^24 <= x,z < 2^24", row["statement"])
        self.assertIn("docs/active-regions.md#activation-and-ordering", row["discrepancy"])
        self.assertIn("`docs/active-regions.md#activation-and-ordering`", row["discrepancy"])
        self.assertIn("integrated boundary regression", row["discrepancy"])
        self.assertEqual(row["source"], "docs/active-regions.md#activation-and-ordering")
        self.assertIn("ActiveRegionLedgerTest.ValidAnchorBoundaries", row["discrepancy"])
        self.assertIn("half-open interval [-2^24, 2^24)", row["discrepancy"])
        self.assertIn("still require end-to-end evidence", row["discrepancy"])
        self.assertEqual(row["source"], "docs/active-regions.md#activation-and-ordering")
        self.assertIn("issue #250", row["discrepancy"])
        self.assertIn("docs/active-regions.md", row["discrepancy"])
        self.assertIn("coverage remains planned", row["discrepancy"])
        planned = next(item for item in row["evidence"]
                       if item["ref"] == "ActiveRegionLedgerTest.AnchorBoundaryInputRefusals")
        self.assertIn("owner: ds", planned["planned"])
        self.assertEqual(planned["ref"], "ActiveRegionLedgerTest.AnchorBoundaryInputRefusals")
        self.assertIn("half-open-boundary regression", planned["planned"])
        self.assertIn("rejecting and counting +2^24", planned["planned"])
        self.assertIn("accepting -2^24", planned["planned"])
        self.assertIn("adjacent exterior coordinates", planned["planned"])
        self.assertIn("next representable value below -2^24", planned["planned"])
        self.assertIn("accept -2^24", planned["planned"])
        self.assertIn("reject +2^24", planned["planned"])
        self.assertIn("both X and Z endpoints", planned["planned"])
        self.assertIn("adjacent interior coordinates", planned["planned"])
        self.assertIn("adjacent exterior coordinates", planned["planned"])
        self.assertIn("next representable float below -2^24", planned["planned"])
        self.assertIn("walking anchors, ground-object edits and pins", planned["planned"])
        self.assertIn(MODULE._escape_cell("Discrepancy: " + row["discrepancy"]), MODULE.render_doc(matrix))
        watchdog = row_of(matrix, "em.client-hang")
        self.assertIn("HangWatchdog.RearmsAfterHeartbeatResumes", {e["ref"] for e in watchdog["evidence"]})

    def test_repository_boundary_discrepancy_and_watchdog_rearming_evidence(self) -> None:
        matrix = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_matrix.json")
        rows = [row for row in matrix["rows"] if "ar.activation.050" in row.get("requirements", [])]
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row["status"], "partial")
        self.assertEqual(row["source"], "docs/active-regions.md#activation-and-ordering")
        self.assertIn("outside -2^24 <= x,z < 2^24", row["statement"])
        self.assertIn("observational, never hashed or saved", row["statement"])
        self.assertIn("ActiveRegionLedgerTest.ValidAnchorBoundaries", row["discrepancy"])
        self.assertIn("[-2^24, 2^24)", row["discrepancy"])
        self.assertIn("issue #250", row["discrepancy"])
        self.assertIn("integrated boundary regression", row["discrepancy"])
        self.assertEqual(row["evidence"][:3], [
            {"kind": "ctest", "ref": "ActiveRegionLedgerTest.ValidAnchorBoundaries"},
            {"kind": "ctest", "ref": "ActiveRegionLedgerTest.InvalidSchedulerInputsAreSkippedAndCounted"},
            {"kind": "ctest", "ref": "GameSessionHeadlessWorldTest.InvalidHostInputsNeverThrowAndAreCounted"},
        ])
        self.assertEqual(len(row["evidence"]), 5)
        planned = row["evidence"][4]
        self.assertEqual(planned["kind"], "ctest")
        self.assertEqual(planned["ref"], "ActiveRegionLedgerTest.SymmetricAnchorBoundaries")
        self.assertIn("owner: ds", planned["planned"])
        self.assertIn("accept -2^24", planned["planned"])
        self.assertIn("reject +2^24", planned["planned"])
        self.assertIn("walking anchors, ground-object edits and pins", planned["planned"])
        self.assertIn(MODULE._escape_cell(row["statement"]), MODULE.render_doc(matrix))
        self.assertIn("docs/active-regions.md", row["discrepancy"])
        self.assertIn("coverage remains planned", row["discrepancy"])
        self.assertIn("next representable float below -2^24", planned["planned"])
        self.assertIn("both X and Z endpoints", planned["planned"])
        self.assertIn("adjacent interior coordinates", planned["planned"])
        self.assertIn("adjacent exterior coordinates", planned["planned"])
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

    def test_merged_rows_with_stale_summary_are_rejected(self) -> None:
        matrix = baseline_matrix()
        original = MODULE.render_doc(matrix)
        added = copy.deepcopy(matrix["rows"][0])
        added.update(id="merged.covered", requirements=[])
        matrix["rows"].append(added)
        self.write_matrix(matrix)
        rendered = MODULE.render_doc(matrix)
        # A clean text merge can retain a parent's unchanged summary while
        # adding the other parent's row. Every row cell still matches exactly.
        stale = original.split("| Row |", 1)[0] + "| Row |" + rendered.split("| Row |", 1)[1]
        doc = self.root / "docs/acceptance-matrix.md"
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=newline):
                doc.write_bytes(stale.replace("\n", newline).encode("utf-8"))
                self.assertFalse(MODULE._doc_matches(doc, rendered))
                code, _, err = self.run_main(*self.base_args(), "--check-doc", str(doc))
                self.assertEqual(code, 1)
                self.assertIn("is stale", err)
                code, _, err = self.run_main(*self.base_args(), "--write-doc", str(doc))
                self.assertEqual(code, 0, err)
                self.assertEqual(doc.read_bytes(), rendered.encode("utf-8"))
                self.assertTrue(MODULE._doc_matches(doc, rendered))

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


class AcceptanceRecoveryTests(AcceptanceFixture):
    def test_recovery_reads_allocate_from_the_verified_file_size(self) -> None:
        path = self.tmp / "bounded-input"
        fdopen = os.fdopen
        for data in (b"", b"small", b"x" * 4096):
            with self.subTest(size=len(data)):
                path.write_bytes(data)
                requests = []
                def opened(*args: Any, **kwargs: Any) -> Any:
                    handle = fdopen(*args, **kwargs)
                    read = handle.read
                    def bounded(size: int) -> bytes:
                        requests.append(size)
                        return read(size)
                    handle.read = bounded
                    return handle
                with mock.patch.object(os, "fdopen", opened):
                    actual, identity = MODULE._read_transaction_file(path, MODULE._TRANSACTION_BYTES)
                self.assertEqual(actual, data)
                self.assertEqual(identity.st_size, len(data))
                self.assertEqual(requests, [len(data) + 1])

    def _check_retargeted_inventory_alias_recovers_the_actual_publication_destinations(self, denied) -> None:
        paths, before = self.prepare_rollback()
        inventory = paths[2]
        program = """
import os, pathlib, importlib.util, sys
spec = importlib.util.spec_from_file_location("acceptance_child", sys.argv[1])
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
module = checker.__dict__
alias, first, second = map(pathlib.Path, sys.argv[2:5])
current = first
resolve, replace = pathlib.Path.resolve, pathlib.Path.replace
verify = module["_RefreshSnapshot"].verify
def resolved(path, *args, **kwargs):
    return current if path == alias else resolve(path, *args, **kwargs)
def publish(path, target):
    global current
    result = replace(path, target)
    if target == first:
        current = second
    return result
def terminate_on_refusal(snapshot, published):
    try:
        verify(snapshot, published)
    except module["MatrixError"]:
        if current == second:
            os._exit(73)
        raise
pathlib.Path.resolve = resolved
pathlib.Path.replace = publish
module["_RefreshSnapshot"].verify = terminate_on_refusal
raise SystemExit(module["main"](sys.argv[5:]))
"""
        with self.subTest(denied=denied), self.retargetable_path(inventory, denied=denied) as (first, second, retarget):
            child = subprocess.run([sys.executable, "-I", "-B", "-c", program, str(MODULE_PATH),
                                    str(inventory), str(first), str(second), *self.base_args(),
                                    "--refresh-inventory"], capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
            self.remember_custody(child.stderr)
            self.assertEqual(child.returncode, 73, (child.stdout, child.stderr))
            journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
            entries = json.loads(journal.read_bytes())["outputs"]
            self.assertEqual([Path(entry["path"]) for entry in entries], [*paths[:2], first])
            retarget()
            identity = inventory.lstat()
            with mock.patch.object(MODULE, "load_matrix", side_effect=MODULE.MatrixError("stop after recovery")):
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
            self.assertEqual(code, 2, err)
            self.assertIn("stop after recovery", err)
            self.assertEqual(first.read_bytes(), before[inventory])
            self.assertEqual(second.read_bytes(), before[inventory])
            self.assertEqual(inventory.resolve(), second)
            self.assertTrue(os.path.samestat(identity, inventory.lstat()))
            self.assertEqual({path: path.read_bytes() for path in paths[:2]},
                             {path: before[path] for path in paths[:2]})
            self.assertFalse(journal.exists())
            self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        # Capture the actual staged bytes without a child per partition.
        # A dedicated terminated-writer regression qualifies this image; every
        # recovery refusal still gets fresh files and identities.
        seed = cls("runTest")
        try:
            seed.setUp()
            paths, _ = seed.prepare_rollback()
            def capture(journal, entries, **kwargs):
                cls.crash_image = tuple((paths.index(Path(entry["path"])),
                                         Path(entry["backup"]["path"]).read_bytes(),
                                         Path(entry["new"]["path"]).read_bytes()) for entry in entries)
                raise OSError("captured unpublished recovery fixture")
            with mock.patch.object(MODULE, "_write_transaction", side_effect=capture):
                code, _, error = seed.run_main(*seed.base_args(), "--refresh-inventory")
            if code != 2 or "captured unpublished recovery fixture" not in error:
                raise AssertionError(error)
        finally:
            seed.doCleanups()

    def test_crash_fixture_matches_terminated_publication(self) -> None:
        paths, _ = self.prepare_rollback()
        self.crash_publication(3, ["--refresh-inventory"])
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        entries = json.loads(journal.read_bytes())["outputs"]
        actual = tuple((paths.index(Path(entry["path"])),
                        Path(entry["backup"]["path"]).read_bytes(),
                        Path(entry["path"]).read_bytes()) for entry in entries)
        self.assertEqual(actual, self.crash_image)

    def interrupted_refresh(self, count: int) -> None:
        paths = (self.root / "docs/acceptance-matrix.md", self.matrix_path,
                 self.root / "tools/ci/acceptance_requirements.json")
        entries = []
        for index, (destination, old, new) in enumerate(self.crash_image):
            path = paths[destination]
            self.assertEqual(path.read_bytes(), old)
            records = []
            for offset, data in enumerate((old, new)):
                artifact = path.parent / (".acceptance-" + f"{2 * index + offset:032x}")
                with artifact.open("xb") as handle:
                    handle.write(data)
                records.append(MODULE._transaction_record(artifact))
            entries.append({"path": str(path), "old": records[0]["sha256"],
                            "backup": records[0], "new": records[1]})
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        MODULE._write_transaction(journal, entries, committed=False)
        for entry in entries[:count]:
            Path(entry["new"]["path"]).replace(entry["path"])

    def test_outputs_and_journal_use_real_fsync_after_flush(self) -> None:
        self.prepare_rollback()
        real_sync = self.real_fsync
        synced = []
        def sync(descriptor: int) -> None:
            metadata = os.fstat(descriptor)
            self.assertGreater(metadata.st_size, 0)
            synced.append(metadata.st_size)
            real_sync(descriptor)
        with mock.patch.object(os, "fsync", side_effect=sync):
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        # Six payloads, eighteen artifact records, four pending records, two
        # journals, initial enrollment, independent receipt, cleanup reset and retirement.
        self.assertEqual(len(synced), 34)

    def test_readonly_first_writer_journal_is_recovered_or_refused_before_retry_inputs(self) -> None:
        lock = MODULE._refresh_lock_path(self.root)
        journal = lock.with_suffix(".transaction")
        system_open, load, read = os.open, MODULE.load_matrix, MODULE._read_transaction_file
        for failure in (None, "partial", "unreadable", "unknown"):
            with self.subTest(failure=failure):
                paths, before = self.prepare_rollback()
                lock.unlink()
                matrix_reads = 0
                def readonly(path: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                    if Path(path) == lock and flags & os.O_CREAT:
                        raise OSError(errno.EROFS, "read-only metadata")
                    return system_open(path, flags, *args, **kwargs)
                def interleave(path: Path) -> Any:
                    nonlocal matrix_reads
                    if path == self.matrix_path:
                        matrix_reads += 1
                        if matrix_reads == 2:
                            self.assertFalse(journal.exists())
                            self.assertEqual({item: item.read_bytes() for item in paths}, before)
                            raise MODULE.MatrixError("stop after recovery")
                        old = load(path)
                        with MODULE._refresh_lock(self.root):
                            if failure is None:
                                self.interrupted_refresh(1)
                            else:
                                journal.write_bytes(b"{" if failure == "partial" else b'{"version":2}')
                        return old
                    return load(path)
                def read_journal(path: Path, limit: int) -> Any:
                    if path == journal and failure == "unreadable":
                        raise PermissionError("journal unreadable")
                    return read(path, limit)
                with mock.patch.object(os, "open", readonly), \
                        mock.patch.object(MODULE, "load_matrix", interleave), \
                        mock.patch.object(MODULE, "_read_transaction_file", read_journal):
                    code, out, err = self.run_main(*self.base_args())
                self.assertEqual(code, 2, err)
                self.assertEqual(out, "")
                self.assertIn("detect-and-retry", err)
                self.assertEqual(matrix_reads, 2 if failure is None else 1)
                if failure is None:
                    self.assertIn("stop after recovery", err)
                else:
                    self.assertIn("journal unreadable" if failure == "unreadable" else "invalid recovery journal", err)
                    self.assertEqual({item: item.read_bytes() for item in paths}, before)
                    journal.unlink()

    def test_static_validation_reads_source_archive_without_git_or_any_writes(self) -> None:
        self.prepare_refresh()
        metadata = self.root / ".git"
        (metadata / "acceptance-refresh.lock").unlink()
        metadata.rmdir()
        doc = self.root / "docs/acceptance-matrix.md"
        open_path, system_open = Path.open, os.open
        def read_only(path: Path, mode: str = "r", *args: Any, **kwargs: Any) -> Any:
            self.assertFalse(any(flag in mode for flag in "wax+"), (path, mode))
            return open_path(path, mode, *args, **kwargs)
        def no_write(path: Any, flags: int, *args: Any, **kwargs: Any) -> int:
            self.assertEqual(flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC), 0)
            return system_open(path, flags, *args, **kwargs)
        with mock.patch.object(Path, "open", read_only), mock.patch.object(os, "open", no_write), \
                mock.patch.object(MODULE.time, "monotonic", side_effect=OSError("timer unavailable")):
            for options in ([], ["--check-doc", str(doc)]):
                code, out, err = self.run_main(*self.base_args(), *options)
                self.assertEqual(code, 0, err)
                self.assertIn("static references ok", out)
        self.assertFalse(metadata.exists())

    def _check_termination_at_every_publication_boundary_recovers_before_reading(self, refresh, existing, count) -> None:
        with self.subTest(refresh=refresh, existing=existing, count=count):
            paths, before = self.prepare_rollback()
            if not refresh:
                (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
                    "# Contract\nEvery requirement has evidence.\n", encoding="utf-8")
            discovery, report = self.execution_inputs()
            doc, receipt = self.tmp / "additional.md", self.tmp / "receipt.json"
            doc.write_bytes(b"previous document\n")
            before[doc] = doc.read_bytes()
            receipt.unlink(missing_ok=True)
            if existing:
                receipt.write_bytes(b"previous receipt\n")
                before[receipt] = receipt.read_bytes()
            options = ["--write-doc", str(doc), "--junit", str(report),
                       "--discovery", str(discovery), "--execution-receipt", str(receipt)]
            if refresh:
                options.append("--refresh-inventory")
            self.crash_publication(count, options)
            self.assertTrue(any(path.read_bytes() != data for path, data in before.items())
                            or receipt.exists() != existing)
            # Stop at the very first input read: recovery must have
            # restored all outputs, rather than hiding damage by regenerating.
            with mock.patch.object(MODULE, "load_matrix",
                                   side_effect=MODULE.MatrixError("stop after recovery")):
                code, _, err = self.run_main(*self.base_args())
            self.assertEqual(code, 2, err)
            self.assertIn("stop after recovery", err)
            self.assertEqual({path: path.read_bytes() for path in before}, before)
            self.assertEqual(receipt.exists(), existing)
            self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])
            self.assertFalse(MODULE._refresh_lock_path(self.root).with_suffix(".transaction").exists())
            code, _, err = self.run_main(*self.base_args(), *options)
            self.assertEqual(code, 0, err)

    def _check_recovery_rejects_damaged_or_unknown_journal_before_any_input_read(self, batch) -> None:
        paths, _ = self.prepare_rollback()
        self.interrupted_refresh(1)
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        original = journal.read_bytes()
        baseline = json.loads(original)
        current = {path: path.read_bytes() for path in paths}
        mutations = [b"", b"{", b"\xff", b" " * (MODULE._JOURNAL_BYTES + 1)]
        changes = [
            lambda value: value.update(version=2),
            lambda value: value.update(version=True),
            lambda value: value.update(committed=None),
            lambda value: value.update(extra="unknown"),
            lambda value: value.update(outputs=[]),
            lambda value: value.update(outputs=[value["outputs"][0]] * 6),
            lambda value: value.update(outputs={}),
            lambda value: value["outputs"].__setitem__(0, None),
            lambda value: value["outputs"][0].update(path="relative"),
            lambda value: value["outputs"][0].update(path=str(journal)),
            lambda value: value["outputs"][1].update(path=value["outputs"][0]["path"]),
            lambda value: value["outputs"][0].update(old="unknown"),
            lambda value: value["outputs"][0].update(backup=None),
            lambda value: value["outputs"][0].update(new=None),
            lambda value: value["outputs"][0]["new"].update(extra="unknown"),
            lambda value: value["outputs"][0]["new"].update(path=str(self.tmp / "foreign")),
            lambda value: value["outputs"][0]["new"].update(inode=0),
            lambda value: value["outputs"][0]["new"].update(device=-1),
            lambda value: value["outputs"][0]["new"].update(size=-1),
            lambda value: value["outputs"][0]["new"].update(size=MODULE._TRANSACTION_BYTES + 1),
            lambda value: value["outputs"][0]["new"].update(inode="unknown"),
            lambda value: value["outputs"][0]["new"].update(sha256="unknown"),
            lambda value: value["outputs"][0]["backup"].update(sha256="0" * 64),
        ]
        for change in changes:
            value = copy.deepcopy(baseline)
            change(value)
            mutations.append(json.dumps(value).encode("utf-8"))
        for index, data in enumerate(mutations):
            if index % 3 != batch:
                continue
            with self.subTest(index=index):
                journal.write_bytes(data)
                with mock.patch.object(MODULE, "load_matrix") as load:
                    code, _, err = self.run_main(*self.base_args())
                self.assertEqual(code, 2, err)
                load.assert_not_called()
                self.assertEqual({path: path.read_bytes() for path in paths}, current)
                self.assertEqual(journal.read_bytes(), data)
        journal.write_bytes(original)
        read = MODULE._read_transaction_file
        def denied(path: Path, limit: int) -> Any:
            if path == journal:
                raise PermissionError("journal unreadable")
            return read(path, limit)
        with mock.patch.object(MODULE, "_read_transaction_file", denied), \
                mock.patch.object(MODULE, "load_matrix") as load:
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("journal unreadable", err)
        load.assert_not_called()
        self.assertEqual({path: path.read_bytes() for path in paths}, current)
        journal.unlink()
        os.link(MODULE._refresh_lock_path(self.root), journal)
        with mock.patch.object(MODULE, "load_matrix") as load:
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("aliases refresh lock", err)
        load.assert_not_called()

    def test_recovery_faults_preserve_files_and_retry_without_consuming_inputs(self) -> None:
        for failure in ("edited", "missing-output", "replaced-output", "missing-backup", "partial-backup",
                        "unreadable-backup", "unknown-identity", "restore-denied", "cleanup-denied"):
            with self.subTest(failure=failure):
                paths, before = self.prepare_rollback()
                self.interrupted_refresh(3)
                journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
                value = json.loads(journal.read_bytes())
                backup = Path(value["outputs"][1]["backup"]["path"])
                saved_backup = backup.read_bytes()
                current = {path: path.read_bytes() for path in paths}
                displaced = self.tmp / "displaced"
                if failure == "edited":
                    paths[1].write_bytes(b"external edit\n")
                elif failure == "missing-output":
                    paths[1].rename(displaced)
                elif failure == "replaced-output":
                    paths[1].rename(displaced)
                    paths[1].write_bytes(current[paths[1]])
                elif failure == "missing-backup":
                    backup.rename(displaced)
                elif failure == "partial-backup":
                    backup.write_bytes(saved_backup[:8])
                at_failure = {path: path.read_bytes() if path.exists() else None for path in paths}
                read, replace, unlink = MODULE._read_transaction_file, os.replace, Path.unlink
                def faulted_read(path: Path, limit: int) -> Any:
                    if path == backup and failure == "unreadable-backup":
                        raise PermissionError("backup unreadable")
                    data, metadata = read(path, limit)
                    if path == paths[1] and failure == "unknown-identity":
                        fields = list(metadata)
                        fields[1] = 0
                        metadata = os.stat_result(fields)
                    return data, metadata
                def denied_restore(source: Any, target: Any) -> None:
                    if Path(target) == paths[1] and failure == "restore-denied":
                        raise PermissionError("restore denied")
                    replace(source, target)
                def denied_cleanup(path: Path, *args: Any, **kwargs: Any) -> None:
                    if path == journal and failure == "cleanup-denied":
                        raise PermissionError("journal cleanup denied")
                    unlink(path, *args, **kwargs)
                with mock.patch.object(MODULE, "_read_transaction_file", faulted_read), \
                        mock.patch.object(os, "replace", denied_restore), \
                        mock.patch.object(Path, "unlink", denied_cleanup), \
                        mock.patch.object(MODULE, "load_matrix") as load:
                    code, _, err = self.run_main(*self.base_args())
                self.assertEqual(code, 2, err)
                load.assert_not_called()
                self.assertTrue(journal.exists())
                if failure not in ("restore-denied", "cleanup-denied"):
                    self.assertEqual({path: path.read_bytes() if path.exists() else None for path in paths}, at_failure)
                # Repair only the injected external damage, then retry recovery.
                if failure == "edited":
                    paths[1].write_bytes(current[paths[1]])
                elif failure in ("missing-output", "replaced-output"):
                    paths[1].unlink(missing_ok=True)
                    displaced.rename(paths[1])
                elif failure == "missing-backup":
                    displaced.rename(backup)
                elif failure == "partial-backup":
                    backup.write_bytes(saved_backup)
                with mock.patch.object(MODULE, "load_matrix",
                                       side_effect=MODULE.MatrixError("stop after recovery")):
                    code, _, err = self.run_main(*self.base_args())
                self.assertEqual(code, 2, err)
                self.assertIn("stop after recovery", err)
                self.assertEqual({path: path.read_bytes() for path in paths}, before)
                self.assertFalse(journal.exists())
                self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def _check_recovery_can_itself_be_terminated_between_restores(self, count) -> None:
        program = """
import os, pathlib, importlib.util, sys
spec = importlib.util.spec_from_file_location("acceptance_child", sys.argv[1])
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
module = checker.__dict__
replace = os.replace
remaining = int(sys.argv[2])
def restore(source, target):
    global remaining
    result = replace(source, target)
    remaining -= 1
    if remaining == 0:
        os._exit(73)
    return result
os.replace = restore
raise SystemExit(module["main"](sys.argv[3:]))
"""
        with self.subTest(count=count):
            paths, before = self.prepare_rollback()
            self.interrupted_refresh(3)
            child = subprocess.run([sys.executable, "-I", "-B", "-c", program,
                                    str(MODULE_PATH), str(count), *self.base_args()],
                                   capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
            self.remember_custody(child.stderr)
            self.assertEqual(child.returncode, 73, (child.stdout, child.stderr))
            with mock.patch.object(MODULE, "load_matrix",
                                   side_effect=MODULE.MatrixError("stop after recovery")):
                code, _, err = self.run_main(*self.base_args())
            self.assertEqual(code, 2, err)
            self.assertIn("stop after recovery", err)
            self.assertEqual({path: path.read_bytes() for path in paths}, before)
            self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def test_committed_recovery_keeps_new_outputs_and_handles_windows_readonly_cleanup(self) -> None:
        paths, before = self.prepare_rollback()
        self.interrupted_refresh(3)
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        value = json.loads(journal.read_bytes())
        value["committed"] = True
        journal.write_text(json.dumps(value), encoding="utf-8")
        current = {path: path.read_bytes() for path in paths}
        self.assertTrue(all(current[path] != before[path] for path in paths))
        backup = Path(value["outputs"][0]["backup"]["path"])
        unlink = Path.unlink
        denied = False
        def readonly(path: Path, *args: Any, **kwargs: Any) -> None:
            nonlocal denied
            if path == backup and not denied:
                denied = True
                raise PermissionError("read-only candidate")
            unlink(path, *args, **kwargs)
        with mock.patch.object(Path, "unlink", readonly), \
                mock.patch.object(MODULE, "load_matrix", side_effect=MODULE.MatrixError("stop after recovery")):
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("stop after recovery", err)
        self.assertTrue(denied)
        self.assertEqual({path: path.read_bytes() for path in paths}, current)
        self.assertFalse(journal.exists())
        self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])

    def test_transaction_bounds_and_special_files_fail_before_publication(self) -> None:
        output, journal = self.tmp / "output", self.tmp / "journal.transaction"
        output.write_bytes(b"original")
        with mock.patch.object(Path, "replace") as publish:
            with self.assertRaisesRegex(MODULE.MatrixError, "too many"):
                MODULE._replace_outputs([(output, "new")] * 6, journal=journal)
            for text in ("x" * 9, "é" * 5):
                with self.subTest(text=text), mock.patch.object(MODULE, "_TRANSACTION_BYTES", 8):
                    with self.assertRaisesRegex(MODULE.MatrixError, "size bound"):
                        MODULE._replace_outputs([(output, text)], journal=journal)
            with mock.patch.object(MODULE, "_TRANSACTION_BYTES", 4):
                with self.assertRaisesRegex(MODULE.MatrixError, "size bound"):
                    MODULE._replace_outputs([(output, "new")], journal=journal)
            publish.assert_not_called()
        self.assertEqual(output.read_bytes(), b"original")
        self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])
        for entries in ([], [{}] * 6):
            with self.subTest(entries=entries):
                with self.assertRaisesRegex(MODULE.MatrixError, "bound"):
                    MODULE._write_transaction(journal, entries, committed=False)
        with mock.patch.object(MODULE, "_JOURNAL_BYTES", 8):
            with self.assertRaisesRegex(MODULE.MatrixError, "bound"):
                MODULE._write_transaction(journal, [{}], committed=False)
        metadata = output.stat()
        for kind in ("identity", "special", "size", "changed"):
            fields = list(metadata)
            if kind == "identity":
                fields[1] = 0
            elif kind == "special":
                fields[0] = stat.S_IFIFO | 0o600
            elif kind == "size":
                fields[6] = MODULE._TRANSACTION_BYTES + 1
            else:
                fields[6] += 1
            changed = os.stat_result(fields)
            values = [metadata, changed, metadata] if kind == "changed" else [changed]
            with self.subTest(kind=kind), mock.patch.object(MODULE, "_transaction_metadata", side_effect=values):
                with self.assertRaises(MODULE.MatrixError):
                    MODULE._read_transaction_file(output, MODULE._TRANSACTION_BYTES)
        with self.assertRaisesRegex(MODULE.MatrixError, "size"):
            MODULE._read_transaction_file(output, 4)

    def test_journal_write_flush_and_publication_failures_preserve_outputs(self) -> None:
        for failure in ("write", "flush", "payload-sync", "sync", "commit-sync", "replace", "commit",
                        "candidate-identity"):
            with self.subTest(failure=failure):
                paths, before = self.prepare_rollback()
                journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
                candidate = journal.with_suffix(".pending")
                fdopen, replace, sync, metadata = os.fdopen, os.replace, os.fsync, MODULE._transaction_metadata
                calls = 0
                journal_syncs = 0
                journal_descriptor = None
                unknown_reported = False
                invocation = self.base_args()
                authority_file = Path(invocation[-1])
                def faulted_fdopen(descriptor: int, mode: str) -> Any:
                    nonlocal journal_descriptor
                    handle = fdopen(descriptor, mode)
                    if mode == "wb" and candidate.exists():
                        journal_descriptor = descriptor
                    if mode == "wb" and candidate.exists() and failure in ("write", "flush"):
                        if failure == "write":
                            handle.write = lambda data: 0
                        else:
                            handle.flush = mock.Mock(side_effect=OSError("journal flush denied"))
                    return handle
                def faulted_replace(source: Any, target: Any) -> None:
                    nonlocal calls
                    if Path(target) == journal:
                        calls += 1
                        if failure == "replace" or (failure == "commit" and calls == 2):
                            raise PermissionError("journal publication denied")
                    replace(source, target)
                def faulted_sync(descriptor: int) -> None:
                    nonlocal journal_syncs
                    # Enrollment also syncs metadata while pending is open.
                    # Fault the intended payload handle on both platforms.
                    with fdopen(os.dup(descriptor), "rb") as handle:
                        actual = metadata(handle)
                    is_journal = candidate.exists() and descriptor == journal_descriptor
                    enrollment_paths = [journal.with_suffix(".preparation"), authority_file,
                                        *journal.parent.glob(".acceptance-enrollment-*")]
                    is_marker = any(path.exists() and MODULE._same_transaction_identity(path, actual)
                                    for path in enrollment_paths)
                    if is_journal:
                        journal_syncs += 1
                    if ((failure == "payload-sync" and not is_marker and not is_journal) or (is_journal and (
                            (failure == "sync" and journal_syncs == 1)
                            or (failure == "commit-sync" and journal_syncs == 2)))):
                        raise OSError("sync denied")
                    sync(descriptor)
                def unknown_candidate(handle: Any) -> Any:
                    nonlocal unknown_reported
                    result = metadata(handle)
                    if (failure == "candidate-identity" and handle.fileno() == journal_descriptor
                            and candidate.exists() and not unknown_reported):
                        unknown_reported = True
                        fields = list(result)
                        fields[1] = 0
                        return os.stat_result(fields)
                    return result
                with mock.patch.object(os, "fdopen", faulted_fdopen), \
                        mock.patch.object(os, "replace", faulted_replace), \
                        mock.patch.object(os, "fsync", faulted_sync), \
                        mock.patch.object(MODULE, "_transaction_metadata", unknown_candidate):
                    code, _, err = self.run_main(*invocation, "--refresh-inventory")
                self.assertEqual(code, 2, err)
                if failure in ("sync", "commit-sync"):
                    self.assertEqual(journal_syncs, 1 if failure == "sync" else 2)
                self.assertEqual({path: path.read_bytes() for path in paths}, before)
                self.assertFalse(journal.exists())
                self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])
                # An unidentifiable candidate is retained rather than unlinked.
                self.assertEqual(candidate.exists(), failure == "candidate-identity")
                candidate.unlink(missing_ok=True)

    def test_pending_recovery_exclusive_lock_still_obeys_deadline(self) -> None:
        self.prepare_rollback()
        self.interrupted_refresh(1)
        with MODULE._refresh_lock(self.root, shared=True), \
                mock.patch.object(MODULE.time, "monotonic", side_effect=[0.0, 0.0, 3.0]), \
                mock.patch.object(MODULE, "load_matrix") as load:
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("within deadline", err)
        load.assert_not_called()
        with mock.patch.object(MODULE.time, "monotonic", side_effect=OSError("timer unavailable")), \
                mock.patch.object(MODULE, "load_matrix") as load:
            code, _, err = self.run_main(*self.base_args())
        self.assertEqual(code, 2, err)
        self.assertIn("timer unavailable", err)
        load.assert_not_called()

    def _check_refresh_defaults_and_optional_outputs_reject_all_input_aliases(self, destination_name) -> None:
        self.prepare_refresh()
        doc = self.root / "docs/acceptance-matrix.md"
        inventory = self.root / "tools/ci/acceptance_requirements.json"
        discovery, report = self.execution_inputs()
        for destination in ({"doc": doc, "inventory": inventory, "matrix": self.matrix_path}[destination_name],):
            for source in (self.root / MODULE.CONTRACT_DOCUMENTS[0], self.root / "docs/d.md",
                           self.root / "test/x_test.cpp", self.root / "tools/script.py",
                           self.root / "CMakeLists.txt", discovery, report):
                for alias in ("resolved", "hardlink"):
                    with self.subTest(destination=destination, source=source, alias=alias):
                        original, protected = destination.read_bytes(), source.read_bytes()
                        resolve = Path.resolve
                        def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                            result = resolve(path, *args, **kwargs)
                            return source if result == destination else result
                        with contextlib.ExitStack() as stack:
                            if alias == "hardlink":
                                destination.unlink()
                                os.link(source, destination)
                                def restore() -> None:
                                    destination.unlink()
                                    destination.write_bytes(original)
                                stack.callback(restore)
                            else:
                                # Resolution models symlinks and Windows 8.3 aliases
                                # on runners that cannot create symbolic links.
                                stack.enter_context(mock.patch.object(Path, "resolve", resolved))
                            with mock.patch.object(Path, "replace") as publish:
                                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory",
                                                             "--junit", str(report), "--discovery", str(discovery))
                            self.assertEqual(code, 2, err)
                            publish.assert_not_called()
                            self.assertEqual(source.read_bytes(), protected)
                        self.assertEqual(destination.read_bytes(), original)
        if destination_name != "doc":
            return
        # Optional destinations and output/output aliases use the same identity rule.
        for option in ("--write-doc", "--execution-receipt"):
            for source in (inventory, self.matrix_path, self.root / MODULE.CONTRACT_DOCUMENTS[0], doc):
                if option == "--write-doc" and source == doc:
                    continue
                with self.subTest(option=option, source=source):
                    target = self.tmp / "alias-output"
                    os.link(source, target)
                    before = source.read_bytes()
                    try:
                        code, _, err = self.run_main(*self.base_args(), option, str(target),
                                                     "--junit", str(report), "--discovery", str(discovery))
                        self.assertEqual(code, 2, err)
                        self.assertIn("output path overlaps an input", err)
                        self.assertEqual(source.read_bytes(), before)
                    finally:
                        target.unlink()

    def test_default_document_symlinks_and_output_hardlinks_are_refused(self) -> None:
        self.prepare_refresh()
        doc = self.root / "docs/acceptance-matrix.md"
        source = self.root / MODULE.CONTRACT_DOCUMENTS[0]
        original, protected = doc.read_bytes(), source.read_bytes()
        resolve = Path.resolve
        for denied in (False, True):
            with self.subTest(symlink_denied=denied), contextlib.ExitStack() as stack:
                doc.unlink()
                if denied:
                    stack.enter_context(mock.patch.object(Path, "symlink_to", side_effect=PermissionError("denied")))
                try:
                    doc.symlink_to(source)
                except OSError:
                    # Exercise equivalent resolution even without Windows
                    # symlink privileges. Neither case skips the refusal check.
                    doc.write_bytes(original)
                    def resolved(path: Path, *args: Any, **kwargs: Any) -> Path:
                        result = resolve(path, *args, **kwargs)
                        return source if result == doc else result
                    stack.enter_context(mock.patch.object(Path, "resolve", resolved))
                code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                self.assertEqual(code, 2, err)
                self.assertIn("output path overlaps an input", err)
                self.assertEqual(source.read_bytes(), protected)
                doc.unlink()
                doc.write_bytes(original)
        inventory = self.root / "tools/ci/acceptance_requirements.json"
        for destination, target in ((doc, inventory), (doc, self.matrix_path), (inventory, self.matrix_path)):
            with self.subTest(destination=destination, target=target):
                original = destination.read_bytes()
                destination.unlink()
                os.link(target, destination)
                before = target.read_bytes()
                try:
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
                    self.assertEqual(code, 2, err)
                    self.assertIn("output paths must be distinct", err)
                    self.assertEqual(target.read_bytes(), before)
                finally:
                    destination.unlink()
                    destination.write_bytes(original)

    def test_temporary_cleanup_rechecks_identity_and_preserves_replaced_candidates(self) -> None:
        output = self.tmp / "output"
        output.write_bytes(b"original")
        replaced: list[Path] = []
        def refuse_after_edit(published: Any) -> None:
            self.assertEqual(published, {})
            for path in self.tmp.glob(".acceptance-*"):
                if path.read_bytes() == b"candidate":
                    path.rename(self.tmp / "displaced-candidate")
                    path.write_bytes(b"external replacement")
                    replaced.append(path)
            raise OSError("source edited")
        with self.assertRaisesRegex(MODULE.MatrixError, "temporary output identity changed"):
            MODULE._replace_outputs([(output, "candidate")], verify=refuse_after_edit)
        self.assertEqual(len(replaced), 1)
        self.assertEqual(replaced[0].read_bytes(), b"external replacement")
        self.assertEqual(output.read_bytes(), b"original")

    def test_partial_pending_journal_is_reused_without_reading_it_as_input(self) -> None:
        paths, before = self.prepare_rollback()
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        pending = journal.with_suffix(".pending")
        foreign = b"{partial unpublished metadata"
        pending.write_bytes(foreign)
        identity = pending.lstat()
        with mock.patch.object(MODULE, "load_matrix") as load:
            code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 2, err)
        self.assertIn("uncertain recovery preparation", err)
        load.assert_not_called()
        self.assertEqual(pending.read_bytes(), foreign)
        self.assertTrue(os.path.samestat(identity, pending.lstat()))
        self.assertEqual({path: path.read_bytes() for path in paths}, before)
        self.assertFalse(journal.exists())

    def test_windows_recovery_identity_uses_full_handle_ids_when_crt_fields_are_zero(self) -> None:
        self.addCleanup(MODULE._windows_file_information.cache_clear)
        path = self.tmp / "identity"
        path.write_bytes(b"data")
        metadata = path.stat()
        fields = list(metadata)
        fields[1] = fields[2] = fields[3] = 0
        incomplete = os.stat_result(fields, {"st_mtime_ns": metadata.st_mtime_ns,
                                            "st_ctime_ns": metadata.st_ctime_ns})
        for failure in (None, 18, 1):
            with self.subTest(failure=failure), path.open("rb") as handle:
                kernel = mock.Mock()
                queried = []
                def query(native: Any, kind: int, pointer: Any, size: int) -> int:
                    self.assertEqual(size, ctypes.sizeof(pointer._obj))
                    self.assertEqual(native, primitive.get_osfhandle.return_value)
                    queried.append(kind)
                    if kind == failure:
                        return 0
                    if kind == 18:
                        pointer._obj.volume = (1 << 48) + 3
                        pointer._obj.identity[0] = 7
                        pointer._obj.identity[15] = 1
                    else:
                        pointer._obj.links = 2
                    return 1
                kernel.GetFileInformationByHandleEx.side_effect = query
                primitive = mock.Mock()
                platform = mock.Mock(wraps=os)
                platform.name = "nt"
                platform.fstat.return_value = incomplete
                with mock.patch.object(MODULE, "os", platform), \
                        mock.patch.dict(sys.modules, {"msvcrt": primitive}), \
                        mock.patch.object(ctypes, "WinDLL", return_value=kernel, create=True) as library, \
                        mock.patch.object(ctypes, "get_last_error", return_value=6, create=True), \
                        mock.patch.object(ctypes, "WinError", return_value=OSError("identity query denied"),
                                          create=True):
                    MODULE._windows_file_information.cache_clear()
                    if failure is None:
                        result = MODULE._transaction_metadata(handle)
                        self.assertEqual(result.st_ino, (1 << 120) + 7)
                        self.assertEqual(result.st_dev, (1 << 48) + 3)
                        self.assertEqual(result.st_nlink, 2)
                        self.assertEqual(result.st_mtime_ns, metadata.st_mtime_ns)
                        self.assertEqual(queried, [18, 1])
                    else:
                        with self.assertRaisesRegex(OSError, "identity query denied"):
                            MODULE._transaction_metadata(handle)
                        self.assertEqual(queried, [18] if failure == 18 else [18, 1])
                    self.assertIs(MODULE._windows_file_information()[0], kernel.GetFileInformationByHandleEx)
                    library.assert_called_once_with("kernel32", use_last_error=True)
                    primitive.get_osfhandle.assert_called_once_with(handle.fileno())
                    self.assertEqual(len(kernel.GetFileInformationByHandleEx.argtypes), 4)
                    self.assertIs(kernel.GetFileInformationByHandleEx.restype, ctypes.wintypes.BOOL)
        opened = []
        def identity(handle: Any) -> Any:
            self.assertFalse(handle.closed)
            opened.append(handle)
            return metadata
        platform = mock.Mock(wraps=os)
        platform.name = "nt"
        platform.O_RDONLY = os.O_RDONLY
        platform.O_NONBLOCK = getattr(os, "O_NONBLOCK", 0)
        with mock.patch.object(MODULE, "os", platform), \
                mock.patch.object(MODULE, "_transaction_metadata", identity):
            self.assertTrue(MODULE._same_transaction_identity(path, metadata))
            fields = list(metadata)
            fields[1] += 1
            self.assertFalse(MODULE._same_transaction_identity(path, os.stat_result(fields)))
        self.assertTrue(all(handle.closed for handle in opened))

    @unittest.skipUnless(os.name == "nt", "requires the Windows file-information API")
    def test_windows_recovery_identity_with_real_handles_hardlinks_and_short_names(self) -> None:
        import msvcrt
        from ctypes import wintypes

        MODULE._windows_file_information.cache_clear()
        path = self.tmp / "long recovery identity filename.dat"
        alias, displaced = self.tmp / "alias.dat", self.tmp / "previous.dat"
        path.write_bytes(b"data")
        with path.open("rb") as handle:
            self.assertNotEqual(msvcrt.get_osfhandle(handle.fileno()), -1)
            identity = MODULE._transaction_metadata(handle)
            crt = os.fstat(handle.fileno())
            fields = list(crt)
            fields[1:4] = [0, 0, 0]
            with mock.patch.object(os, "fstat", return_value=os.stat_result(fields)):
                recovered = MODULE._transaction_metadata(handle)  # Real native queries.
            self.assertTrue(os.path.samestat(identity, recovered))
            self.assertEqual(recovered.st_nlink, identity.st_nlink)
        self.assertGreater(identity.st_ino, 0)
        self.assertGreaterEqual(identity.st_dev, 0)
        self.assertEqual((identity.st_nlink, identity.st_size), (1, 4))
        query, identity_type, standard_type = MODULE._windows_file_information()
        self.assertEqual(ctypes.sizeof(identity_type), 24)
        self.assertEqual(ctypes.sizeof(standard_type), 24)
        self.assertEqual(query.argtypes[0], wintypes.HANDLE)
        os.link(path, alias)
        with alias.open("rb") as handle:
            linked = MODULE._transaction_metadata(handle)
        self.assertEqual(linked.st_nlink, 2)
        self.assertTrue(os.path.samestat(identity, linked))
        self.assertTrue(MODULE._same_transaction_identity(alias, identity))
        with self.assertRaisesRegex(MODULE.MatrixError, "sole link"):
            MODULE._read_transaction_file(alias, 4)
        alias.unlink()
        short_path = ctypes.WinDLL("kernel32", use_last_error=True).GetShortPathNameW
        short_path.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
        short_path.restype = wintypes.DWORD
        # Windows path buffers are bounded to 32,768 UTF-16 code units here.
        buffer = ctypes.create_unicode_buffer(32768)
        length = short_path(str(path), buffer, len(buffer))
        self.assertGreater(length, 0)
        self.assertLess(length, len(buffer))
        self.assertTrue(MODULE._same_transaction_identity(Path(buffer.value), identity))
        data, read_identity = MODULE._read_transaction_file(path, 4)
        self.assertEqual(data, b"data")
        self.assertTrue(os.path.samestat(identity, read_identity))
        path.replace(displaced)
        path.write_bytes(b"data")
        self.assertFalse(MODULE._same_transaction_identity(path, identity))
        self.assertTrue(MODULE._same_transaction_identity(displaced, identity))


class ProductionAcceptanceMatrixTests(_PhaseCase):
    def test_production_inputs_allow_refresh_and_standard_document_write(self) -> None:
        root = MODULE.DEFAULT_ROOT
        doc = root / "docs/acceptance-matrix.md"
        paths = (root / "tools/ci/acceptance_matrix.json", root / "tools/ci/acceptance_requirements.json", doc)
        before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths}
        matrix = MODULE.load_matrix(paths[0])
        for row_id in ("perf.requirement-matrix", "iss.126"):
            self.assertIn({"kind": "doc", "ref": "docs/acceptance-matrix.md"}, row_of(matrix, row_id)["evidence"])
        expected = before[doc][0].decode("utf-8").replace("\r\n", "\n")
        publish = MODULE._replace_outputs
        # Read all production inputs in place. Redirect coordination metadata
        # and publication to scratch so Git metadata may be read-only and an
        # explicit write may normalize CRLF without changing the checkout.
        with tempfile.TemporaryDirectory() as temporary, tempfile.TemporaryDirectory() as caller:
            scratch = Path(temporary).resolve()
            destination = scratch / doc.name
            destination.write_bytes(expected.replace("\n", "\r\n").encode("utf-8"))
            def publish_document(outputs: list[tuple[Path, str]], **kwargs: Any) -> None:
                self.assertTrue(all(path == doc and text == expected for path, text in outputs))
                publish([(destination, text) for _, text in outputs], **kwargs)
            for options in (["--refresh-inventory"], ["--write-doc", str(doc)]):
                with self.subTest(options=options):
                    out, err = io.StringIO(), io.StringIO()
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
                            mock.patch.object(MODULE, "_refresh_lock_path",
                                              return_value=scratch / "acceptance-refresh.lock"), \
                            mock.patch.object(MODULE, "_replace_outputs", side_effect=publish_document) as publisher:
                        code = MODULE.main(["--root", str(root), *options, "--check-doc", str(doc),
                                            "--preparation-receipt-file", str(Path(caller) / "receipt.json")])
                    self.assertEqual(code, 0, err.getvalue())
                    self.assertIn(f"static references ok: {len(matrix['rows'])} rows", out.getvalue())
                    publisher.assert_called_once()
                    self.assertEqual(publisher.call_args.args[0], [] if "--refresh-inventory" in options
                                     else [(doc, expected)])
                    self.assertEqual({path: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths}, before)
                    if "--write-doc" in options:
                        self.assertEqual(destination.read_bytes(), expected.encode("utf-8"))
            self.assertEqual(list(scratch.glob(".acceptance-*")), [])
            self.assertFalse((scratch / "acceptance-refresh.transaction").exists())

    def test_runtime_and_save_rows_keep_native_qualification_pending(self) -> None:
        root = MODULE.DEFAULT_ROOT
        matrix = MODULE.load_matrix(root / "tools/ci/acceptance_matrix.json")
        recipe = (root / "docs/v0.3-acceptance.md").read_text(encoding="utf-8")
        command = ("python3 tools/ci/check_acceptance_matrix.py --discovery <discovery.json> "
                   "--junit <native.junit.xml> --execution-receipt <acceptance-execution.json> "
                   "--preparation-receipt-file <caller-metadata.json>")
        for row_id in ("iss.385", "iss.429"):
            with self.subTest(row=row_id):
                row = row_of(matrix, row_id)
                self.assertEqual(row["status"], "needs-native-run")
                self.assertIn("native execution remains required", row["statement"].lower())
                self.assertTrue(any(item["kind"] == "ctest" for item in row["evidence"]))
                native = [item for item in row["evidence"] if item["kind"] == "native"]
                self.assertEqual(native, [{
                    "kind": "native", "ref": "tools/ci/check_acceptance_matrix.py",
                    "command": command, "receipt_schema": "luminumbra.acceptance_execution.v1",
                }])
                self.assertIn({"kind": "doc", "ref": "docs/v0.3-acceptance.md"}, row["evidence"])
                errors: list[str] = []
                MODULE._check_status(errors.append, "covered", row["evidence"])
                self.assertIn("status covered inconsistent with evidence: "
                              "native evidence requires needs-native-run", errors)
                self.assertIn(f"| {row_id} | needs-native-run |", MODULE.render_doc(matrix))
        for text in ("### Native runtime configuration and durable saves",
                     "luminumbra.acceptance_execution.v1", "full_acceptance_qualified: false",
                     "separate-evidence-required", "issue #516"):
            self.assertIn(text, recipe)

    def test_runtime_and_save_ctest_receipts_cannot_qualify_native_evidence(self) -> None:
        source = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_matrix.json")
        matrix = dict(source, rows=[row_of(source, row_id) for row_id in ("iss.385", "iss.429")])
        before = copy.deepcopy(matrix)
        names = sorted({item["ref"] for row in matrix["rows"] for item in row["evidence"]
                        if item["kind"] == "ctest"})
        index = repository_index()
        with tempfile.TemporaryDirectory() as directory:
            discovery, report = (Path(directory) / name for name in ("discovery.json", "native.xml"))
            discovery.write_text(json.dumps({"kind": "ctestInfo", "tests": [
                {"name": name} for name in names
            ]}), encoding="utf-8")
            suite = ET.Element("testsuite")
            for name in names:
                ET.SubElement(suite, "testcase", name=name, status="run")
            ET.ElementTree(suite).write(report, encoding="utf-8", xml_declaration=True)
            receipt = MODULE.validate_execution(matrix, index, [report], discovery)
            self.assertTrue(receipt["ctest_references_passed"])
            self.assertEqual(receipt["schema"], "luminumbra.acceptance_execution.v1")
            self.assertEqual(receipt["scope"], "ctest-references-only")
            self.assertFalse(receipt["full_acceptance_qualified"])
            self.assertEqual({(item["row"], item["kind"], item["reason"])
                              for item in receipt["unevaluated"]}, {
                (row_id, kind, "separate-evidence-required")
                for row_id in ("iss.385", "iss.429") for kind in ("native", "doc")
            })
            # Each row must still refuse missing or adverse native test results.
            for row in matrix["rows"]:
                name = next(item["ref"] for item in row["evidence"] if item["kind"] == "ctest")
                for adverse in ("missing", "skipped", "failure", "error"):
                    with self.subTest(row=row["id"], adverse=adverse):
                        changed = copy.deepcopy(suite)
                        case = next(case for case in changed if case.get("name") == name)
                        if adverse == "missing":
                            changed.remove(case)
                        else:
                            ET.SubElement(case, adverse)
                        ET.ElementTree(changed).write(report, encoding="utf-8", xml_declaration=True)
                        refused = MODULE.validate_execution(matrix, index, [report], discovery)
                        self.assertFalse(refused["ctest_references_passed"])
                        self.assertFalse(refused["full_acceptance_qualified"])
            self.assertEqual(matrix, before)

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


class AcceptanceMatrixPublishabilityTests(_PhaseCase):
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


class AcceptanceMatrixPublicTreeTests(_PhaseCase):
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


class PublicTreeScanTests(_PhaseCase):
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


class AcceptanceCustodyTests(AcceptanceFixture):
    def legacy_recovery(self) -> tuple[Path, Path]:
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        output = self.tmp / "legacy-output"
        backup, candidate = (self.tmp / (".acceptance-" + str(i) * 32) for i in (1, 2))
        backup.write_bytes(b"old\r\n")
        candidate.write_bytes(b"new\r\n")
        old, new = MODULE._transaction_record(backup), MODULE._transaction_record(candidate)
        MODULE._write_transaction(journal, [{"path": str(output), "old": old["sha256"],
                                            "backup": old, "new": new}], committed=False)
        candidate.replace(output)
        return journal, output

    def _check_legacy_recovery_shares_custody_through_publication(self, mode) -> None:
        self.prepare_rollback() if mode == "refresh" else self.prepare_refresh()
        journal, recovered = self.legacy_recovery()
        marker = journal.with_suffix(".preparation")
        args = self.base_args()
        authority = Path(args[-1])
        export = self.tmp / "export.md"
        options = []
        if mode == "write":
            options = ["--write-doc", str(export)]
        elif mode in ("refresh", "unchanged"):
            options = ["--refresh-inventory"]
        elif mode in ("execution", "junit"):
            discovery, report = self.execution_inputs()
            options = ["--junit", str(report), "--discovery", str(discovery)]
            if mode == "execution":
                options.extend(("--execution-receipt", str(export)))
        run_locked, retire = MODULE._run_locked, MODULE._PreparationCustody.retire
        observed = []
        def after_recovery(invocation, root):
            observed.append((marker.exists(), invocation.preparation_custody))
            return run_locked(invocation, root)
        with mock.patch.object(MODULE, "_create_cli_custody", wraps=MODULE._create_cli_custody) as enroll, \
                mock.patch.object(MODULE, "_run_locked", after_recovery), \
                mock.patch.object(MODULE, "_replace_outputs", wraps=MODULE._replace_outputs) as publish, \
                mock.patch.object(os, "open", wraps=os.open) as opened, \
                mock.patch.object(MODULE._PreparationCustody, "retire", autospec=True, side_effect=retire) as retired:
            code, _, err = self.run_main(*args, *options)
        self.assertEqual(code, 0, err)
        receipt = authority.read_text(encoding="utf-8").strip()
        self.assertEqual(observed, [(True, receipt)])
        self.assertEqual(enroll.call_count, 1)
        receipt_opens = [call for call in opened.call_args_list if call.args[0] == authority]
        self.assertEqual(len(receipt_opens), 1)
        self.assertTrue(receipt_opens[0].args[1] & os.O_EXCL)
        self.assertEqual(retired.call_count, 1)
        self.assertEqual(err.count("preparation custody receipt: "), 1)
        self.assertEqual(recovered.read_bytes(), b"old\r\n")
        self.assertFalse(marker.exists())
        self.assertFalse(journal.exists())
        self.assertFalse(journal.with_suffix(".pending").exists())
        self.assertEqual(list(self.tmp.rglob(".acceptance-*")), [])
        if mode in ("write", "refresh", "execution"):
            self.assertEqual(publish.call_args.kwargs["custody"], receipt)
            if mode == "execution":
                self.assertTrue(json.loads(export.read_text(encoding="utf-8"))["ctest_references_passed"])
            else:
                document = export if mode == "write" else self.root / "docs/acceptance-matrix.md"
                self.assertEqual(document.read_text(encoding="utf-8"), MODULE.render_doc(MODULE.load_matrix(self.matrix_path)))
        else:
            self.assertEqual(publish.call_args.args[0], [])

    def _check_legacy_recovery_validation_failure_retires_custody(self, failure) -> None:
        self.prepare_refresh()
        original = self.matrix_path.read_bytes()
        journal, recovered = self.legacy_recovery()
        marker = journal.with_suffix(".preparation")
        args = self.base_args()
        authority = Path(args[-1])
        export = self.tmp / "export.md"
        export.write_bytes(b"previous export")
        if failure == "partial":
            self.matrix_path.write_bytes(b'{"schema":')
        elif failure in ("empty", "unknown"):
            matrix = baseline_matrix()
            if failure == "empty":
                matrix["rows"] = []
            else:
                matrix["schema"] = "unknown"
            self.write_matrix(matrix)
        run_locked, load, retire = MODULE._run_locked, MODULE.load_matrix, MODULE._PreparationCustody.retire
        observed = []
        def after_recovery(invocation, root):
            observed.append((marker.exists(), invocation.preparation_custody))
            if failure == "boundary":
                raise TimeoutError("validation deadline expired")
            return run_locked(invocation, root)
        def load_input(path):
            if failure == "unreadable" and path == self.matrix_path:
                raise PermissionError("matrix denied")
            return load(path)
        with mock.patch.object(MODULE, "_run_locked", after_recovery), \
                mock.patch.object(MODULE, "load_matrix", load_input), \
                mock.patch.object(MODULE, "_replace_outputs", wraps=MODULE._replace_outputs) as publish, \
                mock.patch.object(os, "open", wraps=os.open) as opened, \
                mock.patch.object(MODULE._PreparationCustody, "retire", autospec=True, side_effect=retire) as retired:
            code, _, err = self.run_main(*args, "--write-doc", str(export))
        self.assertIn(code, (1, 2), err)
        receipt_bytes = authority.read_bytes()
        self.assertEqual(observed, [(True, receipt_bytes.decode("utf-8").strip())])
        self.assertEqual(sum(call.args[0] == authority for call in opened.call_args_list), 1)
        publish.assert_not_called()
        self.assertEqual(retired.call_count, 1)
        self.assertFalse(marker.exists())
        self.assertFalse(journal.exists())
        self.assertEqual(recovered.read_bytes(), b"old\r\n")
        self.assertEqual(export.read_bytes(), b"previous export")
        self.matrix_path.write_bytes(original)
        retry = self.base_args()
        self.assertNotEqual(retry[-1], str(authority))
        code, _, err = self.run_main(*retry, "--write-doc", str(export))
        self.assertEqual(code, 0, err)
        self.assertTrue(Path(retry[-1]).is_file())
        self.assertEqual(authority.read_bytes(), receipt_bytes)
        self.assertFalse(marker.exists())
        self.assertEqual(export.read_text(encoding="utf-8"), MODULE.render_doc(MODULE.load_matrix(self.matrix_path)))

    def test_explicit_enrollment_refuses_legacy_recovery_without_consuming_receipt(self) -> None:
        self.prepare_refresh()
        self.legacy_recovery()
        args = self.base_args()
        before = self.image()
        code, _, err = self.run_main(*args, "--enroll-preparation")
        self.assertEqual(code, 2, err)
        self.assertFalse(Path(args[-1]).exists())
        self.assertEqual(self.image(), before)
        code, _, err = self.run_main(*args, "--write-doc", str(self.tmp / "export.md"))
        self.assertEqual(code, 0, err)

    def test_metadata_directory_aliases_share_canonical_transaction_paths(self) -> None:
        for denied in (False, True):
            with self.subTest(denied=denied), self.retargetable_path(self.root / ".git", denied=denied) as (target, _, _):
                lock = MODULE._refresh_lock_path(self.root)
                self.assertEqual(lock.parent, target.resolve())
                journal = (self.root / ".git" / lock.name).with_suffix(".transaction")
                receipt = MODULE._PreparationCustody.create(journal)
                output = self.tmp / "output"
                MODULE._replace_outputs([(output, "published")], journal=journal, custody=receipt)
                MODULE._recover_outputs(journal, receipt)
                MODULE._PreparationCustody(journal, receipt).retire()
                self.assertEqual(output.read_bytes(), b"published")
                self.assertFalse(journal.with_suffix(".pending").exists())
                self.assertFalse(journal.with_suffix(".preparation").exists())
                previous = Path.cwd()
                try:
                    os.chdir(self.tmp)
                    self.assertEqual(MODULE._refresh_lock_path(Path("repo")), lock)
                finally:
                    os.chdir(previous)

    def test_cli_requires_independent_new_authority_outside_recovery_tree(self) -> None:
        self.prepare_refresh()
        plain = ["--root", str(self.root), "--matrix", str(self.matrix_path)]
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        for options in ((), ("--preparation-receipt-file", str(self.root / "receipt")),
                        ("--preparation-receipt-file", str(journal.with_suffix(".receipt")))):
            with self.subTest(options=options):
                before = self.image()
                code, _, err = self.run_main(*plain, *options, "--enroll-preparation")
                self.assertEqual(code, 2, err)
                self.assertIn("receipt", err)
                self.assertEqual(self.image(), before)
        args = self.base_args()
        authority = Path(args[-1])
        authority.write_bytes(b"caller evidence")
        before = self.image()
        self.assertEqual(self.run_main(*args, "--enroll-preparation")[0], 2)
        self.assertEqual(authority.read_bytes(), b"caller evidence")
        self.assertEqual(self.image(), before)
        # A caller file may also be an output alias; it is protected before staging.
        args = self.base_args()
        code, _, err = self.run_main(*args, "--write-doc", args[-1])
        self.assertEqual(code, 2, err)
        self.assertIn("overlaps an input", err)
        self.assertFalse(Path(args[-1]).exists())
        code, _, err = self.run_main(*self.base_args(), "--write-doc", str(journal.with_suffix(".preparation")))
        self.assertEqual(code, 2, err)
        self.assertIn("overlaps an input", err)
        self.assertFalse(journal.with_suffix(".preparation").exists())

    def test_enrollment_publication_rechecks_staging_and_preserves_foreign_marker(self) -> None:
        for change in ("content", "replacement", "link", "marker"):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                journal, authority = base / "refresh.transaction", base / "caller.json"
                marker = journal.with_suffix(".preparation")
                native_open, native_sync = os.open, os.fsync
                receipt_descriptor = None
                retained = {}
                def opened(path, flags, *args, **kwargs):
                    nonlocal receipt_descriptor
                    descriptor = native_open(path, flags, *args, **kwargs)
                    if Path(path) == authority:
                        receipt_descriptor = descriptor
                    return descriptor
                def synced(descriptor):
                    native_sync(descriptor)
                    if descriptor == receipt_descriptor:
                        staged, = base.glob(".acceptance-enrollment-*")
                        if change == "content":
                            staged.write_bytes(b"foreign content")
                        elif change == "replacement":
                            staged.rename(base / "displaced")
                            staged.write_bytes(b"foreign replacement")
                        elif change == "link":
                            os.link(staged, base / "alias")
                        else:
                            marker.write_bytes(b"foreign marker")
                        retained.update({path: (path.read_bytes(), path.stat()) for path in base.iterdir()
                                         if path != authority and (change != "marker" or path == marker)})
                with mock.patch.object(os, "open", opened), mock.patch.object(os, "fsync", synced), \
                        self.assertRaises(MODULE.MatrixError):
                    MODULE._PreparationCustody.create(journal, receipt_file=authority)
                for path, (data, identity) in retained.items():
                    self.assertEqual(path.read_bytes(), data)
                    self.assertTrue(os.path.samestat(path.stat(), identity))
                self.assertEqual(marker.exists(), change == "marker")

    def test_receipt_persistence_failures_never_publish_a_marker(self) -> None:
        for failure in ("open", "fdopen", "write", "flush", "sync", "close", "rename"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                journal, authority = base / "refresh.transaction", base / "caller.json"
                native_open, native_fdopen, native_sync = os.open, os.fdopen, os.fsync
                descriptors = []
                def opened(path, flags, *args, **kwargs):
                    if Path(path) == authority and failure == "open":
                        raise PermissionError("receipt denied")
                    descriptor = native_open(path, flags, *args, **kwargs)
                    if Path(path) == authority:
                        descriptors.append(descriptor)
                    return descriptor
                def handled(descriptor, mode):
                    if descriptor in descriptors and mode == "wb" and failure == "fdopen":
                        raise OSError("receipt handle denied")
                    handle = native_fdopen(descriptor, mode)
                    if descriptor in descriptors and mode == "wb":
                        if failure == "write":
                            write = handle.write
                            handle.write = lambda data: write(data[:3])
                        elif failure == "flush":
                            handle.flush = mock.Mock(side_effect=OSError("receipt flush denied"))
                        elif failure == "close":
                            close = handle.close
                            def closed():
                                close()
                                raise OSError("receipt close denied")
                            handle.close = closed
                    return handle
                def synced(descriptor):
                    if descriptor in descriptors and failure == "sync":
                        raise OSError("receipt sync denied")
                    native_sync(descriptor)
                with mock.patch.object(os, "open", opened), mock.patch.object(os, "fdopen", handled), \
                        mock.patch.object(os, "fsync", synced), \
                        mock.patch.object(os, "rename", side_effect=OSError("publication denied") if failure == "rename" else os.rename), \
                        self.assertRaises(OSError):
                    MODULE._PreparationCustody.create(journal, receipt_file=authority)
                self.assertFalse(journal.with_suffix(".preparation").exists())
                self.assertEqual(list(base.glob(".acceptance-enrollment-*")), [])
                # Partial caller data never causes automatic enrollment/adoption.
                MODULE._recover_outputs(journal)
                for descriptor in descriptors:
                    with self.assertRaises(OSError):
                        os.fstat(descriptor)

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            journal, authority = base / "refresh.transaction", base / "caller.json"
            encode = MODULE._custody_json
            def exact_bound(value):
                if isinstance(value, dict) and set(value) == {"version", "journal", "device", "inode", "nonce", "key"}:
                    return b"x" * MODULE._JOURNAL_BYTES
                return encode(value)
            with mock.patch.object(MODULE, "_custody_json", exact_bound), \
                    self.assertRaisesRegex(MODULE.MatrixError, "exceeds its bound"):
                MODULE._PreparationCustody.create(journal, receipt_file=authority)
            self.assertEqual(list(base.iterdir()), [])

    def _check_enrollment_crash_before_receipt_delivery_is_recoverable(self, mode, boundary) -> None:
        self.prepare_rollback() if mode == "write" else self.prepare_refresh()
        args = self.base_args()
        authority = Path(args[-1])
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        if mode == "legacy":
            output = self.tmp / "legacy-output"
            backup, candidate = (self.tmp / (".acceptance-" + str(i) * 32) for i in (1, 2))
            backup.write_bytes(b"old")
            candidate.write_bytes(b"new")
            old, new = MODULE._transaction_record(backup), MODULE._transaction_record(candidate)
            MODULE._write_transaction(journal, [{"path": str(output), "old": old["sha256"],
                                                "backup": old, "new": new}], committed=False)
            candidate.replace(output)
        program = r'''
import importlib.util, os, pathlib, sys
spec = importlib.util.spec_from_file_location("acceptance_child", sys.argv[1])
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
mode, boundary = sys.argv[2:4]
create = checker._PreparationCustody.create
rename = os.rename
def interrupted(*args, **kwargs):
    receipt = create(*args, **kwargs)
    os._exit(73)
def publication(source, target):
    if boundary == "before":
        os._exit(73)
    rename(source, target)
    os._exit(73)
if boundary == "delivery":
    checker._PreparationCustody.create = interrupted
else:
    os.rename = publication
options = ["--enroll-preparation"] if mode == "enroll" else (["--refresh-inventory"] if mode == "write" else [])
raise SystemExit(checker.main(sys.argv[4:] + options))
'''
        child = subprocess.run([sys.executable, "-I", "-B", "-c", program, str(MODULE_PATH), mode, boundary, *args],
                               capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
        self.assertEqual(child.returncode, 73, (child.stdout, child.stderr))
        self.assertNotIn("preparation custody receipt:", child.stderr)
        self.assertEqual(child.stdout, "")
        receipt = authority.read_text(encoding="utf-8")
        self.assertEqual(json.loads(receipt)["journal"], str(journal))
        marker = journal.with_suffix(".preparation")
        if boundary == "before":
            self.assertFalse(marker.exists())
            # No fixed marker blocks a new enrollment. Retry with fresh caller
            # authority; the inert candidate is never used to authenticate it.
            retry = self.base_args()
            options = ["--enroll-preparation"] if mode == "enroll" else (["--refresh-inventory"] if mode == "write" else [])
            code, delivered, err = self.run_main(*retry, *options)
            self.assertEqual(code, 0, err)
            if mode == "enroll":
                code, _, err = self.run_main(*self.base_args(), "--preparation-custody", delivered, "--retire-preparation")
                self.assertEqual(code, 0, err)
            self.assertFalse(marker.exists())
            return
        self.assertTrue(marker.exists())
        image = self.image()
        with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
            MODULE._recover_outputs(journal)
        self.assertEqual(self.image(), image)
        forged = json.loads(receipt)
        forged["key"] = "0" * 64
        with self.assertRaises(MODULE.MatrixError):
            MODULE._recover_outputs(journal, json.dumps(forged))
        self.assertEqual(self.image(), image)
        code, _, err = self.run_main(*self.base_args(), "--preparation-custody", receipt, "--retire-preparation")
        self.assertEqual(code, 0, err)
        self.assertFalse(marker.exists())
        self.assertFalse(journal.exists())

    def custody(self) -> tuple[Path, str]:
        journal = self.tmp / "refresh.transaction"
        return journal, MODULE._PreparationCustody.create(journal)

    def image(self) -> dict[Path, tuple[bytes, int, int]]:
        return {p: (p.read_bytes(), p.stat().st_dev, p.stat().st_ino)
                for p in self.tmp.rglob("*") if p.is_file()}

    def test_enrollment_cli_recovery_reuse_and_retirement(self) -> None:
        self.prepare_refresh()
        code, receipt, err = self.run_main(*self.base_args(), "--enroll-preparation")
        self.assertEqual(code, 0, err)
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        before = journal.with_suffix(".preparation").stat()
        for text in ("first\r\n", "second\r\n"):
            output = self.tmp / "output"
            MODULE._replace_outputs([(output, text)], journal=journal, custody=receipt)
            code, _, err = self.run_main(*self.base_args(), "--preparation-custody", receipt)
            self.assertEqual(code, 0, err)
            self.assertEqual(output.read_bytes(), text.encode())
            self.assertTrue(os.path.samestat(before, journal.with_suffix(".preparation").stat()))
        code, _, err = self.run_main(*self.base_args(), "--preparation-custody", receipt, "--retire-preparation")
        self.assertEqual(code, 0, err)
        self.assertFalse(journal.with_suffix(".preparation").exists())

    def test_cli_rejects_incomplete_or_combined_enrollment_requests(self) -> None:
        base = self.base_args()
        for options in (("--retire-preparation",), ("--enroll-preparation", "--preparation-custody", "{}"),
                        ("--enroll-preparation", "--write-doc", str(self.tmp / "document")),
                        ("--retire-preparation", "--preparation-custody", "{}", "--refresh-inventory")):
            with self.subTest(options=options):
                before = self.image()
                self.assertEqual(self.run_main(*base, *options)[0], 2)
                self.assertEqual(self.image(), before)

    def test_interrupted_enrollment_never_adopts_a_marker_or_returns_a_receipt(self) -> None:
        journal = self.tmp / "refresh.transaction"
        with mock.patch.object(os, "fdopen", side_effect=OSError("handle denied")), \
                self.assertRaisesRegex(OSError, "handle denied"):
            MODULE._PreparationCustody.create(journal)
        before = self.image()
        for action in (lambda: MODULE._PreparationCustody.create(journal), lambda: MODULE._recover_outputs(journal)):
            with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                action()
            self.assertEqual(self.image(), before)

    def test_enrollment_short_write_flush_and_sync_failures_preserve_unknown_state(self) -> None:
        for failure in ("write", "flush", "sync"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                journal = Path(directory).resolve() / "refresh.transaction"
                fdopen = os.fdopen
                def opened(*args: Any, **kwargs: Any) -> Any:
                    handle = fdopen(*args, **kwargs)
                    if failure == "write":
                        write = handle.write
                        handle.write = lambda data: write(data[:len(data) // 2])
                    elif failure == "flush":
                        handle.flush = mock.Mock(side_effect=OSError("flush failed"))
                    return handle
                with mock.patch.object(os, "fdopen", opened), \
                        mock.patch.object(os, "fsync", side_effect=OSError("sync failed") if failure == "sync" else None), \
                        self.assertRaises(OSError):
                    MODULE._PreparationCustody.create(journal)
                marker = journal.with_suffix(".preparation")
                data, identity = marker.read_bytes(), marker.stat()
                with self.assertRaises(MODULE.MatrixError):
                    MODULE._recover_outputs(journal)
                self.assertEqual(marker.read_bytes(), data)
                self.assertTrue(os.path.samestat(marker.stat(), identity))

    def _check_restart_reacquires_enrolled_partial_output_and_pending_files(self, phase) -> None:
        program = r'''
import builtins, os, pathlib, importlib.util, sys
spec = importlib.util.spec_from_file_location("acceptance_child", sys.argv[1])
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
g = checker.__dict__
journal, receipt, phase = pathlib.Path(sys.argv[2]), sys.argv[3], sys.argv[4]
enroll = g["_PreparationCustody"].enroll
def enrolled(owner, path, identity):
    enroll(owner, path, identity)
    if phase in ("output", "pending") and (path.suffix == ".pending") == (phase == "pending"):
        os._exit(73)
g["_PreparationCustody"].enroll = enrolled
opened = builtins.open
def opening(path, *args, **kwargs):
    handle = opened(path, *args, **kwargs)
    if phase == "payload" and pathlib.Path(path).name.startswith(".acceptance-"):
        write = handle.write
        def writing(data):
            write(data[:len(data) // 2])
            handle.flush()
            os._exit(73)
        handle.write = writing
    return handle
builtins.open = opening
replace_path, replace = pathlib.Path.replace, os.replace
def published(path, target):
    result = replace_path(path, target)
    if phase == "publication" and pathlib.Path(target).name == "output":
        os._exit(73)
    return result
pathlib.Path.replace = published
renames = 0
def renamed(source, target):
    global renames
    result = replace(source, target)
    if pathlib.Path(target) == journal:
        renames += 1
        if phase == "commit" and renames == 2:
            os._exit(73)
    return result
os.replace = renamed
g["_replace_outputs"]([(journal.parent / "output", "candidate\r\n")], journal=journal, custody=receipt)
'''
        with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            journal, output = base / "refresh.transaction", base / "output"
            output.write_bytes(b"original\r\n")
            receipt = MODULE._PreparationCustody.create(journal)
            child = subprocess.run([sys.executable, "-I", "-B", "-c", program, str(MODULE_PATH),
                                    str(journal), receipt, phase], capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
            self.assertEqual(child.returncode, 73, (child.stdout, child.stderr))
            self.assertTrue(list(base.glob(".acceptance-*")))
            restart = subprocess.run([sys.executable, "-I", "-B", "-c",
                'import importlib.util,pathlib,sys;spec=importlib.util.spec_from_file_location("acceptance_child",sys.argv[1]);'
                'checker=importlib.util.module_from_spec(spec);sys.modules[spec.name]=checker;spec.loader.exec_module(checker);'
                'checker._recover_outputs(pathlib.Path(sys.argv[2]),sys.argv[3])',
                str(MODULE_PATH), str(journal), receipt], capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
            self.assertEqual(restart.returncode, 0, (restart.stdout, restart.stderr))
            self.assertEqual(output.read_bytes(), b"candidate\r\n" if phase == "commit" else b"original\r\n")
            self.assertEqual(list(base.glob(".acceptance-*")), [])
            MODULE._replace_outputs([(output, "retry")], journal=journal, custody=receipt)
            self.assertEqual(output.read_bytes(), b"retry")

    def test_reset_removes_only_enrolled_partial_content_and_preserves_unrelated_files(self) -> None:
        journal, receipt = self.custody()
        owner = MODULE._PreparationCustody(journal, receipt)
        artifact, unrelated = self.tmp / (".acceptance-" + "a" * 32), self.tmp / "unrelated"
        owner.intend(artifact, self.tmp / "output", b"whole payload")
        artifact.touch()
        with artifact.open("r+b") as handle:
            owner.enroll(artifact, MODULE._transaction_metadata(handle))
            handle.write(b"part")
        unrelated.write_bytes(b"keep\r\n")
        MODULE._recover_outputs(journal, receipt)
        self.assertFalse(artifact.exists())
        self.assertEqual(unrelated.read_bytes(), b"keep\r\n")
        self.assertEqual(len(MODULE._PreparationCustody(journal, receipt).events), 1)

    def test_torn_event_reset_retains_the_committed_enrollment(self) -> None:
        journal, receipt = self.custody()
        marker = journal.with_suffix(".preparation")
        initial = marker.read_bytes()
        with marker.open("ab") as handle:
            handle.write(b'{"event":')
        MODULE._recover_outputs(journal, receipt)
        self.assertEqual(marker.read_bytes(), initial)
        MODULE._recover_outputs(journal, receipt)

    def test_uncommitted_artifact_enrollment_is_unknown_and_untouched(self) -> None:
        journal, receipt = self.custody()
        owner = MODULE._PreparationCustody(journal, receipt)
        artifact = self.tmp / (".acceptance-" + "b" * 32)
        owner.intend(artifact, self.tmp / "output", b"whole")
        artifact.write_bytes(b"unknown")
        before = self.image()
        with self.assertRaisesRegex(MODULE.MatrixError, "unenrolled"):
            MODULE._recover_outputs(journal, receipt)
        self.assertEqual(self.image(), before)

    def test_foreign_stale_and_forged_receipts_preserve_all_bytes_and_identities(self) -> None:
        journal, receipt = self.custody()
        value = json.loads(receipt)
        for field, replacement in (("key", "0" * 64), ("nonce", "0" * 64), ("inode", value["inode"] + 1),
                                   ("journal", str(self.tmp / "other.transaction")), ("version", 2)):
            with self.subTest(field=field):
                before = self.image()
                with self.assertRaises(MODULE.MatrixError):
                    MODULE._recover_outputs(journal, json.dumps(dict(value, **{field: replacement})))
                self.assertEqual(self.image(), before)
        marker = journal.with_suffix(".preparation")
        marker.rename(self.tmp / "displaced")
        marker.write_bytes(b"foreign\r\n")
        before = self.image()
        with self.assertRaises(MODULE.MatrixError):
            MODULE._recover_outputs(journal, receipt)
        self.assertEqual(self.image(), before)

    def test_foreign_pending_and_replaced_enrolled_artifacts_refuse_before_cleanup(self) -> None:
        for phase in ("unlisted-pending", "replaced", "hardlink"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                journal = base / "refresh.transaction"
                receipt = MODULE._PreparationCustody.create(journal)
                owner = MODULE._PreparationCustody(journal, receipt)
                artifact = base / (".acceptance-" + "c" * 32)
                owner.intend(artifact, base / "output", b"whole")
                artifact.touch()
                with artifact.open("r+b") as handle:
                    owner.enroll(artifact, MODULE._transaction_metadata(handle))
                if phase == "unlisted-pending":
                    journal.with_suffix(".pending").write_bytes(b"foreign")
                elif phase == "replaced":
                    artifact.rename(base / "displaced")
                    artifact.write_bytes(b"foreign")
                else:
                    os.link(artifact, base / "alias")
                before = {p: (p.read_bytes(), p.stat()) for p in base.iterdir()}
                with self.assertRaises(MODULE.MatrixError):
                    MODULE._recover_outputs(journal, receipt)
                self.assertEqual(set(base.iterdir()), set(before))
                for path, (data, identity) in before.items():
                    self.assertEqual(path.read_bytes(), data)
                    self.assertTrue(os.path.samestat(path.stat(), identity))

    def test_unknown_authenticated_metadata_and_tampering_refuse_without_reset(self) -> None:
        for unknown in (False, True):
            with self.subTest(unknown=unknown), tempfile.TemporaryDirectory() as directory:
                journal = Path(directory).resolve() / "refresh.transaction"
                receipt = MODULE._PreparationCustody.create(journal)
                owner = MODULE._PreparationCustody(journal, receipt)
                owner.append({"unknown": True})
                if not unknown:
                    owner.path.write_bytes(owner.path.read_bytes().replace(b"true", b"null"))
                before = owner.path.read_bytes()
                with self.assertRaises(MODULE.MatrixError):
                    MODULE._recover_outputs(journal, receipt)
                self.assertEqual(owner.path.read_bytes(), before)

    def test_unreadable_marker_and_artifact_refuse_without_reset(self) -> None:
        journal, receipt = self.custody()
        owner = MODULE._PreparationCustody(journal, receipt)
        artifact = self.tmp / (".acceptance-" + "d" * 32)
        owner.intend(artifact, self.tmp / "output", b"whole")
        artifact.touch()
        with artifact.open("r+b") as handle:
            owner.enroll(artifact, MODULE._transaction_metadata(handle))
        read = MODULE._read_transaction_file
        for denied in (owner.path, artifact):
            before = self.image()
            def reading(path: Path, limit: int) -> Any:
                if path == denied:
                    raise PermissionError("read denied")
                return read(path, limit)
            with mock.patch.object(MODULE, "_read_transaction_file", reading), self.assertRaises(PermissionError):
                MODULE._recover_outputs(journal, receipt)
            self.assertEqual(self.image(), before)

    def test_record_metadata_file_and_output_bounds_are_enforced(self) -> None:
        journal, receipt = self.custody()
        owner = MODULE._PreparationCustody(journal, receipt)
        for _ in range(MODULE._PREPARATION_EVENTS - 1):
            owner.intend(self.tmp / (".acceptance-" + "e" * 32), self.tmp / "output", b"")
        before = self.image()
        with self.assertRaisesRegex(MODULE.MatrixError, "record bound"):
            owner.intend(self.tmp / (".acceptance-" + "f" * 32), self.tmp / "output", b"")
        self.assertEqual(self.image(), before)
        owner.path.write_bytes(owner.data + owner.data.splitlines(keepends=True)[-1])
        with self.assertRaisesRegex(MODULE.MatrixError, "record bound"):
            MODULE._PreparationCustody(journal, receipt)
        owner.path.write_bytes(owner.data)
        MODULE._recover_outputs(journal, receipt)
        for count in (6, 16):
            owner = MODULE._PreparationCustody(journal, receipt)
            for index in range(count):
                owner.intend(self.tmp / (".acceptance-" + f"{index:032x}"),
                             self.tmp / (str(index) if count == 6 else "output"), b"")
            before = self.image()
            with self.assertRaisesRegex(MODULE.MatrixError, "count exceeds bound"):
                MODULE._recover_outputs(journal, receipt)
            self.assertEqual(self.image(), before)
            # This deliberately malformed authenticated fixture has no files;
            # restore its committed enrollment for the next independent bound.
            marker = journal.with_suffix(".preparation")
            marker.write_bytes(marker.read_bytes().split(b"\n", 1)[0] + b"\n")
        marker.write_bytes(b"x" * (MODULE._JOURNAL_BYTES + 1))
        with self.assertRaisesRegex(MODULE.MatrixError, "size"):
            MODULE._recover_outputs(journal, receipt)
        for bad in ("", "{}", "[]", "x" * (MODULE._JOURNAL_BYTES + 1)):
            with self.assertRaisesRegex(MODULE.MatrixError, "receipt"):
                MODULE._PreparationCustody(journal, bad)

    def test_invalid_journal_refuses_before_owned_pending_cleanup_or_marker_reset(self) -> None:
        journal, receipt = self.custody()
        owner = MODULE._PreparationCustody(journal, receipt)
        pending = journal.with_suffix(".pending")
        owner.intend(pending, journal, b"journal")
        pending.touch()
        with pending.open("r+b") as handle:
            owner.enroll(pending, MODULE._transaction_metadata(handle))
            handle.write(b"partial")
        journal.write_bytes(b'{"version":99}')
        with owner.path.open("ab") as handle:
            handle.write(b'{"event":')
        before = self.image()
        with self.assertRaisesRegex(MODULE.MatrixError, "invalid recovery journal"):
            MODULE._recover_outputs(journal, receipt)
        self.assertEqual(self.image(), before)

    def test_malformed_authenticated_events_and_byte_bounds_refuse_untouched(self) -> None:
        for value in ('"\\\b\f\n\r\t\x00\x7f', "é雪🚀", {"keys": [None, True, False, 128, -128]}, []):
            expected = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
            with mock.patch.object(MODULE, "_JOURNAL_BYTES", len(expected)):
                self.assertEqual(MODULE._custody_json(value), expected)
            with mock.patch.object(MODULE, "_JOURNAL_BYTES", len(expected) - 1), \
                    mock.patch.object(MODULE.json, "dumps", side_effect=AssertionError("allocated before size check")), \
                    self.assertRaisesRegex(MODULE.MatrixError, "exceeds its bound"):
                MODULE._custody_json(value)
        nested: Any = None
        for _ in range(65):
            nested = [nested]
        with self.assertRaisesRegex(MODULE.MatrixError, "nesting"):
            MODULE._custody_json(nested)
        journal, receipt = self.custody()
        initial = journal.with_suffix(".preparation").read_bytes()
        artifact = self.tmp / (".acceptance-" + "d" * 32)
        intention = {"create": str(artifact), "destination": str(self.tmp / "output"),
                     "size": 0, "sha256": hashlib.sha256(b"").hexdigest()}
        events = [[], {"enrolled": {}}, {"cancelled": str(artifact)}, {"bound": {}},
                  dict(intention, destination="relative"), dict(intention, create=str(self.tmp / "foreign")),
                  dict(intention, size=True), dict(intention, size=-1), dict(intention, sha256="unknown"),
                  dict(intention, create=str(journal.with_suffix(".pending"))),
                  {"enrolled": {"path": str(artifact), "device": False, "inode": 1}}]
        for event in events:
            with self.subTest(event=event):
                owner = MODULE._PreparationCustody(journal, receipt)
                owner.append(event)
                before = self.image()
                with self.assertRaises(MODULE.MatrixError):
                    MODULE._recover_outputs(journal, receipt)
                self.assertEqual(self.image(), before)
                owner.path.write_bytes(initial)
        owner = MODULE._PreparationCustody(journal, receipt)
        before = self.image()
        with mock.patch.object(MODULE, "_JOURNAL_BYTES", len(initial) + 1), \
                self.assertRaisesRegex(MODULE.MatrixError, "exceeds its bound"):
            owner.intend(artifact, self.tmp / "output", b"")
        self.assertEqual(self.image(), before)
        with self.assertRaisesRegex(MODULE.MatrixError, "requires a recovery journal"):
            MODULE._replace_outputs([(self.tmp / "output", "new")], custody=receipt)
        owner.intend(artifact, self.tmp / "output", b"")
        with self.assertRaisesRegex(MODULE.MatrixError, "recover enrolled preparation"):
            MODULE._replace_outputs([(self.tmp / "output", "new")], journal=journal, custody=receipt)
        MODULE._recover_outputs(journal, receipt)
        owner = MODULE._PreparationCustody(journal, receipt)
        owner.retire()
        new_receipt = MODULE._PreparationCustody.create(journal)
        before = self.image()
        with self.assertRaises(MODULE.MatrixError):
            MODULE._recover_outputs(journal, receipt)
        self.assertEqual(self.image(), before)
        MODULE._recover_outputs(journal, new_receipt)

    def test_cleanup_and_reset_failures_remain_restartable(self) -> None:
        journal, receipt = self.custody()
        owner = MODULE._PreparationCustody(journal, receipt)
        artifact = self.tmp / (".acceptance-" + "f" * 32)
        owner.intend(artifact, self.tmp / "output", b"whole")
        artifact.touch()
        with artifact.open("r+b") as handle:
            owner.enroll(artifact, MODULE._transaction_metadata(handle))
        with mock.patch.object(MODULE, "_remove_transaction_file", side_effect=PermissionError("remove denied")), \
                self.assertRaises(PermissionError):
            MODULE._recover_outputs(journal, receipt)
        self.assertTrue(artifact.exists())
        with mock.patch.object(os, "fsync", side_effect=OSError("sync denied")), self.assertRaises(OSError):
            MODULE._recover_outputs(journal, receipt)
        MODULE._recover_outputs(journal, receipt)
        self.assertFalse(artifact.exists())
        self.assertEqual(len(MODULE._PreparationCustody(journal, receipt).events), 1)
        self.prepare_refresh()
        cli_journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        for failure in (PermissionError("retirement denied"), MODULE.MatrixError("retirement uncertain")):
            with self.subTest(failure=str(failure)):
                with mock.patch.object(MODULE._PreparationCustody, "retire", side_effect=failure):
                    code, _, err = self.run_main(*self.base_args(), "--write-doc", str(self.tmp / "document"))
                self.assertEqual(code, 2, err)
                self.assertIn(str(failure), err)
                self.assertTrue(cli_journal.with_suffix(".preparation").exists())
                self.assertFalse(cli_journal.exists())
                code, _, err = self.run_main(*self.base_args(), "--retire-preparation")
                self.assertEqual(code, 0, err)
                self.assertFalse(cli_journal.with_suffix(".preparation").exists())

    def test_v1_v2_journals_recover_with_custody_and_compact_before_restoration(self) -> None:
        for version in (1, 2):
            for committed in (False, True):
                with self.subTest(version=version, committed=committed), tempfile.TemporaryDirectory() as directory:
                    base = Path(directory).resolve()
                    journal, output = base / "refresh.transaction", base / "output"
                    receipt = MODULE._PreparationCustody.create(journal)
                    owner = MODULE._PreparationCustody(journal, receipt)
                    old, new = [base / (".acceptance-" + f"{i:032x}") for i in (1, 2)]
                    old.write_bytes(b"old\r\n")
                    new.write_bytes(b"new\r\n")
                    backup, candidate = MODULE._transaction_record(old), MODULE._transaction_record(new)
                    entry = {"path": str(output), "old": backup["sha256"], "backup": backup, "new": candidate}
                    MODULE._write_transaction(journal, [entry], committed=committed,
                                              restorations=[None] if version == 2 else None)
                    new.replace(output)
                    # A full preparation log must be compacted using validated
                    # journal authority before new restoration enrollment.
                    for _ in range(MODULE._PREPARATION_EVENTS - 1):
                        owner.intend(base / (".acceptance-" + "e" * 32), output, b"")
                    MODULE._recover_outputs(journal, receipt)
                    self.assertEqual(output.read_bytes(), b"new\r\n" if committed else b"old\r\n")
                    self.assertFalse(journal.exists())
                    self.assertEqual(list(base.glob(".acceptance-*")), [])
                    self.assertEqual(len(MODULE._PreparationCustody(journal, receipt).events), 1)


# Disjoint selections bound each native entry's filesystem fault sweeps.
# Direct execution still discovers the complete module, including recovery and
# all four public-tree partitions.
_MATRIX_CLASSES = (AcceptanceMatrixTests, ProductionAcceptanceMatrixTests,
                   AcceptanceMatrixPublishabilityTests, PublicTreeScanTests)
# Populated after all parameterized cases are registered.
_CONTRACT_CASE_PARTITIONS = {}

def contract_suite(number: int) -> unittest.TestSuite:
    if type(number) is not int or not 0 <= number < _CONTRACT_PARTITIONS:
        raise ValueError("invalid contract partition")
    _phase("selection-start", selector=number)
    loader = unittest.TestLoader()
    cases = [case(name) for case in (*_MATRIX_CLASSES, AcceptanceSelectionTests,
                                   AcceptanceRecoveryTests, AcceptanceCustodyTests)
             for name in loader.getTestCaseNames(case)]
    suite = unittest.TestSuite(case for index, case in enumerate(cases)
                              if _CONTRACT_CASE_PARTITIONS.get(
                                  type(case).__name__ + "." + case._testMethodName,
                                  index % _LOCAL_PARTITIONS) == number)
    _phase("selection-finish", selector=number)
    return suite


class AcceptanceSelectionTests(_PhaseCase):
    @staticmethod
    def link_image(base: Path) -> dict[Path, tuple[Any, ...]]:
        return {path: (path.read_bytes(), info.st_dev, info.st_ino, info.st_nlink, info.st_mode)
                for path in base.iterdir() if path.is_file() for info in (path.stat(),)}

    def _check_restart_refuses_linked_transaction_members_before_any_mutation(self, committed) -> None:
        for selected_commit in (committed,):
            for persistent in (False, True):
                for kind in ("backup", "candidate", "restoration", "journal", "preparation", "pending"):
                    with self.subTest(committed=committed, persistent=persistent, kind=kind), \
                            AcceptanceMatrixTests.recovery_transaction(self) as (base, paths, backups, journal):
                        entries = json.loads(journal.read_bytes())["outputs"]
                        restorations = None
                        artifact = backups[-1]
                        if kind == "candidate":
                            artifact = Path(entries[-1]["new"]["path"])
                            paths[-1].rename(artifact)
                            paths[-1].write_bytes(backups[-1].read_bytes())
                        elif kind == "restoration":
                            artifact = base / (".acceptance-" + "f" * 32)
                            artifact.write_bytes(backups[-1].read_bytes())
                            restorations = [None, MODULE._transaction_record(artifact)]
                        elif kind == "journal":
                            artifact = journal
                        MODULE._write_transaction(journal, entries, committed=committed, restorations=restorations)
                        receipt = None
                        if persistent:
                            receipt = MODULE._PreparationCustody.create(journal, MODULE._transaction_record(journal))
                        if kind in ("preparation", "pending"):
                            artifact = journal.with_suffix("." + kind)
                            if kind == "pending" and persistent:
                                owner = MODULE._PreparationCustody(journal, receipt)
                                owner.intend(artifact, journal, b"partial")
                                artifact.touch()
                                with artifact.open("r+b") as handle:
                                    owner.enroll(artifact, MODULE._transaction_metadata(handle))
                                    handle.write(b"part")
                            elif not artifact.exists():
                                artifact.write_bytes(b"unknown partial record")
                        artifact.chmod(stat.S_IRUSR)
                        os.link(artifact, base / "alias")
                        before = self.link_image(base)
                        with self.assertRaisesRegex(MODULE.MatrixError, "sole link|uncertain recovery preparation"):
                            MODULE._recover_outputs(journal, receipt)
                        self.assertEqual(self.link_image(base), before)
                        # Restore writability solely for Windows temporary-directory teardown.
                        artifact.chmod(stat.S_IRUSR | stat.S_IWUSR)
        for persistent in (False, True):
            for kind in ("preparation", "pending"):
                with self.subTest(journal=False, persistent=persistent, kind=kind), \
                        tempfile.TemporaryDirectory() as directory:
                    base = Path(directory).resolve()
                    journal = base / "refresh.transaction"
                    receipt = MODULE._PreparationCustody.create(journal) if persistent else None
                    artifact = journal.with_suffix("." + kind)
                    if persistent and kind == "pending":
                        owner = MODULE._PreparationCustody(journal, receipt)
                        owner.intend(artifact, journal, b"whole record")
                        artifact.touch()
                        with artifact.open("r+b") as handle:
                            owner.enroll(artifact, MODULE._transaction_metadata(handle))
                            handle.write(b"partial")
                    elif not artifact.exists():
                        artifact.write_bytes(b"unknown partial record")
                    artifact.chmod(stat.S_IRUSR)
                    os.link(artifact, base / "alias")
                    before = self.link_image(base)
                    with self.assertRaisesRegex(MODULE.MatrixError, "sole link|uncertain recovery preparation"):
                        MODULE._recover_outputs(journal, receipt)
                    self.assertEqual(self.link_image(base), before)
                    artifact.chmod(stat.S_IRUSR | stat.S_IWUSR)

    def test_removal_rechecks_links_before_readonly_fallback_and_unlink(self) -> None:
        for phase in ("before-unlink", "before-chmod", "after-chmod"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                artifact, alias = base / "artifact", base / "alias"
                artifact.write_bytes(b"retained bytes")
                artifact.chmod(stat.S_IRUSR)
                initial_mode = artifact.stat().st_mode
                record = MODULE._transaction_record(artifact)
                chmod, verify = Path.chmod, MODULE._verify_transaction_file
                calls, changed_modes = [], []
                def checked(value: Any, limit: Any = None) -> Path:
                    if phase == "before-unlink" and not calls:
                        os.link(artifact, alias)
                        calls.append("linked")
                    return verify(value, limit)
                def removed(path: Path, *args: Any, **kwargs: Any) -> None:
                    calls.append("unlink")
                    if phase == "before-chmod":
                        os.link(artifact, alias)
                    raise PermissionError("read-only file")
                def changed(path: Path, mode: int, *args: Any, **kwargs: Any) -> None:
                    calls.append("chmod")
                    chmod(path, mode, *args, **kwargs)
                    changed_modes.append(path.stat().st_mode)
                    os.link(artifact, alias)
                with mock.patch.object(MODULE, "_verify_transaction_file", checked), \
                        mock.patch.object(Path, "unlink", removed), mock.patch.object(Path, "chmod", changed), \
                        self.assertRaisesRegex(MODULE.MatrixError, "sole link"):
                    MODULE._remove_transaction_file(record)
                self.assertEqual(artifact.read_bytes(), b"retained bytes")
                self.assertEqual(alias.read_bytes(), b"retained bytes")
                self.assertEqual(calls, ["linked"] if phase == "before-unlink" else
                                 ["unlink"] if phase == "before-chmod" else ["unlink", "chmod"])
                expected_mode = changed_modes[0] if phase == "after-chmod" else initial_mode
                self.assertEqual(artifact.stat().st_mode, expected_mode)
                self.assertEqual(alias.stat().st_mode, expected_mode)
                chmod(artifact, stat.S_IRUSR | stat.S_IWUSR)

    def test_live_rollback_and_cleanup_refuse_entire_linked_set(self) -> None:
        for journaled in (False, True):
            for fail in (False, True):
                for kind in ("backup", "candidate", "preparation", "output"):
                    with self.subTest(journaled=journaled, fail=fail, kind=kind), \
                            tempfile.TemporaryDirectory() as directory:
                        base = Path(directory).resolve()
                        output = base / "output"
                        output.write_bytes(b"old")
                        journal = base / "refresh.transaction" if journaled else None
                        snapshot = []
                        def changed(published: Any) -> None:
                            if snapshot or kind == "output" and not published:
                                return
                            if kind == "output":
                                artifact = output
                            elif kind == "preparation":
                                artifact = journal.with_suffix(".preparation") if journal else base / ".refresh-preparation"
                            else:
                                artifact = next(p for p in base.glob(".acceptance-*")
                                                if p.read_bytes() == (b"old" if kind == "backup" else b"new"))
                            os.link(artifact, base / "alias")
                            snapshot.append(self.link_image(base))
                            if fail:
                                raise RuntimeError("publication refused")
                        with self.assertRaisesRegex(MODULE.MatrixError, "sole link"):
                            MODULE._replace_outputs([(output, "new")], journal=journal, verify=changed)
                        self.assertEqual(len(snapshot), 1)
                        self.assertEqual(self.link_image(base), snapshot[0])

    def test_custody_reset_revalidates_marker_before_removing_enrolled_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            journal = base / "refresh.transaction"
            receipt = MODULE._PreparationCustody.create(journal)
            owner = MODULE._PreparationCustody(journal, receipt)
            artifact = base / (".acceptance-" + "a" * 32)
            owner.intend(artifact, base / "output", b"payload")
            artifact.touch()
            with artifact.open("r+b") as handle:
                owner.enroll(artifact, MODULE._transaction_metadata(handle))
                handle.write(b"part")
            os.link(owner.path, base / "alias")
            before = self.link_image(base)
            with self.assertRaisesRegex(MODULE.MatrixError, "sole link"):
                owner.reset()
            self.assertEqual(self.link_image(base), before)

    def test_staged_chmod_refuses_link_added_after_payload_close(self) -> None:
        for journaled in (False, True):
            with self.subTest(journaled=journaled), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                output = base / "output"
                output.write_bytes(b"old")
                output.chmod(stat.S_IRUSR)
                journal = base / "refresh.transaction" if journaled else None
                sync = os.fsync
                snapshot = []
                def linked(descriptor: int) -> None:
                    sync(descriptor)
                    artifacts = list(base.glob(".acceptance-*"))
                    if artifacts and not snapshot:
                        os.link(artifacts[0], base / "alias")
                        snapshot.append(self.link_image(base))
                with mock.patch.object(os, "fsync", linked), self.assertRaisesRegex(MODULE.MatrixError, "sole link"):
                    MODULE._replace_outputs([(output, "new")], journal=journal)
                self.assertEqual(self.link_image(base), snapshot[0])
                output.chmod(stat.S_IRUSR | stat.S_IWUSR)

    def test_recovery_read_refuses_unknown_link_counts_and_late_aliases(self) -> None:
        for links in (0, 2):
            for late in (False, True):
                with self.subTest(links=links, late=late), tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "artifact"
                    path.write_bytes(b"payload")
                    metadata = MODULE._transaction_metadata
                    reads = []
                    def changed(handle: Any) -> Any:
                        actual = metadata(handle)
                        reads.append(actual)
                        if not late or len(reads) == 2:
                            fields = list(actual)
                            fields[3] = links
                            return os.stat_result(fields, {"st_mtime_ns": actual.st_mtime_ns,
                                                           "st_ctime_ns": actual.st_ctime_ns})
                        return actual
                    with mock.patch.object(MODULE, "_transaction_metadata", changed), \
                            self.assertRaisesRegex(MODULE.MatrixError, "sole link"):
                        MODULE._read_transaction_file(path, 7)
                    self.assertEqual(path.read_bytes(), b"payload")

    def test_recovery_cleanup_preflights_links_added_during_restore(self) -> None:
        with AcceptanceMatrixTests.recovery_transaction(self) as (base, paths, backups, journal):
            replace = os.replace
            snapshot = []
            def changed(source: Any, target: Any) -> None:
                replace(source, target)
                if Path(target) == paths[0]:
                    os.link(backups[-1], base / "alias")
                    snapshot.append(self.link_image(base))
            with mock.patch.object(os, "replace", changed), self.assertRaisesRegex(MODULE.MatrixError, "sole link"):
                MODULE._recover_outputs(journal)
            self.assertEqual(self.link_image(base), snapshot[0])

    def test_journal_replacement_rechecks_both_names_after_handoff(self) -> None:
        for existing in (False, True):
            for edit in ("journal-link", "journal-replaced", "pending-link", "pending-replaced"):
                with self.subTest(existing=existing, edit=edit), tempfile.TemporaryDirectory() as directory:
                    base = Path(directory).resolve()
                    journal = base / "refresh.transaction"
                    if existing:
                        journal.write_bytes(b"previous journal")
                    preparation = MODULE._OutputPreparation(journal.with_suffix(".preparation"))
                    handoff = preparation.handoff
                    snapshot = []
                    def changed(record: Any) -> None:
                        handoff(record)
                        artifact = journal if edit.startswith("journal") else journal.with_suffix(".pending")
                        if artifact.exists():
                            if edit.endswith("link"):
                                os.link(artifact, base / "alias")
                            else:
                                artifact.rename(base / "displaced")
                                artifact.write_bytes(b"foreign journal")
                        else:
                            artifact.write_bytes(b"appeared journal")
                        snapshot.append(self.link_image(base))
                    with mock.patch.object(preparation, "handoff", changed), self.assertRaisesRegex(
                            MODULE.MatrixError, "sole link|identity or content changed|journal appeared"):
                        MODULE._write_transaction(journal, [{}], committed=False, preparation=preparation)
                    self.assertEqual(self.link_image(base), snapshot[0])

    def test_custody_truncate_rechecks_links_through_owning_handle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            journal = base / "refresh.transaction"
            receipt = MODULE._PreparationCustody.create(journal)
            owner = MODULE._PreparationCustody(journal, receipt)
            fdopen = os.fdopen
            snapshot, truncations = [], []
            def opened(*args: Any, **kwargs: Any) -> Any:
                handle = fdopen(*args, **kwargs)
                if args[1] != "r+b":
                    return handle
                write, truncate = handle.write, handle.truncate
                def written(data: bytes) -> int:
                    count = write(data)
                    handle.flush()
                    os.link(owner.path, base / "alias")
                    snapshot.append(self.link_image(base))
                    return count
                def truncated(size: int) -> Any:
                    truncations.append(size)
                    return truncate(size)
                handle.write, handle.truncate = written, truncated
                return handle
            with mock.patch.object(os, "fdopen", opened), self.assertRaisesRegex(MODULE.MatrixError, "sole link"):
                owner.append({"cancelled": "unused"})
            self.assertEqual(self.link_image(base), snapshot[0])
            self.assertEqual(truncations, [])

    def setUp(self) -> None:
        super().setUp()
        # Logical fault sweeps need ordering and injected failures, just as in
        # AcceptanceFixture. Every terminating/restarting child still uses real
        # disk barriers; avoid repeating them in the parent's fault grid.
        sync_patch = mock.patch.object(os, "fsync", autospec=_NATIVE_FSYNC)
        sync_patch.start()
        self.addCleanup(sync_patch.stop)

    def _check_native_creation_retries(self, journaled) -> None:
        for selected_journal in (journaled,):
            for failure in ("missing-parent", "backup-denied", "payload-denied"):
                with self.subTest(staging=failure, journaled=journaled), tempfile.TemporaryDirectory() as directory:
                    base = Path(directory).resolve()
                    output, extra = base / "output", base / "missing" / "extra"
                    output.write_bytes(b"original\r\n")
                    journal = base / "refresh.transaction" if journaled else None
                    outputs = [(output, "candidate\r\n")]
                    if failure == "missing-parent":
                        outputs.append((extra, "extra\r\n"))
                    system_open = os.open
                    attempts, errors = [], []
                    def opened(name: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                        path = Path(name)
                        if path.name.startswith(".acceptance-") and flags & os.O_CREAT:
                            attempts.append(path)
                            if failure != "missing-parent" and len(attempts) == (
                                    1 if failure == "backup-denied" else 2):
                                errors.append(errno.EACCES)
                                raise PermissionError(errno.EACCES, "native creation denied", str(path))
                            try:
                                return system_open(name, flags, *args, **kwargs)
                            except OSError as error:
                                errors.append(error.errno)
                                raise
                        return system_open(name, flags, *args, **kwargs)
                    with mock.patch.object(os, "open", opened), self.assertRaises(OSError):
                        MODULE._replace_outputs(outputs, journal=journal)
                    self.assertEqual(errors, [errno.ENOENT if failure == "missing-parent" else errno.EACCES])
                    self.assertEqual(len(attempts), {"missing-parent": 3, "backup-denied": 1,
                                                     "payload-denied": 2}[failure])
                    self.assertEqual(output.read_bytes(), b"original\r\n")
                    self.assertFalse(extra.exists())
                    self.assertEqual(set(base.iterdir()), {output})
                    extra.parent.mkdir()
                    MODULE._recover_outputs(journal)
                    MODULE._replace_outputs(outputs, journal=journal)
                    self.assertEqual(output.read_bytes(), b"candidate\r\n")
                    if failure == "missing-parent":
                        self.assertEqual(extra.read_bytes(), b"extra\r\n")
                    self.assertEqual(list(base.rglob(".acceptance-*")), [])
                    self.assertFalse((base / ".refresh-preparation").exists())
                    if journal is not None:
                        self.assertFalse(journal.exists())
                        self.assertFalse(MODULE._preparation_pending(journal))

        if journaled:
            return
        for failure in ("missing-parent", "denied"):
            with self.subTest(recovery=failure), AcceptanceMatrixTests.recovery_transaction(self) as fixture:
                base, paths, backups, old_journal = fixture
                metadata = base / "metadata"
                metadata.mkdir()
                journal = metadata / old_journal.name
                old_journal.replace(journal)
                original_journal = journal.read_bytes()
                displaced = base / "displaced"
                system_open = os.open
                attempts, errors = [], []
                def opened(name: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                    path = Path(name)
                    if path.name.startswith(".acceptance-") and flags & os.O_CREAT:
                        attempts.append(path)
                        if failure == "denied":
                            errors.append(errno.EACCES)
                            raise PermissionError(errno.EACCES, "native recovery creation denied", str(path))
                        # Move only the payload directory; keep the live marker
                        # parent available so its retained identity can be checked.
                        payload.rename(displaced)
                        try:
                            return system_open(name, flags, *args, **kwargs)
                        except OSError as error:
                            errors.append(error.errno)
                            raise
                    return system_open(name, flags, *args, **kwargs)
                payload = base / "payload"
                payload.mkdir()
                transaction = json.loads(original_journal)
                for index, backup in enumerate(backups):
                    target_output = payload / paths[index].name
                    paths[index].replace(target_output)
                    paths[index] = target_output
                    transaction["outputs"][index]["path"] = str(target_output)
                    new = transaction["outputs"][index]["new"]
                    new["path"] = str(payload / Path(new["path"]).name)
                    target = payload / backup.name
                    backup.replace(target)
                    backups[index] = target
                    transaction["outputs"][index]["backup"] = MODULE._transaction_record(target)
                MODULE._write_transaction(journal, transaction["outputs"], committed=False)
                original_journal = journal.read_bytes()
                try:
                    with mock.patch.object(os, "open", opened), self.assertRaises(OSError):
                        MODULE._recover_outputs(journal)
                finally:
                    if displaced.exists():
                        displaced.rename(payload)
                self.assertEqual(len(attempts), 1)
                self.assertEqual(errors, [errno.ENOENT if failure == "missing-parent" else errno.EACCES])
                self.assertEqual([path.read_bytes() for path in paths], [b"candidate 0", b"candidate 1"])
                self.assertEqual([path.read_bytes() for path in backups], [b"original 0", b"original 1"])
                self.assertEqual(journal.read_bytes(), original_journal)
                self.assertFalse(MODULE._preparation_pending(journal))
                self.assertEqual(set(payload.iterdir()), set(backups) | set(paths))
                MODULE._recover_outputs(journal)
                self.assertEqual([path.read_bytes() for path in paths], [b"original 0", b"original 1"])
                self.assertFalse(journal.exists())
                self.assertEqual(list(base.rglob(".acceptance-*")), [])

        fixture = AcceptanceMatrixTests("runTest")
        try:
            fixture._create_fixture()
            matrix = fixture.prepare_refresh()
            document = fixture.tmp / "missing" / "acceptance.md"
            before = {path: path.read_bytes() for path in fixture.tmp.rglob("*") if path.is_file()}
            code, _, error = fixture.run_main(*fixture.base_args(), "--write-doc", str(document))
            self.assertEqual(code, 2, error)
            self.assertIn("cannot replace output", error)
            self.assertEqual({path: path.read_bytes() for path in fixture.tmp.rglob("*") if path.is_file()}, before)
            document.parent.mkdir()
            self.assertEqual(fixture.run_main(*fixture.base_args())[0], 0)
            code, _, error = fixture.run_main(*fixture.base_args(), "--write-doc", str(document))
            self.assertEqual(code, 0, error)
            self.assertEqual(document.read_bytes(), MODULE.render_doc(matrix).encode("utf-8"))
            self.assertEqual(fixture.run_main(*fixture.base_args(), "--check-doc", str(document))[0], 0)
        finally:
            fixture.doCleanups()

    def _check_native_creation_refusals(self, recovery, batch) -> None:
        failures = ("before-opener", "created-enoent", "created-denied", "created-exists", "created-interrupt",
                    "created-runtime", "handle-after-create", "interrupted-absent", "unknown-error",
                    "stat-before", "stat-after", "stat-after-runtime", "collision", "collision-replaced",
                    "collision-content", "collision-link", "collision-unknown", "collision-after-unknown",
                    "collision-removed", "marker-replaced", "marker-content", "marker-link", "marker-stat")
        for selected_recovery in (recovery,):
            for failure in failures[batch::3]:
                with self.subTest(recovery=recovery, creation=failure), \
                        AcceptanceMatrixTests.recovery_transaction(self) as fixture:
                    base, paths, backups, journal = fixture
                    if not recovery:
                        # This fixture's pre-existing journal belongs to the
                        # recovery case only; direct staging owns a fresh marker.
                        journal.unlink()
                    marker = journal.with_suffix(".preparation")
                    candidate = base / (".acceptance-" + "a" * 32)
                    displaced, alias = base / "displaced", base / "alias"
                    if failure.startswith("collision"):
                        candidate.write_bytes(b"foreign\r\n")
                    original_outputs = [path.read_bytes() for path in paths]
                    original_backups = [path.read_bytes() for path in backups]
                    original_journal = journal.read_bytes() if recovery else None
                    system_open, builtin_open, lstat = os.open, open, Path.lstat
                    native, stats, identities, marker_bytes = [], [], {}, []
                    def checked_stat(path: Path, *args: Any, **kwargs: Any) -> Any:
                        if path == marker and failure == "marker-stat" and native:
                            raise PermissionError(errno.EACCES, "marker identity unavailable")
                        if path == candidate:
                            stats.append(path)
                            if failure == "stat-before" or (failure == "stat-after" and len(stats) == 2):
                                raise PermissionError(errno.EACCES, "candidate identity unavailable")
                            if failure == "stat-after-runtime" and len(stats) == 2:
                                raise RuntimeError("candidate identity interrupted")
                            if failure == "collision-unknown" or (failure == "collision-after-unknown" and len(stats) == 2):
                                fields = list(lstat(path, *args, **kwargs))
                                fields[1] = 0
                                return os.stat_result(fields)
                        return lstat(path, *args, **kwargs)
                    def opened(name: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                        if Path(name) != candidate or not flags & os.O_CREAT:
                            return system_open(name, flags, *args, **kwargs)
                        native.append(flags)
                        marker_bytes.append(marker.read_bytes())
                        if failure in ("stat-after", "stat-after-runtime", "unknown-error", "interrupted-absent") \
                                or failure.startswith("marker-"):
                            if failure == "marker-replaced":
                                marker.rename(displaced)
                                marker.write_bytes(b"foreign marker\r\n")
                            elif failure == "marker-content":
                                marker.write_bytes(b"foreign marker\r\n")
                            elif failure == "marker-link":
                                os.link(marker, alias)
                            if failure.startswith("marker-"):
                                identities[marker] = lstat(marker)
                            if failure == "unknown-error":
                                raise OSError("native error without status")
                            if failure == "interrupted-absent":
                                raise InterruptedError(errno.EINTR, "native create interrupted")
                            raise PermissionError(errno.EACCES, "native create denied")
                        if failure == "collision-removed":
                            candidate.rename(displaced)
                            identities[displaced] = lstat(displaced)
                            raise FileExistsError(errno.EEXIST, "collision disappeared during native create")
                        if failure == "collision-replaced":
                            candidate.rename(displaced)
                            candidate.write_bytes(b"replacement\r\n")
                        elif failure == "collision-content":
                            candidate.write_bytes(b"changed foreign content\r\n")
                        elif failure == "collision-link":
                            os.link(candidate, alias)
                        if failure.startswith("collision"):
                            identities[candidate] = lstat(candidate)
                            return system_open(name, flags, *args, **kwargs)
                        descriptor = system_open(name, flags, *args, **kwargs)
                        identities[candidate] = lstat(candidate)
                        if failure == "handle-after-create":
                            return descriptor
                        os.close(descriptor)
                        if failure == "created-enoent":
                            raise FileNotFoundError(errno.ENOENT, "signal after native create")
                        if failure == "created-denied":
                            raise PermissionError(errno.EACCES, "signal after native create")
                        if failure == "created-exists":
                            raise FileExistsError(errno.EEXIST, "signal after native create")
                        if failure == "created-interrupt":
                            raise KeyboardInterrupt("signal after native create")
                        raise RuntimeError("signal after native create")
                    def handled(name: Any, *args: Any, **kwargs: Any) -> Any:
                        if Path(name) == candidate:
                            if failure == "before-opener":
                                raise PermissionError(errno.EACCES, "before native opener")
                            handle = builtin_open(name, *args, **kwargs)
                            if failure == "handle-after-create":
                                handle.close()
                                raise OSError(errno.EIO, "handle construction interrupted")
                            return handle
                        return builtin_open(name, *args, **kwargs)
                    with mock.patch.object(MODULE.secrets, "token_hex", return_value="a" * 32), \
                            mock.patch.object(os, "open", opened), mock.patch.object(Path, "lstat", checked_stat), \
                            mock.patch.object(MODULE, "open", handled, create=True), \
                            self.assertRaises((MODULE.MatrixError, OSError, RuntimeError, KeyboardInterrupt)):
                        if recovery:
                            MODULE._recover_outputs(journal)
                        else:
                            MODULE._replace_outputs([(paths[0], "new\r\n")], journal=journal)
                    self.assertEqual(len(native), 0 if failure in ("before-opener", "stat-before") else 1)
                    self.assertEqual([path.read_bytes() for path in paths], original_outputs)
                    self.assertEqual([path.read_bytes() for path in backups], original_backups)
                    if recovery:
                        self.assertEqual(journal.read_bytes(), original_journal)
                    if failure in ("before-opener", "collision"):
                        self.assertFalse(marker.exists())
                        MODULE._recover_outputs(journal)
                        MODULE._replace_outputs([(paths[0], "retry\r\n")], journal=journal)
                        self.assertEqual(paths[0].read_bytes(), b"retry\r\n")
                    else:
                        self.assertTrue(marker.exists())
                        retained = marker.read_bytes()
                        if failure not in ("stat-before", "marker-replaced", "marker-content"):
                            self.assertEqual(retained, marker_bytes[0])
                        with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"), \
                                mock.patch.object(MODULE, "open", side_effect=AssertionError("unknown marker opened"),
                                                  create=True):
                            MODULE._recover_outputs(journal)
                        self.assertEqual(marker.read_bytes(), retained)
                    for path, identity in identities.items():
                        self.assertTrue(os.path.samestat(identity, path.lstat()))
                        if path == candidate:
                            self.assertEqual(path.read_bytes(), b"changed foreign content\r\n" if failure == "collision-content"
                                             else b"replacement\r\n" if failure == "collision-replaced"
                                             else b"foreign\r\n" if failure.startswith("collision") else b"")
                    if failure in ("collision-replaced", "collision-removed"):
                        self.assertEqual(displaced.read_bytes(), b"foreign\r\n")
                    if failure in ("collision-link", "marker-link"):
                        self.assertEqual(alias.read_bytes(), (candidate if failure == "collision-link" else marker).read_bytes())

    def assert_phase_contract(self) -> None:
        source = Path(__file__).read_text(encoding="utf-8")
        prefix = source.split("\nimport contextlib\n", 1)[0]
        events = []
        class Capture(io.StringIO):
            def write(self, value: str) -> int:
                events.append(("write", value))
                return super().write(value)
            def flush(self) -> None:
                events.append(("flush", self.getvalue()))
        namespace: dict[str, Any] = {}
        output = Capture()
        with contextlib.redirect_stderr(output):
            exec(compile(prefix, "<phase-prefix>", "exec"), namespace)
        early = "acceptance-phase=entry\nacceptance-phase=imports-start\n"
        self.assertEqual(output.getvalue(), early)
        self.assertEqual([event[0] for event in events], ["write", "flush"] * 2)
        # The executed prefix has no imports except the future directive and sys.
        import ast
        imports = [node for node in ast.parse(prefix).body if isinstance(node, (ast.Import, ast.ImportFrom))]
        self.assertEqual([(getattr(node, "module", None), [name.name for name in node.names])
                          for node in imports], [("__future__", ["annotations"]), (None, ["sys"])])
        for start, operation, finish in (
                ('_phase("imports-start")', "import contextlib", '_phase("imports-finish")'),
                ('_phase("checker-import-start")', "SPEC.loader.exec_module(MODULE)",
                 '_phase("checker-import-finish")'),
                ('_phase("public-import-start")', "PUBLIC_SPEC.loader.exec_module(PUBLIC_TREE)",
                 '_phase("public-import-finish")')):
            self.assertLess(source.index(start), source.index(operation))
            self.assertLess(source.index(operation), source.index(finish))
        self.assertLess(source.index('_phase("module-ready")'), source.index('if __name__ == "__main__":'))
        self.assertIn("    _PhaseProgram()\n", source.split('if __name__ == "__main__":', 1)[1])
        self.assertEqual(namespace["_PHASE_LIMIT"], 768)
        emit = namespace["_phase"]
        namespace["_phase_records"] = 0
        events.clear()
        output = Capture()
        with contextlib.redirect_stdout(io.StringIO()) as stdout, contextlib.redirect_stderr(output):
            emit("unknown")
            self.assertEqual(namespace["_phase_records"], 0)
            emit("selection-start", selector=7)
            for selector in (-1, _CONTRACT_PARTITIONS, True, "private-selector", None):
                emit("selection-finish", selector=selector)
            emit("selection-finish", selector=_CONTRACT_PARTITIONS - 1)
            emit("case-start", case="a/\n\r\u96ea" * 256)
            for _ in range(800):
                emit("case-finish", case="AcceptanceSelectionTests.test_contract_partition_refuses_invalid_selection")
        records = output.getvalue().splitlines(keepends=True)
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(len(records), 768)
        self.assertEqual(namespace["_phase_records"], 768)
        self.assertEqual(len(events), 2 * 768)
        self.assertEqual([event[0] for event in events], ["write", "flush"] * 768)
        self.assertEqual(records[:7], ["acceptance-phase=selection-start selector=7\n"] +
                         ["acceptance-phase=selection-finish\n"] * 5 +
                         [f"acceptance-phase=selection-finish selector={_CONTRACT_PARTITIONS - 1}\n"])
        self.assertEqual(records[7], "acceptance-phase=case-start case=" + "a????" * 25 + "a??\n")
        for record in records:
            self.assertLessEqual(len(record.encode("ascii")), 256)
            self.assertRegex(record, r"^acceptance-phase=[a-z-]+(?: selector=[0-9]{1,3})?(?: case=[A-Za-z0-9._?]{1,128})?\n$")
        # Failed/partial writes and failed flushes disable further diagnostics.
        # Neither retries nor stream errors may replace a test's result/exception.
        for stream in (None, object(), mock.Mock(write=mock.Mock(return_value=0)),
                       mock.Mock(write=mock.Mock(side_effect=OSError("denied"))),
                       mock.Mock(write=mock.Mock(side_effect=ValueError("closed")))):
            namespace["_phase_records"] = 0
            with mock.patch.object(sys, "stderr", stream):
                emit("entry")
                emit("entry")
            self.assertEqual(namespace["_phase_records"], 768)
            if isinstance(stream, mock.Mock):
                stream.write.assert_called_once_with("acceptance-phase=entry\n")
                stream.flush.assert_not_called()
        stream = mock.Mock(write=mock.Mock(return_value=len("acceptance-phase=entry\n")),
                           flush=mock.Mock(side_effect=OSError("flush denied")))
        namespace["_phase_records"] = 0
        with mock.patch.object(sys, "stderr", stream):
            emit("entry")
            emit("entry")
        self.assertEqual(namespace["_phase_records"], 768)
        stream.write.assert_called_once()
        stream.flush.assert_called_once()
        for cls in (AcceptanceFixture, ProductionAcceptanceMatrixTests, AcceptanceMatrixPublishabilityTests,
                    AcceptanceMatrixPublicTreeTests, PublicTreeScanTests, AcceptanceSelectionTests):
            self.assertEqual(cls.__bases__, (_PhaseCase,))
        # Delegate the same instance/result exactly once and retain return values
        # and raised exceptions, including an interrupted run's final record.
        case, result = _PhaseCase(), object()
        for failure in (None, RuntimeError("case interrupted"), KeyboardInterrupt()):
            output = io.StringIO()
            def original(actual_case: Any, actual_result: Any) -> Any:
                self.assertIs(actual_case, case)
                self.assertIs(actual_result, result)
                self.assertEqual(output.getvalue(), "acceptance-phase=case-start case=_PhaseCase.runTest\n")
                if failure is not None:
                    raise failure
                return result
            with mock.patch.dict(globals(), _phase_records=0), contextlib.redirect_stderr(output), \
                    mock.patch.object(unittest.TestCase, "run", autospec=True, side_effect=original) as delegated:
                if failure is None:
                    self.assertIs(case.run(result), result)
                else:
                    with self.assertRaises(type(failure)) as raised:
                        case.run(result)
                    self.assertIs(raised.exception, failure)
            delegated.assert_called_once_with(case, result)
            self.assertEqual(output.getvalue(), "acceptance-phase=case-start case=_PhaseCase.runTest\n"
                             "acceptance-phase=case-finish case=_PhaseCase.runTest\n")
        output = io.StringIO()
        selected = "AcceptanceSelectionTests.test_contract_partition_refuses_invalid_selection"
        with mock.patch.dict(globals(), _phase_records=0), contextlib.redirect_stderr(output):
            program = _PhaseProgram(module=sys.modules[__name__], argv=["tests", selected], exit=False,
                                    testRunner=unittest.TextTestRunner(stream=io.StringIO()))
        self.assertTrue(program.result.wasSuccessful())
        self.assertEqual(program.result.testsRun, 1)
        self.assertEqual(output.getvalue().splitlines(), ["acceptance-phase=selection-start",
                         "acceptance-phase=selection-finish", "acceptance-phase=case-start case=" + selected,
                         "acceptance-phase=case-finish case=" + selected])
        output = io.StringIO()
        with mock.patch.dict(globals(), _phase_records=0), contextlib.redirect_stderr(output):
            suite = contract_suite(7)
        self.assertEqual(suite.countTestCases(), 3)
        self.assertEqual(output.getvalue(), "acceptance-phase=selection-start selector=7\n"
                         "acceptance-phase=selection-finish selector=7\n")

    def assert_artifact_discoverable(self, artifact: Path, authority: bytes) -> None:
        # Preparation JSON lines and transaction JSON use default string encoding.
        # Require the complete quoted path, even when a later record is partial;
        # discoverability does not establish valid metadata or restart ownership.
        self.assertIn(json.dumps(str(artifact)).encode("utf-8"), authority)

    def test_artifact_discovery_requires_complete_json_path_tokens(self) -> None:
        spellings = (
            r"C:\workspace\.acceptance-00000000000000000000000000000001",
            r"\\server\share\.acceptance-00000000000000000000000000000001",
            "C:\\caf\u00e9\\.acceptance-00000000000000000000000000000001",
            '/work/"quoted"/.acceptance-00000000000000000000000000000001',
            "/work/back\\slash/.acceptance-00000000000000000000000000000001",
            "/work/\u96ea\U0001f680/.acceptance-00000000000000000000000000000001",
        )
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            payload = base / "payload"
            payload.write_bytes(b"candidate\r\n")
            for spelling in spellings:
                artifact = Path(spelling)
                record = dict(MODULE._transaction_record(payload), path=str(artifact))
                preparation = MODULE._OutputPreparation(base / "refresh.preparation")
                preparation.intend(artifact, base / "destination", payload.read_bytes())
                entries = [{"path": str(base / "destination"), "old": None, "backup": None, "new": record}]
                for version in (1, 2):
                    journal = base / f"refresh{version}.transaction"
                    MODULE._write_transaction(journal, entries, committed=False,
                                              restorations=None if version == 1 else [None])
                metadata = (preparation.path, base / "refresh1.transaction", base / "refresh2.transaction")
                token = json.dumps(str(artifact)).encode("utf-8")
                for path in metadata:
                    with self.subTest(spelling=spelling, metadata=path.name):
                        original = path.read_bytes()
                        self.assert_artifact_discoverable(artifact, original)
                        # The token remains discoverable before a torn suffix.
                        self.assert_artifact_discoverable(artifact, original + b'\n{"create":')
                        wrong_paths = (str(artifact) + "-other", "other-" + str(artifact),
                                       ".acceptance-" + f"{1:032x}")
                        mutations = (b"", original.replace(token, b'null'),
                                     original[:original.index(token)] + token[:len(token) // 2],
                                     original[:original.index(token)] + token[:-1],
                                     original.replace(token, token[1:-1]),
                                     original.replace(token, json.dumps(token.decode("utf-8")).encode("utf-8")),
                                     *(original.replace(token, json.dumps(wrong).encode("utf-8"))
                                       for wrong in wrong_paths))
                        try:
                            for mutated in mutations:
                                path.write_bytes(mutated)
                                with self.assertRaises(AssertionError):
                                    self.assert_artifact_discoverable(artifact, path.read_bytes())
                        finally:
                            path.write_bytes(original)
                        self.assertEqual(path.read_bytes(), original)
                preparation.finish()
            # Exercise the shared interruption oracle with real escaped paths
            # on every host, including hosts without Windows path semantics.
            unicode_root = base / "caf\u00e9"
            unicode_root.mkdir()
            with mock.patch.object(tempfile, "tempdir", str(unicode_root)):
                self.preparation_exit("original", "create", True)

    @staticmethod
    def preparation_program() -> str:
        return r'''
import os, sys
os.write(2, b"phase=imports\n")
import atexit, threading
# Eight constant phases and one stack record: at most nine records, each
# below 256 bytes (2304 bytes total). No filenames, locals or payload bytes.
seen = set()
def mark(value):
    if value not in seen:
        seen.add(value)
        os.write(2, b"phase=" + value + b"\n")
main_thread = threading.get_ident()
def dump_stack():
    # One thread, at most six frames, 24 ASCII name bytes + five line digits
    # per frame. This Python watchdog cannot preempt a held GIL or OS latency.
    frame = sys._current_frames().get(main_thread)
    frames = []
    try:
        for _ in range(6):
            if frame is None:
                break
            name = "".join(c if c.isascii() and (c.isalnum() or c in "_<>") else "?"
                           for c in frame.f_code.co_name[:24])
            frames.append(name + ":" + str(min(frame.f_lineno, 99999)))
            frame = frame.f_back
        os.write(2, ("stack-main-limit6=" + ";".join(frames) + "\n").encode("ascii"))
    finally:
        del frame
watchdog = threading.Timer(3, dump_stack)
watchdog.daemon = True
atexit.register(watchdog.cancel)
watchdog.start()
import json, pathlib, importlib.util
mark(b"loader")
spec = importlib.util.spec_from_file_location("acceptance_child", sys.argv[1])
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
g = checker.__dict__
mark(b"ready")
output_count = int(sys.argv[7])
base, phase, boundary, after, number = pathlib.Path(sys.argv[2]), sys.argv[3], sys.argv[4], int(sys.argv[5]), int(sys.argv[6])
paths = [base / str(index) for index in range(output_count)]
journal = base / "refresh.transaction"
active = phase != "rollback"
selected = None
descriptor_selected = None
creations = 0
def stop(operation, completed):
    if active and operation == boundary and completed == after:
        mark(b"selected-exit")
        watchdog.cancel()
        os._exit(73)
system_open, fdopen, create, sync = os.open, os.fdopen, open, os.fsync
replace, publish, unlink = os.replace, pathlib.Path.replace, pathlib.Path.unlink
def opened(path, flags, *args, **kwargs):
    global creations, selected, descriptor_selected
    name = pathlib.Path(path)
    match = (name.name.startswith(".acceptance-") if phase not in ("marker", "pending") else
             name == journal.with_suffix(".preparation" if phase == "marker" else ".pending"))
    chosen = False
    if active and match and flags & os.O_CREAT:
        creations += 1
        chosen = creations == number
        if chosen:
            selected = name
            stop("create", 0)
    descriptor = system_open(path, flags, *args, **kwargs)
    if chosen:
        descriptor_selected = descriptor
        stop("create", 1)  # Native creation succeeded; no Python identity bookkeeping yet.
    return descriptor
class Probe:
    def __init__(self, handle):
        self.handle = handle
    def __getattr__(self, name):
        return getattr(self.handle, name)
    def __enter__(self):
        return self
    def __exit__(self, *args):
        stop("close", 0)
        result = self.handle.__exit__(*args)
        stop("close", 1)
        return result
    def write(self, data):
        stop("write", 0)
        if boundary == "partial":
            self.handle.write(data[:3])
            self.handle.flush()
            mark(b"selected-exit")
            watchdog.cancel()
            os._exit(73)
        result = self.handle.write(data)
        stop("write", 1)
        return result
    def flush(self):
        stop("flush", 0)
        result = self.handle.flush()
        stop("flush", 1)
        return result
def payload(*args, **kwargs):
    handle = create(*args, **kwargs)
    return Probe(handle) if selected is not None and pathlib.Path(args[0]) == selected else handle
def journal_handle(descriptor, mode):
    handle = fdopen(descriptor, mode)
    return Probe(handle) if phase in ("marker", "pending") and descriptor == descriptor_selected else handle
def flushed(descriptor):
    if descriptor == descriptor_selected:
        stop("sync", 0)
    sync(descriptor)
    if descriptor == descriptor_selected:
        stop("sync", 1)
preparation_class = g.get("_OutputPreparation")
bind = preparation_class.bind if preparation_class is not None else None
def bound(preparation, record):
    if pathlib.Path(record["path"]) == selected:
        stop("bind", 0)
    result = bind(preparation, record)
    if pathlib.Path(record["path"]) == selected:
        stop("bind", 1)
    return result
record_file = g["_transaction_record"]
def recorded(path, *args, **kwargs):
    match = (phase in ("marker", "pending") or preparation_class is None) and path == selected
    if match:
        stop("bind", 0)
    result = record_file(path, *args, **kwargs)
    if match:
        stop("bind", 1)
    return result
write = g["_write_transaction"]
def metadata(*args, **kwargs):
    mark(b"metadata")
    stop("metadata", 0)
    result = write(*args, **kwargs)
    stop("metadata", 1)
    return result
def renamed(source, target):
    match = (pathlib.Path(target) in paths if phase in ("rollback", "recovery") else pathlib.Path(target) == journal)
    if match:
        stop("rename", 0)
    result = replace(source, target)
    if match:
        stop("rename", 1)  # Native rename succeeded; caller bookkeeping has not run.
    return result
def published(source, target):
    global active
    mark(b"publication")
    if phase in ("original", "direct"):
        stop("publication", 0)
    result = publish(source, target)
    if phase in ("original", "direct"):
        stop("publication", 1)
    if phase == "rollback" and target == paths[-1]:
        active = True
        mark(b"rollback")
        raise KeyboardInterrupt("start restoration")
    return result
def removed(path, *args, **kwargs):
    match = path == journal.with_suffix(".preparation")
    if match:
        stop("handoff", 0)
    result = unlink(path, *args, **kwargs)
    if match:
        stop("handoff", 1)
    return result
os.open, os.fdopen, os.fsync, os.replace = opened, journal_handle, flushed, renamed
g["open"] = payload
if preparation_class is not None:
    preparation_class.bind = bound
g["_transaction_record"] = recorded
g["_write_transaction"] = metadata
pathlib.Path.replace = published
pathlib.Path.unlink = removed
mark(b"staging")
if phase == "recovery":
    g["_recover_outputs"](journal)
elif phase == "pending":
    g["_write_transaction"](journal, json.loads((base / "entries.json").read_bytes()), committed=False)
else:
    g["_replace_outputs"]([(path, "candidate\r\n") for path in paths], journal=None if phase == "direct" else journal)
raise SystemExit("termination boundary was not reached")
'''

    def preparation_exit(self, phase: str, boundary: str, after: bool, *, candidate: bool = False, output_count: int = 1) -> None:
        # Both processes load the actual checker. Boundary checks need one
        # destination; the full-set regression separately exercises all five.
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            paths = [base / str(index) for index in range(output_count)]
            before = {path: ("original " + path.name + "\r\n").encode() for path in paths}
            for path, data in before.items():
                path.write_bytes(data)
                path.chmod(0o640)
            modes = {path: stat.S_IMODE(path.stat().st_mode) for path in paths}
            journal = base / "refresh.transaction"
            if phase in ("recovery", "pending"):
                entries = []
                for index, path in enumerate(paths):
                    old, new = [base / (".acceptance-" + f"{2 * index + offset:032x}") for offset in (0, 1)]
                    old.write_bytes(before[path])
                    old.chmod(modes[path])
                    new.write_bytes(b"candidate\r\n")
                    entries.append({"path": str(path), "old": MODULE._transaction_record(old)["sha256"],
                                    "backup": MODULE._transaction_record(old), "new": MODULE._transaction_record(new)})
                if phase == "recovery":
                    MODULE._write_transaction(journal, entries, committed=False)
                    for entry in entries:
                        Path(entry["new"]["path"]).replace(entry["path"])
                else:
                    MODULE._write_transaction(journal, entries, committed=False)
                    (base / "entries.json").write_bytes(json.dumps(entries).encode())
            program = self.preparation_program()
            try:
                child = subprocess.run([sys.executable, "-I", "-B", "-c", program, str(MODULE_PATH),
                                        str(base), phase, boundary, str(int(after)), "2" if candidate else "1", str(output_count)],
                                       capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
            except subprocess.TimeoutExpired as error:
                # subprocess.run has killed/reaped the child. Keep the original
                # exception and streams; expose bounded stderr in hosted logs.
                captured = error.stderr or b""
                if isinstance(captured, bytes):
                    captured = captured[:4096].decode("ascii", errors="replace")
                captured = captured[:4096].encode("ascii", errors="replace").decode("ascii")
                print("isolated child diagnostics (stderr, limit 4096):\n" + captured, file=sys.stderr)
                raise
            self.assertEqual(child.returncode, 73, (child.stdout, child.stderr))
            if phase in ("marker", "pending") or (phase in ("original", "direct") and boundary != "publication"):
                self.assertEqual({path: path.read_bytes() for path in paths}, before)
                self.assertEqual({path: stat.S_IMODE(path.stat().st_mode) for path in paths}, modes)
            markers = [journal.with_suffix(suffix) for suffix in (".preparation", ".pending")]
            if phase == "direct":
                markers.append(base / ".refresh-preparation")
            authority = b"\n".join(path.read_bytes() for path in [*markers, journal] if path.exists())
            for artifact in base.glob(".acceptance-*"):
                # No prefix/filename inference: each created large-file name
                # must actually be discoverable in fixed, written metadata.
                self.assert_artifact_discoverable(artifact, authority)
            snapshot = {path: (path.read_bytes(), path.stat()) for path in base.iterdir() if path.is_file()}
            restart = r'''
import pathlib, importlib.util, sys
spec = importlib.util.spec_from_file_location("acceptance_child", sys.argv[1])
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
g = checker.__dict__
try:
    journal = pathlib.Path(sys.argv[2])
    output_count = int(sys.argv[4])
    if sys.argv[3] == "direct":
        g["_replace_outputs"]([(journal.parent / str(index), "original " + str(index) + "\r\n")
                               for index in range(output_count)])
    else:
        g["_recover_outputs"](journal)
except (g["MatrixError"], OSError) as error:
    print(str(error), file=sys.stderr)
    raise SystemExit(2)
'''
            resumed = subprocess.run([sys.executable, "-I", "-B", "-c", restart, str(MODULE_PATH), str(journal), phase, str(output_count)],
                                     capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
            if any(path.exists() for path in markers):
                self.assertEqual(resumed.returncode, 2, (resumed.stdout, resumed.stderr))
                self.assertIn("uncertain recovery preparation", resumed.stderr)
                self.assertEqual(set(snapshot), {path for path in base.iterdir() if path.is_file()})
                for path, (data, identity) in snapshot.items():
                    self.assertEqual(path.read_bytes(), data)
                    self.assertTrue(os.path.samestat(identity, path.stat()))
            else:
                self.assertEqual(resumed.returncode, 0, (resumed.stdout, resumed.stderr))
                self.assertEqual({path: path.read_bytes() for path in paths}, before)
                self.assertEqual({path: stat.S_IMODE(path.stat().st_mode) for path in paths}, modes)
                self.assertEqual(list(base.glob(".acceptance-*")), [])
                self.assertFalse(journal.exists())

    def _check_full_output_set_native_publication_is_discoverable(self, phase, boundary) -> None:
        self.preparation_exit(phase, boundary, True, output_count=MODULE._TRANSACTION_OUTPUTS)

    def test_interruption_diagnostics_bound_stderr_and_preserve_selected_exit(self) -> None:
        run = subprocess.run
        observed = []
        def capture(command: list[str], **kwargs: Any) -> Any:
            result = run(command, **kwargs)
            observed.append((command, result))
            return result
        with mock.patch.object(subprocess, "run", capture):
            self.preparation_exit("rollback", "write", True)
        command, child = observed[0]
        self.assertEqual(command[-5:], ["rollback", "write", "1", "1", "1"])
        self.assertEqual(child.returncode, 73)
        self.assertEqual(child.stdout, "")
        self.assertEqual([line for line in child.stderr.splitlines() if line.startswith("phase=")], [
            "phase=imports", "phase=loader", "phase=ready", "phase=staging",
            "phase=metadata", "phase=publication", "phase=rollback", "phase=selected-exit"])
        records = child.stderr.encode("ascii").splitlines(keepends=True)
        self.assertLessEqual(child.stderr.count("stack-main-limit6="), 1)
        self.assertLessEqual(len(records), 9)
        self.assertLessEqual(max(map(len, records)), 256)
        self.assertLessEqual(len(child.stderr.encode("ascii")), 2304)

    def test_interruption_stack_diagnostics_are_bounded(self) -> None:
        run = subprocess.run
        # Exercise the exact diagnostic prefix with a deeper stack and long,
        # non-ASCII code names; no source lines, paths or locals are printed.
        prefix = self.preparation_program().split("import json, pathlib, importlib.util\n", 1)[0]
        control = prefix.replace("watchdog.start()", "watchdog.cancel()\nwatchdog.start()") + r'''
watchdog.cancel()
watchdog.join()
assert not watchdog.is_alive()
def deep(depth):
    if depth:
        deep(depth - 1)
    else:
        dump_stack()
deep.__code__ = deep.__code__.replace(co_name="x\u96ea" * 40)
deep(10)
# Qualify the real timer callback too, without depending on elapsed startup
# time or on which main-thread frame it happens to observe.
watchdog = threading.Timer(0, dump_stack)
watchdog.daemon = True
watchdog.start()
watchdog.join()
'''
        bounded = run([sys.executable, "-I", "-B", "-c", control],
                      capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
        self.assertEqual(bounded.returncode, 0, bounded.stderr)
        self.assertEqual(bounded.stdout, "")
        records = bounded.stderr.encode("ascii").splitlines(keepends=True)
        self.assertEqual(len(records), 3)
        self.assertEqual(records[0], b"phase=imports\n")
        self.assertEqual(records[1].count(b";"), 5)
        self.assertIn(b"x?" * 12 + b":", records[1])
        self.assertTrue(records[2].startswith(b"stack-main-limit6="))
        self.assertLessEqual(records[2].count(b";"), 5)
        self.assertLessEqual(max(map(len, records)), 256)
        self.assertLessEqual(len(bounded.stderr.encode("ascii")), 4096)

    def test_interruption_watchdog_is_cancelled_on_normal_exit(self) -> None:
        run = subprocess.run
        prefix = self.preparation_program().split("import json, pathlib, importlib.util\n", 1)[0]
        # Exit handlers run in reverse registration order: observe cancellation
        # after the real handler, even on ordinary interpreter exit.
        control = prefix.replace("watchdog.start()", "").replace("atexit.register(watchdog.cancel)",
                                 'atexit.register(lambda: os.write(2, b"cancelled\\n" '
                                 'if watchdog.finished.is_set() else b"active\\n"))\n'
                                 "atexit.register(watchdog.cancel)")
        control += "main_thread = -1\ndump_stack()\n"
        normal = run([sys.executable, "-I", "-B", "-c", control],
                     capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
        self.assertEqual(normal.returncode, 0, normal.stderr)
        self.assertEqual(normal.stdout, "")
        self.assertEqual(normal.stderr, "phase=imports\nstack-main-limit6=\ncancelled\n")

    def test_interruption_timeout_retains_exception_and_captured_stack(self) -> None:
        run, popen = subprocess.run, subprocess.Popen
        processes, failures = [], []
        def started(*args: Any, **kwargs: Any) -> Any:
            child = popen(*args, **kwargs)
            processes.append(child)
            return child
        def blocked(command: list[str], **kwargs: Any) -> Any:
            command = list(command)
            # The block is in the actual child, at the selected stop after the
            # buffered write. Capture the stack at entry to the real wait call;
            # startup latency must not decide which frame the oracle sees.
            # The separate timer control qualifies the actual watchdog callback.
            anchor = '\n        mark(b"selected-exit")\n'
            self.assertEqual(command[4].count(anchor), 1)
            self.assertEqual(command[4].count("watchdog.start()"), 1)
            command[4] = command[4].replace("watchdog.start()", "", 1)
            block = '''
        def waiting(frame, event, argument):
            if event == "call" and frame.f_code is threading.Event.wait.__code__:
                sys.settrace(None)
                dump_stack()
                return None
            return waiting
        sys.settrace(waiting)
        threading.Event().wait()'''
            command[4] = command[4].replace(anchor, block + anchor, 1)
            try:
                return run(command, **kwargs)
            except subprocess.TimeoutExpired as error:
                failures.append(error)
                raise
        output = io.StringIO()
        with mock.patch.object(subprocess, "run", blocked), \
                mock.patch.object(subprocess, "Popen", started), contextlib.redirect_stderr(output):
            with self.assertRaises(subprocess.TimeoutExpired) as raised:
                self.preparation_exit("rollback", "write", True)
        self.assertEqual(len(failures), 1)
        self.assertIs(raised.exception, failures[0])
        self.assertEqual(raised.exception.timeout, _CHILD_TIMEOUT)
        captured = raised.exception.stderr
        # Windows run() drains text pipes after kill; POSIX retains bytes.
        self.assertIsInstance(captured, (bytes, str))
        if isinstance(captured, str):
            captured = captured.encode("ascii")
        self.assertIn(b"phase=rollback\n", captured)
        self.assertNotIn(b"phase=selected-exit\n", captured)
        self.assertEqual(captured.count(b"stack-main-limit6="), 1)
        self.assertIn(b"stop:", captured)
        self.assertIn(b"wait:", captured)
        records = captured.splitlines(keepends=True)
        self.assertLessEqual(len(records), 16)
        self.assertLessEqual(max(map(len, records)), 256)
        self.assertLessEqual(len(captured), 4096)
        self.assertEqual(output.getvalue(), "isolated child diagnostics (stderr, limit 4096):\n" +
                         captured.decode("ascii") + "\n")
        self.assertEqual(len(processes), 1)
        self.assertIsNotNone(processes[0].returncode)
        for stderr in (None, b"", b"x" * 5000, "x" * 5000, b"\xff" * 5000, "\u96ea" * 5000):
            with self.subTest(captured_type=type(stderr).__name__):
                error = subprocess.TimeoutExpired(["fixture"], 5, output=b"retained", stderr=stderr)
                output = io.StringIO()
                with mock.patch.object(subprocess, "run", side_effect=error) as invoked, \
                        contextlib.redirect_stderr(output), self.assertRaises(subprocess.TimeoutExpired) as raised:
                    self.preparation_exit("rollback", "write", True)
                self.assertIs(raised.exception, error)
                self.assertIs(error.stderr, stderr)
                self.assertEqual(error.stdout, b"retained")
                self.assertEqual(invoked.call_args.kwargs["timeout"], _CHILD_TIMEOUT)
                expected = "?" * 4096 if stderr in (b"\xff" * 5000, "\u96ea" * 5000) else (
                    "x" * 4096 if stderr else "")
                self.assertEqual(output.getvalue(), "isolated child diagnostics (stderr, limit 4096):\n" +
                                 expected + "\n")

    def _check_initial_backup_creation_is_discoverable_on_both_sides(self, after) -> None:
        with self.subTest(after=after):
            self.preparation_exit("original", "create", after)

    def _check_initial_candidate_creation_is_discoverable_on_both_sides(self, after) -> None:
        with self.subTest(after=after):
            self.preparation_exit("original", "create", after, candidate=True)

    def _check_initial_payload_buffer_boundaries_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("original", operation, after)

    def _check_initial_candidate_binding_and_partial_writes_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("original", operation, after, candidate=True)

    def _check_initial_candidate_buffer_boundaries_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("original", operation, after, candidate=True)

    def _check_initial_metadata_and_publication_bookkeeping_are_restartable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("original", operation, after)

    def _check_rollback_creation_and_binding_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("rollback", operation, after)

    def _check_rollback_payload_buffer_boundaries_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("rollback", operation, after)

    def _check_rollback_partial_writes_and_native_bookkeeping_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("rollback", operation, after)

    def _check_rollback_marker_release_preserves_restart_discovery(self, after) -> None:
        with self.subTest(after=after):
            self.preparation_exit("rollback", "handoff", after)

    def _check_restart_creation_and_binding_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("recovery", operation, after)

    def _check_restart_payload_buffer_boundaries_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("recovery", operation, after)

    def _check_restart_metadata_and_native_bookkeeping_are_restartable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("recovery", operation, after)

    def test_restart_partial_writes_are_discoverable(self) -> None:
        self.preparation_exit("recovery", "partial", True)

    def _check_pending_journal_creation_and_binding_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("pending", operation, after)

    def _check_pending_journal_buffer_boundaries_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("pending", operation, after)

    def _check_owned_interrupted_journal_native_publication_recovers_successfully(self, after) -> None:
        with self.subTest(after=after):
            self.preparation_exit("pending", "rename", after)

    def test_pending_journal_partial_writes_are_discoverable(self) -> None:
        self.preparation_exit("pending", "partial", True)

    def _check_first_marker_creation_is_discoverable_without_changing_outputs(self, after) -> None:
        with self.subTest(after=after):
            self.preparation_exit("marker", "create", after)

    def _check_first_marker_buffer_boundaries_are_discoverable_without_changing_outputs(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("marker", operation, after)

    def _check_first_marker_binding_is_discoverable_without_changing_outputs(self, after) -> None:
        with self.subTest(after=after):
            self.preparation_exit("marker", "bind", after)

    def _check_direct_preparation_creation_and_binding_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("direct", operation, after)

    def _check_direct_preparation_buffer_boundaries_are_discoverable(self, operation, after) -> None:
        with self.subTest(operation=operation, after=after):
            self.preparation_exit("direct", operation, after)

    def _check_direct_preparation_native_publication_is_discoverable(self, after) -> None:
        with self.subTest(after=after):
            self.preparation_exit("direct", "publication", after)

    def test_foreign_preparation_and_pending_occupants_are_never_opened_or_modified(self) -> None:
        for suffix in (".preparation", ".pending"):
            for contents in (b"", b"{partial", b'{"version":1,"outputs":[]}', b"x" * (MODULE._JOURNAL_BYTES + 1)):
                with self.subTest(suffix=suffix, size=len(contents)), tempfile.TemporaryDirectory() as directory:
                    base = Path(directory).resolve()
                    journal, output = base / "refresh.transaction", base / "output"
                    marker = journal.with_suffix(suffix)
                    output.write_bytes(b"original\r\n")
                    marker.write_bytes(contents)
                    identity = marker.stat()
                    with mock.patch.object(MODULE, "_read_transaction_file") as read:
                        for action in (lambda: MODULE._recover_outputs(journal),
                                       lambda: MODULE._replace_outputs([(output, "candidate")], journal=journal)):
                            with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                                action()
                        read.assert_not_called()
                    if suffix == ".pending":
                        with self.assertRaisesRegex(MODULE.MatrixError, "pending bytes retained"):
                            MODULE._write_transaction(journal, [{}], committed=False)
                    self.assertEqual(marker.read_bytes(), contents)
                    self.assertTrue(os.path.samestat(identity, marker.stat()))
                    self.assertEqual(output.read_bytes(), b"original\r\n")
                    self.assertFalse(journal.exists())
                    self.assertEqual(list(base.glob(".acceptance-*")), [])

    def test_foreign_preparation_aliases_preserve_the_actual_foreign_file(self) -> None:
        for suffix in (".preparation", ".pending"):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                journal, output, foreign = base / "refresh.transaction", base / "output", base / "foreign"
                output.write_bytes(b"original")
                foreign.write_bytes(b"foreign alias bytes\r\n")
                marker = journal.with_suffix(suffix)
                os.link(foreign, marker)  # An actual alias, also available without Windows symlink privilege.
                identity = foreign.stat()
                with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                    MODULE._recover_outputs(journal)
                if suffix == ".pending":
                    with self.assertRaisesRegex(MODULE.MatrixError, "pending bytes retained"):
                        MODULE._write_transaction(journal, [{}], committed=False)
                self.assertTrue(os.path.samestat(identity, foreign.stat()))
                self.assertTrue(os.path.samestat(identity, marker.stat()))
                self.assertEqual(foreign.read_bytes(), b"foreign alias bytes\r\n")
                self.assertEqual(output.read_bytes(), b"original")

    def test_preparation_and_pending_replacements_preserve_foreign_identity_and_bytes(self) -> None:
        for suffix in (".preparation", ".pending"):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                output, journal = base / "output", base / "refresh.transaction"
                output.write_bytes(b"original\r\n")
                marker, displaced = journal.with_suffix(suffix), base / "displaced"
                foreign_identity = []
                bind, replace = MODULE._OutputPreparation.bind, os.replace
                def substitute() -> None:
                    data = marker.read_bytes()
                    marker.rename(displaced)
                    marker.write_bytes(data)  # Identical contents cannot prove ownership.
                    foreign_identity.append(marker.stat())
                def bound(preparation: Any, record: Any) -> None:
                    if suffix == ".preparation" and not foreign_identity:
                        substitute()
                    bind(preparation, record)
                def renamed(source: Any, target: Any) -> None:
                    if suffix == ".pending" and Path(target) == journal and not foreign_identity:
                        substitute()
                        raise OSError("publication refused")
                    replace(source, target)
                with mock.patch.object(MODULE._OutputPreparation, "bind", bound), \
                        mock.patch.object(os, "replace", renamed), self.assertRaises((MODULE.MatrixError, OSError)):
                    MODULE._replace_outputs([(output, "candidate")], journal=journal)
                self.assertEqual(len(foreign_identity), 1)
                self.assertTrue(os.path.samestat(foreign_identity[0], marker.stat()))
                self.assertEqual(marker.read_bytes(), displaced.read_bytes())
                self.assertEqual(output.read_bytes(), b"original\r\n")
                snapshot = {path: path.read_bytes() for path in base.iterdir() if path.is_file()}
                with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                    MODULE._recover_outputs(journal)
                self.assertEqual({path: path.read_bytes() for path in base.iterdir() if path.is_file()}, snapshot)

    def test_preparation_in_place_edits_and_added_aliases_are_preserved(self) -> None:
        for edit in ("content", "hardlink"):
            with self.subTest(edit=edit), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                output, journal = base / "output", base / "refresh.transaction"
                output.write_bytes(b"original")
                marker, alias = journal.with_suffix(".preparation"), base / "alias"
                bind = MODULE._OutputPreparation.bind
                retained = []
                def changed(preparation: Any, record: Any) -> None:
                    if not retained:
                        if edit == "content":
                            marker.write_bytes(b"external preparation edit\r\n")
                        else:
                            os.link(marker, alias)
                        retained.append((marker.read_bytes(), marker.stat()))
                    bind(preparation, record)
                with mock.patch.object(MODULE._OutputPreparation, "bind", changed), \
                        self.assertRaisesRegex(MODULE.MatrixError, "preparation"):
                    MODULE._replace_outputs([(output, "candidate")], journal=journal)
                self.assertEqual(len(retained), 1)
                data, identity = retained[0]
                self.assertEqual(marker.read_bytes(), data)
                self.assertTrue(os.path.samestat(identity, marker.stat()))
                self.assertEqual(output.read_bytes(), b"original")
                if edit == "hardlink":
                    self.assertEqual(alias.read_bytes(), data)
                    self.assertTrue(os.path.samestat(identity, alias.stat()))

    def test_first_marker_failures_are_closed_and_preserve_all_outputs(self) -> None:
        for failure in ("create", "fdopen", "identity", "write", "flush", "sync", "close", "unreadable"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                base = Path(directory).resolve()
                output, journal = base / "output", base / "refresh.transaction"
                output.write_bytes(b"original\r\n")
                identity = output.stat()
                marker = journal.with_suffix(".preparation")
                system_open, fdopen, metadata, sync = os.open, os.fdopen, MODULE._transaction_metadata, os.fsync
                read = MODULE._read_transaction_file
                owned = []
                def opened(path: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                    if Path(path) == marker and flags & os.O_CREAT:
                        if failure == "create":
                            raise PermissionError("marker creation denied")
                        descriptor = system_open(path, flags, *args, **kwargs)
                        owned.append(descriptor)
                        return descriptor
                    return system_open(path, flags, *args, **kwargs)
                def handle(descriptor: int, mode: str) -> Any:
                    if descriptor in owned and mode == "r+b" and failure == "fdopen":
                        raise OSError("marker handle denied")
                    result = fdopen(descriptor, mode)
                    if descriptor in owned and mode == "r+b":
                        if failure == "write":
                            write = result.write
                            def partial(data: bytes) -> int:
                                write(data[:3])
                                return 3
                            result.write = partial
                        elif failure == "flush":
                            result.flush = mock.Mock(side_effect=OSError("marker flush denied"))
                        elif failure == "close":
                            close = result.close
                            def closed() -> None:
                                close()
                                raise OSError("marker close interrupted")
                            result.close = closed
                    return result
                def unknown(handle: Any) -> Any:
                    result = metadata(handle)
                    if failure == "identity" and handle.fileno() in owned:
                        fields = list(result)
                        fields[1] = 0
                        return os.stat_result(fields)
                    return result
                def flushed(descriptor: int) -> None:
                    if failure == "sync" and descriptor in owned:
                        raise OSError("marker sync denied")
                    sync(descriptor)
                def unreadable(path: Path, limit: int, **kwargs: Any) -> Any:
                    if failure == "unreadable" and path == marker:
                        raise PermissionError("marker unreadable")
                    return read(path, limit, **kwargs)
                with mock.patch.object(os, "open", opened), mock.patch.object(os, "fdopen", handle), \
                        mock.patch.object(os, "fsync", flushed), mock.patch.object(MODULE, "_transaction_metadata", unknown), \
                        mock.patch.object(MODULE, "_read_transaction_file", unreadable), \
                        self.assertRaises((MODULE.MatrixError, OSError)):
                    MODULE._replace_outputs([(output, "candidate")], journal=journal)
                self.assertEqual(output.read_bytes(), b"original\r\n")
                self.assertTrue(os.path.samestat(identity, output.stat()))
                self.assertEqual(list(base.glob(".acceptance-*")), [])
                self.assertFalse(journal.exists())
                if marker.exists():
                    retained = marker.read_bytes()
                    with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                        MODULE._recover_outputs(journal)
                    self.assertEqual(marker.read_bytes(), retained)
                self.assertEqual(marker.exists(), failure in ("fdopen", "identity", "unreadable"))
                if failure == "fdopen":
                    with self.assertRaises(OSError):
                        os.fstat(owned[0])

        # The journal's fixed pending file is also a first marker. If handle
        # construction fails, close the descriptor and retain the uncertain
        # file rather than inventing ownership from its name or empty bytes.
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            output, journal = base / "output", base / "refresh.transaction"
            output.write_bytes(b"original\r\n")
            pending = journal.with_suffix(".pending")
            system_open = os.open
            descriptors = []
            def created(path: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                descriptor = system_open(path, flags, *args, **kwargs)
                if Path(path) == pending:
                    descriptors.append(descriptor)
                return descriptor
            with mock.patch.object(os, "open", created), \
                    mock.patch.object(os, "fdopen", side_effect=OSError("journal handle denied")), \
                    self.assertRaisesRegex(OSError, "journal handle denied"):
                MODULE._write_transaction(journal, [{}], committed=False)
            self.assertEqual(len(descriptors), 1)
            with self.assertRaises(OSError):
                os.fstat(descriptors[0])
            identity = pending.stat()
            self.assertEqual(pending.read_bytes(), b"")
            with self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                MODULE._recover_outputs(journal)
            self.assertTrue(os.path.samestat(identity, pending.stat()))
            self.assertEqual(pending.read_bytes(), b"")
            self.assertEqual(output.read_bytes(), b"original\r\n")
            self.assertFalse(journal.exists())

    def test_preparation_bound_is_checked_before_creating_an_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            output, journal = base / "output", base / "refresh.transaction"
            output.write_bytes(b"original")
            with mock.patch.object(MODULE, "_JOURNAL_BYTES", 8), \
                    self.assertRaisesRegex(MODULE.MatrixError, "preparation exceeds its bound"):
                MODULE._replace_outputs([(output, "candidate")], journal=journal)
            self.assertEqual({path: path.read_bytes() for path in base.iterdir()}, {output: b"original"})

    def test_direct_preparation_is_detected_in_every_output_parent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            paths = [base / str(index) / "output" for index in range(5)]
            for path in paths:
                path.parent.mkdir()
                path.write_bytes(b"original\r\n")
            marker = paths[-1].parent / ".refresh-preparation"
            marker.write_bytes(b"foreign preparation\r\n")
            identity = marker.stat()
            for order in (paths, list(reversed(paths))):
                with self.subTest(first=order[0]), \
                        self.assertRaisesRegex(MODULE.MatrixError, "uncertain recovery preparation"):
                    MODULE._replace_outputs([(path, "candidate") for path in order])
            self.assertTrue(os.path.samestat(identity, marker.stat()))
            self.assertEqual(marker.read_bytes(), b"foreign preparation\r\n")
            self.assertEqual([path.read_bytes() for path in paths], [b"original\r\n"] * 5)
            self.assertEqual(list(base.rglob(".acceptance-*")), [])




    def test_native_selections_match_complete_python_discovery_once(self) -> None:
        loader = unittest.TestLoader()
        expected = [case(name).id() for case in (*_MATRIX_CLASSES, AcceptanceSelectionTests,
                                                  AcceptanceRecoveryTests, AcceptanceCustodyTests, AcceptanceMatrixPublicTreeTests)
                    for name in loader.getTestCaseNames(case)]
        original = sorted({case.__name__ + "." + getattr(getattr(case, name), "_contract_origin", name)
                          for case in (*_MATRIX_CLASSES, AcceptanceRecoveryTests, AcceptanceMatrixPublicTreeTests)
                          for name in loader.getTestCaseNames(case)
                          if name != "test_crash_fixture_matches_terminated_publication"})
        self.assertEqual(len(original), 252)
        self.assertEqual(hashlib.sha256("\n".join(original).encode("utf-8")).hexdigest(),
                         "44160a315d62abb65391cbf8bf512816dbc86db2f5955941311a642b47c43cf2")
        selected = [test.id() for number in range(_CONTRACT_PARTITIONS) for test in contract_suite(number)]
        known = {type(test).__name__ + "." + test._testMethodName for number in range(_CONTRACT_PARTITIONS)
                 for test in contract_suite(number)}
        self.assertTrue(set(_CONTRACT_CASE_PARTITIONS) <= known)
        self.assertTrue(all(type(number) is int and 0 <= number < _CONTRACT_PARTITIONS
                            for number in _CONTRACT_CASE_PARTITIONS.values()))
        cmake = (MODULE.DEFAULT_ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        blocks = re.findall(r'add_test\(\s*NAME (Acceptance\w+Contract\d*)\s+COMMAND(.*?)\)', cmake, re.S)
        native = []
        partitions = []
        for name, command in blocks:
            if name.startswith("AcceptanceMatrixPublicTreeContract"):
                selector = re.search(r'AcceptanceMatrixPublicTreeTests\.test_partition_[0-3]', command)
                self.assertIsNotNone(selector, name)
                native.extend(test.id() for test in loader.loadTestsFromName(selector[0], sys.modules[__name__]))
            else:
                partition = re.search(r'--contract-partition ([0-9]+)\s*$', command)
                self.assertIsNotNone(partition, name)
                number = int(partition[1])
                partitions.append(number)
                native.extend(test.id() for test in contract_suite(number))
            self.assertRegex(cmake, rf'set_tests_properties\(\s*{name}\s+PROPERTIES LABELS "hygiene;tools" TIMEOUT 30\)'
                             if not name.startswith("AcceptanceMatrixPublicTreeContract") else
                             r'AcceptanceMatrixPublicTreeContract4\s+PROPERTIES LABELS "hygiene;tools" TIMEOUT 30\)')
        selected.extend(test.id() for case in (AcceptanceMatrixPublicTreeTests,)
                        for test in loader.loadTestsFromTestCase(case))
        matrix = MODULE.load_matrix(MODULE.DEFAULT_ROOT / "tools/ci/acceptance_matrix.json")
        names = {"AcceptanceMatrixContract" + (str(number + 1) if number else "")
                 for number in range(_CONTRACT_PARTITIONS)}
        for row_id in ("perf.requirement-matrix", "iss.126", "ci.acceptance-preparation-custody"):
            evidence = {item["ref"] for item in row_of(matrix, row_id)["evidence"] if item["kind"] == "ctest"}
            self.assertEqual(evidence, names)
        self.assertEqual(sorted(partitions), list(range(_CONTRACT_PARTITIONS)))
        self.assertTrue(all(contract_suite(number).countTestCases() for number in partitions))
        self.assertEqual(sorted(native), sorted(expected))
        self.assertEqual(sorted(selected), sorted(expected))
        self.assertEqual(len(native), len(set(native)))
        self.assertEqual(len(selected), len(set(selected)))
        discovered = loader.loadTestsFromModule(sys.modules[__name__])
        self.assertEqual(discovered.countTestCases(), len(expected))
        self.assertEqual(len(expected), _EXPECTED_CASES)
        self.assert_phase_contract()

    def _check_partition_entrypoint_refuses_invalid_selection(self, value) -> None:
        with self.subTest(value=value):
            child = subprocess.run([sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                                    "--contract-partition", value],
                                   capture_output=True, text=True, timeout=_CHILD_TIMEOUT)
            self.assertEqual(child.returncode, 1)
            self.assertIn("invalid contract partition" if value != "unknown" else
                          "invalid literal for int()", child.stderr)
            self.assertEqual(child.stdout, "")

    def test_child_allowances_fit_every_registered_partition(self) -> None:
        import ast
        functions = [node for node in ast.walk(ast.parse(Path(__file__).read_text(encoding="utf-8")))
                     if isinstance(node, ast.FunctionDef)]
        # Inventory all launch sites, aliases and self-helper calls. This also
        # catches a new child hidden in an otherwise launch-free fault sweep.
        direct, helpers = set(), {}
        for function in functions:
            nodes = list(ast.walk(function))
            aliases = {target.id for node in nodes if isinstance(node, ast.Assign)
                       and any(isinstance(value, ast.Attribute) and isinstance(value.value, ast.Name)
                               and value.value.id == "subprocess" and value.attr in ("run", "Popen")
                               for value in ast.walk(node.value))
                       for target in ast.walk(node) if isinstance(target, ast.Name)
                       and isinstance(target.ctx, ast.Store)}
            calls = [node.func for node in nodes if isinstance(node, ast.Call)]
            if any((isinstance(call, ast.Attribute) and isinstance(call.value, ast.Name)
                    and call.value.id == "subprocess" and call.attr in ("run", "Popen"))
                   or (isinstance(call, ast.Name) and call.id in aliases) for call in calls):
                direct.add(function.name)
            helpers.setdefault(function.name, set()).update(
                call.attr for call in calls if isinstance(call, ast.Attribute)
                and isinstance(call.value, ast.Name) and call.value.id == "self")
        # At most one new helper per pass; the source's function count bounds
        # the transitive closure, independently of execution or runner speed.
        for _ in functions:
            expanded = direct | {name for name, calls in helpers.items() if calls & direct}
            if expanded == direct:
                break
            direct = expanded
        launched = set()
        loader = unittest.TestLoader()
        for owner in (*_MATRIX_CLASSES, AcceptanceSelectionTests, AcceptanceRecoveryTests, AcceptanceCustodyTests):
            for name in loader.getTestCaseNames(owner):
                implementation = getattr(getattr(owner, name), "_contract_check", name)
                if implementation in direct:
                    launched.add(owner.__name__ + "." + name)
        self.assertEqual(set(_CHILD_ALLOWANCES), launched)
        selected = {}
        for number in range(_CONTRACT_PARTITIONS):
            cases = list(contract_suite(number))
            allowances = sum(_CHILD_ALLOWANCES.get(type(case).__name__ + "." + case._testMethodName, 0)
                           for case in cases)
            self.assertLessEqual(allowances * _CHILD_TIMEOUT, 24, number)
            # One second per child launch or complete outer storage operation;
            # pure cases still reserve one second. Count the full nested grid.
            storage = sum(max(1, _STORAGE_ALLOWANCES.get(type(case).__name__ + "." + case._testMethodName, 1))
                          for case in cases)
            self.assertLessEqual(storage + allowances, 24, number)
            self.assertTrue(cases, number)
            if allowances:
                self.assertEqual(len(cases), 1, number)
                self.assertGreaterEqual(number, _LOCAL_PARTITIONS)
            else:
                self.assertLess(number, _LOCAL_PARTITIONS)
            selected.update((type(case).__name__ + "." + case._testMethodName, number) for case in cases)
        self.assertTrue(set(_CHILD_ALLOWANCES) <= selected.keys())
        self.assertTrue(set(_STORAGE_ALLOWANCES) <= selected.keys())
        self.assertEqual(len(set(selected[name] for name in _CHILD_ALLOWANCES)), len(_CHILD_ALLOWANCES))

    def test_contract_partition_refuses_invalid_selection(self) -> None:
        for number in (-1, _CONTRACT_PARTITIONS, True, "0", None):
            with self.subTest(number=number), self.assertRaises(ValueError):
                contract_suite(number)


# Each parameter tuple remains a separately discoverable unittest case. Keep
# the first tuple's historical name so existing direct selectors still work.
def _child_case(check, values):
    def run(self):
        check(self, *values)
    return run


_CHILD_ALLOWANCES = {}
# Fixed per-case bounds on outer CLI/transaction operations. The run wrapper
# checks actual work including nested fault grids and setup/cleanup helpers.
# Every other case reserves one unit for ordinary fixture and import work.
_STORAGE_ALLOWANCES = {
    'AcceptanceCustodyTests.test_explicit_enrollment_refuses_legacy_recovery_without_consuming_receipt': 2,
    'AcceptanceCustodyTests.test_legacy_recovery_validation_failure_retires_custody': 2,
    'AcceptanceCustodyTests.test_legacy_recovery_validation_failure_retires_custody_case_1': 2,
    'AcceptanceCustodyTests.test_legacy_recovery_validation_failure_retires_custody_case_2': 2,
    'AcceptanceCustodyTests.test_legacy_recovery_validation_failure_retires_custody_case_3': 2,
    'AcceptanceCustodyTests.test_legacy_recovery_validation_failure_retires_custody_case_4': 2,
    'AcceptanceCustodyTests.test_cleanup_and_reset_failures_remain_restartable': 8,
    'AcceptanceCustodyTests.test_cli_rejects_incomplete_or_combined_enrollment_requests': 4,
    'AcceptanceCustodyTests.test_cli_requires_independent_new_authority_outside_recovery_tree': 6,
    'AcceptanceCustodyTests.test_enrollment_cli_recovery_reuse_and_retirement': 6,
    'AcceptanceCustodyTests.test_enrollment_crash_before_receipt_delivery_is_recoverable': 2,
    'AcceptanceCustodyTests.test_enrollment_crash_before_receipt_delivery_is_recoverable_case_1': 3,
    'AcceptanceCustodyTests.test_enrollment_crash_before_receipt_delivery_is_recoverable_case_2': 3,
    'AcceptanceCustodyTests.test_enrollment_crash_before_receipt_delivery_is_recoverable_case_4': 3,
    'AcceptanceCustodyTests.test_enrollment_crash_before_receipt_delivery_is_recoverable_case_5': 3,
    'AcceptanceCustodyTests.test_enrollment_crash_before_receipt_delivery_is_recoverable_case_7': 3,
    'AcceptanceCustodyTests.test_enrollment_crash_before_receipt_delivery_is_recoverable_case_8': 3,
    'AcceptanceCustodyTests.test_enrollment_publication_rechecks_staging_and_preserves_foreign_marker': 4,
    'AcceptanceCustodyTests.test_enrollment_short_write_flush_and_sync_failures_preserve_unknown_state': 6,
    'AcceptanceCustodyTests.test_foreign_pending_and_replaced_enrolled_artifacts_refuse_before_cleanup': 6,
    'AcceptanceCustodyTests.test_foreign_stale_and_forged_receipts_preserve_all_bytes_and_identities': 7,
    'AcceptanceCustodyTests.test_interrupted_enrollment_never_adopts_a_marker_or_returns_a_receipt': 3,
    'AcceptanceCustodyTests.test_invalid_journal_refuses_before_owned_pending_cleanup_or_marker_reset': 2,
    'AcceptanceCustodyTests.test_malformed_authenticated_events_and_byte_bounds_refuse_untouched': 18,
    'AcceptanceCustodyTests.test_metadata_directory_aliases_share_canonical_transaction_paths': 6,
    'AcceptanceCustodyTests.test_receipt_persistence_failures_never_publish_a_marker': 15,
    'AcceptanceCustodyTests.test_record_metadata_file_and_output_bounds_are_enforced': 5,
    'AcceptanceCustodyTests.test_reset_removes_only_enrolled_partial_content_and_preserves_unrelated_files': 2,
    'AcceptanceCustodyTests.test_restart_reacquires_enrolled_partial_output_and_pending_files': 2,
    'AcceptanceCustodyTests.test_restart_reacquires_enrolled_partial_output_and_pending_files_case_1': 2,
    'AcceptanceCustodyTests.test_restart_reacquires_enrolled_partial_output_and_pending_files_case_2': 2,
    'AcceptanceCustodyTests.test_restart_reacquires_enrolled_partial_output_and_pending_files_case_3': 2,
    'AcceptanceCustodyTests.test_restart_reacquires_enrolled_partial_output_and_pending_files_case_4': 2,
    'AcceptanceCustodyTests.test_torn_event_reset_retains_the_committed_enrollment': 3,
    'AcceptanceCustodyTests.test_uncommitted_artifact_enrollment_is_unknown_and_untouched': 2,
    'AcceptanceCustodyTests.test_unknown_authenticated_metadata_and_tampering_refuse_without_reset': 4,
    'AcceptanceCustodyTests.test_unreadable_marker_and_artifact_refuse_without_reset': 3,
    'AcceptanceCustodyTests.test_v1_v2_journals_recover_with_custody_and_compact_before_restoration': 8,
    'AcceptanceMatrixTests.test_alias_change_before_snapshot_never_reads_the_new_target': 4,
    'AcceptanceMatrixTests.test_changed_or_unreadable_commit_input_restores_old_outputs': 3,
    'AcceptanceMatrixTests.test_cli_alias_retargeting_refuses_before_and_after_every_publication': 16,
    'AcceptanceMatrixTests.test_cli_alias_retargeting_refuses_before_and_after_every_publication_case_1': 16,
    'AcceptanceMatrixTests.test_cli_alias_retargeting_refuses_before_and_after_every_publication_case_2': 16,
    'AcceptanceMatrixTests.test_cli_alias_retargeting_refuses_before_and_after_every_publication_case_3': 16,
    'AcceptanceMatrixTests.test_cli_alias_retargeting_refuses_before_and_after_every_publication_case_4': 16,
    'AcceptanceMatrixTests.test_cli_alias_retargeting_refuses_before_and_after_every_publication_case_5': 16,
    'AcceptanceMatrixTests.test_cli_alias_retargeting_refuses_before_and_after_every_publication_case_6': 16,
    'AcceptanceMatrixTests.test_cli_parser_setup_is_shared_without_sharing_invocation_inputs': 5,
    'AcceptanceMatrixTests.test_crlf_doc_is_accepted': 2,
    'AcceptanceMatrixTests.test_default_destinations_cannot_read_the_mandatory_lock_range': 6,
    'AcceptanceMatrixTests.test_default_input_destinations_are_resolved_for_readers': 12,
    'AcceptanceMatrixTests.test_default_output_destinations_use_resolved_paths_for_publish_and_rollback': 6,
    'AcceptanceMatrixTests.test_direct_publication_refuses_external_destination_changes_before_replace': 2,
    'AcceptanceMatrixTests.test_edited_doc_is_stale': 2,
    'AcceptanceMatrixTests.test_execution_cli_writes_scoped_receipt_and_requires_discovery': 2,
    'AcceptanceMatrixTests.test_first_reader_holds_lock_before_first_writer_can_publish': 3,
    'AcceptanceMatrixTests.test_generated_document_as_source_or_other_evidence_remains_protected': 12,
    'AcceptanceMatrixTests.test_generated_document_evidence_aliases_remain_real_inputs': 6,
    'AcceptanceMatrixTests.test_generated_document_evidence_allows_both_writers_and_separate_exports': 4,
    'AcceptanceMatrixTests.test_generated_document_evidence_stays_in_refresh_snapshot': 3,
    'AcceptanceMatrixTests.test_generated_document_resolution_and_validation_precede_publication': 4,
    'AcceptanceMatrixTests.test_in_place_candidate_edits_are_preserved_during_refusal_cleanup': 8,
    'AcceptanceMatrixTests.test_interrupt_reconciliation_preserves_external_output_and_source_occupants': 12,
    'AcceptanceMatrixTests.test_interrupted_native_restore_and_removal_are_reconciled_before_cleanup': 4,
    'AcceptanceMatrixTests.test_journal_native_publication_interrupt_covers_prepare_commit_and_rollback_marker': 10,
    'AcceptanceMatrixTests.test_merged_rows_with_stale_summary_are_rejected': 4,
    'AcceptanceMatrixTests.test_missing_lock_creation_failures_use_fallback_only_for_readonly_metadata': 4,
    'AcceptanceMatrixTests.test_native_publication_then_interrupt_restores_every_staged_destination': 15,
    'AcceptanceMatrixTests.test_native_recovery_interrupt_keeps_independent_original_copies_until_verification': 6,
    'AcceptanceMatrixTests.test_new_document_modes_follow_umask_and_receipts_stay_private': 12,
    'AcceptanceMatrixTests.test_nonrefresh_writers_reject_cli_and_referenced_input_alias_retargeting': 18,
    'AcceptanceMatrixTests.test_nonstandard_output_cannot_alias_generated_document': 4,
    'AcceptanceMatrixTests.test_optional_outputs_cannot_overwrite_inputs_or_each_other': 18,
    'AcceptanceMatrixTests.test_optional_outputs_cannot_overwrite_inputs_or_each_other_case_1': 18,
    'AcceptanceMatrixTests.test_output_staging_collision_never_changes_or_removes_existing_files': 2,
    'AcceptanceMatrixTests.test_overlapping_refresh_is_refused_without_stealing_live_lock': 2,
    'AcceptanceMatrixTests.test_parent_and_checkout_alias_retargeting_rolls_back_actual_destinations': 4,
    'AcceptanceMatrixTests.test_read_lock_failures_do_not_read_inputs': 5,
    'AcceptanceMatrixTests.test_reader_creation_closes_descriptor_when_handle_construction_fails': 2,
    'AcceptanceMatrixTests.test_readers_refuse_mixed_publication_then_accept_completed_refresh': 6,
    'AcceptanceMatrixTests.test_readers_share_the_lock_and_writers_wait': 2,
    'AcceptanceMatrixTests.test_readonly_checkout_detects_first_writer_and_discards_speculative_results': 9,
    'AcceptanceMatrixTests.test_readonly_modes_do_not_expand_publication_inputs': 7,
    'AcceptanceMatrixTests.test_receipt_rollback_guards_existing_and_created_outputs_for_every_writer': 14,
    'AcceptanceMatrixTests.test_recovery_candidate_failures_keep_original_copies_and_publication_unchanged': 13,
    'AcceptanceMatrixTests.test_refresh_aliased_inputs_and_standard_document_share_one_identity': 2,
    'AcceptanceMatrixTests.test_refresh_ambiguous_and_duplicate_accounting_is_never_guessed': 2,
    'AcceptanceMatrixTests.test_refresh_and_validation_refuse_unhashable_status_and_evidence_kind': 10,
    'AcceptanceMatrixTests.test_refresh_audited_historical_exclusion_follows_only_unique_exact_relocation': 7,
    'AcceptanceMatrixTests.test_refresh_bytes_do_not_depend_on_entry_row_or_object_key_order': 2,
    'AcceptanceMatrixTests.test_refresh_consistent_crlf_doc_and_failed_doc_replacement': 2,
    'AcceptanceMatrixTests.test_refresh_cross_document_move_and_aggregate_source_decision': 3,
    'AcceptanceMatrixTests.test_refresh_decisions_persist_without_any_writes_alongside_a_move': 8,
    'AcceptanceMatrixTests.test_refresh_duplicate_new_occurrences_get_distinct_editable_fragments': 3,
    'AcceptanceMatrixTests.test_refresh_empty_inventory_or_documents_remains_incomplete': 2,
    'AcceptanceMatrixTests.test_refresh_invalid_sources_use_validation_messages_before_any_relocation': 14,
    'AcceptanceMatrixTests.test_refresh_invalid_sources_use_validation_messages_before_any_relocation_case_1': 14,
    'AcceptanceMatrixTests.test_refresh_inventory_only_move_canonicalizes_matrix_object_order': 2,
    'AcceptanceMatrixTests.test_refresh_is_idempotent_and_consistent_tree_is_byte_and_mtime_noop': 3,
    'AcceptanceMatrixTests.test_refresh_lock_location_and_failures_are_private_and_clear': 11,
    'AcceptanceMatrixTests.test_refresh_malformed_metadata_is_rejected_without_writes': 6,
    'AcceptanceMatrixTests.test_refresh_missing_unreadable_or_partial_contract_never_writes': 4,
    'AcceptanceMatrixTests.test_refresh_moves_statement_and_clears_only_derived_inventory_error': 2,
    'AcceptanceMatrixTests.test_refresh_new_reworded_removed_and_punctuation_changes_need_decisions': 4,
    'AcceptanceMatrixTests.test_refresh_prepares_optional_doc_and_receipt_before_writes_and_retries': 11,
    'AcceptanceMatrixTests.test_refresh_preserves_file_permissions_and_cleans_readonly_replacement_failure': 2,
    'AcceptanceMatrixTests.test_refresh_preserves_unknown_entry_metadata_and_rejects_invalid_row_lists': 7,
    'AcceptanceMatrixTests.test_refresh_refuses_colliding_output_paths_without_writes': 3,
    'AcceptanceMatrixTests.test_refresh_reviewed_normative_override_retains_current_behaviour_context': 2,
    'AcceptanceMatrixTests.test_refresh_rolls_back_every_failed_replacement_including_windows_locks': 9,
    'AcceptanceMatrixTests.test_refresh_source_changes_before_or_during_publication_are_rolled_back': 18,
    'AcceptanceMatrixTests.test_refresh_stages_all_outputs_before_replacement_and_retries_partial_data': 16,
    'AcceptanceMatrixTests.test_refresh_stages_all_outputs_before_replacement_and_retries_partial_data_case_1': 16,
    'AcceptanceMatrixTests.test_refresh_unreadable_output_prevents_every_replacement': 5,
    'AcceptanceMatrixTests.test_restoration_journal_rejects_invalid_records_and_unreadable_or_partial_candidates': 11,
    'AcceptanceMatrixTests.test_restore_candidate_substitution_is_never_adopted_after_native_success': 2,
    'AcceptanceMatrixTests.test_restore_interrupt_before_native_operation_retains_copies_for_recovery': 2,
    'AcceptanceMatrixTests.test_restored_set_is_reverified_before_discarding_any_recovery_copy': 2,
    'AcceptanceMatrixTests.test_rollback_faults_retain_all_copies_and_continue_other_restores': 19,
    'AcceptanceMatrixTests.test_same_byte_input_hardlink_rebindings_are_refused': 8,
    'AcceptanceMatrixTests.test_snapshot_inputs_and_outputs_use_the_bounded_regular_file_reader': 8,
    'AcceptanceMatrixTests.test_snapshot_unknown_or_unreadable_name_identity_refuses_without_publication': 2,
    'AcceptanceMatrixTests.test_speculative_diagnostics_are_bounded_and_overflow_refuses_success': 2,
    'AcceptanceMatrixTests.test_validation_creates_missing_lock_and_uses_existing_lock_read_only': 3,
    'AcceptanceMatrixTests.test_write_doc_then_check_doc_passes': 2,
    'AcceptanceRecoveryTests.test_default_document_symlinks_and_output_hardlinks_are_refused': 5,
    'AcceptanceRecoveryTests.test_journal_write_flush_and_publication_failures_preserve_outputs': 8,
    'AcceptanceRecoveryTests.test_pending_recovery_exclusive_lock_still_obeys_deadline': 2,
    'AcceptanceRecoveryTests.test_readonly_first_writer_journal_is_recovered_or_refused_before_retry_inputs': 4,
    'AcceptanceRecoveryTests.test_recovery_faults_preserve_files_and_retry_without_consuming_inputs': 18,
    'AcceptanceRecoveryTests.test_recovery_rejects_damaged_or_unknown_journal_before_any_input_read': 11,
    'AcceptanceRecoveryTests.test_recovery_rejects_damaged_or_unknown_journal_before_any_input_read_case_1': 11,
    'AcceptanceRecoveryTests.test_recovery_rejects_damaged_or_unknown_journal_before_any_input_read_case_2': 11,
    'AcceptanceRecoveryTests.test_refresh_defaults_and_optional_outputs_reject_all_input_aliases': 21,
    'AcceptanceRecoveryTests.test_refresh_defaults_and_optional_outputs_reject_all_input_aliases_case_1': 14,
    'AcceptanceRecoveryTests.test_refresh_defaults_and_optional_outputs_reject_all_input_aliases_case_2': 14,
    'AcceptanceRecoveryTests.test_static_validation_reads_source_archive_without_git_or_any_writes': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_1': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_10': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_11': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_12': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_13': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_2': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_3': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_4': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_5': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_6': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_7': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_8': 2,
    'AcceptanceRecoveryTests.test_termination_at_every_publication_boundary_recovers_before_reading_case_9': 2,
    'AcceptanceRecoveryTests.test_transaction_bounds_and_special_files_fail_before_publication': 4,
    'AcceptanceSelectionTests.test_direct_preparation_is_detected_in_every_output_parent': 2,
    'AcceptanceSelectionTests.test_first_marker_failures_are_closed_and_preserve_all_outputs': 12,
    'AcceptanceSelectionTests.test_foreign_preparation_aliases_preserve_the_actual_foreign_file': 2,
    'AcceptanceSelectionTests.test_foreign_preparation_and_pending_occupants_are_never_opened_or_modified': 16,
    'AcceptanceSelectionTests.test_live_rollback_and_cleanup_refuse_entire_linked_set': 16,
    'AcceptanceSelectionTests.test_native_creation_refusals': 18,
    'AcceptanceSelectionTests.test_native_creation_refusals_case_1': 16,
    'AcceptanceSelectionTests.test_native_creation_refusals_case_2': 14,
    'AcceptanceSelectionTests.test_native_creation_refusals_case_3': 18,
    'AcceptanceSelectionTests.test_native_creation_refusals_case_4': 16,
    'AcceptanceSelectionTests.test_native_creation_refusals_case_5': 14,
    'AcceptanceSelectionTests.test_native_creation_retries': 17,
    'AcceptanceSelectionTests.test_native_creation_retries_case_1': 9,
    'AcceptanceSelectionTests.test_preparation_and_pending_replacements_preserve_foreign_identity_and_bytes': 4,
    'AcceptanceSelectionTests.test_preparation_in_place_edits_and_added_aliases_are_preserved': 2,
    'AcceptanceSelectionTests.test_restart_refuses_linked_transaction_members_before_any_mutation': 24,
    'AcceptanceSelectionTests.test_restart_refuses_linked_transaction_members_before_any_mutation_case_1': 24,
    'AcceptanceSelectionTests.test_staged_chmod_refuses_link_added_after_payload_close': 2,
    'ProductionAcceptanceMatrixTests.test_production_inputs_allow_refresh_and_standard_document_write': 2,
}


def _register_child_cases(owner, name, check, values, children):
    for index, arguments in enumerate(values):
        selected = name + ("_case_" + str(index) if index else "")
        case = _child_case(check, arguments)
        case.__name__ = selected
        case._contract_origin = name
        case._contract_check = check.__name__
        setattr(owner, selected, case)
        _CHILD_ALLOWANCES[owner.__name__ + "." + selected] = children


_register_child_cases(AcceptanceCustodyTests, 'test_enrollment_crash_before_receipt_delivery_is_recoverable',
                      AcceptanceCustodyTests._check_enrollment_crash_before_receipt_delivery_is_recoverable,
                      tuple((mode, boundary) for mode in ("enroll", "write", "legacy")
                            for boundary in ("before", "after", "delivery")), 1)

_register_child_cases(AcceptanceRecoveryTests, 'test_retargeted_inventory_alias_recovers_the_actual_publication_destinations',
                      AcceptanceRecoveryTests._check_retargeted_inventory_alias_recovers_the_actual_publication_destinations,
                      ((False,), (True,)), 1)
_register_child_cases(AcceptanceRecoveryTests, 'test_termination_at_every_publication_boundary_recovers_before_reading',
                      AcceptanceRecoveryTests._check_termination_at_every_publication_boundary_recovers_before_reading,
                      ((False, False, 1), (False, False, 2), (False, True, 1), (False, True, 2), (True, False, 1), (True, False, 2), (True, False, 3), (True, False, 4), (True, False, 5), (True, True, 1), (True, True, 2), (True, True, 3), (True, True, 4), (True, True, 5)), 1)
_register_child_cases(AcceptanceRecoveryTests, 'test_recovery_can_itself_be_terminated_between_restores',
                      AcceptanceRecoveryTests._check_recovery_can_itself_be_terminated_between_restores,
                      ((1,), (2,), (3,)), 1)
_register_child_cases(AcceptanceCustodyTests, 'test_restart_reacquires_enrolled_partial_output_and_pending_files',
                      AcceptanceCustodyTests._check_restart_reacquires_enrolled_partial_output_and_pending_files,
                      (('output',), ('payload',), ('pending',), ('publication',), ('commit',)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_initial_backup_creation_is_discoverable_on_both_sides',
                      AcceptanceSelectionTests._check_initial_backup_creation_is_discoverable_on_both_sides,
                      ((False,), (True,)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_initial_candidate_creation_is_discoverable_on_both_sides',
                      AcceptanceSelectionTests._check_initial_candidate_creation_is_discoverable_on_both_sides,
                      ((False,), (True,)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_initial_payload_buffer_boundaries_are_discoverable',
                      AcceptanceSelectionTests._check_initial_payload_buffer_boundaries_are_discoverable,
                      (('write', False), ('write', True), ('flush', False), ('flush', True), ('close', False), ('close', True), ('sync', False), ('sync', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_initial_candidate_binding_and_partial_writes_are_discoverable',
                      AcceptanceSelectionTests._check_initial_candidate_binding_and_partial_writes_are_discoverable,
                      (('bind', False), ('bind', True), ('partial', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_initial_candidate_buffer_boundaries_are_discoverable',
                      AcceptanceSelectionTests._check_initial_candidate_buffer_boundaries_are_discoverable,
                      (('write', False), ('write', True), ('flush', False), ('flush', True), ('close', False), ('close', True), ('sync', False), ('sync', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_initial_metadata_and_publication_bookkeeping_are_restartable',
                      AcceptanceSelectionTests._check_initial_metadata_and_publication_bookkeeping_are_restartable,
                      (('metadata', False), ('metadata', True), ('rename', False), ('rename', True), ('publication', False), ('publication', True), ('handoff', False), ('handoff', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_rollback_creation_and_binding_are_discoverable',
                      AcceptanceSelectionTests._check_rollback_creation_and_binding_are_discoverable,
                      (('create', False), ('create', True), ('bind', False), ('bind', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_rollback_payload_buffer_boundaries_are_discoverable',
                      AcceptanceSelectionTests._check_rollback_payload_buffer_boundaries_are_discoverable,
                      (('write', False), ('write', True), ('flush', False), ('flush', True), ('close', False), ('close', True), ('sync', False), ('sync', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_rollback_partial_writes_and_native_bookkeeping_are_discoverable',
                      AcceptanceSelectionTests._check_rollback_partial_writes_and_native_bookkeeping_are_discoverable,
                      (('partial', True), ('rename', False), ('rename', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_rollback_marker_release_preserves_restart_discovery',
                      AcceptanceSelectionTests._check_rollback_marker_release_preserves_restart_discovery,
                      ((False,), (True,)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_restart_creation_and_binding_are_discoverable',
                      AcceptanceSelectionTests._check_restart_creation_and_binding_are_discoverable,
                      (('create', False), ('create', True), ('bind', False), ('bind', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_restart_payload_buffer_boundaries_are_discoverable',
                      AcceptanceSelectionTests._check_restart_payload_buffer_boundaries_are_discoverable,
                      (('write', False), ('write', True), ('flush', False), ('flush', True), ('close', False), ('close', True), ('sync', False), ('sync', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_restart_metadata_and_native_bookkeeping_are_restartable',
                      AcceptanceSelectionTests._check_restart_metadata_and_native_bookkeeping_are_restartable,
                      (('metadata', False), ('metadata', True), ('rename', False), ('rename', True), ('handoff', False), ('handoff', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_pending_journal_creation_and_binding_are_discoverable',
                      AcceptanceSelectionTests._check_pending_journal_creation_and_binding_are_discoverable,
                      (('create', False), ('create', True), ('bind', False), ('bind', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_pending_journal_buffer_boundaries_are_discoverable',
                      AcceptanceSelectionTests._check_pending_journal_buffer_boundaries_are_discoverable,
                      (('write', False), ('write', True), ('flush', False), ('flush', True), ('close', False), ('close', True), ('sync', False), ('sync', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_owned_interrupted_journal_native_publication_recovers_successfully',
                      AcceptanceSelectionTests._check_owned_interrupted_journal_native_publication_recovers_successfully,
                      ((False,), (True,)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_first_marker_creation_is_discoverable_without_changing_outputs',
                      AcceptanceSelectionTests._check_first_marker_creation_is_discoverable_without_changing_outputs,
                      ((False,), (True,)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_first_marker_buffer_boundaries_are_discoverable_without_changing_outputs',
                      AcceptanceSelectionTests._check_first_marker_buffer_boundaries_are_discoverable_without_changing_outputs,
                      (('write', False), ('write', True), ('flush', False), ('flush', True), ('close', False), ('close', True), ('sync', False), ('sync', True), ('partial', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_first_marker_binding_is_discoverable_without_changing_outputs',
                      AcceptanceSelectionTests._check_first_marker_binding_is_discoverable_without_changing_outputs,
                      ((False,), (True,)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_direct_preparation_creation_and_binding_are_discoverable',
                      AcceptanceSelectionTests._check_direct_preparation_creation_and_binding_are_discoverable,
                      (('create', False), ('create', True), ('bind', False), ('bind', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_direct_preparation_buffer_boundaries_are_discoverable',
                      AcceptanceSelectionTests._check_direct_preparation_buffer_boundaries_are_discoverable,
                      (('write', False), ('write', True), ('flush', False), ('flush', True), ('close', False), ('close', True), ('sync', False), ('sync', True)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_direct_preparation_native_publication_is_discoverable',
                      AcceptanceSelectionTests._check_direct_preparation_native_publication_is_discoverable,
                      ((False,), (True,)), 2)
_register_child_cases(AcceptanceSelectionTests, 'test_partition_entrypoint_refuses_invalid_selection',
                      AcceptanceSelectionTests._check_partition_entrypoint_refuses_invalid_selection,
                      (('-1',), (str(_CONTRACT_PARTITIONS),), ('unknown',)), 1)

_register_child_cases(AcceptanceSelectionTests, 'test_full_output_set_native_publication_is_discoverable',
                      AcceptanceSelectionTests._check_full_output_set_native_publication_is_discoverable,
                      (('direct', 'publication'), ('original', 'publication'), ('rollback', 'rename'),
                       ('recovery', 'rename'), ('pending', 'rename'), ('marker', 'bind')), 2)

def _register_storage_cases(owner, name, values):
    check = getattr(owner, "_check_" + name.removeprefix("test_"))
    for index, arguments in enumerate(values):
        selected = name + ("_case_" + str(index) if index else "")
        case = _child_case(check, arguments)
        case.__name__ = selected
        case._contract_origin = name
        case._contract_check = check.__name__
        setattr(owner, selected, case)


_register_storage_cases(AcceptanceCustodyTests, 'test_legacy_recovery_shares_custody_through_publication',
                        tuple((mode,) for mode in ("write", "refresh", "execution", "validate", "junit", "unchanged")))
_register_storage_cases(AcceptanceCustodyTests, 'test_legacy_recovery_validation_failure_retires_custody',
                        tuple((failure,) for failure in ("boundary", "unreadable", "partial", "empty", "unknown")))

_register_storage_cases(AcceptanceMatrixTests,
    "test_cli_alias_retargeting_refuses_before_and_after_every_publication",
    tuple((boundary,) for boundary in ("loaded", "staging", 1, 2, 3, 4, 5)))
_register_storage_cases(AcceptanceMatrixTests,
    "test_optional_outputs_cannot_overwrite_inputs_or_each_other", ((False,), (True,)))
_register_storage_cases(AcceptanceMatrixTests,
    "test_refresh_invalid_sources_use_validation_messages_before_any_relocation", (("iss.126",), ("planned.doc",)))
_register_storage_cases(AcceptanceRecoveryTests,
    "test_recovery_rejects_damaged_or_unknown_journal_before_any_input_read", ((0,), (1,), (2,)))
_register_storage_cases(AcceptanceRecoveryTests,
    "test_refresh_defaults_and_optional_outputs_reject_all_input_aliases", (("doc",), ("inventory",), ("matrix",)))
_register_storage_cases(AcceptanceSelectionTests,
    "test_restart_refuses_linked_transaction_members_before_any_mutation", ((False,), (True,)))


_register_storage_cases(AcceptanceMatrixTests,
    "test_refresh_stages_all_outputs_before_replacement_and_retries_partial_data", (("create",), ("partial",)))
_register_storage_cases(AcceptanceSelectionTests,
    "test_native_creation_retries", ((False,), (True,)))
_register_storage_cases(AcceptanceSelectionTests,
    "test_native_creation_refusals", tuple((recovery, batch) for recovery in (False, True) for batch in range(3)))


_CHILD_ALLOWANCES.update({
    'AcceptanceMatrixTests.test_crashed_holder_releases_lock_without_removing_persistent_file': 2,
    'AcceptanceMatrixTests.test_external_edits_invalidate_path_discovery_and_evidence_answers': 1,
    'AcceptanceMatrixTests.test_overlapping_refresh_is_refused_without_stealing_live_lock': 2,
    'AcceptanceMatrixTests.test_refresh_lock_location_and_failures_are_private_and_clear': 1,
    'AcceptanceRecoveryTests.test_crash_fixture_matches_terminated_publication': 1,
    'AcceptanceSelectionTests.test_interruption_diagnostics_bound_stderr_and_preserve_selected_exit': 2,
    'AcceptanceSelectionTests.test_interruption_stack_diagnostics_are_bounded': 1,
    'AcceptanceSelectionTests.test_interruption_timeout_retains_exception_and_captured_stack': 1,
    'AcceptanceSelectionTests.test_interruption_watchdog_is_cancelled_on_normal_exit': 1,
    'AcceptanceSelectionTests.test_pending_journal_partial_writes_are_discoverable': 2,
    'AcceptanceSelectionTests.test_artifact_discovery_requires_complete_json_path_tokens': 2,
    'AcceptanceSelectionTests.test_restart_partial_writes_are_discoverable': 2,
})

# A paused holder reserves startup/completion plus kill/drain deadlines.
# Launches are independently bounded by deadlines and by slow-storage units.
_CONTRACT_CASE_PARTITIONS.update({name: _LOCAL_PARTITIONS + index
                                  for index, name in enumerate(sorted(_CHILD_ALLOWANCES))})
_local_load = [0] * _LOCAL_PARTITIONS
_local_cases = [owner.__name__ + "." + name
                for owner in (*_MATRIX_CLASSES, AcceptanceSelectionTests, AcceptanceRecoveryTests, AcceptanceCustodyTests)
                for name in unittest.TestLoader().getTestCaseNames(owner)
                if owner.__name__ + "." + name not in _CHILD_ALLOWANCES]
for _case in sorted(_local_cases, key=lambda name: (-_STORAGE_ALLOWANCES.get(name, 1), name)):
    _partition = min(range(_LOCAL_PARTITIONS), key=lambda number: (_local_load[number], number))
    _CONTRACT_CASE_PARTITIONS[_case] = _partition
    _local_load[_partition] += _STORAGE_ALLOWANCES.get(_case, 1)

_phase("module-ready")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--contract-partition":
        try:
            suite = contract_suite(int(sys.argv[2]))
        except ValueError as error:
            raise SystemExit(str(error)) from error
        raise SystemExit(not unittest.TextTestRunner().run(suite).wasSuccessful())
    _PhaseProgram()
