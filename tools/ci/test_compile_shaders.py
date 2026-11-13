"""Shader inventory, runtime preprocessing parity and offline compiler contracts.

Pass --validator glslangValidator to exercise real valid/broken GLSL in CI.
The default suite is hermetic: subprocess results test compiler error handling,
while preprocessing and inventory always run on real synthetic source trees.
"""

import argparse
from contextlib import redirect_stderr, redirect_stdout
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_shaders as shaders
import ci_paths

ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = None
VALID = "#version 450 core\nvoid main() {}\n"
BROKEN = "#version 450 core\nvoid main() { this is deliberately invalid GLSL; }\n"


class ShaderInventoryTests(unittest.TestCase):
    def make_inventory(self, root):
        names = {name for group in shaders.STAGE_FILES.values() for name in group}
        for name in sorted(names | shaders.INCLUDES | shaders.EXCLUDED):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(VALID, encoding="utf-8")

    def test_repository_inventory_and_every_runtime_permutation(self):
        sources = shaders.inventory(ROOT / "res/shaders")
        self.assertEqual(len(sources), 63)
        units = shaders.compile_units(sources)
        self.assertEqual(len(units), 74)
        self.assertEqual({unit.name for unit in units}, sources.keys() - shaders.INCLUDES - shaders.EXCLUDED)
        self.assertEqual({unit.name for unit in units if unit.defines},
                         shaders.LADDER_FILES | shaders.DIAGNOSTIC_FILES)
        self.assertEqual({name for name, text in sources.items() if "#ifdef LUMIN_DIAG_IDS" in text},
                         shaders.DIAGNOSTIC_FILES)
        for unit in units:
            self.assertEqual(unit.source.text.splitlines()[0], "#version 450 core")
            self.assertNotIn('#include "', unit.source.text)
            if unit.defines:
                expected = {shaders.LADDER_DEFINES} if unit.name in shaders.LADDER_FILES else set()
                if unit.name in shaders.DIAGNOSTIC_FILES:
                    expected.add(shaders.DIAGNOSTIC_DEFINES)
                    if unit.name in shaders.LADDER_FILES:
                        expected.add((*shaders.DIAGNOSTIC_DEFINES, *shaders.LADDER_DEFINES))
                self.assertIn(unit.defines, expected)
                injected = "".join(f"#define {define}\n" for define in unit.defines) + "#line 2\n"
                self.assertIn(injected, unit.source.text.replace("\r\n", "\n"))
        for name in shaders.STAGE_FILES["frag"]:
            permutations = {unit.defines for unit in units if unit.name == name}
            expected = {()}
            if name in shaders.LADDER_FILES:
                expected.add(shaders.LADDER_DEFINES)
            if name in shaders.DIAGNOSTIC_FILES:
                expected |= {(*shaders.DIAGNOSTIC_DEFINES, *defines) for defines in expected}
            self.assertEqual(permutations, expected, name)
        self.assertEqual(sum(unit.stage == "comp" for unit in units), 5)
        self.assertEqual(sum(unit.stage == "geom" for unit in units), 1)

    def test_enumeration_is_sorted_and_refuses_growth_deletion_and_unknown_extensions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_inventory(root)
            sources = shaders.inventory(root)
            self.assertEqual(list(sources), sorted(sources))
            for name in ("extra.frag", "extra.compute", "extra.glsl", "pilot/extra.hlsl", "extra.txt"):
                path = root / name
                path.write_text(VALID, encoding="utf-8")
                with self.subTest(name=name), self.assertRaisesRegex(shaders.ShaderError, "uncovered=.*" + name):
                    shaders.inventory(root)
                path.unlink()
            (root / "basic.vert").unlink()
            with self.assertRaisesRegex(shaders.ShaderError, "missing=.*basic.vert"):
                shaders.inventory(root)

    def test_empty_missing_unreadable_and_invalid_utf8_sources_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(shaders.ShaderError, "missing="):
                shaders.inventory(root)
            with self.assertRaises(FileNotFoundError):
                shaders.inventory(root / "absent")
            with patch.object(shaders.os, "scandir", side_effect=PermissionError("denied")):
                with self.assertRaises(PermissionError):
                    shaders.inventory(root)
            source = root / "bad.frag"
            source.write_bytes(b"\xff")
            with self.assertRaises(UnicodeError):
                shaders.read_source(source)
            with patch.object(shaders.os, "open", side_effect=PermissionError("denied")):
                with self.assertRaises(PermissionError):
                    shaders.read_source(source)
            with self.assertRaises((shaders.ShaderError, OSError)):
                shaders.read_source(root)

    def test_scan_and_source_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "basic.vert"
            source.write_bytes(b"abcd")
            with patch.object(shaders, "MAX_SOURCE_BYTES", 4):
                self.assertEqual(shaders.read_source(source), "abcd")
            with patch.object(shaders, "MAX_SOURCE_BYTES", 3):
                with self.assertRaisesRegex(shaders.ShaderError, "size bound"):
                    shaders.read_source(source)
            with patch.object(shaders, "MAX_ENTRIES", 0):
                with self.assertRaisesRegex(shaders.ShaderError, "entry bound"):
                    shaders.inventory(root)
            with patch.object(shaders.os, "fstat", return_value=os.stat_result((0o100644, 0, 0, 1, 0, 0, 0, 0, 0, 0))):
                with patch.object(shaders, "MAX_SOURCE_BYTES", 3):
                    with self.assertRaisesRegex(shaders.ShaderError, "size bound exceeded"):
                        shaders.read_source(source)

    def test_special_files_and_redirections_are_refused_without_reading(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "basic.vert"
            source.write_text(VALID, encoding="utf-8")
            for mode in (0o010644, 0o020644):
                info = os.stat_result((mode, 0, 0, 1, 0, 0, 0, 0, 0, 0))
                with self.subTest(mode=mode), patch.object(shaders.os, "fstat", return_value=info), \
                        patch.object(shaders.os, "fdopen") as wrap:
                    with self.assertRaisesRegex(shaders.ShaderError, "not a regular shader"):
                        shaders.read_source(source)
                    wrap.assert_not_called()
            info = os.stat_result((0o120777, 0, 0, 1, 0, 0, 0, 0, 0, 0))
            with patch.object(shaders.os, "lstat", return_value=info), \
                    patch.object(shaders, "read_source") as read:
                with self.assertRaisesRegex(shaders.ShaderError, "redirections"):
                    shaders.inventory(root)
                read.assert_not_called()

    def test_orphan_include_and_raw_compute_loading(self):
        sources = shaders.inventory(ROOT / "res/shaders")
        sources["basic.vert"] = '#version 450 core\n#include "far_world_position.glsl"\n'
        sources["grass_scatter.comp"] = '#version 450 core\n#include "not-expanded.glsl"\n'
        units = shaders.compile_units(sources)
        compute = next(unit for unit in units if unit.name == "grass_scatter.comp")
        self.assertEqual(compute.source.text, sources["grass_scatter.comp"])
        for name in sources:
            if name.endswith(".frag"):
                sources[name] = VALID
        sources["basic.vert"] = VALID
        with self.assertRaisesRegex(shaders.ShaderError, "no compile consumer"):
            shaders.compile_units(sources)


class ShaderPreprocessingTests(unittest.TestCase):
    def expand(self, sources, name="main.frag"):
        used = set()
        result = shaders.expand_source(name, sources, used)
        return result, used

    def test_nested_repeated_includes_and_original_locations(self):
        sources = {"main.frag": '#version 450 core\n#include "sub/a.glsl"\n#include "sub/a.glsl"\nvoid main() {}',
                   "sub/a.glsl": '#include "../b.glsl"\nfloat a;', "b.glsl": "float b;"}
        result, used = self.expand(sources)
        self.assertEqual(result.text, "#version 450 core\nfloat b;\nfloat a;\nfloat b;\nfloat a;\nvoid main() {}")
        self.assertEqual(result.locations, (("main.frag", 1), ("b.glsl", 1), ("sub/a.glsl", 2),
                                            ("b.glsl", 1), ("sub/a.glsl", 2), ("main.frag", 4)))
        self.assertEqual(used, set(sources))

    def test_crlf_empty_include_and_no_final_newline(self):
        for ending in ("\n", "\r\n"):
            for included in ("", "float x;", "float x;\r\n"):
                for tail in ("", ending):
                    with self.subTest(ending=repr(ending), included=included, tail=repr(tail)):
                        result, _ = self.expand({"main.frag": '#include "a.glsl"' + tail,
                                                "a.glsl": included})
                        expected = included + (ending if tail and not included.endswith("\n") else "")
                        self.assertEqual(result.text, expected)
                        self.assertEqual(len(result.locations), len(shaders.lines(expected)))

    def test_runtime_include_grammar_is_exact_even_in_comments_or_inactive_branches(self):
        for line in (' #include "a.glsl"', '#include "a.glsl" // tail', '#include "a.glsl',
                     '// #include "a.glsl"', '#include "a.glsl"\r'):
            with self.subTest(line=line), self.assertRaisesRegex(shaders.ShaderError, "main.frag:1: malformed"):
                self.expand({"main.frag": line, "a.glsl": ""})
        result, used = self.expand({"main.frag": '#if 0\n#include "a.glsl"\n#endif\n', "a.glsl": "float x;\n"})
        self.assertIn("a.glsl", used)
        self.assertIn("float x;", result.text)
        result, used = self.expand({"main.frag": '#include <untouched>\n'})
        self.assertEqual(result.text, '#include <untouched>\n')

    def test_missing_cycle_escape_depth_and_expansion_bounds_fail(self):
        for operand, reason in (("absent.glsl", "cannot open"), ("main.frag", "cyclic"),
                                ("../out.glsl", "escapes"), ("/out.glsl", "escapes"),
                                ("C:/out.glsl", "escapes"), ("..\\out.glsl", "escapes"), ("", "escapes")):
            with self.subTest(operand=operand), self.assertRaisesRegex(shaders.ShaderError, reason):
                self.expand({"main.frag": f'#include "{operand}"\n'})
        with patch.object(shaders, "MAX_INCLUDE_DEPTH", 1):
            self.assertEqual(self.expand({"main.frag": "x"})[0].text, "x")
            with self.assertRaisesRegex(shaders.ShaderError, "depth bound"):
                self.expand({"main.frag": '#include "a.glsl"\n', "a.glsl": "x"})
        with patch.object(shaders, "MAX_EXPANDED_BYTES", 4):
            self.assertEqual(self.expand({"main.frag": '#include "a.glsl"\n', "a.glsl": "abc\n"})[0].text, "abc\n")
            with self.assertRaisesRegex(shaders.ShaderError, "size bound"):
                self.expand({"main.frag": '#include "a.glsl"\n#include "a.glsl"\n', "a.glsl": "abc\n"})

    def test_define_injection_matches_runtime_header_lexer(self):
        for ending in ("\n", "\r\n"):
            for leading in ("", "// leading" + ending, "/* leading\n#version 110\n*/\n"):
                for trailing in ("", " // #version 110", " /* trailing\ncomment */"):
                    header = leading + "#version 450 core" + trailing + ending
                    source = header + "void main() {}" + ending
                    line = header.count("\n") + 1
                    injected_ending = "\n" if "\n" in trailing else ending
                    with self.subTest(leading=leading, trailing=trailing, ending=repr(ending)):
                        self.assertEqual(shaders.inject_defines(source, ("FEATURE 1", "OTHER 2")),
                                         header + f"#define FEATURE 1{injected_ending}#define OTHER 2{injected_ending}#line {line}{injected_ending}"
                                         + "void main() {}" + ending)
        self.assertEqual(shaders.inject_defines("#version 450 core", ("FEATURE 1",)),
                         "#version 450 core\n#define FEATURE 1\n#line 2\n")
        for source in ("", "void main() {}", "/* #version 110 */\nvoid main() {}",
                       "#versioned 450\n", "#\nversion 450\n"):
            with self.subTest(source=source):
                self.assertEqual(shaders.inject_defines(source, ("FEATURE 1",)),
                                 "#define FEATURE 1\n#line 1\n" + source)
        self.assertEqual(shaders.inject_defines("# /* hello */ version 450\nmain", ("X",)),
                         "# /* hello */ version 450\n#define X\n#line 2\nmain")
        quoted = '"escaped \\" /* not a comment"\n'
        self.assertEqual(shaders.inject_defines(quoted, ("X",)), "#define X\n#line 1\n" + quoted)

    def test_no_defines_is_byte_identical_and_broken_snippet_fails_before_compilation(self):
        for source in (VALID, BROKEN, "/* unterminated", '"escaped \\" /* not a comment"\n'):
            self.assertEqual(shaders.inject_defines(source, ()), source)
        for source in ("/* unterminated\n#version 450 core\n", VALID + "/*", "#version 450 /*"):
            with self.subTest(source=source), self.assertRaisesRegex(shaders.ShaderError, "unterminated"):
                shaders.inject_defines(source, ("FEATURE 1",))


class ShaderCompilerTests(unittest.TestCase):
    def unit(self, text=VALID):
        return shaders.Unit("fixture.frag", "frag", (), shaders.raw_source("fixture.frag", text))

    def test_valid_and_deliberately_broken_glsl(self):
        if VALIDATOR:
            shaders.compile_unit(self.unit(), VALIDATOR)
            with self.assertRaisesRegex(shaders.ShaderError, "fixture.frag.*failed"):
                shaders.compile_unit(self.unit(BROKEN), VALIDATOR)
        else:
            # No local glslang dependency: only the error plumbing is simulated.
            for text, status in ((VALID, 0), (BROKEN, 2)):
                result = subprocess.CompletedProcess([], status, "ERROR: 0:2: syntax error\n" if status else "")
                with self.subTest(status=status), patch.object(shaders.subprocess, "run", return_value=result) as run:
                    if status:
                        with self.assertRaisesRegex(shaders.ShaderError, "fixture.frag:2: error: syntax error"):
                            shaders.compile_unit(self.unit(text), "validator")
                    else:
                        self.assertEqual(shaders.compile_unit(self.unit(text), "validator"), "")
                    self.assertEqual(run.call_args.args[0], ["validator", "-d", "--stdin", "-S", "frag"])
                    self.assertEqual(run.call_args.kwargs["input"], text)
                    self.assertFalse(run.call_args.kwargs.get("shell", False))

    def test_include_diagnostics_map_to_original_file_and_line_after_define_injection(self):
        source = shaders.expand_source("main.frag", {"main.frag": '#version 450 core\n#include "a.glsl"\n',
                                                   "a.glsl": "\nbroken;\n"}, set())
        unit = shaders.Unit("main.frag", "frag", shaders.LADDER_DEFINES,
                            shaders.Source(shaders.inject_defines(source.text, shaders.LADDER_DEFINES), source.locations))
        result = subprocess.CompletedProcess([], 2, "ERROR: 0:3: bad syntax\nERROR: 0:0: unknown\n")
        with patch.object(shaders.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(shaders.ShaderError, "a.glsl:2: error: bad syntax") as caught:
                shaders.compile_unit(unit, "validator")
            self.assertIn("main.frag:0: error: unknown", str(caught.exception))
            self.assertIn("LUMINUMBRA_FAR_WORLD_LADDER 1", str(caught.exception))

    def test_nonzero_without_log_still_fails_and_warnings_are_retained(self):
        with patch.object(shaders.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, "")):
            with self.assertRaisesRegex(shaders.ShaderError, "failed \\(1\\)"):
                shaders.compile_unit(self.unit(), "validator")
        with patch.object(shaders.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "WARNING: 0:1: message\n")):
            self.assertEqual(shaders.compile_unit(self.unit(), "validator"), "fixture.frag:1: warning: message\n")

    def test_cli_refusals_and_all_compile_errors_are_reported(self):
        for failure in (OSError("missing validator"), UnicodeError("bad encoding"),
                        shaders.ShaderError("bad inventory"), subprocess.TimeoutExpired("validator", 30)):
            with self.subTest(failure=failure), patch.object(sys, "argv", ["compile_shaders.py"]), \
                    patch.object(shaders, "inventory", side_effect=failure), redirect_stderr(io.StringIO()) as error:
                self.assertEqual(shaders.main(), 1)
                self.assertIn("shader compile failed", error.getvalue())
        for results, status in ((["warning\n", ""], 0),
                                ([shaders.ShaderError("first error"), shaders.ShaderError("second error")], 1)):
            with patch.object(sys, "argv", ["compile_shaders.py"]), \
                    patch.object(shaders, "inventory", return_value={}), \
                    patch.object(shaders, "compile_units", return_value=[self.unit(), self.unit()]), \
                    patch.object(shaders, "compile_unit", side_effect=results) as compile_one, \
                    redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()) as error:
                self.assertEqual(shaders.main(), status)
                self.assertEqual(compile_one.call_count, 2)
                if status:
                    self.assertIn("first error", error.getvalue())
                    self.assertIn("second error", error.getvalue())
                else:
                    self.assertIn("Compiled 2 shader permutations", output.getvalue())
                    self.assertIn("warning", output.getvalue())

    def test_suite_is_registered_on_all_python_routes_and_ctest(self):
        name = "tools/ci/test_compile_shaders.py"
        self.assertEqual(ci_paths.python_test_commands(ROOT).count((name,)), 1)
        self.assertTrue(ci_paths.covered_python("tools/ci/compile_shaders.py"))
        for path in (name, "tools/ci/compile_shaders.py", "res/shaders/basic.frag"):
            self.assertTrue(ci_paths.needs_cpp(path))
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_test(NAME ShaderCompilePython", cmake)
        self.assertIn("${CMAKE_SOURCE_DIR}/tools/ci/test_compile_shaders.py", cmake)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--validator")
    options, remaining = parser.parse_known_args()
    VALIDATOR = options.validator
    unittest.main(argv=[sys.argv[0], *remaining])
