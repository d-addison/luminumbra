#!/usr/bin/env python3
"""Isolated stdlib contracts for dependency-expanded compile and tidy checks."""

import argparse
from contextlib import contextmanager, redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import ntpath
import os
from pathlib import Path, PureWindowsPath
import re
import shlex
import stat
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("changed_tus", ROOT / "tools/ci/changed_tus.py")
tus = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tus)


class ChangedTranslationUnitsTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "scratch").mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="changed tus ", dir=ROOT / "scratch")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.src = self.root / "src"
        self.src.mkdir()
        self.build = self.root / "scratch/build with spaces"
        self.build.mkdir(parents=True)
        self.a = self.src / "a with spaces.cpp"
        self.b = self.src / "b.cpp"
        self.header = self.src / "transitive header.hpp"
        self.other = self.src / "ordinary.hpp"
        self.generated = self.build / "generated.hpp"
        for file in (self.a, self.b, self.header, self.other, self.generated):
            file.write_text("// fixture\n")
        (self.root / ".clang-tidy").write_text("Checks: '-*,bugprone-*'\n")
        (self.root / "scripts").mkdir()
        (self.root / "scripts/tidy.sh").write_text((ROOT / "scripts/tidy.sh").read_text())
        (self.root / "CMakeLists.txt").write_text("")
        self.raw = [self.raw_entry(self.a), self.raw_entry(self.b)]
        self.save_db()
        self.commands = []
        self.mock_execute = patch.object(tus, "execute", side_effect=self.fake_execute).start()
        # Most tests model compiler replay; dedicated search-list tests below
        # exercise the real probe with subprocess results for both drivers.
        patch.object(tus, "compiler_header_roots", return_value=set()).start()
        self.addCleanup(patch.stopall)

    def raw_entry(self, source, extra=(), directory=None):
        return {"file": str(source), "directory": str(directory or self.build),
                "arguments": ["/usr/bin/g++", "-O3", "-DNDEBUG", "-ffp-contract=off",
                              "-I", str(self.src), *extra, "-c", str(source), "-o", "original.o",
                              "-MD", "-MF", "original.d"]}

    def save_db(self):
        (self.build / "compile_commands.json").write_text(json.dumps(self.raw))

    def entries(self):
        return tus.read_database(self.root, self.build)

    def fake_execute(self, command, directory, check=True):
        self.commands.append((command, directory))
        if "-M" in command:
            _, operands = tus.parse_arguments(command[:-3])
            source = (directory / operands[0]).resolve()
            paths = [source, self.other, self.generated]
            # A transitive conditional header is consumed by only one target
            # variant, and a separate include is visible only to the analyzer.
            if "-DENABLE_FEATURE" in command or "-D__clang_analyzer__" in command:
                paths.append(self.header)
            escape = lambda p: str(p).replace("$", "$$").replace(" ", "\\ ").replace("#", "\\#")
            output = "selected-tu: " + " \\\n ".join(map(escape, paths)) + "\n"
            return subprocess.CompletedProcess(command, 0, output, "")
        return subprocess.CompletedProcess(command, 0, "", "")

    def select(self, changed):
        return tus.select(self.root, self.entries(), set(changed), 2, self.build)

    def check_commands(self):
        return [(command, directory) for command, directory in self.commands if "-M" not in command]

    def compile_commands_by_variant(self):
        # Workers may finish in either order; output names retain input indices.
        return sorted(self.check_commands(), key=lambda item: int(Path(item[0][-1]).stem))

    def arguments(self, command="compile", **updates):
        values = dict(command=command, ccache=False, jobs=2,
                      config_file=Path(".clang-tidy"), also_ci_gate=False,
                      warning_profile="none", write_warning_baseline=None,
                      baseline_records=[], clean_prefixes=())
        values.update(updates)
        return argparse.Namespace(**values)

    def test_changed_source_selects_every_command_variant_after_scanning(self):
        self.raw.append(self.raw_entry(self.a, ["-DENABLE_FEATURE"]))
        self.save_db()
        selected, report = self.select([self.a])
        self.assertEqual([e["file"] for e in selected], [self.a, self.a])
        self.assertEqual(report["scan_variants"], 3)
        self.assertEqual(len(self.commands), 6)
        self.assertTrue(all("-M" in command for command, _ in self.commands))

    def test_changed_source_selects_its_includers_in_both_contexts(self):
        self.raw.append(self.raw_entry(self.b, ["-DENABLE_FEATURE"]))
        self.raw[-1]["arguments"][0] = "clang++"
        self.save_db()
        execute = self.fake_execute

        def including_execute(command, directory, check=True):
            result = execute(command, directory, check)
            if str(self.b) in command and ("-DENABLE_FEATURE" in command
                                          or "-D__clang_analyzer__" in command):
                escaped = str(self.a).replace(" ", "\\ ")
                result.stdout = result.stdout.rstrip("\n") + " " + escaped + "\n"
            return result

        self.mock_execute.side_effect = including_execute
        selected, report = self.select([self.a])
        self.assertEqual([e["file"] for e in selected], [self.a, self.b, self.b])
        self.assertEqual(report["scan_variants"], 3)
        self.assertFalse(report["full_selection"])
        self.assertEqual(len(self.commands), 6)

    def test_included_only_source_selects_only_consuming_variants(self):
        included = self.src / "included.cpp"
        included.write_text("// included fixture\n", encoding="utf-8")
        self.raw.append(self.raw_entry(self.b, ["-DENABLE_FEATURE"]))
        self.save_db()

        def scan(entry):
            return {entry["file"]} | ({included} if "-DENABLE_FEATURE" in entry["arguments"] else set())

        with patch.object(tus, "dependencies", side_effect=scan):
            selected, report = self.select([included])
        self.assertEqual(len(selected), 1)
        self.assertEqual(selected[0]["file"], self.b)
        self.assertIn("-DENABLE_FEATURE", selected[0]["arguments"])
        self.assertFalse(report["full_selection"])

    def test_failed_source_scan_never_silently_omits_includers(self):
        self.mock_execute.side_effect = tus.SelectionError("scan failed")
        with self.assertRaisesRegex(tus.SelectionError, "scan failed"):
            self.select([self.a])

    def test_transitive_headers_scan_actual_flags_in_both_contexts(self):
        self.raw.append(self.raw_entry(self.a, ["-DENABLE_FEATURE"]))
        self.save_db()
        selected, report = self.select([self.header])
        self.assertEqual(len(selected), 3)
        self.assertEqual(report["scan_variants"], 3)
        self.assertEqual(len(self.commands), 6)
        self.assertTrue(any("-DENABLE_FEATURE" in command for command, _ in self.commands))
        self.assertEqual(sum("-D__clang_analyzer__" in c for c, _ in self.commands), 3)
        for command, directory in self.commands:
            self.assertEqual(directory, self.build)
            self.assertIn(str(self.src), command)
            self.assertNotIn("original.o", command)
            self.assertNotIn("original.d", command)

    def test_conditional_headers_map_only_consuming_variant(self):
        self.raw.append(self.raw_entry(self.a, ["-DENABLE_FEATURE"]))
        self.save_db()
        def scan(entry):
            return {entry["file"]} | ({self.header} if "-DENABLE_FEATURE" in entry["arguments"] else set())
        with patch.object(tus, "dependencies", side_effect=scan):
            selected, _ = self.select([self.header])
        self.assertEqual(len(selected), 1)
        self.assertIn("-DENABLE_FEATURE", selected[0]["arguments"])

    def test_generated_headers_are_real_dependencies(self):
        selected, _ = self.select([self.generated])
        self.assertEqual(len(selected), 2)

    def test_untracked_source_missing_database_is_failure(self):
        source = self.src / "new.cpp"
        source.write_text("// new")
        with self.assertRaisesRegex(tus.SelectionError, "missing compile database"):
            self.select([source])

    def test_untracked_header_selects_its_consumers(self):
        selected, _ = self.select([self.header])
        self.assertEqual(len(selected), 2)

    def test_unmatched_header_falls_back_after_complete_scan(self):
        unused = self.src / "unused.hpp"
        unused.write_text("unused")
        selected, report = self.select([unused])
        self.assertEqual(len(selected), 2)
        self.assertEqual(report["scan_variants"], 2)
        self.assertTrue(report["full_selection"])
        self.assertIn("changed header has no proven dependency mapping", report["selection_fallback"])

    def test_configured_and_copied_headers_map_back_to_inputs(self):
        for command in (
            'configure_file("${CMAKE_SOURCE_DIR}/src/ordinary.hpp" "${generated}" @ONLY)',
            'configure_file("${CMAKE_SOURCE_DIR}/src/ordinary.hpp" "${generated}" COPYONLY)',
            'file(COPY_FILE "${CMAKE_SOURCE_DIR}/src/ordinary.hpp" "${generated}")',
            'file(COPY "${CMAKE_SOURCE_DIR}/src/ordinary.hpp" DESTINATION "${CMAKE_BINARY_DIR}")',
        ):
            with self.subTest(command=command):
                output = self.build / ("ordinary.hpp" if command.startswith('file(COPY ') else "generated.hpp")
                output.write_text("configured or copied content")
                (self.root / "CMakeLists.txt").write_text(
                    'set(generated "${CMAKE_BINARY_DIR}/generated.hpp")\n' + command)
                def scan(entry):
                    return {entry["file"]} | ({output} if entry["file"] == self.a else {self.header})
                with patch.object(tus, "dependencies", side_effect=scan):
                    selected, report = self.select([self.other])
                self.assertEqual([e["file"] for e in selected], [self.a])
                self.assertFalse(report["full_selection"])

    def test_copy_mapping_chain_includes_direct_and_indirect_consumers(self):
        intermediate = self.build / "intermediate.hpp"
        intermediate.write_text("intermediate")
        (self.root / "CMakeLists.txt").write_text(
            'configure_file("${CMAKE_SOURCE_DIR}/src/ordinary.hpp" "${CMAKE_BINARY_DIR}/intermediate.hpp" COPYONLY)\n'
            'file(COPY_FILE "${CMAKE_BINARY_DIR}/intermediate.hpp" "${CMAKE_BINARY_DIR}/generated.hpp")\n')
        with patch.object(tus, "dependencies", side_effect=lambda e: {
                e["file"], self.other if e["file"] == self.a else self.generated}):
            selected, report = self.select([self.other])
        self.assertEqual(len(selected), 2)
        self.assertFalse(report["full_selection"])

    def test_unknown_generated_relationship_widens_even_a_direct_header_match(self):
        with patch.object(tus, "dependencies", side_effect=lambda e: {
                e["file"], self.other if e["file"] == self.a else self.generated}):
            selected, report = self.select([self.other])
        self.assertEqual(len(selected), 2)
        self.assertIn("generated dependency has no proven input mapping", report["selection_fallback"])

    def test_unresolved_copy_variables_do_not_establish_a_mapping(self):
        (self.root / "CMakeLists.txt").write_text(
            'configure_file("${unknown}/ordinary.hpp" "${CMAKE_BINARY_DIR}/generated.hpp")')
        selected, report = self.select([self.other])
        self.assertEqual(len(selected), 2)
        self.assertTrue(report["full_selection"])

    def test_ambiguous_copy_syntax_widens_even_a_direct_match(self):
        for text in (
            'file(COPY "${CMAKE_SOURCE_DIR}/src" DESTINATION "${CMAKE_BINARY_DIR}")',
            'configure_file("src/ordinary.hpp" "generated(with-parens).hpp")',
            'configure_file("${unknown}/ordinary.hpp" "${CMAKE_BINARY_DIR}/generated.hpp")',
        ):
            with self.subTest(text=text):
                (self.root / "CMakeLists.txt").write_text(text)
                with patch.object(tus, "dependencies", side_effect=lambda e: {e["file"], self.other}):
                    selected, report = self.select([self.other])
                self.assertEqual(len(selected), 2)
                self.assertIn("unresolved configure/copy relationships", report["selection_fallback"])

    def assert_unproven_cmake_selects_full_scope(self, text):
        (self.root / "CMakeLists.txt").write_text(text)
        # The direct consumer makes the changed input appear matched even if a
        # bad mapping incorrectly drops the generated-header consumer.
        with patch.object(tus, "dependencies", side_effect=lambda e: {
                e["file"], self.header if e["file"] == self.a else self.generated}):
            selected, report = self.select([self.header])
        self.assertEqual([e["file"] for e in selected], [self.a, self.b])
        self.assertTrue(report["full_selection"])
        self.assertIn("unresolved configure/copy relationships", report["selection_fallback"])
        self.assertEqual(tus.configured_header_inputs(self.root, self.build), ({}, True))

    def test_if_off_override_cannot_drop_generated_header_consumer(self):
        self.assert_unproven_cmake_selects_full_scope(
            'set(input "${CMAKE_SOURCE_DIR}/src/transitive header.hpp")\n'
            'if(OFF)\n'
            '  set(input "${CMAKE_SOURCE_DIR}/src/ordinary.hpp")\n'
            'endif()\n'
            'configure_file("${input}" "${CMAKE_BINARY_DIR}/generated.hpp" COPYONLY)\n')

    def test_control_flow_assignments_and_copies_never_establish_provenance(self):
        for opening, closing in (
            ("if(OFF)", "endif()"),
            ("if(OFF)\nelseif(ON)", "endif()"),
            ("if(OFF)\nelse()", "endif()"),
            ("foreach(item IN ITEMS first second)", "endforeach()"),
            ("while(OFF)", "endwhile()"),
            ("function(generate)", "endfunction()"),
            ("macro(generate)", "endmacro()"),
        ):
            for statement in (
                'set(input "${CMAKE_SOURCE_DIR}/src/ordinary.hpp")',
                'configure_file("${input}" "${CMAKE_BINARY_DIR}/generated.hpp" COPYONLY)',
                'file(COPY_FILE "${input}" "${CMAKE_BINARY_DIR}/generated.hpp")',
            ):
                with self.subTest(opening=opening, statement=statement):
                    self.assert_unproven_cmake_selects_full_scope(
                        'set(input "${CMAKE_SOURCE_DIR}/src/transitive header.hpp")\n'
                        + opening + '\n' + statement + '\n' + closing + '\n'
                        'configure_file("${input}" "${CMAKE_BINARY_DIR}/generated.hpp" COPYONLY)\n')

    def test_unsupported_mutations_and_syntax_invalidate_all_script_mappings(self):
        for unsupported in (
            'unset(input)', 'list(POP_BACK input)',
            'string(REPLACE header ordinary input "${input}")',
            'include(overrides.cmake)', 'override_input()',
            'set(input changed.hpp CACHE STRING "input" FORCE)',
            'set(input)', 'set(input changed.hpp PARENT_SCOPE)',
            'set(input "${unknown}")', 'set(input "[=[quoted]=]")',
            'set(input "unterminated)', 'set(input nested(value))',
            'set(input "escaped\\;value")', 'trailing garbage',
            'file(READ input contents)',
            'configure_file(input output UNSUPPORTED)',
        ):
            for before in (False, True):
                with self.subTest(unsupported=unsupported, before=before):
                    supported = (
                        'set(input "${CMAKE_SOURCE_DIR}/src/transitive header.hpp")\n'
                        'configure_file("${input}" "${CMAKE_BINARY_DIR}/generated.hpp" COPYONLY)\n')
                    self.assert_unproven_cmake_selects_full_scope(
                        unsupported + '\n' + supported if before else supported + unsupported)

    def test_unsupported_other_script_invalidates_cross_script_provenance(self):
        (self.root / "overrides.cmake").write_text('override_input()\n')
        self.assert_unproven_cmake_selects_full_scope(
            'configure_file("${CMAKE_SOURCE_DIR}/src/transitive header.hpp" '
            '"${CMAKE_BINARY_DIR}/generated.hpp" COPYONLY)\n')

    def test_short_parent_scope_form_cannot_override_local_input(self):
        # With no value, PARENT_SCOPE removes the parent's variable and leaves
        # this scope's input unchanged. Quoting does not disable the keyword.
        (self.root / "PARENT_SCOPE").write_text("unrelated literal file")
        for value in ("PARENT_SCOPE", '"PARENT_SCOPE"'):
            with self.subTest(value=value):
                self.assert_unproven_cmake_selects_full_scope(
                    'set(input "${CMAKE_SOURCE_DIR}/src/transitive header.hpp")\n'
                    f'set(input {value})\n'
                    'configure_file("${input}" "${CMAKE_BINARY_DIR}/generated.hpp" COPYONLY)\n')

    def test_literal_zero_and_quoted_hash_are_valid_mapping_inputs(self):
        source = self.root / "0"
        source.write_text("literal input")
        (self.root / "CMakeLists.txt").write_text(
            '# if(OFF)\nset(input 0)\nset(label "literal # comment")\n'
            'configure_file("${input}" "${CMAKE_BINARY_DIR}/generated.hpp" COPYONLY) # end\n')
        self.assertEqual(tus.configured_header_inputs(self.root, self.build),
                         ({self.generated: {source}}, False))

    def test_configure_output_directory_outside_build_requires_full_scope(self):
        output_dir = self.root / "include"
        output_dir.mkdir()
        generated = output_dir / self.header.name
        generated.write_text("generated content")
        (self.root / "CMakeLists.txt").write_text(
            'configure_file("${CMAKE_SOURCE_DIR}/src/transitive header.hpp" '
            '"${CMAKE_SOURCE_DIR}/include" COPYONLY)\n')
        with patch.object(tus, "dependencies", side_effect=lambda e: {
                e["file"], self.header if e["file"] == self.a else generated}):
            selected, report = self.select([self.header])
        self.assertEqual([e["file"] for e in selected], [self.a, self.b])
        self.assertTrue(report["full_selection"])
        self.assertIn("unresolved configure/copy relationships", report["selection_fallback"])
        self.assertEqual(tus.configured_header_inputs(self.root, self.build), ({}, True))

    def test_engine_version_script_requires_full_scope_for_control_flow(self):
        declaration = (ROOT / "cmake/engine_version.cmake").read_text(encoding="utf-8")
        self.assertIn('"${CMAKE_SOURCE_DIR}/src/luminumbra_common/core/EngineVersion.h.in"', declaration)
        self.assertIn("if(Git_FOUND)", declaration)
        mappings, incomplete = tus.configured_header_inputs(ROOT, self.build)
        self.assertEqual(mappings, {})
        self.assertTrue(incomplete)

    def test_non_utf8_cmake_script_requires_full_scope(self):
        (self.root / "external.cmake").write_bytes(b"# legacy comment: \x92\n")
        self.assertEqual(tus.configured_header_inputs(self.root, self.build), ({}, True))
        selected, report = self.select([self.header])
        self.assertEqual([e["file"] for e in selected], [self.a, self.b])
        self.assertTrue(report["full_selection"])
        self.assertIn("unresolved configure/copy relationships", report["selection_fallback"])

    def test_cmake_fallback_reason_is_printed_and_inputs_are_unchanged(self):
        self.assert_unproven_cmake_selects_full_scope('if(OFF)\nendif()\n')
        before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        stream = io.StringIO()
        with patch.object(tus, "changed_paths", return_value={self.header}), patch.object(tus, "run_checks"):
            with redirect_stdout(stream):
                self.assertEqual(tus.main(["compile", "--root", str(self.root),
                                           "--build-dir", str(self.build)]), 0)
        report = json.loads(stream.getvalue().splitlines()[0])
        self.assertTrue(report["full_selection"])
        self.assertIn("unresolved configure/copy relationships", report["selection_fallback"])
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})

    def test_deleted_source_selects_remaining_tree_and_stale_entry_fails(self):
        self.a.unlink()
        self.raw = [self.raw[1]]
        self.save_db()
        selected, report = self.select([self.a])
        self.assertEqual([e["file"] for e in selected], [self.b])
        self.assertTrue(report["full_selection"])
        self.raw.append(self.raw_entry(self.a))
        self.save_db()
        with self.assertRaisesRegex(tus.SelectionError, "incomplete|deleted"):
            self.select([self.a])

    def test_deleted_header_conservatively_selects_all_or_fails_scan(self):
        self.header.unlink()
        with patch.object(tus, "dependencies", side_effect=lambda e: {e["file"]}):
            selected, report = self.select([self.header])
        self.assertEqual(len(selected), 2)
        self.assertTrue(report["full_selection"])
        with self.assertRaisesRegex(tus.SelectionError, "incomplete"):
            self.select([self.header])

    def test_build_flags_dependency_and_unknown_edits_select_all(self):
        for name in ("CMakeLists.txt", "vendor/CMakeLists.txt", "CMakePresets.json", "toolchain", "scripts/flags.sh"):
            selected, report = self.select([self.root / name])
            self.assertEqual(len(selected), 2, name)
            self.assertTrue(report["full_selection"])

    def test_documentation_python_only_select_no_native_work(self):
        selected, report = self.select([self.root / "docs/usage.md", self.root / "tools/audio/bank_validate.py"])
        self.assertEqual(selected, [])
        self.assertEqual(report["scan_variants"], 0)

    def test_missing_database_entries_report_disabled_configuration(self):
        missing = self.src / "OptionalBackend.cpp"
        missing.write_text("optional")
        with self.assertRaisesRegex(tus.SelectionError, "disabled configuration"):
            self.select([missing])

    def test_native_python_generators_invalidate_selection(self):
        selected, report = self.select([self.root / "tools/config_codegen.py"])
        self.assertEqual(len(selected), 2)
        self.assertTrue(report["full_selection"])

    def test_empty_database_cannot_prove_header_selection(self):
        self.raw = []
        self.save_db()
        with self.assertRaisesRegex(tus.SelectionError, "incomplete selection"):
            self.select([self.header])

    def test_failed_scan_never_silently_omits_tu(self):
        self.mock_execute.side_effect = tus.SelectionError("scan failed")
        with self.assertRaisesRegex(tus.SelectionError, "scan failed"):
            self.select([self.header])

    def test_malformed_or_incomplete_dependency_output_fails(self):
        for output in ("not a rule", "target: missing.hpp", f"target: {self.other}"):
            self.mock_execute.side_effect = None
            self.mock_execute.return_value = subprocess.CompletedProcess([], 0, output, "")
            with self.assertRaises(tus.SelectionError):
                self.select([self.header])

    def test_paths_with_spaces_hash_and_dollar(self):
        unusual = self.src / "a #$ file.cpp"
        unusual.write_text("unusual")
        self.raw = [self.raw_entry(unusual)]
        self.save_db()
        output = "target: " + str(unusual).replace("$", "$$").replace(" ", "\\ ").replace("#", "\\#")
        self.mock_execute.side_effect = None
        self.mock_execute.return_value = subprocess.CompletedProcess([], 0, output, "")
        selected, report = self.select([self.header])
        self.assertEqual([e["file"] for e in selected], [unusual])
        self.assertTrue(report["full_selection"])

    @unittest.skipUnless(os.name == "posix", "Windows forbids newline characters in filenames")
    def test_newline_filename_is_selected_on_posix(self):
        newline = self.src / "new\nline.cpp"
        newline.write_text("source")
        self.raw = [self.raw_entry(newline)]
        self.save_db()
        # Selection preserves pathname bytes when a dependency graph is known.
        # Make output cannot encode this name unambiguously; scanning refuses it.
        with patch.object(tus, "dependencies", return_value={newline}):
            selected, _ = self.select([newline])
        self.assertEqual(selected[0]["file"], newline)
        with self.assertRaisesRegex(tus.SelectionError, "incomplete dependency scan"):
            self.select([newline])

    def test_relative_database_source_and_shell_quoted_command(self):
        self.raw[0]["file"] = os.path.relpath(self.a, self.build)
        self.raw[0]["command"] = shlex.join(self.raw[0].pop("arguments"))
        self.save_db()
        self.assertEqual(self.entries()[0]["file"], self.a)

    def test_malformed_commands_and_unsafe_options_fail_before_execution(self):
        for arguments in ([], [1], "g++ source.cpp", ["sh", "-c", "anything"],
                          ["g++", "@response"], ["g++", "-o"], ["g++", "-fplugin=plugin"],
                          ["g++", "-save-temps"], ["g++", "-Wp,-MD,escape"], ["g++", str(self.b)]):
            self.raw[0]["arguments"] = arguments
            self.save_db()
            with self.assertRaises(tus.SelectionError, msg=str(arguments)):
                self.entries()
        self.assertEqual(self.commands, [])

    def test_windows_and_versioned_compilers_preserve_executable_paths(self):
        for compiler in ("gcc.exe", "clang.exe", "g++.exe", "clang++.exe", "g++-14", "clang++-18",
                         "G++-14.EXE", "CLANG++-18.EXE",
                         "x86_64-w64-mingw32-g++-14.exe",
                         r"C:\msys64\ucrt64\bin\g++.exe",
                         r"C:\Program Files\LLVM\bin\clang++.exe"):
            for command_string in (False, True):
                with self.subTest(compiler=compiler, command_string=command_string):
                    self.raw = [self.raw_entry(self.a)]
                    self.raw[0]["arguments"][0] = compiler
                    if command_string:
                        arguments = self.raw[0].pop("arguments")
                        self.raw[0]["command"] = (subprocess.list2cmdline(arguments)
                                                  if compiler.lower().endswith(".exe")
                                                  else shlex.join(arguments))
                        if compiler.lower().endswith(".exe"):
                            self.raw[0]["command_platform"] = "windows"
                    self.save_db()
                    entry = self.entries()[0]
                    command = tus.check_arguments(entry, "msvc-approx")
                    self.assertEqual(command[0], compiler)
                    self.assertIn("-Wshadow-all" if "clang" in compiler.lower()
                                  else "-Wshadow", command)
                    self.assertIn("-Wnarrowing", command)
        self.assertEqual(self.commands, [])

    def test_quoted_windows_command_preserves_paths_and_embedded_quotes(self):
        for compiler in (r"C:\msys64\ucrt64\bin\g++.exe",
                         r"C:\Program Files\LLVM\bin\clang++.exe"):
            with self.subTest(compiler=compiler):
                arguments = [compiler, '-DVALUE=a"b', r'-DPATH=C:\some\path\\',
                             "-c", str(self.a)]
                self.raw = [{"file": str(self.a), "directory": str(self.build),
                             "command_platform": "windows",
                             "command": subprocess.list2cmdline(arguments)}]
                self.save_db()
                self.assertEqual(self.entries()[0]["arguments"], arguments)
        self.assertEqual(self.commands, [])

    def test_posix_exe_command_keeps_shell_quoted_source(self):
        for compiler in ("g++.exe", "clang++.exe"):
            arguments = [compiler, "-c", str(self.a)]
            self.raw = [{"file": str(self.a), "directory": str(self.build),
                         "command_platform": "posix", "command": shlex.join(arguments)}]
            self.save_db()
            self.assertEqual(self.entries()[0]["arguments"], arguments)

    def test_windows_warning_profiles_ignore_compiler_directory_names(self):
        for compiler, shadow, cxx in ((r"C:\clang++\gcc.exe", "-Wshadow", False),
                                      (r"C:\gcc\clang.exe", "-Wshadow-all", False),
                                      (r"C:\msys64\G++.EXE", "-Wshadow", True),
                                      (r"C:\LLVM\CLANG++-18.EXE", "-Wshadow-all", True)):
            with self.subTest(compiler=compiler):
                entry = {"file": self.src / "plain.c", "directory": self.build,
                         "arguments": [compiler, str(self.src / "plain.c")]}
                flags = tus.profile_flags(entry, "msvc-approx")
                self.assertEqual(flags[0], shadow)
                self.assertEqual("-Wnarrowing" in flags, cxx)
        self.assertEqual(self.commands, [])

    def test_windows_wrappers_and_driver_names_use_shared_normalization(self):
        for wrapper in ("ccache.exe", "sccache.exe", r"C:\Tools\CCACHE.EXE"):
            with self.subTest(wrapper=wrapper):
                self.assertEqual(tus.clean_arguments([wrapper, "G++.EXE", str(self.a)]),
                                 ["G++.EXE", str(self.a)])
        with patch.object(tus.os, "name", "nt"):
            self.assertEqual(tus.executable_name(r"C:\Tools\CLANG++-18"), "clang++-18")
            self.assertEqual(tus.executable_name("SCCACHE"), "sccache")

    def test_non_compiler_and_unsupported_windows_drivers_are_rejected(self):
        for compiler in ("not-a-compiler.exe", r"C:\msys64\ucrt64\bin\cmd.exe",
                         "g++.exe.exe", "clang-cl.exe", "CL.EXE"):
            with self.subTest(compiler=compiler):
                self.raw = [self.raw_entry(self.a)]
                self.raw[0]["arguments"][0] = compiler
                self.save_db()
                with self.assertRaisesRegex(tus.SelectionError, "unsupported compiler"):
                    self.entries()
        self.assertEqual(self.commands, [])

    def test_configured_codegen_options_preserve_exact_source_operand(self):
        flags = shlex.split(
            "-fPIC -fexceptions -ffast-math -ffp-contract=off -ffp-model=precise "
            "-flto=auto -flto=thin -fno-fast-math -fno-fat-lto-objects -fno-lto "
            "-fno-rtti -fno-stack-protector -fpch-instantiate-templates -fvisibility=hidden "
            "-mavx2 -mavx512dq -mavx512f -mbmi -mf16c -mfma -mfpmath=sse "
            "-mlzcnt -mpopcnt -msse3 -msse4.1 -msse4.2 -mssse3 -W")
        for compiler in ("g++", "clang++"):
            with self.subTest(compiler=compiler):
                self.raw = [self.raw_entry(self.a, [*flags, "-isystem", str(self.src),
                                                    "-MT", str(self.b)])]
                self.raw[0]["arguments"][0] = compiler
                self.save_db()
                entry = self.entries()[0]
                cleaned, operands = tus.parse_arguments(entry["arguments"])
                self.assertEqual(operands, [str(self.a)])
                self.assertEqual([flag for flag in cleaned if flag in flags],
                                 ["-ffp-contract=off", *flags])
                self.assertNotIn(str(self.b), cleaned)
                # A source-looking option value cannot repair a wrong operand.
                args = self.raw[0]["arguments"]
                args[args.index("-c") + 1] = str(self.b)
                args.extend(["-include", str(self.a)])
                self.save_db()
                with self.assertRaisesRegex(tus.SelectionError, "exactly one database source"):
                    self.entries()
        self.assertEqual(self.commands, [])

    def test_codegen_dangerous_neighbours_still_refused_without_writes(self):
        for flags in (["-fplugin=x.so"], ["-specs=x"], ["-B/x"], ["-B", "/x"],
                      ["-wrapper", "x"], ["@rsp"], ["-load", "x.so"],
                      ["-fprofile-use=data"], ["-flto=plugin.so"], ["-mfoo"],
                      ["-mno-foo"], ["-Xclang", "-load", "-Xclang", "x.so"]):
            with self.subTest(flags=flags):
                self.raw = [self.raw_entry(self.a, flags)]
                self.save_db()
                before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
                with self.assertRaisesRegex(tus.SelectionError, "unsupported"):
                    self.entries()
                self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})
        self.assertEqual(self.commands, [])

    def test_explicit_x86_disable_forms_are_preserved(self):
        flags = ["-mno-bmi", "-mno-avx512f", "-mno-f16c", "-mno-popcnt"]
        self.assertEqual(tus.clean_arguments(["g++", *flags, str(self.a)]),
                         ["g++", *flags, str(self.a)])

    def test_clang_pch_replays_textual_header_without_binary_cache(self):
        header = self.build / "cmake_pch.hxx"
        header.write_text('#include "ordinary.hpp"\n')
        binary = Path(str(header) + ".pch")
        # The recipe need not build dependency PCH binaries for local checks.
        self.assertFalse(binary.exists())
        for binary_args in (["-Xclang", "-include-pch", "-Xclang", str(binary)],
                            ["-include-pch", str(binary)]):
            for text_args in (["-Xclang", "-include", "-Xclang", str(header)],
                              ["-include", str(header)]):
                with self.subTest(binary_args=binary_args, text_args=text_args):
                    self.assert_pch_replays_text(binary_args, text_args, header, binary)

    def assert_pch_replays_text(self, binary_args, text_args, header, binary):
        self.commands.clear()
        self.raw = [self.raw_entry(self.a, [*binary_args, *text_args])]
        self.raw[0]["arguments"][0] = "clang++"
        self.save_db()
        selected, _ = self.select([self.header])
        self.assertEqual([entry["file"] for entry in selected], [self.a])
        tus.run_checks(self.arguments(), self.root, selected, self.build)
        databases = []

        def inspect_tidy(command, directory, check=True):
            database = Path(command[command.index("-p") + 1]) / "compile_commands.json"
            databases.append(json.loads(database.read_text())[0]["arguments"])
            return self.fake_execute(command, directory, check)

        with patch.object(tus.shutil, "which", return_value="clang-tidy"), \
                patch.object(tus, "execute", side_effect=inspect_tidy):
            tus.run_checks(self.arguments("tidy", also_ci_gate=True), self.root, selected, self.build)
        self.assertEqual(len(databases), 2)
        for command in [c for c, _ in self.commands[:3]] + databases:
            self.assertNotIn("-Xclang", command)
            self.assertNotIn(str(binary), command)
            self.assertIn(str(header), command)
        _, operands = tus.parse_arguments(selected[0]["arguments"])
        self.assertEqual(operands, [str(self.a)])

    def test_gcc_cmake_pch_text_header_is_bounded_and_preserved(self):
        header = self.build / "cmake_pch.hxx"
        header.write_text("// textual header\n")
        self.raw = [self.raw_entry(self.a, ["-include", str(header)])]
        self.save_db()
        command = tus.compile_command(self.entries()[0], self.build / "isolated.o")
        self.assertIn(str(header), command)
        outside = self.src / "cmake_pch.hxx"
        outside.write_text("// not in the build tree\n")
        self.raw = [self.raw_entry(self.a, ["-include", str(outside)])]
        self.save_db()
        with self.assertRaisesRegex(tus.SelectionError, "PCH reference escapes"):
            self.entries()
        self.assertEqual(self.commands, [])

    def test_pch_values_do_not_count_as_source_operands(self):
        for source in (None, self.b):
            with self.subTest(source=source):
                self.raw = [self.raw_entry(self.a)]
                self.raw[0]["arguments"] = ["clang++", "-Xclang", "-include", "-Xclang",
                                           str(self.a)] + ([str(source)] if source else [])
                # Locate this database alongside the source so the forced
                # include passes containment and reaches operand verification.
                (self.src / "compile_commands.json").write_text(json.dumps(self.raw))
                with self.assertRaisesRegex(tus.SelectionError, "exactly one database source"):
                    tus.read_database(self.root, self.src)
        self.assertEqual(self.commands, [])

    def test_adjacent_binary_pch_files_and_directories_are_refused(self):
        for compiler in ("g++", "clang++"):
            for name in ("cmake_pch.h", "cmake_pch.hxx", "ordinary.hpp"):
                header = self.build / name
                header.write_text("// current textual header\n")
                forms = [["-include", str(header)], ["-imacros", str(header)]]
                if compiler == "clang++":
                    forms += [["-Xclang", "-include", "-Xclang", str(header)],
                              ["-include-pch", str(header) + ".pch", "-include", str(header)]]
                for suffix in (".gch", ".pch"):
                    for directory in (False, True):
                        binary = Path(str(header) + suffix)
                        if directory:
                            binary.mkdir()
                            (binary / "cached").write_bytes(b"stale binary header")
                        else:
                            binary.write_bytes(b"stale binary header")
                        for flags in forms:
                            with self.subTest(compiler=compiler, header=name, suffix=suffix,
                                              directory=directory, flags=flags):
                                self.raw = [self.raw_entry(self.a, flags)]
                                self.raw[0]["arguments"][0] = compiler
                                self.save_db()
                                before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
                                with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                                    self.entries()
                                self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*")
                                                          if p.is_file()})
                        if directory:
                            (binary / "cached").unlink()
                            binary.rmdir()
                        else:
                            binary.unlink()
        self.assertEqual(self.commands, [])

    def test_ordinary_include_binary_cache_is_refused_in_every_search_root(self):
        self.a.write_text('#include "nested/current.hpp"\n')
        search = self.build / "header search"
        for flags, directory in (([], self.src), (["-I", str(search)], search),
                                 (["-I" + str(search)], search),
                                 (["-iquote", str(search)], search),
                                 (["-isystem", str(search)], search),
                                 (["-idirafter", str(search)], search)):
            nested = directory / "nested"
            nested.mkdir(parents=True, exist_ok=True)
            (nested / "current.hpp").write_text("// edited header\n")
            self.raw = [self.raw_entry(self.a, flags)]
            self.save_db()
            entry = self.entries()[0]
            for is_directory in (False, True):
                binary = nested / "current.hpp.gch"
                if is_directory:
                    binary.mkdir()
                else:
                    binary.write_bytes(b"stale binary header")
                actions = [self.entries, lambda: tus.dependencies(entry),
                           lambda: tus.run_checks(self.arguments(), self.root, [entry], self.build),
                           lambda: tus.run_checks(self.arguments("tidy"), self.root, [entry], self.build)]
                with patch.object(tus.shutil, "which", return_value="clang-tidy"):
                    for index, action in enumerate(actions):
                        with self.subTest(flags=flags, directory=is_directory, action=index):
                            with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                                action()
                if is_directory:
                    binary.rmdir()
                else:
                    binary.unlink()
            self.assertEqual(tus.replay_arguments(entry), tus.clean_arguments(entry["arguments"]))
        self.assertEqual(self.commands, [])

    def test_unrelated_binary_cache_outside_search_roots_does_not_block_replay(self):
        (self.build / "unrelated.hpp.gch").write_bytes(b"unrelated cache")
        entry = self.entries()[0]
        self.assertEqual(tus.replay_arguments(entry), tus.clean_arguments(entry["arguments"]))

    def test_forced_system_and_relative_project_headers_use_compiler_search(self):
        search = self.root / "compiler headers"
        search.mkdir()
        system = search / "cstdint"
        system.write_text("// system header\n")
        relative = self.build / "relative.hpp"
        relative.write_text("// working directory header\n")
        cases = [("cstdint", [], system), (relative.name, [], relative)]
        for option in (["-I", str(self.src)], ["-I" + str(self.src)],
                       ["-isystem", str(self.src)], ["-iquote", str(self.src)],
                       ["-idirafter", str(self.src)]):
            cases.append((self.other.name, option, self.other))
        for compiler in ("g++", "clang++", "G++.EXE", "CLANG++.EXE"):
            for forced in ("-include", "-imacros"):
                for name, flags, header in cases:
                    with self.subTest(compiler=compiler, forced=forced, flags=flags, name=name):
                        self.raw = [self.raw_entry(self.a, [*flags, forced, name])]
                        self.raw[0]["arguments"][0] = compiler
                        self.save_db()
                        tus._compiler_header_roots.cache_clear()
                        result = subprocess.CompletedProcess([], 0, "",
                            '#include "..." search starts here:\r\n ' + str(self.src)
                            + '\r\n#include <...> search starts here:\r\n ' + str(search)
                            + '\r\nEnd of search list.\r\n')
                        with patch.object(tus, "compiler_header_roots", side_effect=COMPILER_HEADER_ROOTS), \
                                patch.object(tus._DEPS, "run_process", return_value=result) as probe:
                            entry = self.entries()[0]
                            self.assertEqual(tus.replay_arguments(entry), tus.clean_arguments(entry["arguments"]))
                            self.assertEqual(probe.call_count, 1)
                            binary = Path(str(header) + ".pch")
                            binary.write_bytes(b"stale cache")
                            try:
                                for action in (self.entries, lambda: tus.dependencies(entry),
                                               lambda: tus.compile_command(entry, self.build / "isolated.o"),
                                               lambda: tus.run_checks(self.arguments("tidy"), self.root,
                                                                      [entry], self.build)):
                                    with patch.object(tus.shutil, "which", return_value="clang-tidy"), \
                                            self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                                        action()
                            finally:
                                binary.unlink()
        self.assertEqual(self.commands, [])

    def test_forced_header_search_preserves_working_directory_and_driver_order(self):
        first, second = self.root / "z headers", self.root / "a headers"
        first.mkdir()
        second.mkdir()
        for root in (self.build, first, second):
            (root / "forced.hpp").write_text("// header\n")
        self.raw = [self.raw_entry(self.a, ["-include", "forced.hpp"])]
        self.save_db()
        result = subprocess.CompletedProcess([], 0, "", '#include "..." search starts here:\n '
            + str(first) + '\n#include <...> search starts here:\n ' + str(second)
            + '\nEnd of search list.\n')
        # A cache beside a shadowed header must not change forced-header lookup.
        (second / "forced.hpp.pch").write_bytes(b"shadowed cache")
        with patch.object(tus, "compiler_header_roots", side_effect=COMPILER_HEADER_ROOTS), \
                patch.object(tus._DEPS, "run_process", return_value=result) as probe:
            self.entries()
            probe.assert_not_called()
            (first / "forced.hpp.pch").write_bytes(b"shadowed cache")
            self.entries()
            (self.build / "forced.hpp").unlink()
            with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                self.entries()
            (first / "forced.hpp.pch").unlink()
            self.entries()
            self.assertEqual(probe.call_count, 1)

    def test_missing_forced_headers_refuse_even_with_known_search_paths(self):
        (self.src / "missing.hpp").mkdir()
        for forced in ("-include", "-imacros"):
            for roots in ((), (self.src, self.build / "missing directory")):
                for name in ("missing.hpp", str(self.root / "missing.hpp")):
                    with self.subTest(forced=forced, roots=roots, name=name), \
                            patch.object(tus, "compiler_header_roots", return_value=roots):
                        self.raw = [self.raw_entry(self.a, [forced, name])]
                        self.save_db()
                        with self.assertRaisesRegex(tus.SelectionError, "cannot locate textual forced include"):
                            self.entries()
        self.assertEqual(self.commands, [])

    def test_forced_header_resolution_refuses_unknown_search_and_unreadable_paths(self):
        self.raw = [self.raw_entry(self.a, ["-include", "cstdint"])]
        self.save_db()
        results = [OSError("unavailable compiler"), subprocess.TimeoutExpired("compiler", 1800),
                   subprocess.CompletedProcess([], 0, "", ""),
                   subprocess.CompletedProcess([], 0, "", '#include <...> search starts here:\n '
                       + str(self.src) + '\n'),
                   subprocess.CompletedProcess([], 1, "", '#include <...> search starts here:\n '
                       + str(self.src) + '\nEnd of search list.\n')]
        for result in results:
            with self.subTest(result=result), \
                    patch.object(tus, "compiler_header_roots", side_effect=COMPILER_HEADER_ROOTS), \
                    patch.object(tus._DEPS, "run_process", side_effect=[result]), \
                    self.assertRaisesRegex(tus.SelectionError, "compiler header search paths"):
                self.entries()
        is_file = Path.is_file

        def readable(path):
            if path == self.src / "cstdint":
                raise PermissionError("unreadable header")
            return is_file(path)

        with patch.object(tus, "compiler_header_roots", return_value=(self.src,)), \
                patch.object(Path, "is_file", autospec=True, side_effect=readable), \
                self.assertRaisesRegex(OSError, "unreadable header"):
            self.entries()
        self.assertEqual(self.commands, [])

    def test_environment_header_roots_are_checked_before_every_replay(self):
        search = self.root / "environment headers"
        nested = search / "nested"
        nested.mkdir(parents=True)
        (nested / "current.hpp").write_text("// edited header\n")
        self.a.write_text("#include <nested/current.hpp>\n")
        for variable in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"):
            # Cover multiple components, relative paths and empty components
            # (which GCC interprets as the command's working directory).
            for value, root in ((os.pathsep.join(["missing", os.path.relpath(search, self.build)]), nested),
                                ("missing" + os.pathsep, self.build)):
                with self.subTest(variable=variable, value=value), patch.dict(os.environ, {variable: value}):
                    entry = self.entries()[0]
                    for is_directory in (False, True):
                        binary = root / "current.hpp.gch"
                        if is_directory:
                            binary.mkdir()
                        else:
                            binary.write_bytes(b"stale cache")
                        try:
                            actions = [self.entries, lambda: tus.dependencies(entry),
                                       lambda: tus.compile_command(entry, self.build / "isolated.o"),
                                       lambda: tus.run_checks(self.arguments("tidy"), self.root, [entry], self.build)]
                            with patch.object(tus.shutil, "which", return_value="clang-tidy"):
                                for action in actions:
                                    with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                                        action()
                        finally:
                            binary.rmdir() if is_directory else binary.unlink()
        self.assertEqual(self.commands, [])

    def test_compiler_default_and_system_header_roots_are_checked_at_replay(self):
        search = self.root / "compiler system headers"
        search.mkdir()
        header = search / "current.hpp"
        header.write_text("// edited header\n")
        self.a.write_text("#include <current.hpp>\n")
        for compiler in ("g++", "clang++"):
            self.raw = [self.raw_entry(self.a, ["--sysroot", "sdk", "--target=x86_64-linux-gnu",
                                               "-include", str(self.other)])]
            self.raw[0]["arguments"][0] = compiler
            self.save_db()
            entry = self.entries()[0]
            result = subprocess.CompletedProcess([], 0, "", '#include "..." search starts here:\r\n'
                '#include <...> search starts here:\r\n ' + str(search) + '\r\nEnd of search list.\r\n')
            with patch.object(tus, "compiler_header_roots", side_effect=COMPILER_HEADER_ROOTS), \
                    patch.object(tus._DEPS, "run_process", return_value=result) as probe:
                self.assertEqual(tus.replay_arguments(entry), tus.clean_arguments(entry["arguments"]))
                for is_directory in (False, True):
                    binary = Path(str(header) + ".gch")
                    if is_directory:
                        binary.mkdir()
                    else:
                        binary.write_bytes(b"stale cache")
                    try:
                        for action in (lambda: tus.dependencies(entry),
                                       lambda: tus.compile_command(entry, self.build / "isolated.o"),
                                       lambda: tus.run_checks(self.arguments("tidy"), self.root, [entry], self.build)):
                            with patch.object(tus.shutil, "which", return_value="clang-tidy"):
                                with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                                    action()
                    finally:
                        binary.rmdir() if is_directory else binary.unlink()
                self.assertEqual(probe.call_count, 1)
                for call in probe.call_args_list:
                    command = call.args[0]
                    self.assertEqual(command[0], compiler)
                    self.assertEqual(command[-5:], ["-E", "-v", "-x", "c++", "-"])
                    self.assertIn("--sysroot", command)
                    self.assertIn("sdk", command)
                    self.assertIn("--target=x86_64-linux-gnu", command)
                    self.assertNotIn(str(self.a), command)
                    self.assertNotIn(str(self.other), command)
                    self.assertNotIn("-include", command)
                    self.assertEqual(call.kwargs["env"]["LC_ALL"], "C")
                    self.assertEqual(call.kwargs["cwd"], self.build)
                    self.assertEqual(call.kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(self.commands, [])

    def test_equivalent_variants_reuse_compiler_roots_and_pch_scans(self):
        search = self.root / "compiler headers"
        (search / "nested").mkdir(parents=True)
        self.raw = [self.raw_entry(self.a), self.raw_entry(self.b, ["-DENABLE_FEATURE"])]
        self.save_db()
        entries = self.entries()
        result = subprocess.CompletedProcess([], 0, "", '#include <...> search starts here:\n '
            + str(search) + '\nEnd of search list.\n')
        temporary_directory = tempfile.TemporaryDirectory

        @contextmanager
        def cleanup_with_walk(**kwargs):
            with temporary_directory(**kwargs) as name:
                yield name
                # Windows cleanup also walks directories. Its OS calls must
                # not be mistaken for another compiler header search.
                for _ in os.walk(name):
                    pass

        with patch.object(tus, "compiler_header_roots", side_effect=COMPILER_HEADER_ROOTS), \
                patch.object(tus._DEPS, "run_process", return_value=result) as probe, \
                patch.object(tus, "walk_header_directories", wraps=os.walk) as walk, \
                patch.object(tus.tempfile, "TemporaryDirectory", side_effect=cleanup_with_walk), \
                patch.object(os, "walk", wraps=os.walk) as cleanup_walk:
            tus.dependencies(entries[0])
            self.assertGreater(walk.call_count, 0)
            first_scans = walk.call_count
            tus.dependencies(entries[1])
            tus.compile_command(entries[1], self.build / "isolated.o")
            with patch.object(tus.shutil, "which", return_value="clang-tidy"):
                tus.run_checks(self.arguments("tidy"), self.root, entries, self.build)
            self.assertEqual(walk.call_count, first_scans)
            self.assertGreater(cleanup_walk.call_count, 0)
            self.assertEqual(probe.call_count, 1)
            # Reuse must not conceal caches added below an already scanned root.
            binary = search / "nested/current.hpp.gch"
            binary.mkdir()
            with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                tus.dependencies(entries[1])
            self.assertGreater(walk.call_count, first_scans)
            refused_scans = walk.call_count
            with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                tus.dependencies(entries[0])
            self.assertEqual(walk.call_count, refused_scans)
            binary.rmdir()
            tus.dependencies(entries[1])
            self.assertEqual(probe.call_count, 1)

    def test_compiler_search_probe_refuses_unavailable_or_unknown_defaults(self):
        entry = self.entries()[0]
        results = [OSError("compiler unavailable"),
                   subprocess.TimeoutExpired("compiler", 1800),
                   subprocess.CompletedProcess([], 1, "", "unsupported target"),
                   subprocess.CompletedProcess([], 0, "", "unrecognized search output"),
                   subprocess.CompletedProcess([], 0, "", '#include <...> search starts here:\n /headers\n')]
        for result in results:
            with self.subTest(result=result), \
                    patch.object(tus, "compiler_header_roots", side_effect=COMPILER_HEADER_ROOTS), \
                    patch.object(tus._DEPS, "run_process", side_effect=[result]):
                with self.assertRaisesRegex(tus.SelectionError, "compiler header search paths"):
                    tus.replay_arguments(entry)
        self.assertEqual(self.commands, [])

    def test_pch_cache_tracks_dangling_directory_symlink_targets(self):
        search = self.root / "header search"
        link = search / "linked headers"
        source = search / "unit.cpp"
        entry = {"arguments": ["g++", "-c", str(source)], "directory": self.build}
        stamp = tus.directory_stamp(self.src)
        target_stamp = None
        frames = [(str(search), [], [source.name, link.name])]

        def entries(path, names=None):
            for current, children, files in frames:
                if Path(current) == path:
                    return tuple((name, stat.S_IFLNK if name == link.name else
                                  stat.S_IFDIR if name in children else stat.S_IFREG, 1, 1)
                                 for name in sorted(children + files))
            self.fail(f"unexpected directory: {path}")

        # Model os.walk's dangling-link classification on every platform,
        # including Windows hosts without permission to create symlinks.
        with patch.object(tus, "directory_stamp", side_effect=lambda path:
                          target_stamp if path == link else stamp), \
                patch.object(Path, "is_symlink", autospec=True,
                             side_effect=lambda path: path == link), \
                patch.object(tus, "directory_entries", side_effect=entries), \
                patch.object(tus, "walk_header_directories", side_effect=lambda *args, **kwargs: iter(frames)) as walk:
            tus.replay_arguments(entry)
            tus.replay_arguments(entry)
            self.assertEqual(walk.call_count, 1)
            for is_directory in (False, True):
                with self.subTest(directory=is_directory):
                    # Only the target appears: the parent stamp and membership
                    # are unchanged, so the link's target must invalidate reuse.
                    target_stamp = stamp
                    binary = "current.hpp.gch"
                    frames = [(str(search), [link.name], [source.name]),
                              (str(link), [binary] if is_directory else [],
                               [] if is_directory else [binary])]
                    with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                        tus.replay_arguments(entry)
                    target_stamp = None
                    frames = [(str(search), [], [source.name, link.name])]
                    tus.replay_arguments(entry)
                    scans = walk.call_count
                    tus.replay_arguments(entry)
                    self.assertEqual(walk.call_count, scans)
        self.assertEqual(self.commands, [])

    def test_pch_cache_detects_changes_with_identical_directory_timestamps(self):
        search = self.root / "compiler headers"
        (search / "nested").mkdir(parents=True)
        entry = self.entries()[0]
        stamp = tus.directory_stamp
        snapshots = {}

        def same_tick(path):
            if path not in snapshots:
                snapshots[path] = stamp(path)
            return snapshots[path]

        with patch.object(tus, "compiler_header_roots", return_value={search}), \
                patch.object(tus, "directory_stamp", side_effect=same_tick), \
                patch.object(tus.time, "time_ns", return_value=search.stat().st_mtime_ns):
            tus.replay_arguments(entry)
            for is_directory in (False, True):
                with self.subTest(directory=is_directory):
                    binary = search / "nested/current.hpp.gch"
                    if is_directory:
                        binary.mkdir()
                    else:
                        binary.write_bytes(b"stale cache")
                    with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                        tus.replay_arguments(entry)
                    binary.rmdir() if is_directory else binary.unlink()
                    tus.replay_arguments(entry)
        self.assertEqual(self.commands, [])

    def test_pch_cache_detects_same_tick_insertion_on_delayed_replay(self):
        tick = 10_000_000_000
        entry = self.entries()[0]
        original_stamp = tus.directory_stamp
        for compiler in ("g++", "clang++"):
            for is_directory in (False, True):
                with self.subTest(compiler=compiler, directory=is_directory):
                    search = self.root / f"headers-{compiler}-{is_directory}"
                    (search / "nested").mkdir(parents=True)
                    snapshots = {}

                    def same_tick(path):
                        if path not in snapshots:
                            stamp = original_stamp(path)
                            snapshots[path] = (*stamp[:4], tick, tick) if stamp else None
                        return snapshots[path]

                    variant = {**entry, "arguments": [compiler, *entry["arguments"][1:]]}
                    with patch.object(tus, "compiler_header_roots", return_value={search}), \
                            patch.object(tus, "directory_stamp", side_effect=same_tick), \
                            patch.object(tus.time, "time_ns", return_value=tick + 500_000_000) as clock:
                        tus.replay_arguments(variant)
                        binary = search / "nested/current.hpp.gch"
                        binary.mkdir() if is_directory else binary.write_bytes(b"stale cache")
                        # The insertion shared the walk's timestamp tick, but
                        # no replay occurs until that tick has already closed.
                        clock.return_value = tick + 2_000_000_001
                        with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                            tus.replay_arguments(variant)
        self.assertEqual(self.commands, [])

    def test_pch_cache_promotes_only_after_membership_check_outside_tick(self):
        search = self.root / "header search"
        search.mkdir()
        tick = 10_000_000_000
        stamp = (*tus.directory_stamp(search)[:4], tick, tick)
        with patch.object(tus, "directory_stamp", return_value=stamp), \
                patch.object(tus.time, "time_ns", return_value=tick + 500_000_000) as clock, \
                patch.object(tus.os, "listdir", wraps=os.listdir) as membership, \
                patch.object(tus, "walk_header_directories", wraps=os.walk) as walk:
            tus.check_pch_search_roots("g++", {search})
            self.assertEqual(membership.call_count, 0)
            tus.check_pch_search_roots("g++", {search})
            self.assertEqual(membership.call_count, 1)
            clock.return_value = tick + 2_000_000_000
            tus.check_pch_search_roots("g++", {search})
            self.assertEqual(membership.call_count, 2)
            tus.check_pch_search_roots("g++", {search})
            self.assertEqual(membership.call_count, 2)
            self.assertEqual(walk.call_count, 1)

    def test_pch_cache_trusts_walk_taken_after_tick_closes(self):
        search = self.root / "header search"
        search.mkdir()
        tick = 10_000_000_000
        stamp = (*tus.directory_stamp(search)[:4], tick, tick)
        with patch.object(tus, "directory_stamp", return_value=stamp), \
                patch.object(tus.time, "time_ns", return_value=tick + 2_000_000_000), \
                patch.object(tus.os, "listdir", wraps=os.listdir) as membership, \
                patch.object(tus, "walk_header_directories", wraps=os.walk) as walk:
            tus.check_pch_search_roots("g++", {search})
            tus.check_pch_search_roots("g++", {search})
            self.assertEqual(membership.call_count, 0)
            self.assertEqual(walk.call_count, 1)

    def test_compiler_search_probe_uses_effective_language(self):
        result = subprocess.CompletedProcess([], 0, "", '#include <...> search starts here:\nEnd of search list.\n')
        for compiler in ("gcc", "g++", "clang", "clang++"):
            for suffix in (".c", ".cpp"):
                for language in (None, "c", "c++"):
                    source = self.src / ("unit" + suffix)
                    arguments = [compiler, *(["-x", language] if language else []), str(source)]
                    expected = language or ("c++" if "++" in compiler or suffix == ".cpp" else "c")
                    for trailing in ([], ["-x", "c"], ["-x", "c++"]):
                        # Exercise each probe independently; equivalent probes
                        # are intentionally memoized during normal replay.
                        tus._compiler_header_roots.cache_clear()
                        with self.subTest(arguments=arguments, trailing=trailing), \
                                patch.object(tus._DEPS, "run_process", return_value=result) as run:
                            self.assertEqual(COMPILER_HEADER_ROOTS(arguments + trailing, self.build), ())
                            self.assertEqual(run.call_args.args[0][-3:], ["-x", expected, "-"])

    def test_msvc_pch_replay_is_refused_before_execution(self):
        for compiler in ("cl", "cl.exe", "clang-cl", "g++", "clang++"):
            for flags in (["/Yu", "header.hpp"], ["/Yuheader.hpp"],
                          ["/Fp", "cached.pch"], ["/Fpcached.pch"],
                          ["/Yu", "header.hpp", "/Fp", "cached.pch"],
                          ["/Yuheader.hpp", "/Fpcached.pch"]):
                with self.subTest(compiler=compiler, flags=flags):
                    self.raw = [self.raw_entry(self.a, flags)]
                    self.raw[0]["arguments"][0] = compiler
                    self.save_db()
                    with self.assertRaisesRegex(tus.SelectionError, "unsupported"):
                        self.entries()
        self.assertEqual(self.commands, [])

    def test_binary_pch_created_after_database_read_is_refused_by_every_replay(self):
        header = self.build / "cmake_pch.hxx"
        header.write_text("// current textual header\n")
        self.raw = [self.raw_entry(self.a, ["-include", str(header)])]
        self.save_db()
        entry = self.entries()[0]
        actions = [lambda: tus.dependencies(entry),
                   lambda: tus.run_checks(self.arguments(), self.root, [entry], self.build),
                   lambda: tus.run_checks(self.arguments("tidy", also_ci_gate=True),
                                          self.root, [entry], self.build)]
        for suffix in (".gch", ".pch"):
            binary = Path(str(header) + suffix)
            binary.mkdir()
            with patch.object(tus.shutil, "which", return_value="clang-tidy"):
                for index, action in enumerate(actions):
                    with self.subTest(suffix=suffix, replay=index):
                        with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                            action()
            binary.rmdir()
        self.assertEqual(self.commands, [])
        self.assertEqual(list((self.root / "scratch").glob("local-checks-*")), [])

    def test_forced_include_search_cannot_bypass_binary_cache_check(self):
        header = self.src / "ordinary.hpp"
        Path(str(header) + ".gch").write_bytes(b"stale binary header")
        for option in ("-include", "-imacros"):
            with self.subTest(option=option), \
                    patch.object(tus, "compiler_header_roots", return_value=(self.src,)):
                self.raw = [self.raw_entry(self.a, [option, header.name])]
                self.save_db()
                with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                    self.entries()
        self.assertEqual(self.commands, [])

    def test_relative_textual_forced_include_is_checked_beside_command_directory(self):
        header = self.build / "ordinary.hpp"
        header.write_text("// current textual header\n")
        self.raw = [self.raw_entry(self.a, ["-include", header.name])]
        self.save_db()
        self.assertIn(header.name, self.entries()[0]["arguments"])
        Path(str(header) + ".gch").mkdir()
        with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
            self.entries()
        self.assertEqual(self.commands, [])

    def test_pch_refusals_are_bounded_and_leave_state_unchanged(self):
        header = self.build / "cmake_pch.hxx"
        header.write_text("// textual header\n")
        cases = [
            ["-Xclang"], ["-Xclang", "-include"], ["-Xclang", "-include", "-Xclang"],
            ["-Xclang", "-include", "-Xclang", ""],
            ["-Xclang", "-include", "-Xclang", "@rsp"],
            ["-Xclang", "-include", "-Xclang", "0"],
            ["-Xclang", "-include", "-Xclang", str(self.other)],
            ["-Xclang", "-include", "-Xclang", str(self.build / "missing.hxx")],
            ["-include-pch"], ["-include-pch", str(header) + ".pch"],
            ["-include-pch", str(self.other) + ".pch"],
            ["-include-pch", str(header) + ".gch", "-Xclang", "-include", "-Xclang", str(header)],
            ["-include-pch", str(self.build / "different.pch"),
             "-Xclang", "-include", "-Xclang", str(header)],
        ]
        for flags in cases:
            with self.subTest(flags=flags):
                self.raw = [self.raw_entry(self.a, flags)]
                self.save_db()
                before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
                with self.assertRaises(tus.SelectionError):
                    self.entries()
                self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})
        self.assertEqual(self.commands, [])

    def test_output_options_are_removed_or_rejected_before_any_execution(self):
        outside = str(self.root / "outside")
        rejected = [
            ["-serialize-diagnostics", outside], ["--serialize-diagnostics", outside],
            ["-serialize-diagnostics=" + outside], ["--serialize-diagnostics=" + outside],
            ["-foptimization-record-file=" + outside], ["-fsave-optimization-record"],
            ["-save-temps"], ["-save-temps=obj"], ["-ftime-trace"], ["-ftime-trace=" + outside],
            ["-fprofile-generate"], ["-fprofile-generate=" + outside],
            ["-fprofile-instr-generate"], ["-fprofile-instr-generate=" + outside],
            ["-instr-generate=" + outside], ["-fdump-tree-all"], ["-fdump-tree-all=" + outside],
            ["-unknown-future-output=" + outside], ["-Xclang", "-emit-pch"],
            ["-Wp,-MD," + outside], ["--output=" + outside], ["-fcoverage-notes-file=" + outside],
            ["-x", "c++-header"],
        ]
        for extra in rejected:
            with self.subTest(extra=extra):
                self.raw[0] = self.raw_entry(self.a, extra)
                self.save_db()
                with self.assertRaisesRegex(tus.SelectionError, "unsupported"):
                    self.entries()
        for extra in (["-o", outside], ["-o" + outside], ["-MF", outside], ["-MF" + outside],
                      ["-MD", "-MF", outside], ["-MMD", "-MF", outside],
                      ["-MJ", outside], ["-MT", outside], ["-MQ" + outside]):
            with self.subTest(extra=extra):
                cleaned = tus.clean_arguments(["g++", *extra, str(self.a)])
                self.assertEqual(cleaned, ["g++", str(self.a)])
        for flag in ("-o", "-MF", "-MT", "-MQ", "-MJ", "-I", "-include", "-x"):
            with self.subTest(truncated=flag):
                with self.assertRaisesRegex(tus.SelectionError, "missing value"):
                    tus.clean_arguments(["g++", flag])
        self.assertEqual(self.commands, [])
        self.assertFalse(Path(outside).exists())

    def test_option_values_never_count_as_database_source_operands(self):
        for compiler in ("g++", "clang++"):
            for option in sorted(option for option, policy in tus.VALUE_OPTIONS.items()
                                 if policy != "refuse" and option != "-x"):
                with self.subTest(compiler=compiler, option=option):
                    # The database source is only an option value; b is the
                    # actual positional source. This must fail before scanning.
                    self.raw = [self.raw_entry(self.a)]
                    self.raw[0]["arguments"] = [compiler, option, str(self.a), str(self.b)]
                    self.save_db()
                    before = (self.build / "compile_commands.json").read_bytes()
                    with self.assertRaisesRegex(tus.SelectionError, "exactly one database source"):
                        self.entries()
                    self.assertEqual((self.build / "compile_commands.json").read_bytes(), before)
        self.assertEqual(self.commands, [])

    def test_positional_source_after_include_and_language_values_is_accepted(self):
        for compiler in ("g++", "clang++"):
            for option in ("-imacros", "-include"):
                with self.subTest(compiler=compiler, option=option):
                    self.raw = [self.raw_entry(self.b)]
                    self.raw[0]["arguments"] = [compiler, option, str(self.a), "-x", "c++",
                                                os.path.relpath(self.b, self.build)]
                    self.save_db()
                    selected, _ = self.select([self.b])
                    self.assertEqual([entry["file"] for entry in selected], [self.b])
                    command = tus.compile_command(selected[0], self.build / "isolated.o")
                    self.assertEqual(command[1:5], [option, str(self.a), "-x", "c++"])
        self.assertEqual(len(self.commands), 8)
        self.assertTrue(all("-M" in command for command, _ in self.commands))

    def test_database_requires_one_source_and_refuses_ambiguous_value_options(self):
        for arguments in (
            ["g++", "-imacros", str(self.a)],
            ["g++", str(self.a), str(self.b)],
            ["g++", str(self.a), str(self.a)],
            ["g++", "-x", str(self.a), str(self.b)],
            ["g++", "-unknown-value-option", str(self.a), str(self.b)],
            *(["clang++", flag, str(self.a), str(self.b)] for flag in (
                "-include-pch", "-Xclang", "/Fo", "/Fi", "/FI", "/I")),
            *([compiler, "/Fo", str(self.a), "/Fi" + str(self.a), str(self.b)]
              for compiler in ("clang-cl", "cl", "cl.exe")),
        ):
            with self.subTest(arguments=arguments):
                self.raw = [self.raw_entry(self.a)]
                self.raw[0]["arguments"] = arguments
                self.save_db()
                with self.assertRaises(tus.SelectionError):
                    self.entries()
        for flag in sorted(tus.INPUT_VALUE_OPTIONS):
            with self.subTest(truncated=flag):
                with self.assertRaisesRegex(tus.SelectionError, "missing value"):
                    tus.clean_arguments(["g++", flag])
        self.assertEqual(self.commands, [])

    def test_literal_zero_option_value_is_preserved_without_becoming_a_source(self):
        command, operands = tus.parse_arguments(["g++", "-D", "0", str(self.a)])
        self.assertEqual(command, ["g++", "-D", "0", str(self.a)])
        self.assertEqual(operands, [str(self.a)])

    def test_cli_source_operand_refusal_prints_reason_without_writes(self):
        self.raw[0]["arguments"] = ["g++", "-imacros", str(self.a), str(self.b)]
        self.save_db()
        database = self.build / "compile_commands.json"
        before = database.read_bytes()
        stream = io.StringIO()
        with patch.object(tus, "changed_paths", return_value={self.a}), redirect_stderr(stream):
            self.assertEqual(tus.main(["compile", "--root", str(self.root),
                                       "--build-dir", str(self.build)]), 1)
        self.assertIn("exactly one database source", stream.getvalue())
        self.assertEqual(database.read_bytes(), before)
        self.assertEqual(self.commands, [])
        self.assertEqual(list((self.root / "scratch").glob("local-checks-*")), [])

    def test_compile_rewrites_outputs_preserves_configuration_and_never_links(self):
        entry = self.entries()[0]
        entry["arguments"] += ["-MMD", "-MTold", "-MQold", "-MJ", "old.json", "-oother.o"]
        output = self.build / "isolated object.o"
        command = tus.compile_command(entry, output, True)
        self.assertEqual(command[:2], ["ccache", "/usr/bin/g++"])
        self.assertEqual(command[-3:], ["-c", "-o", str(output)])
        for flag in ("-O3", "-DNDEBUG", "-ffp-contract=off"):
            self.assertIn(flag, command)
        for removed in ("original.o", "original.d", "old.json", "-oother.o", "-MMD"):
            self.assertNotIn(removed, command)
        self.assertNotIn("-Wconversion", command)

    def cmake_warning_flags(self):
        text = (ROOT / "CMakeLists.txt").read_text()
        policy = re.search(r"function\(luminumbra_configure_first_party_target\b(.*?)endfunction\(\)",
                           text, re.S)
        self.assertIsNotNone(policy)
        # Read every explicit GCC/Clang warning option, including all exceptions,
        # from the same helper CI configures with warnings-as-errors enabled.
        return re.findall(r"(?<!\S)-W[a-zA-Z0-9=-]+", policy[1])

    def warning_options(self, command):
        return [arg for arg in command if arg.startswith("-W") or arg in {"-w", "-pedantic", "-pedantic-errors"}]

    def test_client_exclusion_and_strict_target_match_cmake_warning_definitions(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        routed = re.search(r"foreach\(FIRST_PARTY_TARGET IN ITEMS(.*?)\)\s*"
                           r"luminumbra_configure_first_party_target\(\$\{FIRST_PARTY_TARGET\}\)", cmake, re.S)
        self.assertIsNotNone(routed)
        targets = routed[1].split()
        self.assertIn("asset_processor", targets)
        self.assertNotIn("luminumbra_client", targets)
        self.assertNotIn("luminumbra_client_qa", targets)
        for target in ("luminumbra_client", "luminumbra_client_qa"):
            self.assertIn(f"luminumbra_enable_coverage({target})", cmake)
            for script in ("CMakeLists.txt", "src/CMakeLists.txt"):
                self.assertNotRegex((ROOT / script).read_text(),
                                    r"luminumbra_configure_first_party_target\(\s*" + target + r"\s*\)")
        strict_flags = self.cmake_warning_flags()
        self.assertIn("-Wall", strict_flags)
        self.assertIn("-Wextra", strict_flags)
        self.assertIn("-Werror", strict_flags)
        client = self.src / "luminumbra_client/audio/MiniaudioManager.cpp"
        client.parent.mkdir(parents=True)
        client.write_text("// client policy fixture\n")
        strict = self.root / "tools/asset_processor.cpp"
        strict.parent.mkdir()
        strict.write_text("// strict policy fixture\n")
        self.raw = [self.raw_entry(client), self.raw_entry(strict, strict_flags)]
        self.save_db()
        selected, _ = self.select([client, strict])
        tus.run_checks(self.arguments(), self.root, selected, self.build)
        self.assertEqual(len(self.check_commands()), 2)
        for command, _ in self.check_commands():
            self.assertEqual(self.warning_options(command), [] if str(client) in command else strict_flags)

    def test_warning_exceptions_match_all_explicit_ci_definitions(self):
        flags = self.cmake_warning_flags()
        definitions = "\n".join((ROOT / path).read_text()
                                for path in ("CMakeLists.txt", ".github/workflows/ci.yml"))
        expected = set(re.findall(r"-Wno-error(?:=[a-zA-Z0-9-]+)?", definitions))
        self.assertEqual({f for f in flags if f.startswith("-Wno-error")}, expected)
        # Preserve the exact nonfatal unused-parameter policy for the strict
        # target; a switch warning remains absent for the excluded client.
        self.a.write_text("void callback(int unused_named_parameter) {}\n")
        self.raw = [self.raw_entry(self.a, flags)]
        self.save_db()
        command = tus.compile_command(self.entries()[0], self.build / "a.o")
        self.assertEqual(self.warning_options(command), flags)
        self.assertIn("-Wno-error=unused-parameter", command)
        self.assertNotIn("-Werror=unused-parameter", command)
        self.assertGreater(command.index("-Wno-error=unused-parameter"), command.index("-Werror"))

    def test_each_variant_preserves_every_warning_flag_and_override_order(self):
        flags = self.cmake_warning_flags() + [
            "-Wno-switch", "-Wswitch", "-Wno-error=switch", "-Werror=switch",
            "-Wshadow=global", "-Wconversion", "-Wsign-conversion", "-Wnarrowing",
            "-Wunused", "-Wmissing-field-initializers", "-Wno-error=deprecated-declarations",
            "-w", "-pedantic", "-pedantic-errors"]
        self.raw = [self.raw_entry(self.a), self.raw_entry(self.a, flags)]
        self.save_db()
        selected, _ = self.select([self.a])
        tus.run_checks(self.arguments(), self.root, selected, self.build)
        self.assertEqual([self.warning_options(c) for c, _ in self.compile_commands_by_variant()], [[], flags])

    def test_c_source_does_not_receive_cxx_narrowing_option(self):
        source = self.src / "plain.c"
        source.write_text("C source")
        entry = {"file": source, "directory": self.build, "arguments": ["gcc", str(source)]}
        command = tus.compile_command(entry, self.build / "plain.o")
        self.assertFalse(any("narrowing" in flag for flag in command))

    def test_warning_profile_and_clean_prefix_options_parse(self):
        for profile in ("none", "msvc-approx"):
            options = ["--warning-profile", profile]
            if profile == "msvc-approx":
                options += ["--conversion-clean-prefix", "src"]
            with self.subTest(profile=profile), patch.object(tus, "changed_paths", return_value={self.a}):
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(tus.main(["compile", "--root", str(self.root),
                                               "--build-dir", str(self.build), *options]), 0)

    def test_profile_mapping_is_complete_documented_and_stably_layered_per_variant(self):
        docs = (ROOT / "docs/ci/local-checks.md").read_text()
        self.assertEqual([row["warning"] for row in tus.MSVC_APPROX],
                         ["C4456", "C4457", "C4458", "C4459", "C4244", "C4267"])
        for row in tus.MSVC_APPROX:
            self.assertIn(row["warning"], docs)
            self.assertTrue(row["rationale"])
            self.assertTrue(row["gcc"])
            self.assertTrue(row["clang"])
        self.raw = [self.raw_entry(self.a, self.cmake_warning_flags()),
                    self.raw_entry(self.a, ["-DFEATURE", "-Wno-error"])]
        self.save_db()
        selected, _ = self.select([self.a])
        def reverse_parallel(function, items, jobs):
            return [function(item) for item in reversed(items)][::-1]

        with patch.object(tus, "parallel", side_effect=reverse_parallel):
            tus.run_checks(self.arguments(warning_profile="msvc-approx"), self.root, selected, self.build)
        commands = self.compile_commands_by_variant()
        self.assertEqual([int(Path(command[-1]).stem) for command, _ in commands], [0, 1])
        for entry, (command, _) in zip(selected, commands):
            replay = tus.clean_arguments(entry["arguments"])
            self.assertEqual(command[:len(replay)], replay)
            self.assertEqual(command[len(replay):len(replay) + 4],
                             ["-Wshadow", "-Wconversion", "-Wnarrowing", "-Wsign-conversion"])
            self.assertEqual(command[-3], "-c")
        self.assertEqual(len(commands), 2)

    def test_profile_preserves_target_exceptions_and_existing_warning_families(self):
        for flags in ([], self.cmake_warning_flags(), ["-Wno-error"],
                      ["-Wno-shadow"], ["-Wno-error=shadow-local"], ["-Wshadow=local"],
                      ["-Wno-conversion"], ["-Wno-error=sign-conversion"],
                      ["-Wno-implicit-int-conversion"], ["-Wno-shorten-64-to-32"],
                      ["-Wno-error=c++11-narrowing"], ["-Wconversion"],
                      ["-Werror=conversion"], ["-w"], ["-Wno-everything"]):
            with self.subTest(flags=flags):
                entry = self.entries()[0]
                entry["arguments"] += flags
                replay = tus.clean_arguments(entry["arguments"])
                command = tus.compile_command(entry, self.build / "a.o", warning_profile="msvc-approx")
                self.assertEqual(command[:len(replay)], replay)
                additions = command[len(replay):-3]
                self.assertFalse(any(f == "-Werror" or f.startswith("-Werror=") for f in additions))
                for family in ("shadow", "conversion"):
                    if any(tus.warning_family(f) == family for f in flags):
                        self.assertFalse(any(tus.warning_family(f) == family for f in additions))
                if "-w" in flags or "-Wno-everything" in flags:
                    self.assertEqual(tus.profile_flags(entry, "msvc-approx"), [])

    def test_profile_language_and_compiler_mapping_including_explicit_overrides(self):
        for compiler in ("gcc", "g++", "clang", "clang++"):
            for extension in (".c", ".cpp"):
                for language in (None, "c", "c++"):
                    with self.subTest(compiler=compiler, extension=extension, language=language):
                        source = self.src / ("plain" + extension)
                        entry = {"file": source, "directory": self.build,
                                 "arguments": [compiler] + (["-x", language] if language else []) + [str(source)]}
                        flags = tus.profile_flags(entry, "msvc-approx")
                        cxx = language == "c++" if language else extension == ".cpp" or "++" in compiler
                        self.assertEqual("-Wnarrowing" in flags, cxx)
                        self.assertEqual(flags[0], "-Wshadow-all" if "clang" in compiler else "-Wshadow")
        # Language overrides affect only subsequent input files, and values of
        # other flags must not be mistaken for the translation-unit operand.
        entry = {"file": self.src / "plain.c", "directory": self.build,
                 "arguments": ["gcc", "-include", str(self.src / "plain.c"),
                               "-x", "c++", str(self.src / "plain.c"), "-x", "c"]}
        self.assertIn("-Wnarrowing", tus.profile_flags(entry, "msvc-approx"))
        entry["arguments"] = ["gcc", str(entry["file"]), "-x", "c++"]
        self.assertNotIn("-Wnarrowing", tus.profile_flags(entry, "msvc-approx"))

    def test_profile_identifies_generic_compiler_aliases_and_refuses_unknown_drivers(self):
        for driver in ("cc", "c++", "aarch64-linux-gnu-cc"):
            for version, expected in (("Apple clang version 18", "-Wshadow-all"),
                                      ("cc 14\nCopyright Free Software Foundation", "-Wshadow")):
                with self.subTest(driver=driver, version=version):
                    entry = self.entries()[0]
                    entry["arguments"][0] = driver
                    self.mock_execute.reset_mock()
                    self.mock_execute.side_effect = None
                    self.mock_execute.return_value = subprocess.CompletedProcess([], 0, version, "")
                    command = tus.check_arguments(entry, "msvc-approx")
                    self.assertIn(expected, command)
                    tus.check_arguments(entry, "msvc-approx")
                    self.mock_execute.assert_called_once_with([driver, "--version"], self.build)
        entry = self.entries()[0]
        entry["arguments"][0] = "cc"
        self.mock_execute.return_value = subprocess.CompletedProcess([], 0, "unknown driver", "")
        with self.assertRaisesRegex(tus.SelectionError, "cannot identify"):
            tus.check_arguments(entry, "msvc-approx")

    def diagnostic_result(self, option="conversion", message="value may change", line=1,
                          source=None, returncode=1, clang=False):
        flag = "-Werror,-W" if clang else "-Werror="
        text = f"{source or self.a}:{line}:3: error: {message} [{flag}{option}]\n"
        return subprocess.CompletedProcess([], returncode, "", text)

    def baseline_records(self, entry, result=None):
        records, errors, unknown = tus.profile_diagnostics(
            result or self.diagnostic_result(), entry, "msvc-approx", self.root, ())
        self.assertEqual(records, errors)
        self.assertFalse(unknown)
        return records

    def test_calibration_accepts_only_exact_baseline_profile_diagnostics(self):
        entry = self.entries()[0]
        entry["arguments"] += self.cmake_warning_flags()
        baseline = self.baseline_records(entry)
        args = self.arguments(warning_profile="msvc-approx", baseline_records=baseline)
        self.mock_execute.return_value = self.diagnostic_result()
        self.mock_execute.side_effect = None
        with redirect_stdout(io.StringIO()) as output:
            tus.run_checks(args, self.root, [entry], self.build)
        self.assertIn("Accepted 1 calibrated", output.getvalue())
        mutations = [self.diagnostic_result(message="a new conversion"),
                     self.diagnostic_result(option="shadow"), self.diagnostic_result(line=2),
                     self.diagnostic_result(option="unused-parameter"),
                     self.diagnostic_result(source=self.b),
                     self.diagnostic_result(returncode=2)]
        for result in mutations:
            with self.subTest(result=result):
                self.mock_execute.return_value = result
                with self.assertRaises(tus.SelectionError):
                    tus.run_checks(args, self.root, [entry], self.build)
        self.mock_execute.return_value = self.diagnostic_result()
        for mutation in ("source", "variant"):
            with self.subTest(mutation=mutation):
                if mutation == "source":
                    self.a.write_text("// edited offending line\n")
                else:
                    self.a.write_text("// fixture\n")
                    entry["arguments"].append("-DSECOND_VARIANT")
                with self.assertRaises(tus.SelectionError):
                    tus.run_checks(args, self.root, [entry], self.build)
        self.assertEqual(list((self.root / "scratch").glob("local-checks-*")), [])

    def test_calibration_never_hides_additional_or_unknown_compiler_errors(self):
        entry = self.entries()[0]
        baseline = self.baseline_records(entry)
        args = self.arguments(warning_profile="msvc-approx", baseline_records=baseline)
        for suffix in (self.diagnostic_result(message="new diagnostic").stderr,
                       "cc1plus: fatal error: missing input\n", "error: unsupported flag\n",
                       f"{self.a}:1:3: error: syntax error\n"):
            result = self.diagnostic_result()
            result.stderr += suffix
            self.mock_execute.side_effect = None
            self.mock_execute.return_value = result
            with self.subTest(suffix=suffix), self.assertRaises(tus.SelectionError):
                tus.run_checks(args, self.root, [entry], self.build)

    def test_calibration_rejects_partial_streams_and_checks_native_policy_on_replay(self):
        entry = self.entries()[0]
        baseline = self.baseline_records(entry)
        args = self.arguments(warning_profile="msvc-approx", baseline_records=baseline)
        self.mock_execute.side_effect = tus.SelectionError("native policy failed")
        with self.assertRaisesRegex(tus.SelectionError, "native policy failed"):
            tus.run_checks(args, self.root, [entry], self.build)
        self.assertEqual(self.mock_execute.call_count, 1)
        for options, partial in ((["-Wfatal-errors"], True),
                                 (["-Wfatal-errors", "-Wno-fatal-errors"], False),
                                 (["-Wno-fatal-errors", "-Wfatal-errors"], True)):
            variant = dict(entry, arguments=entry["arguments"] + options)
            with self.subTest(options=options):
                _, errors, unknown = tus.profile_diagnostics(
                    self.diagnostic_result(), variant, "msvc-approx", self.root, ())
                self.assertEqual(len(errors), 1)
                self.assertEqual(unknown, partial)
        result = self.diagnostic_result()
        result.stderr += "fatal error: too many errors emitted, stopping now [-ferror-limit=]\n"
        self.assertTrue(tus.profile_diagnostics(result, entry, "msvc-approx", self.root, ())[2])

    def test_diagnostic_location_boundaries_fail_closed(self):
        entry = self.entries()[0]
        for line in (0, 2):
            with self.subTest(line=line):
                records, errors, unknown = tus.profile_diagnostics(
                    self.diagnostic_result(line=line), entry, "msvc-approx", self.root, ())
                self.assertEqual((records, errors, unknown), ([], [], True))
        zero = self.root / "0"
        zero.mkdir()
        self.assertEqual(tus.clean_prefixes(self.root, [Path("0"), Path("0")]), (zero,))
        records, _, unknown = tus.profile_diagnostics(
            self.diagnostic_result(), entry, "msvc-approx", self.root, (zero,))
        self.assertEqual(len(records), 1)
        self.assertFalse(unknown)

    def test_calibration_handles_clang_diagnostics_and_keeps_native_categories_blocking(self):
        entry = self.entries()[0]
        entry["arguments"][0] = "clang++"
        result = self.diagnostic_result(option="shorten-64-to-32", clang=True)
        self.assertEqual(self.baseline_records(entry, result)[0]["option"], "shorten-64-to-32")
        entry["arguments"].append("-Wno-error=sign-conversion")
        records, errors, unknown = tus.profile_diagnostics(result, entry, "msvc-approx", self.root, ())
        self.assertEqual((records, errors, unknown), ([], [], True))

    def test_clean_prefix_overrides_baseline_for_tu_and_included_header(self):
        entry = self.entries()[0]
        baseline = self.baseline_records(entry)
        self.mock_execute.side_effect = None
        self.mock_execute.return_value = self.diagnostic_result()
        args = self.arguments(warning_profile="msvc-approx", baseline_records=baseline,
                              clean_prefixes=tus.clean_prefixes(self.root, [Path("src")]))
        with self.assertRaises(tus.SelectionError):
            tus.run_checks(args, self.root, [entry], self.build)
        header_result = self.diagnostic_result(source=self.generated)
        records, errors, unknown = tus.profile_diagnostics(
            header_result, entry, "msvc-approx", self.root, args.clean_prefixes)
        self.assertEqual((records, errors, unknown), ([], [], True))
        # The clean conversion scope does not silently suppress shadow checks.
        records, errors, unknown = tus.profile_diagnostics(
            self.diagnostic_result(option="shadow"), entry, "msvc-approx", self.root, args.clean_prefixes)
        self.assertEqual(len(errors), 1)
        self.assertFalse(unknown)

    def test_calibration_capture_requires_native_policy_to_pass_and_cleans_failures(self):
        entry = self.entries()[0]
        args = self.arguments(warning_profile="msvc-approx", write_warning_baseline=self.build / "baseline.json")
        self.mock_execute.side_effect = [subprocess.CompletedProcess([], 0, "", ""), self.diagnostic_result()]
        with redirect_stdout(io.StringIO()):
            records = tus.run_checks(args, self.root, [entry], self.build)
        self.assertEqual(records, self.baseline_records(entry))
        calls = self.mock_execute.call_args_list
        self.assertNotIn("-Wconversion", calls[0].args[0])
        self.assertIn("-Wconversion", calls[1].args[0])
        self.mock_execute.reset_mock()
        self.mock_execute.side_effect = tus.SelectionError("native policy failed")
        with self.assertRaisesRegex(tus.SelectionError, "native policy failed"):
            tus.run_checks(args, self.root, [entry], self.build)
        self.assertEqual(self.mock_execute.call_count, 1)
        self.assertFalse(args.write_warning_baseline.exists())
        self.assertEqual(list((self.root / "scratch").glob("local-checks-*")), [])

    def test_profile_baseline_runs_real_execute_contract_and_preserves_nonfatal_targets(self):
        entry = self.entries()[0]
        args = self.arguments(warning_profile="msvc-approx", baseline_records=self.baseline_records(entry))
        with patch.object(tus, "execute", side_effect=EXECUTE), patch.object(tus._DEPS, "run_process") as run:
            run.side_effect = [subprocess.CompletedProcess([], 0, "", ""), self.diagnostic_result()]
            with redirect_stdout(io.StringIO()):
                tus.run_checks(args, self.root, [entry], self.build)
            self.assertEqual(run.call_count, 2)
            run.reset_mock()
            run.side_effect = [subprocess.CompletedProcess([], 1, "", "native policy error")]
            with self.assertRaisesRegex(tus.SelectionError, "native policy error"):
                tus.run_checks(args, self.root, [entry], self.build)
            self.assertEqual(run.call_count, 1)
            # A target without -Werror keeps added warnings advisory.
            result = self.diagnostic_result(returncode=0)
            result.stderr = result.stderr.replace(": error:", ": warning:").replace("-Werror=", "-W")
            run.side_effect = [result]
            with redirect_stdout(io.StringIO()) as output:
                tus.run_checks(self.arguments(warning_profile="msvc-approx"), self.root, [entry], self.build)
            self.assertIn(": warning:", output.getvalue())
            self.assertNotIn("Accepted", output.getvalue())

    def test_baseline_publication_is_exclusive_and_removes_temporary_on_failure(self):
        destination = self.build / "baseline.json"
        records = self.baseline_records(self.entries()[0])
        tus.publish_warning_baseline(self.root, destination, records)
        self.assertEqual(tus.read_warning_baseline(destination), records)
        original = destination.read_bytes()
        with self.assertRaises(tus.SelectionError):
            tus.publish_warning_baseline(self.root, destination, [])
        self.assertEqual(destination.read_bytes(), original)
        with patch.object(tus.os, "link", side_effect=OSError("publication failed")):
            with self.assertRaisesRegex(OSError, "publication failed"):
                tus.publish_warning_baseline(self.root, self.build / "failed.json", records)
        self.assertFalse((self.build / "failed.json").exists())
        self.assertEqual(list(self.build.glob("warning-baseline-*")), [])
        original_temporary = tus.tempfile.NamedTemporaryFile
        def failed_write(*args, **kwargs):
            stream = original_temporary(*args, **kwargs)
            stream.write = lambda text: (_ for _ in ()).throw(OSError("write failed"))
            return stream
        with patch.object(tus.tempfile, "NamedTemporaryFile", side_effect=failed_write):
            with self.assertRaisesRegex(OSError, "write failed"):
                tus.publish_warning_baseline(self.root, self.build / "failed.json", records)
        self.assertEqual(list(self.build.glob("warning-baseline-*")), [])
        with self.assertRaises(tus.SelectionError):
            tus.publish_warning_baseline(self.root, self.root / "scratch/../outside.json", records)

    def test_invalid_calibration_inputs_fail_before_selection(self):
        invalid = [
            ["--warning-baseline", "missing.json"],
            ["--warning-profile", "msvc-approx", "--warning-baseline", "missing.json"],
            ["--warning-profile", "msvc-approx", "--conversion-clean-prefix", "../src"],
            ["--warning-profile", "msvc-approx", "--conversion-clean-prefix", str(self.src)],
            ["--warning-profile", "msvc-approx", "--conversion-clean-prefix", "missing"],
            ["--warning-profile", "msvc-approx", "--write-warning-baseline", "public.json"],
        ]
        for options in invalid:
            with self.subTest(options=options), patch.object(tus, "changed_paths") as changed:
                with redirect_stderr(io.StringIO()):
                    self.assertEqual(tus.main(["compile", "--root", str(self.root),
                                               "--build-dir", str(self.build), *options]), 1)
                changed.assert_not_called()
        for data in ({}, {"version": 2, "diagnostics": []}, {"version": 1, "diagnostics": [0]},
                     {"version": 1, "diagnostics": [{"option": "conversion"}]}):
            destination = self.build / "invalid.json"
            destination.write_text(json.dumps(data))
            with self.subTest(data=data), self.assertRaises(tus.SelectionError):
                tus.read_warning_baseline(destination)
        with patch.object(tus, "changed_paths") as changed, redirect_stderr(io.StringIO()):
            self.assertEqual(tus.main(["tidy", "--build-dir", str(self.build),
                                       "--warning-profile", "msvc-approx"]), 1)
            changed.assert_not_called()

    def test_main_capture_replay_and_fingerprint_cover_resolved_baseline_content(self):
        destination = self.build / "baseline.json"
        entry = self.entries()[0]
        baseline = self.baseline_records(entry)
        argv = ["compile", "--root", str(self.root), "--build-dir", str(self.build),
                "--warning-profile", "msvc-approx"]
        with patch.object(tus, "changed_paths", return_value={self.a}), \
                patch.object(tus, "run_checks", return_value=baseline):
            with redirect_stdout(io.StringIO()):
                self.assertEqual(tus.main([*argv, "--write-warning-baseline", str(destination)]), 0)
            self.assertEqual(tus.read_warning_baseline(destination), baseline)
            fingerprints = []
            for records in (baseline, [dict(baseline[0], message="different baseline content")]):
                destination.write_text(json.dumps({"version": 1, "diagnostics": records}))
                with redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(tus.main([*argv, "--warning-baseline", str(destination)]), 0)
                fingerprints.append(json.loads(output.getvalue().splitlines()[0])["command_fingerprint"])
            with redirect_stdout(io.StringIO()) as output:
                self.assertEqual(tus.main([*argv, "--warning-baseline", str(destination),
                                           "--conversion-clean-prefix", "src"]), 0)
            fingerprints.append(json.loads(output.getvalue().splitlines()[0])["command_fingerprint"])
            self.assertEqual(len(set(fingerprints)), 3)
        failed = self.build / "failed-baseline.json"
        with patch.object(tus, "changed_paths", return_value={self.a}), \
                patch.object(tus, "run_checks", side_effect=tus.SelectionError("variant failed")):
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(tus.main([*argv, "--write-warning-baseline", str(failed)]), 1)
        self.assertFalse(failed.exists())

    def test_compile_variants_get_unique_scratch_outputs_and_cleanup(self):
        self.raw.append(self.raw_entry(self.a, ["-DFEATURE"]))
        self.save_db()
        selected, _ = self.select([self.a])
        tus.run_checks(self.arguments(), self.root, selected, self.build)
        outputs = [Path(command[-1]) for command, _ in self.check_commands()]
        self.assertEqual(len(set(outputs)), 2)
        self.assertTrue(all(p.is_relative_to(self.root / "scratch") for p in outputs))
        self.assertTrue(all(not p.parent.exists() for p in outputs))

    def test_compile_diagnostics_keep_database_order_when_workers_finish_out_of_order(self):
        self.raw.append(self.raw_entry(self.a, ["-DSECOND"]))
        self.save_db()
        selected, _ = self.select([self.a])
        def parallel(function, items, jobs):
            completed = {index: function((index, entry)) for index, entry in reversed(items)}
            return [completed[index] for index, _ in items]
        def execute(command, directory):
            text = "second\n" if "-DSECOND" in command else "first\n"
            return subprocess.CompletedProcess(command, 0, text, "")
        stream = io.StringIO()
        with patch.object(tus, "parallel", side_effect=parallel), patch.object(tus, "execute", side_effect=execute):
            with redirect_stdout(stream):
                tus.run_checks(self.arguments(), self.root, selected, self.build)
        self.assertEqual(stream.getvalue(), "first\nsecond\n")

    def test_tidy_runs_repository_config_and_ci_gate_for_each_variant(self):
        self.raw.append(self.raw_entry(self.a, ["-DFEATURE"]))
        self.save_db()
        selected, _ = self.select([self.a])
        databases = []
        def inspect(command, directory):
            database = Path(command[command.index("-p") + 1]) / "compile_commands.json"
            databases.append(json.loads(database.read_text()))
            return subprocess.CompletedProcess(command, 0, "", "")
        self.mock_execute.side_effect = inspect
        with patch.object(tus.shutil, "which", side_effect=lambda name: "/bin/clang-tidy-18" if name == "clang-tidy-18" else None):
            tus.run_checks(self.arguments("tidy", also_ci_gate=True), self.root, selected, self.build)
        self.assertEqual(len(databases), 4)
        self.assertEqual(sum("-DFEATURE" in db[0]["arguments"] for db in databases), 2)
        self.assertTrue(all("original.d" not in db[0]["arguments"] for db in databases))

    def test_curated_gate_matches_checks_thresholds_and_tracked_exceptions(self):
        regular = tus.gate_config(ROOT, ROOT / "src/example.cpp")
        self.assertEqual(regular["WarningsAsErrors"], "*")
        self.assertIn("bugprone-unchecked-optional-access", regular["Checks"])
        self.assertIn("clang-analyzer-core*", regular["Checks"])
        self.assertEqual(regular["CheckOptions"][0]["value"], "250")
        tracked = tus.gate_config(ROOT, ROOT / "src/luminumbra_common/net/GnsTransport.cpp")
        self.assertEqual(tracked["Checks"], regular["Checks"] + ",-cppcoreguidelines-avoid-non-const-global-variables")

    def test_curated_file_filter_uses_ci_definition(self):
        text = (ROOT / "scripts/tidy.sh").read_text()
        body = re.search(r"is_first_party_tu\(\) \{(.*?)\n\}", text, re.S)[1]
        accepted = set(re.findall(r"(?:src|test|tools)/\*\.(?:cxx|cpp|cc|c)\b", body))
        self.assertEqual(accepted, {f"{p}/*{s}" for p in ("src", "test", "tools") for s in tus.SOURCE_SUFFIXES})
        for prefix in ("src", "test", "tools", "include", "vendor", "src/luminumbra_client/ui/gl3"):
            for suffix in (*sorted(tus.SOURCE_SUFFIXES), ".hpp", ".CPP"):
                with self.subTest(prefix=prefix, suffix=suffix):
                    self.assertEqual(tus.ci_gate_file(ROOT, ROOT / prefix / ("nested/file" + suffix)),
                                     prefix in {"src", "test", "tools"} and suffix in tus.SOURCE_SUFFIXES)
        script = self.root / "scripts/tidy.sh"
        script.write_text(text.replace("tools/*.c|", "include/*.cpp|tools/*.c|"))
        self.assertTrue(tus.ci_gate_file(self.root, self.root / "include/file.cpp"))

    def test_tidy_include_tu_runs_repo_config_but_skips_ci_gate(self):
        source = self.root / "include/a.cpp"
        source.parent.mkdir()
        source.write_text("// source")
        self.raw = [self.raw_entry(source)]
        self.save_db()
        selected, _ = self.select([source])
        with patch.object(tus.shutil, "which", return_value="clang-tidy"):
            tus.run_checks(self.arguments("tidy", also_ci_gate=True), self.root, selected, self.build)
        self.assertEqual(len(self.check_commands()), 1)
        self.assertTrue(any(arg.startswith("--config-file=") for arg in self.check_commands()[0][0]))

    def test_suites_are_registered_and_retain_native_routing(self):
        commands = tus._PATHS.python_test_commands(ROOT)
        for name in ("changed_tus", "offline_deps"):
            self.assertIn((f"tools/ci/test_{name}.py",), commands)
            self.assertTrue(tus._PATHS.covered_python(f"tools/ci/{name}.py"))
            self.assertTrue(tus._PATHS.needs_cpp(f"tools/ci/{name}.py"))
            self.assertIn(f'ci/test_{name}.py', (ROOT / "tools/CMakeLists.txt").read_text())

    def test_unrecognized_gate_configuration_fails_closed(self):
        (self.root / "scripts/tidy.sh").write_text("gate_checks=dynamic\n")
        with self.assertRaisesRegex(tus.SelectionError, "curated"):
            tus.gate_config(self.root, self.a)

    def test_unrecognized_gate_filter_fails_before_tidy_execution(self):
        script = self.root / "scripts/tidy.sh"
        script.write_text(script.read_text().replace("tools/*.c|", "$dynamic/*.cpp|tools/*.c|"))
        selected, _ = self.select([self.a])
        selection_commands = list(self.commands)
        with patch.object(tus.shutil, "which", return_value="clang-tidy"):
            with self.assertRaisesRegex(tus.SelectionError, "curated tidy file pattern"):
                tus.run_checks(self.arguments("tidy", also_ci_gate=True), self.root, selected, self.build)
        self.assertEqual(self.commands, selection_commands)
        self.assertEqual(list((self.root / "scratch").glob("local-checks-*")), [])

    def test_check_failures_propagate_and_cleanup(self):
        selected, _ = self.select([self.a])
        self.mock_execute.side_effect = tus.SelectionError("compile failed")
        with self.assertRaisesRegex(tus.SelectionError, "compile failed"):
            tus.run_checks(self.arguments(), self.root, selected, self.build)
        self.assertEqual(list((self.root / "scratch").glob("local-checks-*")), [])

    def test_subprocess_failures_and_diagnostics_propagate(self):
        with patch.object(tus._DEPS, "run_process", return_value=subprocess.CompletedProcess([], 2, "out", "diagnostic")):
            with self.assertRaisesRegex(tus.SelectionError, "command failed.*"):
                EXECUTE(["g++", "-c", str(self.a)], self.build)

    def test_git_selection_includes_merge_base_staged_unstaged_and_untracked(self):
        outputs = [b"a" * 40 + b"\n", b"src/committed.cpp\0", b"src/unstaged.hpp\0",
                   b"src/staged.cpp\0", b"src/new name\nfile.hpp\0"]
        with patch.object(tus.subprocess, "check_output", side_effect=outputs) as git:
            paths = tus.changed_paths(self.root, "local-base")
        self.assertEqual(paths, {self.root / name for name in (
            "src/committed.cpp", "src/unstaged.hpp", "src/staged.cpp", "src/new name\nfile.hpp")})
        commands = [call.args[0] for call in git.call_args_list]
        self.assertEqual(commands[0], ["git", "merge-base", "local-base", "HEAD"])
        self.assertTrue(any("--cached" in c for c in commands))
        self.assertTrue(any("--others" in c for c in commands))
        self.assertFalse(any("fetch" in c for c in commands))

    def test_missing_local_merge_base_fails(self):
        with patch.object(tus.subprocess, "check_output", side_effect=subprocess.CalledProcessError(1, "git")):
            with self.assertRaises(subprocess.CalledProcessError):
                tus.changed_paths(self.root, "missing")

    def test_no_worktree_mode_uses_only_committed_diff(self):
        with patch.object(tus.subprocess, "check_output", side_effect=[b"a" * 40, b"src/a.cpp\0"]) as git:
            tus.changed_paths(self.root, "base", False, False)
        self.assertEqual(git.call_count, 2)

    def test_changed_path_escape_fails(self):
        with patch.object(tus.subprocess, "check_output", side_effect=[b"a" * 40, b"../escape.hpp\0"]):
            with self.assertRaisesRegex(tus.SelectionError, "escapes"):
                tus.changed_paths(self.root, "base")

    @unittest.skipIf(os.name == "nt", "POSIX directory symlinks")
    def test_scratch_symlink_escape_fails(self):
        escaped = self.root / "alias-root"
        escaped.mkdir()
        (escaped / "scratch").symlink_to(self.build, target_is_directory=True)
        with self.assertRaisesRegex(tus.SelectionError, "scratch escapes"):
            tus.run_checks(self.arguments(), escaped, [], self.build)
        # PCH containment must likewise inspect resolved paths, including an
        # intermediate symlink that stays in the checkout but leaves the build.
        alias = self.build / "pch alias"
        alias.symlink_to(self.src, target_is_directory=True)
        for flags in (["-Xclang", "-include", "-Xclang", str(alias / self.other.name)],
                      ["-include-pch", str(alias / "header.pch")]):
            with self.subTest(flags=flags):
                self.raw = [self.raw_entry(self.a, flags)]
                self.save_db()
                before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
                with self.assertRaisesRegex(tus.SelectionError, "PCH reference escapes"):
                    self.entries()
                self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})
        self.assertEqual(self.commands, [])

    def test_main_failure_exit_and_command_fingerprint(self):
        with patch.object(tus, "changed_paths", return_value={self.a}), patch.object(tus, "run_checks"):
            reports = []
            for flags in ([], self.cmake_warning_flags(), ["-Wno-error=switch"]):
                self.raw = [self.raw_entry(self.a, flags)]
                self.save_db()
                stream = io.StringIO()
                with redirect_stdout(stream):
                    code = tus.main(["compile", "--root", str(self.root), "--build-dir", str(self.build)])
                self.assertEqual(code, 0)
                report = json.loads(stream.getvalue().splitlines()[0])
                self.assertEqual(report["warning_source"], "compile_commands.json")
                reports.append(report)
            self.assertEqual(len({r["command_fingerprint"] for r in reports}), 3)
        with patch.object(tus, "changed_paths", side_effect=tus.SelectionError("incomplete")):
            self.assertEqual(tus.main(["compile", "--build-dir", str(self.build)]), 1)


