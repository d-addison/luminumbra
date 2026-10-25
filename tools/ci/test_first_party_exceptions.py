#!/usr/bin/env python3
"""Generate target compile commands without compiling, linking or fetching."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
POLICY = Path('cmake/first_party_exceptions.cmake')


def compiler_path(language):
    names = ('c++', 'clang++', 'g++', 'cl') if language == 'CXX' else ('cc', 'clang', 'gcc', 'cl')
    for name in names:
        compiler = shutil.which(name)
        if compiler:
            return Path(compiler)
    # The standalone Windows Python lane precedes Enter-VsDevShell. Locate the
    # installed compiler without running its installer or probing the compiler.
    for variable in ('ProgramFiles', 'ProgramFiles(x86)'):
        if variable in os.environ:
            base = Path(os.environ[variable]) / 'Microsoft Visual Studio'
            candidates = sorted(base.glob('*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe'))
            if candidates:
                return candidates[-1]
    raise AssertionError('an installed C/C++ compiler is required for command generation')


def write(path, body):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body + '\n', encoding='utf-8')


def ninja_path():
    program = shutil.which('ninja')
    if program:
        return Path(program)
    for variable in ('ProgramFiles', 'ProgramFiles(x86)'):
        if variable in os.environ:
            base = Path(os.environ[variable]) / 'Microsoft Visual Studio'
            candidates = sorted(base.glob('*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'))
            if candidates:
                return candidates[-1]
    raise AssertionError('an installed Ninja is required for command generation')


class FirstPartyExceptionsTests(unittest.TestCase):
    def configure(self, *, msvc=True, warnings=True, testing=True, flags='/Z7', config='Release'):
        cmake = shutil.which('cmake')
        self.assertIsNotNone(cmake, 'CMake is required')
        ninja = ninja_path()
        source = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        include = 'include(' + POLICY.as_posix() + ')'
        self.assertEqual(source.count(include), 1)
        self.assertGreater(source.index(include), source.index('add_subdirectory(test)'))
        self.assertGreater(source.index(include), source.index('add_subdirectory(src)'))
        self.assertGreater(source.index(include), source.index('add_subdirectory(tools)'))
        helpers = []
        for name in ('luminumbra_enable_frame_pointers', 'luminumbra_enable_coverage',
                     'luminumbra_configure_first_party_target'):
            match = re.search(r'^function\(' + name + r'\b.*?^endfunction\(\)', source,
                              re.MULTILINE | re.DOTALL)
            self.assertIsNotNone(match, name)
            helpers.append(match[0])

        with tempfile.TemporaryDirectory(prefix='exception-options-') as directory:
            root = Path(directory)
            write(root / POLICY, (ROOT / POLICY).read_text(encoding='utf-8'))
            write(root / 'probe.cpp', 'int option_probe;')
            write(root / 'probe.c', 'int c_option_probe;')
            preamble = ['cmake_minimum_required(VERSION 3.20)']
            for language in ('C', 'CXX'):
                compiler = compiler_path(language)
                compiler_id = 'MSVC' if compiler.name.lower() == 'cl.exe' else (
                    'Clang' if 'clang' in compiler.name else 'GNU')
                preamble += [f'set(CMAKE_{language}_COMPILER "{compiler.as_posix()}")',
                             f'set(CMAKE_{language}_COMPILER_ID {compiler_id})',
                             f'set(CMAKE_{language}_COMPILER_ID_RUN TRUE)',
                             f'set(CMAKE_{language}_COMPILER_FORCED TRUE)',
                             f'set(CMAKE_{language}_COMPILER_WORKS TRUE)',
                             f'set(CMAKE_{language}_ABI_COMPILED TRUE)']
                if compiler_id == 'MSVC':
                    preamble += [f'set(CMAKE_{language}_COMPILER_VERSION 19.40)',
                                 f'set(CMAKE_{language}_COMPILER_ARCHITECTURE_ID x64)',
                                 f'set(MSVC_{language}_ARCHITECTURE_ID x64)']
            # Compiler identification/ABI/try-compile are disabled above. Only
            # CMake's real option ordering and per-source generator expressions
            # run; these generated commands are never executed by this suite.
            preamble += ['project(ExceptionOptionsContract LANGUAGES C CXX)',
                         'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)',
                         f'set(MSVC {"TRUE" if msvc else "FALSE"})',
                         f'set(LUMINUMBRA_WARNINGS_AS_ERRORS {"ON" if warnings else "OFF"})',
                         f'set(CMAKE_CXX_FLAGS "{flags}")',
                         f'set(CMAKE_C_FLAGS "{flags}")',
                         f'set(CMAKE_BUILD_TYPE {config})',
                         'set(CMAKE_SYSTEM_PROCESSOR x86_64)',
                         'set(CMAKE_SIZEOF_VOID_P 8)',
                         '\n'.join(helpers),
                         f'include("{ROOT.as_posix()}/cmake/runtime_isa.cmake")',
                         'add_subdirectory(vendor)', 'add_subdirectory(src)',
                         'add_subdirectory(tools)']
            if testing:
                preamble.append('add_subdirectory(test)')
            preamble.append(include)
            write(root / 'CMakeLists.txt', '\n'.join(preamble))
            write(root / 'vendor/CMakeLists.txt', '''
add_library(dependency STATIC ../probe.cpp)
add_library(headers INTERFACE)
add_library(dependency_alias ALIAS dependency)
add_library(imported_dependency INTERFACE IMPORTED)
''')
            ai = ROOT / 'src/luminumbra_common/ai'
            entry = (ROOT / 'src/startup/RuntimeEntry.cpp').as_posix()
            write(root / 'src/CMakeLists.txt', f'''
add_library(luminumbra_common STATIC
    "{ai.as_posix()}/InstinctPlanner.cpp"
    "{ai.as_posix()}/InstinctSystem.cpp"
    "{ai.as_posix()}/InstinctLocomotionSystem.cpp")
luminumbra_configure_first_party_target(luminumbra_common)
add_library(luminumbra_client STATIC ../probe.cpp ../probe.c)
add_library(luminumbra_client_qa STATIC ../probe.cpp)
luminumbra_baseline_startup("{entry}")
foreach(application IN ITEMS luminumbra_client_app luminumbra_client_qa_app luminumbra_server_app)
  add_executable(${{application}} "{entry}")
  luminumbra_configure_first_party_target(${{application}})
endforeach()
add_subdirectory(nested)
add_subdirectory("${{CMAKE_SOURCE_DIR}}/external" "${{CMAKE_BINARY_DIR}}/external")
add_subdirectory("${{CMAKE_SOURCE_DIR}}/src-neighbor" "${{CMAKE_BINARY_DIR}}/neighbor")
''')
            write(root / 'src/nested/CMakeLists.txt',
                  '''add_library(isa_objects OBJECT ../../probe.cpp)
add_library(first_party_headers INTERFACE)
add_library(first_party_import INTERFACE IMPORTED)
add_library(isa_alias ALIAS isa_objects)
add_subdirectory(empty)
''')
            write(root / 'src/nested/empty/CMakeLists.txt', '# No compiled targets here.')
            write(root / 'external/CMakeLists.txt', 'add_library(fetched STATIC ../probe.cpp)')
            write(root / 'src-neighbor/CMakeLists.txt', 'add_library(neighbor STATIC ../probe.cpp)')
            write(root / 'tools/CMakeLists.txt', '''
add_library(station_discovery_numeric_entry OBJECT ../probe.cpp)
luminumbra_configure_first_party_target(station_discovery_numeric_entry)
add_subdirectory(preview)
''')
            write(root / 'tools/preview/CMakeLists.txt', '''
add_library(luminumbra_render_static SHARED ../../probe.cpp)
luminumbra_configure_first_party_target(luminumbra_render_static)
add_library(runtime_module MODULE ../../probe.cpp)
add_custom_target(data_only)
''')
            write(root / 'test/CMakeLists.txt', '''
add_executable(standalone_test ../probe.cpp)
add_executable(warning_checked_test ../probe.cpp)
luminumbra_configure_first_party_target(warning_checked_test)
''')
            result = subprocess.run([cmake, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + ninja.as_posix(),
                                     '-S', str(root), '-B', str(root / 'build')],
                                    capture_output=True, text=True, check=False, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = json.loads((root / 'build/compile_commands.json').read_text(encoding='utf-8'))
        return commands

    def check_commands(self, commands, *, msvc, warnings, testing):
        self.assertEqual(len(commands), 18 if testing else 16)
        for row in commands:
            command = row['command'].replace('\\', '/')
            external = any('/' + name + '.dir/' in command
                           for name in ('dependency', 'fetched', 'neighbor'))
            c_source = Path(row['file']).suffix == '.c'
            with self.subTest(source=row['file'], command=row['command']):
                exception_flags = re.findall(r'(?<!\S)[/-]EH[^\s"]+', command)
                if msvc and not external and not c_source:
                    self.assertTrue(exception_flags, command)
                    self.assertEqual(exception_flags[-1], '/EHsc')
                else:
                    self.assertEqual(exception_flags, [])
                checked = any('/' + name + '.dir/' in command for name in (
                    'luminumbra_common', 'luminumbra_client_app', 'luminumbra_client_qa_app',
                    'luminumbra_server_app', 'station_discovery_numeric_entry',
                    'luminumbra_render_static', 'warning_checked_test'))
                if checked:
                    self.assertIn('/W4' if msvc else '-Wall', command)
                    self.assertIn('/permissive-' if msvc else '-Wextra', command)
                    self.assertIn('/Zc:__cplusplus' if msvc else '-Wpedantic', command)
                    self.assertEqual(('/WX' if msvc else '-Werror') in command, warnings)
                if 'RuntimeEntry.cpp' in row['file']:
                    self.assertIn('/arch:SSE2' if msvc else '-march=x86-64', command)
                    self.assertIn('/GL-' if msvc else '-fno-lto', command)

    def test_effective_cpp_options_survive_replaced_global_flags(self):
        for config, warnings, testing in (('Debug', True, True), ('Release', False, False),
                                         ('RelWithDebInfo', True, True)):
            with self.subTest(config=config, warnings=warnings, testing=testing):
                self.check_commands(self.configure(config=config, warnings=warnings, testing=testing),
                                    msvc=True, warnings=warnings, testing=testing)

    def test_posix_options_and_warning_policy_are_preserved(self):
        self.check_commands(self.configure(msvc=False, flags='-g'),
                            msvc=False, warnings=True, testing=True)

    def test_ci_symbol_flags_preserve_exceptions_for_dependencies_too(self):
        workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        flags = re.search(r"'-DCMAKE_CXX_FLAGS=([^']+)'", workflow)
        self.assertIsNotNone(flags, 'the MSVC cache argument must retain both options')
        self.assertEqual(flags[1].split(), ['/Z7', '/EHsc'])
        for row in self.configure(flags=flags[1]):
            if Path(row['file']).suffix == '.cpp':
                exception_flags = re.findall(r'(?<!\S)[/-]EH[^\s"]+', row['command'])
                self.assertTrue(exception_flags, row['command'])
                self.assertEqual(exception_flags[-1], '/EHsc')

    def test_tools_are_located_without_a_developer_shell_or_executable_probes(self):
        with tempfile.TemporaryDirectory(prefix='installed-tools-') as directory, \
                patch.object(shutil, 'which', return_value=None), \
                patch.dict(os.environ, {'ProgramFiles': directory}, clear=True):
            for language in ('C', 'CXX'):
                with self.assertRaises(AssertionError):
                    compiler_path(language)
            with self.assertRaises(AssertionError):
                ninja_path()
            installation = Path(directory) / 'Microsoft Visual Studio/18/Enterprise'
            compiler = installation / 'VC/Tools/MSVC/14.40/bin/Hostx64/x64/cl.exe'
            ninja = installation / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
            write(compiler, 'fixture; never executed')
            write(ninja, 'fixture; never executed')
            self.assertEqual(compiler_path('C'), compiler)
            self.assertEqual(compiler_path('CXX'), compiler)
            self.assertEqual(ninja_path(), ninja)


if __name__ == '__main__':
    unittest.main()
