#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import copy
import ctypes
import errno
import importlib.util
import io
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
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


class AcceptanceFixture(unittest.TestCase):
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
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name).resolve()
        self.root = self.tmp / "repo"
        self.root.mkdir()
        for relative in self.fixture_directories:
            (self.root / relative).mkdir()
        for relative, data in self.fixture_files:
            (self.root / relative).write_bytes(data)
        self.inventory = copy.deepcopy(self.fixture_inventory)
        self.index = self.fixture_index
        self.matrix_path = self.tmp / "matrix.json"
        # Logical fault/interleaving tests exercise flush ordering and injected
        # sync failures. Physical disk barriers are qualified separately below
        # and in every actual process-termination case, not repeated per fault.
        sync_patch = mock.patch.object(os, "fsync", autospec=True)
        self.sync = sync_patch.start()
        self.addCleanup(sync_patch.stop)

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

    def prepare_refresh(self) -> dict[str, Any]:
        matrix = baseline_matrix()
        matrix["rows"][0]["requirements"].remove("ar.synthetic")
        matrix["rows"].append({
            "id": "iss.126", "source": MODULE.CONTRACT_DOCUMENTS[1] + "#contract",
            "statement": "Every requirement has evidence.", "status": "covered",
            "requirements": ["ar.synthetic"],
            "evidence": [{"kind": "script", "ref": "tools/script.py"}],
        })
        self.write_matrix(matrix)
        doc = self.root / "docs/acceptance-matrix.md"
        doc.write_bytes(MODULE.render_doc(matrix).encode("utf-8"))
        return matrix

    def prepare_rollback(self) -> tuple[tuple[Path, ...], dict[Path, bytes]]:
        self.write_inventory()
        self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        paths = (self.root / "docs/acceptance-matrix.md", self.matrix_path,
                 self.root / "tools/ci/acceptance_requirements.json")
        return paths, {path: path.read_bytes() for path in paths}

    def crash_publication(self, count: int, options: list[str]) -> None:
        program = """
import os, pathlib, runpy, sys
module = runpy.run_path(sys.argv[1])["main"].__globals__
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
                               capture_output=True, text=True, timeout=5)
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
import pathlib, runpy, sys
module = runpy.run_path(sys.argv[1])["main"].__globals__
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
raise SystemExit(module["main"](["--root", sys.argv[2], "--matrix", sys.argv[3], "--refresh-inventory"]))
"""
        child = subprocess.Popen([sys.executable, "-I", "-B", "-c", program, str(MODULE_PATH),
                                  str(self.root), str(self.matrix_path), str(ready),
                                  "publish" if during_publication else "prepare"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        def cleanup() -> None:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)
        self.addCleanup(cleanup)
        deadline = time.monotonic() + 5
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
        out, err = child.communicate("x", timeout=5)
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
        child.communicate(timeout=5)
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
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True, capture_output=True)
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

    def test_optional_outputs_cannot_overwrite_inputs_or_each_other(self) -> None:
        self.prepare_refresh()
        discovery, report = self.execution_inputs()
        for refresh in (False, True):
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
            code, _, err = self.run_main(*args)
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

    def test_refresh_aliased_inputs_and_standard_document_share_one_identity(self) -> None:
        self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        doc = self.root / "docs/acceptance-matrix.md"
        args = ["--root", str(self.root / "test/.."),
                "--matrix", str(self.root / ".." / self.matrix_path.name),
                "--write-doc", str(self.root / "docs/../docs/acceptance-matrix.md"),
                "--refresh-inventory", "--check-doc", str(doc)]
        code, _, err = self.run_main(*args)
        self.assertEqual(code, 0, err)
        matrix = MODULE.load_matrix(self.matrix_path)
        self.assertEqual(row_of(matrix, "iss.126")["source"], MODULE.CONTRACT_DOCUMENTS[1] + "#moved")
        self.assertEqual(self.errors(matrix), [])
        self.assertEqual(doc.read_text(encoding="utf-8"), MODULE.render_doc(matrix))
        before = doc.read_bytes(), doc.stat().st_mtime_ns
        self.assertEqual(self.run_main(*args)[0], 0)
        self.assertEqual((doc.read_bytes(), doc.stat().st_mtime_ns), before)

    def test_default_output_destinations_use_resolved_paths_for_publish_and_rollback(self) -> None:
        self.matrix_path = self.root / "tools/ci/acceptance_matrix.json"
        resolve, replace = Path.resolve, Path.replace
        for name in ("document", "matrix", "inventory"):
            for fail in (False, True):
                with self.subTest(output=name, fail=fail):
                    paths, before = self.prepare_rollback()
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
                            code, _, err = self.run_main("--root", str(self.root), "--refresh-inventory")
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
                read = Path.read_bytes
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
                def unreadable(file: Path) -> bytes:
                    if replaced and file == source and failure == "unreadable":
                        raise PermissionError("commit input unreadable")
                    return read(file)
                try:
                    with mock.patch.object(Path, "replace", replace_then_change), \
                            mock.patch.object(Path, "read_bytes", unreadable):
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
                def faulted_bounded_read(file: Path, limit: int) -> Any:
                    data, file_metadata = bounded_read(file, limit)
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
                    code, _, err = self.run_main(*args)
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

    def test_refresh_invalid_sources_use_validation_messages_before_any_relocation(self) -> None:
        matrix = self.prepare_refresh()
        (self.root / MODULE.CONTRACT_DOCUMENTS[1]).write_text(
            "# Moved\nEvery requirement has evidence.\n", encoding="utf-8")
        for row_id in ("iss.126", "planned.doc"):
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

    def test_refresh_stages_all_outputs_before_replacement_and_retries_partial_data(self) -> None:
        create = open
        for failure, fail_at in ((failure, stage) for failure in ("create", "partial")
                                 for stage in range(1, 9)):
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
                    code, _, err = self.run_main(*args)
                self.assertEqual(code, 2, err)
                self.assertIn("partial staged output" if failure == "partial"
                              else "staged output creation denied", err)
                replace.assert_not_called()
                self.assertEqual([path.read_bytes() for path in paths], before)
                self.assertFalse(extra_doc.exists())
                self.assertFalse(receipt.exists())
                self.assertEqual(set(self.tmp.rglob("*")), before_paths)
                code, _, err = self.run_main(*args)
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
        read = Path.read_bytes
        for blocked in paths:
            with self.subTest(path=blocked):
                def denied(file: Path) -> bytes:
                    if file == blocked:
                        raise PermissionError("output read denied")
                    return read(file)
                with mock.patch.object(Path, "read_bytes", denied), mock.patch.object(Path, "replace") as replace:
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
            code, _, err = self.run_main(*args)
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
            code, _, err = self.run_main(*args)
        self.assertEqual(code, 2, err)
        self.assertEqual(paths[0].read_bytes(), before[0])
        self.assertFalse(receipt.exists())
        self.assertFalse(extra_doc.exists())
        self.assertEqual([path.read_bytes() for path in paths], before)
        code, _, err = self.run_main(*args)
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
            code, _, err = self.run_main(*args)
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
        with mock.patch.object(MODULE.secrets, "token_hex", return_value="a" * 32), \
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
                        code, _, err = self.run_main(*args)
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
                    code, _, err = self.run_main(*args)
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
        self.assertIn("owner: ds", row["evidence"][0]["planned"])
        self.assertIn("accept -2^24", row["evidence"][0]["planned"])
        self.assertIn("reject +2^24", row["evidence"][0]["planned"])
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
    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        # One genuinely terminated writer supplies the reusable crash image.
        # Each use gets fresh file identities; no subprocess is needed to test
        # journal damage or recovery refusal against the same published bytes.
        seed = cls("runTest")
        try:
            seed.setUp()
            paths, _ = seed.prepare_rollback()
            seed.crash_publication(3, ["--refresh-inventory"])
            journal = MODULE._refresh_lock_path(seed.root).with_suffix(".transaction")
            entries = json.loads(journal.read_bytes())["outputs"]
            cls.crash_image = tuple((paths.index(Path(entry["path"])),
                                     Path(entry["backup"]["path"]).read_bytes(),
                                     Path(entry["path"]).read_bytes()) for entry in entries)
        finally:
            seed.doCleanups()

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
        self.assertEqual(len(synced), 8)  # Three payload/backup pairs, two journals.

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

    def test_termination_at_every_publication_boundary_recovers_before_reading(self) -> None:
        for refresh in (False, True):
            for existing in (False, True):
                for count in range(1, 6 if refresh else 3):
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

    def test_recovery_rejects_damaged_or_unknown_journal_before_any_input_read(self) -> None:
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

    def test_recovery_can_itself_be_terminated_between_restores(self) -> None:
        program = """
import os, pathlib, runpy, sys
module = runpy.run_path(sys.argv[1])["main"].__globals__
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
        for count in (1, 2, 3):
            with self.subTest(count=count):
                paths, before = self.prepare_rollback()
                self.interrupted_refresh(3)
                child = subprocess.run([sys.executable, "-I", "-B", "-c", program,
                                        str(MODULE_PATH), str(count), *self.base_args()],
                                       capture_output=True, text=True, timeout=5)
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
                unknown_reported = False
                def faulted_fdopen(descriptor: int, mode: str) -> Any:
                    handle = fdopen(descriptor, mode)
                    if mode == "wb" and failure in ("write", "flush"):
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
                    is_journal = candidate.exists()
                    if is_journal:
                        journal_syncs += 1
                    if (failure == "payload-sync" or (is_journal and (
                            (failure == "sync" and journal_syncs == 1)
                            or (failure == "commit-sync" and journal_syncs == 2)))):
                        raise OSError("sync denied")
                    sync(descriptor)
                def unknown_candidate(handle: Any) -> Any:
                    nonlocal unknown_reported
                    result = metadata(handle)
                    if failure == "candidate-identity" and candidate.exists() and not unknown_reported:
                        unknown_reported = True
                        fields = list(result)
                        fields[1] = 0
                        return os.stat_result(fields)
                    return result
                with mock.patch.object(os, "fdopen", faulted_fdopen), \
                        mock.patch.object(os, "replace", faulted_replace), \
                        mock.patch.object(os, "fsync", faulted_sync), \
                        mock.patch.object(MODULE, "_transaction_metadata", unknown_candidate):
                    code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
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

    def test_refresh_defaults_and_optional_outputs_reject_all_input_aliases(self) -> None:
        self.prepare_refresh()
        doc = self.root / "docs/acceptance-matrix.md"
        inventory = self.root / "tools/ci/acceptance_requirements.json"
        discovery, report = self.execution_inputs()
        for destination in (doc, inventory, self.matrix_path):
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
        self.prepare_rollback()
        journal = MODULE._refresh_lock_path(self.root).with_suffix(".transaction")
        pending = journal.with_suffix(".pending")
        pending.write_bytes(b"{partial unpublished metadata")
        code, _, err = self.run_main(*self.base_args(), "--refresh-inventory")
        self.assertEqual(code, 0, err)
        self.assertFalse(pending.exists())
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