EXECUTE = tus.execute
COMPILER_HEADER_ROOTS = tus.compiler_header_roots
# The implementation is saved before any setUp patch replaces the entry point.


class ProcessTimeoutTests(unittest.TestCase):
    def test_replay_and_probe_timeout_terminate_grandchildren(self):
        spec = importlib.util.spec_from_file_location("tree_timeout_tests", ROOT / "tools/ci/test_tidy_cache.py")
        fixtures = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fixtures)
        actions = (
            (lambda command, directory: EXECUTE(command, directory), subprocess.TimeoutExpired),
            (lambda command, directory: tus._compiler_header_roots(
                tuple(command), directory, tuple(os.environ.items())), tus.SelectionError),
        )
        for launch, error_type in actions:
            with self.subTest(error_type=error_type):
                fixtures.assert_tree_timeout(self, launch, error_type)


class WindowsSearchPathTests(unittest.TestCase):
    class SearchPath(PureWindowsPath):
        def resolve(self):
            # Model the handle-based short-name expansion at the OS boundary.
            parts = ["Header Directory" if part.casefold() == "header~1" else part
                     for part in self.parts]
            return type(self)(ntpath.normpath(str(type(self)(*parts))))

    def test_equivalent_windows_roots_reuse_scan_and_keep_invalidation(self):
        root = self.SearchPath(r"C:\Header Directory")
        variants = [root, self.SearchPath("c:/header directory/"),
                    self.SearchPath(r"C:\HEADER~1"),
                    self.SearchPath(r"c:\HEADER~1\nested\..")]
        tick = 10_000_000_000
        stamp = [1, 1, stat.S_IFDIR, 0, tick, tick]
        files = []
        with patch.object(tus.os, "name", "nt"), \
                patch.object(tus, "Path", self.SearchPath), \
                patch.object(tus, "_PCH_SEARCH_CACHE", {}), \
                patch.object(tus.time, "time_ns", return_value=tick + 2_000_000_000), \
                patch.object(tus, "directory_stamp", side_effect=lambda path: tuple(stamp)) as stamps, \
                patch.object(tus, "directory_entries", side_effect=lambda *args: tuple(
                    (name, stat.S_IFREG, 1, 2) for name in files)), \
                patch.object(tus, "walk_header_directories", side_effect=lambda *args, **kwargs:
                             iter([(str(root), [], list(files))])) as walk:
            compilers = (r"C:\Toolchain\G++.EXE", "c:/toolchain/g++.exe",
                         r"c:\TOOLCHAIN\.\g++.exe", r"C:\Toolchain\bin\..\g++.exe")
            for compiler, variant in zip(compilers, variants):
                tus.check_pch_search_roots(compiler, {variant, root})
            self.assertEqual(walk.call_count, 1)
            self.assertEqual(len(tus._PCH_SEARCH_CACHE), 1)
            self.assertTrue(all(call.args[0] == root for call in stamps.call_args_list))
            files.append("header.hpp.gch")
            stamp[4] += 1
            with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                tus.check_pch_search_roots(compilers[-1], {variants[-1]})
            self.assertEqual(walk.call_count, 2)
            with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                tus.check_pch_search_roots(compilers[1], {variants[1]})
            self.assertEqual(walk.call_count, 2)

    def test_equivalent_windows_probe_directories_reuse_search_list(self):
        result = subprocess.CompletedProcess([], 0, "", '#include <...> search starts here:\r\n '
            'C:/Header Directory\r\nEnd of search list.\r\n')
        tus._compiler_header_roots.cache_clear()
        with patch.object(tus.os, "name", "nt"), \
                patch.object(tus, "Path", self.SearchPath), \
                patch.object(tus._DEPS, "run_process", return_value=result) as probe:
            for directory in (r"C:\Header Directory", "c:/header directory", r"C:\HEADER~1"):
                roots = COMPILER_HEADER_ROOTS(["g++.exe", r"C:\changed_tus\unit.cpp"],
                                             self.SearchPath(directory))
                self.assertEqual(roots, (self.SearchPath(r"C:\Header Directory"),))
            self.assertEqual(probe.call_count, 1)

    def test_unresolvable_root_refuses_and_empty_roots_do_not_walk(self):
        with patch.object(tus, "_PCH_SEARCH_CACHE", {}), \
                patch.object(tus, "walk_header_directories") as walk:
            tus.check_pch_search_roots("g++", set())
            with patch.object(Path, "resolve", side_effect=PermissionError("unreadable root")), \
                    self.assertRaisesRegex(tus.SelectionError, "cannot check binary PCH search directory"):
                tus.check_pch_search_roots("g++", {Path("headers")})
            walk.assert_not_called()

    def test_posix_compiler_case_remains_distinct(self):
        with patch.object(tus.os, "name", "posix"), \
                patch.object(tus, "_PCH_SEARCH_CACHE", {}) as cache:
            for compiler in ("G++", "g++"):
                tus.check_pch_search_roots(compiler, set())
            self.assertEqual(len(cache), 2)


