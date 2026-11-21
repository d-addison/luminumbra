#!/usr/bin/env python3
"""Deterministic ratchet policy, adapter, refusal and repository regressions."""

import contextlib
import copy
import ctypes
import fnmatch
import importlib.util
import io
import json
import ntpath
import os
from pathlib import Path, PureWindowsPath
import re
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ratchet", Path(__file__).with_name("ratchet.py"))
ratchet = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ratchet)


class MemoryTree:
    def __init__(self, files):
        self.files = files
        self.paths = set(files)

    def read(self, path):
        if path not in self.paths:
            raise ratchet.AnalysisError(f"missing input: {path}")
        return self.files[path]

    def json(self, path):
        return json.loads(self.read(path))


def fixture(source="void Pass::execute() { glGetError(); }", rule_id="query"):
    rule = dict(id=rule_id, family="performance", description="Queries", rationale="Driver work",
                scope="runtime", auto_fix="Review", adapter="source-pattern", anchor="call",
                pattern=r"\bglGet\w*\s*\(")
    files = {
        ratchet.REGISTRY: ratchet.canonical(dict(schema=1, rules=[rule])),
        f"{ratchet.CONFIG}/scopes/runtime.json": ratchet.canonical(dict(schema=1, groups={rule_id: [
            dict(files=["src/pass.cpp"], symbols=["Pass::execute"], frequency="frame")]})),
        "src/pass.cpp": source,
    }
    tree = MemoryTree(files)
    found = ratchet.analyze(tree, ratchet.registry(tree), ratchet.scopes(tree, ratchet.registry(tree)))
    files[f"{ratchet.CONFIG}/baselines/performance/runtime.json"] = ratchet.canonical(
        dict(schema=1, entries=sorted(found.values(), key=lambda item: item["key"])))
    return MemoryTree(files)


def edit_json(tree, path, operation):
    value = tree.json(path)
    operation(value)
    tree.files[path] = ratchet.canonical(value)


BASELINE = f"{ratchet.CONFIG}/baselines/performance/runtime.json"
SCOPE = f"{ratchet.CONFIG}/scopes/runtime.json"


