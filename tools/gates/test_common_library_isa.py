#!/usr/bin/env python3
"""Compile the virtual CPU refusal controls; never run a native executable."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CommonLibraryIsaContract(unittest.TestCase):
    compiler = shutil.which("c++") or shutil.which("cl")
    cmake = shutil.which("cmake")
    msvc = False

    def test_injected_cpu_profiles(self):
        self.assertIsNotNone(self.compiler, "a C++ compiler is required")
        source = ROOT / "test/common/CommonLibraryIsa_compile_test.cpp"
        if self.msvc:
            flags = ["/nologo", "/std:c++20", "/Zs", "/W4", "/WX", "/arch:SSE2",
                     "/I" + str(ROOT / "src")]
        else:
            flags = ["-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Wshadow",
                     "-Wmissing-field-initializers", "-Werror", "-I" + str(ROOT / "src")]
        result = subprocess.run([self.compiler, *flags, str(source)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_startup_source_properties(self):
        self.assertIsNotNone(self.cmake, "CMake is required")
        # Configure without enabling a language: check Windows and POSIX source
        # properties without compiler detection, dependency fetches or a build.
        root = ROOT.as_posix()
        lines = ["cmake_minimum_required(VERSION 3.20)",
                 "project(StartupIsaContract LANGUAGES NONE)",
                 'include("' + root + '/cmake/runtime_isa.cmake")']
        for compiler in ("MSVC", "GNU", "Clang"):
            for pointer_bytes in (4, 8):
                name = compiler + str(pointer_bytes) + ".cpp"
                lines.extend(['set(CMAKE_SYSTEM_PROCESSOR x86_64)',
                              'set(CMAKE_SIZEOF_VOID_P ' + str(pointer_bytes) + ')',
                              'set(CMAKE_CXX_COMPILER_ID ' + compiler + ')',
                              'set(MSVC ' + ("TRUE" if compiler == "MSVC" else "FALSE") + ')',
                              'luminumbra_baseline_startup("' + name + '")',
                              'get_source_file_property(flags "' + name + '" COMPILE_OPTIONS)'])
                if compiler == "MSVC":
                    expected = ["/arch:SSE2" if pointer_bytes == 8 else "/arch:IA32", "/GL-"]
                else:
                    expected = (["-march=x86-64"] if pointer_bytes == 8 else
                                ["-march=i686", "-mno-sse", "-mno-sse2"])
                    expected += ["-mno-avx", "-mno-avx2", "-mno-fma", "-mno-bmi",
                                 "-mno-bmi2", "-mno-popcnt", "-mno-lzcnt", "-mno-f16c",
                                 "-mno-movbe", "-fno-lto"]
                lines.extend(['if(NOT flags STREQUAL "' + ';'.join(expected) + '")',
                              'message(FATAL_ERROR "incorrect baseline options: ${flags}")',
                              'endif()'])
        for application in ("client", "server"):
            prefix = root + '/src/luminumbra_' + application
            lines.extend(['include("' + prefix + '/sources.cmake")',
                          'get_source_file_property(definition "' + prefix + '/main_' +
                          application + '.cpp" COMPILE_DEFINITIONS)',
                          'if(NOT definition STREQUAL "main=LuminumbraApplicationMain")',
                          'message(FATAL_ERROR "application bypasses guarded entry")', 'endif()'])
        with tempfile.TemporaryDirectory(prefix="startup-isa-") as directory:
            path = Path(directory)
            (path / "CMakeLists.txt").write_text('\n'.join(lines) + '\n', encoding="utf-8")
            result = subprocess.run([self.cmake, "-S", str(path), "-B", str(path / "build")],
                                    capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("c++") or shutil.which("cl"))
    parser.add_argument("--msvc", action="store_true")
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    args = parser.parse_args()
    CommonLibraryIsaContract.compiler = args.compiler
    CommonLibraryIsaContract.msvc = args.msvc
    CommonLibraryIsaContract.cmake = args.cmake
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(CommonLibraryIsaContract)
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