class PchEntryIdentityTests(unittest.TestCase):
    def setUp(self):
        self.root = Path("virtual headers").absolute()
        self.child = self.root / "generated"
        self.binary = self.child / "header.hpp.gch"
        self.tick = 10_000_000_000
        self.nodes = {self.root: (stat.S_IFDIR, 1, 1)}
        # Keep stamps fixed to isolate entry identity from timestamp detection.
        stamp = (1, 1, stat.S_IFDIR, 0, self.tick, self.tick)

        def names(path):
            return sorted(node.name for node in self.nodes if node.parent == path)

        def walk(path, **kwargs):
            children, files = [], []
            for name in names(path):
                kind = self.nodes[path / name][0]
                (children if kind == stat.S_IFDIR else files).append(name)
            yield str(path), children, files
            for name in children:
                yield from walk(path / name, **kwargs)

        def lstat(path):
            kind, device, inode = self.nodes[Path(path)]
            return SimpleNamespace(st_mode=kind, st_dev=device, st_ino=inode)

        patches = [patch.object(tus, "_PCH_SEARCH_CACHE", {}),
                   patch.object(tus, "directory_stamp",
                                side_effect=lambda path: stamp if path in self.nodes
                                and self.nodes[path][0] != stat.S_IFLNK else None),
                   patch.object(tus.os, "listdir", side_effect=names),
                   patch.object(tus.os, "lstat", side_effect=lstat),
                   patch.object(Path, "resolve", autospec=True, side_effect=lambda path: path),
                   patch.object(Path, "is_symlink", autospec=True,
                                side_effect=lambda path: self.nodes[path][0] == stat.S_IFLNK)]
        for mock in patches:
            mock.start()
            self.addCleanup(mock.stop)
        self.walk = patch.object(tus, "walk_header_directories", side_effect=walk).start()
        self.clock = patch.object(tus.time, "time_ns", return_value=self.tick + 500_000_000).start()
        self.addCleanup(patch.stopall)

    def test_file_becoming_directory_with_pch_invalidates_approval(self):
        for compiler in ("g++", "clang++"):
            for replay_delay in (750_000_000, 2_000_000_000):
                with self.subTest(compiler=compiler, replay_delay=replay_delay):
                    tus._PCH_SEARCH_CACHE.clear()
                    self.walk.reset_mock()
                    self.clock.return_value = self.tick + 500_000_000
                    self.nodes = {self.root: (stat.S_IFDIR, 1, 1),
                                  self.child: (stat.S_IFREG, 1, 2)}
                    tus.check_pch_search_roots(compiler, {self.root})
                    self.nodes[self.child] = (stat.S_IFDIR, 1, 2)
                    self.nodes[self.binary] = (stat.S_IFREG, 1, 3)
                    self.clock.return_value = self.tick + replay_delay
                    for _ in range(2):
                        with self.assertRaisesRegex(tus.SelectionError, "binary PCH.*remove"):
                            tus.check_pch_search_roots(compiler, {self.root})
                    self.assertEqual(self.walk.call_count, 2)

    def test_directory_becoming_file_requires_rescan(self):
        for compiler in ("g++", "clang++"):
            for replay_delay in (750_000_000, 2_000_000_000):
                with self.subTest(compiler=compiler, replay_delay=replay_delay):
                    tus._PCH_SEARCH_CACHE.clear()
                    self.walk.reset_mock()
                    self.clock.return_value = self.tick + 500_000_000
                    self.nodes = {self.root: (stat.S_IFDIR, 1, 1),
                                  self.child: (stat.S_IFDIR, 1, 2)}
                    tus.check_pch_search_roots(compiler, {self.root})
                    self.nodes[self.child] = (stat.S_IFREG, 1, 2)
                    self.clock.return_value = self.tick + replay_delay
                    tus.check_pch_search_roots(compiler, {self.root})
                    tus.check_pch_search_roots(compiler, {self.root})
                    self.assertEqual(self.walk.call_count, 2)

    def test_entry_kind_device_and_inode_changes_require_rescan(self):
        replacements = [(stat.S_IFLNK, 1, 2), (stat.S_IFIFO, 1, 2),
                        (stat.S_IFREG, 2, 2), (stat.S_IFREG, 1, 3)]
        for replacement in replacements:
            with self.subTest(replacement=replacement):
                tus._PCH_SEARCH_CACHE.clear()
                self.walk.reset_mock()
                self.clock.return_value = self.tick + 500_000_000
                self.nodes = {self.root: (stat.S_IFDIR, 1, 1),
                              self.child: (stat.S_IFREG, 1, 2)}
                tus.check_pch_search_roots("g++", {self.root})
                self.nodes[self.child] = replacement
                self.clock.return_value = self.tick + 2_000_000_000
                tus.check_pch_search_roots("g++", {self.root})
                self.assertEqual(self.walk.call_count, 2)
                tus.check_pch_search_roots("g++", {self.root})
                self.assertEqual(self.walk.call_count, 2)

    def test_type_change_during_walk_refuses_snapshot(self):
        for kind in (stat.S_IFDIR, stat.S_IFLNK):
            with self.subTest(kind=kind):
                self.nodes[self.child] = (kind, 1, 2)
                self.walk.side_effect = None
                self.walk.return_value = [(str(self.root), [], [self.child.name])]
                stamp = (1, 1, stat.S_IFDIR, 0, self.tick, self.tick)
                with patch.object(tus, "directory_stamp", return_value=stamp), \
                        self.assertRaisesRegex(tus.SelectionError, "entry changed type during scan"):
                    tus.check_pch_search_roots("g++", {self.root})
                self.assertEqual(tus._PCH_SEARCH_CACHE, {})


if __name__ == "__main__":
    unittest.main()