class EngineTests(unittest.TestCase):
    def test_subset_and_stale_allowances(self):
        target = fixture()
        current = copy.deepcopy(target)
        current.files["src/pass.cpp"] = "void Pass::execute() {}"
        with self.assertRaisesRegex(ratchet.AnalysisError, "stale baseline"):
            ratchet.evaluate(current, target)
        self.assertEqual(ratchet.evaluate(current, target, updating=True)[2], [])
        edit_json(current, BASELINE, lambda data: data.update(entries=[]))
        self.assertEqual(ratchet.evaluate(current, target)[2], [])

    def test_removed_site_does_not_buy_another(self):
        target = fixture()
        current = fixture("void Pass::execute() { glGetIntegerv(GL_VIEWPORT, &value); }")
        _, _, failures = ratchet.evaluate(current, target)
        self.assertTrue(failures)
        self.assertEqual({item["construct"] for item in failures}, {"glGetIntegerv ( GL_VIEWPORT , & value )"})

    def test_multiplicity_cannot_increase(self):
        target = fixture()
        current = fixture("void Pass::execute() { glGetError(); glGetError(); }")
        failures = ratchet.evaluate(current, target)[2]
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["count"], 1)

    def test_new_rule_seeds_once_and_only_current_findings(self):
        current = fixture()
        self.assertEqual(ratchet.evaluate(current, MemoryTree({}))[2], [])
        edit_json(current, BASELINE, lambda data: data["entries"][0].update(count=2))
        self.assertTrue(ratchet.evaluate(current, MemoryTree({}))[2])
        self.assertEqual(ratchet.evaluate(current, MemoryTree({}), updating=True)[2], [])

    def test_update_seeds_an_unbaselined_new_rule(self):
        current = fixture()
        edit_json(current, BASELINE, lambda data: data.update(entries=[]))
        self.assertTrue(ratchet.evaluate(current, MemoryTree({}))[2])
        self.assertEqual(ratchet.evaluate(current, MemoryTree({}), updating=True)[2], [])

    def test_new_rule_alongside_existing_registry(self):
        target = fixture(rule_id="original")
        current = copy.deepcopy(target)
        rule = fixture().json(ratchet.REGISTRY)["rules"][0]
        edit_json(current, ratchet.REGISTRY, lambda data: data["rules"].append(rule))
        declaration = fixture().json(SCOPE)["groups"]["query"]
        edit_json(current, SCOPE, lambda data: data["groups"].update(query=declaration))
        seeds = fixture().json(BASELINE)["entries"]
        edit_json(current, BASELINE, lambda data: data["entries"].extend(seeds))
        self.assertEqual(ratchet.evaluate(current, target)[2], [])
        self.assertTrue(ratchet.evaluate(current, current)[2] == [])

    def test_shrink_only_refuses_new_or_restored_entries(self):
        target = fixture("void Pass::execute() {}")
        current = fixture()
        self.assertTrue(ratchet.evaluate(current, target, updating=True)[2])
        target = fixture()
        current = copy.deepcopy(target)
        edit_json(current, BASELINE, lambda data: data.update(entries=[]))
        self.assertTrue(ratchet.evaluate(current, target, updating=True)[2])

    def test_missing_input_and_unresolved_symbol_are_errors(self):
        for mutation in (lambda tree: tree.files.pop("src/pass.cpp"),
                         lambda tree: tree.files.update({"src/pass.cpp": "void Pass::renamed() {}"}),
                         lambda tree: tree.files.update({"src/pass.cpp": "void Pass::execute() {"}),
                         lambda tree: tree.files.update({"src/pass.cpp": 'void Pass::execute() { "oops; }'}),
                         lambda tree: tree.files.update({"src/pass.cpp": "void Pass::execute() {} /*oops"})):
            current = fixture()
            mutation(current)
            # Keep cached path enumeration: both deleted-on-disk and unmatched
            # manifest cases must fail, rather than look like fixes.
            current.paths = set(current.files)
            with self.subTest(files=current.files), self.assertRaises(ratchet.AnalysisError):
                ratchet.evaluate(current, fixture(), updating=True)

    def test_adapter_failure_is_an_error(self):
        current = fixture()
        with patch.object(ratchet, "analyze", side_effect=ratchet.AnalysisError("failed adapter")):
            with self.assertRaisesRegex(ratchet.AnalysisError, "failed adapter"):
                ratchet.evaluate(current, fixture(), updating=True)
        edit_json(current, ratchet.REGISTRY, lambda data: data["rules"][0].update(pattern="glGetError"))
        with self.assertRaisesRegex(ratchet.AnalysisError, "invalid call adapter"):
            ratchet.evaluate(current, MemoryTree({}), updating=True)

    def test_line_comment_and_whitespace_stability(self):
        original = fixture("void Pass::execute() { glGetIntegerv(7, &value); }")
        shifted = fixture("\n// unrelated\nvoid Pass::execute( ) {\n glGetIntegerv /* query */ ( 7 , &value );\n}")
        self.assertEqual(original.json(BASELINE), shifted.json(BASELINE))

    def test_argument_literal_file_symbol_and_rule_are_key_inputs(self):
        original = ratchet.finding("r", "src/p.cpp", "Pass::execute()", 'call("one", 1)')
        variants = [("s", "src/p.cpp", "Pass::execute()", 'call("one", 1)'),
                    ("r", "src/q.cpp", "Pass::execute()", 'call("one", 1)'),
                    ("r", "src/p.cpp", "Pass::other()", 'call("one", 1)'),
                    ("r", "src/p.cpp", "Pass::execute()", 'call("two", 1)'),
                    ("r", "src/p.cpp", "Pass::execute()", 'call("one", 2)')]
        for args in variants:
            self.assertNotEqual(original["key"], ratchet.finding(*args)["key"])

    def test_callee_qualifiers_and_receivers_are_key_inputs(self):
        originals = []
        for callee in ("one::glGetError", "two::glGetError", "device.glGetError", "other->glGetError"):
            entries = fixture("void Pass::execute() { " + callee + "(); }").json(BASELINE)["entries"]
            self.assertEqual(len(entries), 1)
            originals.append(entries[0]["key"])
        self.assertEqual(len(set(originals)), 4)

    def test_masking_braces_raw_strings_nested_lambdas_and_overloads(self):
        source = '''void Pass::execute(int value) {
            // glGetError(); }
            const char* text = R"tag(glGetError(); } ")tag";
            auto work = [] { glGetIntegerv(1, nested(2, 3)); };
        }
        void Pass::execute(float value) { /* glGetError(); } */ glGetError(); }
        void Pass::cold() { glGetError(); }
        '''
        entries = fixture(source).json(BASELINE)["entries"]
        self.assertEqual(len(entries), 2)
        self.assertEqual(len({item["symbol"] for item in entries}), 2)
        self.assertIn("glGetIntegerv ( 1 , nested ( 2 , 3 ) )", {item["construct"] for item in entries})

    def test_inline_class_function(self):
        clean, _, found = ratchet.functions("class Pass { void execute() const { glGetError(); } };")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][0], "Pass::execute")
        self.assertIn("glGetError", clean[found[0][2]:found[0][3]])

    def test_digit_separators_do_not_hide_following_calls(self):
        tree = fixture("void Pass::execute() { int n = 100'000; glGetError(); char c = '}'; }")
        self.assertEqual(sum(item["count"] for item in tree.json(BASELINE)["entries"]), 1)
        raw = 'u8R"tag(a  " b)tag"'
        self.assertEqual(ratchet.normalize(raw), raw)
        source = "// continued \\\n glGetError();\nvoid Pass::execute() { glGetError(); }"
        self.assertEqual(len(fixture(source).json(BASELINE)["entries"]), 1)

    def test_free_functions_namespaces_and_innermost_named_owner(self):
        current = fixture('''namespace one { void run() { glGetError(); } }
            namespace two { void run() { glGetError(); } }
            void Pass::execute() {
                class Local { void run() { glGetError(); } };
            }''')
        edit_json(current, SCOPE, lambda data: data["groups"]["query"][0].update(symbols=["*"]))
        rules = ratchet.registry(current)
        entries = ratchet.analyze(current, rules, ratchet.scopes(current, rules))
        self.assertEqual(len(entries), 3)
        self.assertEqual({item["symbol"] for item in entries.values()}, {"one::run( )", "two::run( )", "Local::run( )"})

    def test_whole_source_scope_covers_empty_files_and_refuses_unowned_matches(self):
        current = fixture()
        edit_json(current, SCOPE, lambda data: data["groups"]["query"][0].update(symbols=["*"]))
        current.files["src/pass.cpp"] = ""
        rules = ratchet.registry(current)
        self.assertEqual(ratchet.analyze(current, rules, ratchet.scopes(current, rules)), {})
        current.files["src/pass.cpp"] = "const int value = glGetError();"
        with self.assertRaisesRegex(ratchet.AnalysisError, "cannot identify enclosing"):
            ratchet.analyze(current, rules, ratchet.scopes(current, rules))

    def test_bad_metadata_unknown_adapter_and_duplicate_entries(self):
        mutations = [
            (ratchet.REGISTRY, lambda data: data["rules"][0].update(adapter="unknown")),
            (ratchet.REGISTRY, lambda data: data["rules"][0].update(pattern="[")),
            (ratchet.REGISTRY, lambda data: data["rules"].append(data["rules"][0])),
            (BASELINE, lambda data: data["entries"].append(data["entries"][0])),
            (BASELINE, lambda data: data["entries"][0].update(count=True)),
            (BASELINE, lambda data: data["entries"][0].update(key="0" * 64)),
            (BASELINE, lambda data: data["entries"][0].update(rule="unknown")),
            (SCOPE, lambda data: data["groups"]["query"][0].update(files=["../escape.cpp"])),
        ]
        for path, operation in mutations:
            with self.subTest(path=path, operation=operation):
                current = fixture()
                edit_json(current, path, operation)
                with self.assertRaises((ratchet.AnalysisError, ValueError)):
                    ratchet.evaluate(current, fixture())

    def test_empty_and_malformed_policy_are_errors(self):
        for path in (ratchet.REGISTRY, SCOPE, BASELINE):
            for value in (None, [], {}, {"schema": True}, {"schema": 1, "groups": []}):
                with self.subTest(path=path, value=value):
                    current = fixture()
                    current.files[path] = ratchet.canonical(value)
                    with self.assertRaises(ratchet.AnalysisError):
                        ratchet.evaluate(current, fixture(), updating=True)

    def test_rule_and_scope_cannot_be_removed_or_weakened(self):
        for field, value in (("pattern", r"never"), ("family", "other"), ("scope", "other")):
            current = fixture()
            edit_json(current, ratchet.REGISTRY, lambda data: data["rules"][0].update({field: value}))
            with self.assertRaises(ratchet.AnalysisError):
                ratchet.evaluate(current, fixture())
        current = fixture()
        edit_json(current, SCOPE, lambda data: data["groups"]["query"][0].update(symbols=["Pass::*"]))
        with self.assertRaisesRegex(ratchet.AnalysisError, "cannot remove existing scope"):
            ratchet.evaluate(current, fixture())

    def test_foreign_or_moved_shards_and_orphan_policy_refuse(self):
        current = fixture()
        current.files[BASELINE.replace("performance", "other")] = current.files.pop(BASELINE)
        current.paths = set(current.files)
        with self.assertRaisesRegex(ratchet.AnalysisError, "unknown baseline"):
            ratchet.evaluate(current, fixture())
        orphan = MemoryTree({BASELINE: ratchet.canonical(dict(schema=1, entries=[]))})
        with self.assertRaisesRegex(ratchet.AnalysisError, "missing registry"):
            ratchet.evaluate(fixture(), orphan)


class GitAndCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ratchet-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git("init", "-q")
        self.install(fixture())
        self.target = self.snapshot()

    def git(self, *args):
        env = dict(os.environ, GIT_AUTHOR_NAME="Fixture", GIT_AUTHOR_EMAIL="fixture@example.invalid",
                   GIT_COMMITTER_NAME="Fixture", GIT_COMMITTER_EMAIL="fixture@example.invalid")
        return subprocess.check_output(["git", *args], cwd=self.root, env=env, text=True).strip()

    def install(self, tree):
        for path, value in tree.files.items():
            file = self.root / path
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(value, encoding="utf-8")

    def snapshot(self):
        self.git("add", ".")
        tree = self.git("write-tree")
        commit = self.git("commit-tree", tree, "-m", "fixture")
        self.git("update-ref", "HEAD", commit)
        return commit

    def run_cli(self, *args):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = ratchet.main(list(args), self.root)
        return code, output.getvalue(), errors.getvalue()

    def test_no_target_uses_committed_registry_and_baseline(self):
        self.install(fixture("void Pass::execute() { glGetError(); glGetError(); }"))
        code, output, _ = self.run_cli("check")
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(output)["new_entries"][0]["count"], 1)
        before = (self.root / BASELINE).read_bytes()
        self.assertEqual(self.run_cli("update", "--shrink-only")[0], 1)
        self.assertEqual((self.root / BASELINE).read_bytes(), before)

    def test_explicit_target_uses_git_show_instead_of_head(self):
        self.install(fixture("void Pass::execute() {}"))
        shrunk = self.snapshot()
        self.install(fixture())
        self.assertEqual(self.run_cli("check", "--target", self.target)[0], 0)
        self.assertEqual(self.run_cli("check", "--target", shrunk)[0], 1)
        self.assertEqual(self.run_cli("check")[0], 1)
        self.assertEqual(self.run_cli("check", "--target", "missing-target")[0], 2)
        self.assertEqual(self.run_cli("check", "--target", "")[0], 2)

    def test_target_registry_controls_seed_eligibility(self):
        self.install(fixture(rule_id="second"))
        # Removing the existing rule is forbidden even when a new rule seeds.
        self.assertEqual(self.run_cli("check", "--target", self.target)[0], 2)

    def test_rule_new_in_target_can_seed_even_when_present_in_head(self):
        current = fixture()
        second = fixture(rule_id="second").json(ratchet.REGISTRY)["rules"][0]
        second["pattern"] = r"\bother\s*\("
        edit_json(current, ratchet.REGISTRY, lambda data: data["rules"].append(second))
        declaration = fixture().json(SCOPE)["groups"]["query"]
        edit_json(current, SCOPE, lambda data: data["groups"].update(second=declaration))
        self.install(current)
        self.snapshot()  # HEAD now contains the rule with zero findings.
        current.files["src/pass.cpp"] = "void Pass::execute() { glGetError(); other(); }"
        rules = ratchet.registry(current)
        found = ratchet.analyze(current, rules, ratchet.scopes(current, rules))
        current.files[BASELINE] = ratchet.canonical(dict(schema=1, entries=list(found.values())))
        self.install(current)
        self.assertEqual(self.run_cli("check", "--target", self.target)[0], 0)
        self.assertEqual(self.run_cli("check")[0], 1)

    def test_shrink_update_is_stable_and_partial_failure_writes_nothing(self):
        (self.root / "src/pass.cpp").write_text("void Pass::execute() {}", encoding="utf-8")
        self.assertEqual(self.run_cli("check")[0], 2)
        self.assertEqual(self.run_cli("update", "--shrink-only")[0], 0)
        before = (self.root / BASELINE).read_bytes()
        self.assertEqual(self.run_cli("check")[0], 0)
        self.assertEqual(self.run_cli("update", "--shrink-only")[0], 0)
        self.assertEqual((self.root / BASELINE).read_bytes(), before)
        (self.root / "src/pass.cpp").unlink()
        self.assertEqual(self.run_cli("update", "--shrink-only")[0], 2)
        self.assertEqual((self.root / BASELINE).read_bytes(), before)

    def test_read_failures_types_encodings_and_bounds(self):
        tree = ratchet.Tree(self.root)
        source = self.root / "src/pass.cpp"
        source.write_bytes(b"\xff")
        self.assertEqual(self.run_cli("check")[0], 2)
        with patch.object(ratchet, "MAX_INPUT", 4):
            with self.assertRaisesRegex(ratchet.AnalysisError, "10 MiB"):
                tree.read(ratchet.REGISTRY)
        with patch.object(ratchet.os, "open", side_effect=PermissionError("denied")):
            self.assertEqual(self.run_cli("update", "--shrink-only")[0], 2)
        source.write_text("void Pass::execute() {}", encoding="utf-8")
        partial = SimpleNamespace(st_mode=stat.S_IFREG, st_size=200, st_mtime_ns=1, st_ctime_ns=1)
        with patch.object(ratchet.os, "fstat", return_value=partial):
            with self.assertRaisesRegex(ratchet.AnalysisError, "incomplete or changing"):
                tree.read("src/pass.cpp")
        size = source.stat().st_size
        changed = SimpleNamespace(st_mode=stat.S_IFREG, st_size=size, st_mtime_ns=1, st_ctime_ns=1)
        changed_after = SimpleNamespace(st_mode=stat.S_IFREG, st_size=size, st_mtime_ns=2, st_ctime_ns=1)
        with patch.object(ratchet.os, "fstat", side_effect=[changed, changed_after]):
            with self.assertRaisesRegex(ratchet.AnalysisError, "incomplete or changing"):
                tree.read("src/pass.cpp")
        source.unlink()
        source.mkdir()
        with self.assertRaises((OSError, ratchet.AnalysisError)):
            tree.read("src/pass.cpp")

    def test_links_and_nonregular_opened_objects_refuse(self):
        tree = ratchet.Tree(self.root)
        source = self.root / "src/pass.cpp"
        source.unlink()
        try:
            source.symlink_to(self.root / ratchet.REGISTRY)
        except OSError:
            # Windows runners may deny link creation. Exercise the resulting
            # missing input instead; link privileges are never required.
            self.assertEqual(self.run_cli("check")[0], 2)
        else:
            with self.assertRaises((OSError, ratchet.AnalysisError)):
                tree.read("src/pass.cpp")
            linked = self.snapshot()
            with self.assertRaisesRegex(ratchet.AnalysisError, "non-regular Git"):
                ratchet.Tree(self.root, linked).read("src/pass.cpp")
            source.unlink()
        source.write_text("void Pass::execute() {}", encoding="utf-8")
        with patch.object(ratchet.os, "fstat", return_value=os.stat_result((stat.S_IFIFO,) + (0,) * 9)):
            with self.assertRaisesRegex(ratchet.AnalysisError, "non-regular input"):
                tree.read("src/pass.cpp")
        if hasattr(os, "mkfifo"):
            source.unlink()
            os.mkfifo(source)
            with self.assertRaisesRegex(ratchet.AnalysisError, "non-regular input"):
                tree.read("src/pass.cpp")

    def test_parent_link_refuses(self):
        tree = ratchet.Tree(self.root)
        directory = self.root / "src"
        directory.rename(self.root / "actual")
        try:
            directory.symlink_to(self.root / "actual", target_is_directory=True)
        except OSError:
            self.assertEqual(self.run_cli("check")[0], 2)
        else:
            with self.assertRaises((OSError, ratchet.AnalysisError)):
                tree.read("src/pass.cpp")

    def test_duplicate_json_and_escaping_paths(self):
        (self.root / ratchet.REGISTRY).write_text('{"schema":1,"schema":1}', encoding="utf-8")
        self.assertEqual(self.run_cli("check")[0], 2)
        for path in ("../outside", "", "/absolute", "C:/drive", "src/../outside", "src\\file", "src//file"):
            with self.subTest(path=path), self.assertRaises(ratchet.AnalysisError):
                ratchet.relative_path(path)

    def test_replace_failure_keeps_baseline_and_cleans_staging(self):
        tree = ratchet.Tree(self.root)
        rules = ratchet.registry(tree)
        before = (self.root / BASELINE).read_bytes()
        with patch.object(ratchet.os, "replace", side_effect=OSError("replace failed")):
            with self.assertRaises(OSError):
                ratchet.update(tree, rules, {})
        self.assertEqual((self.root / BASELINE).read_bytes(), before)
        self.assertEqual(sorted(path.name for path in (self.root / BASELINE).parent.iterdir()), ["runtime.json"])

    def test_second_shard_replace_failure_rolls_back_first(self):
        current = fixture()
        second = fixture(rule_id="second").json(ratchet.REGISTRY)["rules"][0]
        second["family"] = "code-practices"
        edit_json(current, ratchet.REGISTRY, lambda data: data["rules"].append(second))
        declaration = fixture().json(SCOPE)["groups"]["query"]
        edit_json(current, SCOPE, lambda data: data["groups"].update(second=declaration))
        other = BASELINE.replace("performance", "code-practices")
        current.files[other] = fixture(rule_id="second").files[BASELINE]
        self.install(current)
        tree = ratchet.Tree(self.root)
        before = {path: (self.root / path).read_bytes() for path in (BASELINE, other)}
        replace = os.replace
        calls = []
        def fail_second(source, destination):
            calls.append(destination)
            if len(calls) == 2:
                raise OSError("second shard failed")
            replace(source, destination)
        with patch.object(ratchet.os, "replace", side_effect=fail_second):
            with self.assertRaises(OSError):
                ratchet.update(tree, ratchet.registry(tree), {})
        self.assertEqual(len(calls), 3)
        for path, payload in before.items():
            self.assertEqual((self.root / path).read_bytes(), payload)
            self.assertEqual(sorted(file.name for file in (self.root / path).parent.iterdir()), ["runtime.json"])

    def test_staging_write_failure_changes_nothing(self):
        before = (self.root / BASELINE).read_bytes()
        tree = ratchet.Tree(self.root)
        with patch.object(ratchet.tempfile, "NamedTemporaryFile", side_effect=OSError("unwritable")):
            with self.assertRaises(OSError):
                ratchet.update(tree, ratchet.registry(tree), {})
        self.assertEqual((self.root / BASELINE).read_bytes(), before)

    def test_update_requires_explicit_shrink_only(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            ratchet.main(["update"], self.root)
        self.assertEqual(error.exception.code, 2)

    def test_explain_and_unknown_rule(self):
        code, output, _ = self.run_cli("explain", "query")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output)["rationale"], "Driver work")
        self.assertEqual(self.run_cli("explain", "absent")[0], 2)


class WindowsHandleTests(unittest.TestCase):
    def test_canonical_drive_unc_and_failure_results(self):
        # Exercise the Windows branch on every runner without requiring link
        # privileges. Tree.root is already resolved to the long handle path.
        cases = [
            ("C:/Long directory", "\\\\?\\c:\\Long directory\\src\\pass.cpp", None, False),
            ("//server/share/root", "\\\\?\\UNC\\server\\share\\root\\src\\pass.cpp", None, False),
            ("C:/repo", "\\\\?\\C:\\outside\\src\\pass.cpp", None, True),
            ("C:/repo", "", 0, True),
            ("C:/repo", "", 32768, True),
        ]
        for root, result, length, refuses in cases:
            with self.subTest(root=root, result=result, length=length):
                def final_path(handle, buffer, capacity, flags):
                    self.assertEqual(handle, 99)
                    buffer.value = result
                    return len(result) if length is None else length
                api = SimpleNamespace(GetFinalPathNameByHandleW=final_path)
                with patch.object(ctypes, "WinDLL", return_value=api, create=True), \
                        patch.dict(sys.modules, msvcrt=SimpleNamespace(get_osfhandle=lambda descriptor: 99)), \
                        patch.object(ratchet.os, "name", "nt"), \
                        patch.object(ratchet.os, "path", ntpath), \
                        patch.object(ratchet.os, "open", return_value=123), \
                        patch.object(ratchet.os, "close") as close:
                    if refuses:
                        with self.assertRaises(ratchet.AnalysisError):
                            ratchet.open_input(PureWindowsPath(root), "src/pass.cpp")
                        close.assert_called_once_with(123)
                    else:
                        self.assertEqual(ratchet.open_input(PureWindowsPath(root), "src/pass.cpp"), 123)
                        close.assert_not_called()


class RepositoryTests(unittest.TestCase):
    def test_registered_check_against_committed_baseline(self):
        result = subprocess.run([sys.executable, "-I", "-B", str(ROOT / "tools/ci/ratchet.py"), "check"],
                                cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_registration_and_reviewed_source_inventory(self):
        spec = importlib.util.spec_from_file_location("ci_paths", ROOT / "tools/ci/ci_paths.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.python_test_commands(ROOT).count(("tools/ci/test_ratchet.py",)), 1)
        tree = ratchet.Tree(ROOT)
        rules = ratchet.registry(tree)
        self.assertTrue({"hot-gl-query", "hot-draw-file-load", "owner-blocking-wait", "double-lookup-after-find"} <= set(rules))
        scope = ratchet.scopes(tree, rules)
        for declarations in scope.values():
            for declaration in declarations:
                for glob in declaration["files"]:
                    for path in tree.paths:
                        if fnmatch.fnmatchcase(path, glob):
                            self.assertTrue(module.needs_python(path), path)
        for path in ("src/future-owner/new.cpp", "src/future-owner/deleted.h"):
            self.assertTrue(module.needs_python(path), path)
        entries = ratchet.analyze(tree, rules, ratchet.scopes(tree, rules))
        # Ceilings may shrink as fixes land; unlike equality pins, these do not
        # require updating a source-contract test to burn down debt.
        for rule, ceiling in (("hot-gl-query", 85), ("hot-draw-file-load", 3),
                              ("owner-blocking-wait", 3), ("double-lookup-after-find", 1)):
            self.assertLessEqual(sum(item["count"] for item in entries.values() if item["rule"] == rule), ceiling)

    def test_duplicate_lookup_detector_matches_only_same_container_and_key(self):
        tree = ratchet.Tree(ROOT)
        pattern = ratchet.registry(tree)["double-lookup-after-find"]["pattern"]
        self.assertIsNotNone(re.search(pattern, "if (cache.find(key) != cache.end()) { return cache[key]; }"))
        for source in ("if (cache.find(key) != cache.end()) { return other[key]; }",
                       "if (cache.find(key) != cache.end()) { return cache[other]; }",
                       "if (cache.find(key) == cache.end()) { return cache[key]; }"):
            self.assertIsNone(re.search(pattern, source))


if __name__ == "__main__":
    unittest.main()
