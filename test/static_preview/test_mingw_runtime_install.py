"""Portable CMake install/refusal controls using a compiler-response fixture.

These Linux controls do not execute a Windows compiler or qualify PE DLL loading.
The native installed consumer and restricted-PATH host runs supply that evidence.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


HELPER = Path(__file__).resolve().parents[2] / "tools/static_preview/InstallMinGWRuntime.cmake"
NAMES = ("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")
NOTICES = ("gcc-libs/README", "gcc-libs/COPYING3", "gcc-libs/COPYING.LIB",
           "gcc-libs/COPYING.RUNTIME", "winpthreads/COPYING")


@unittest.skipIf(os.name == "nt", "POSIX compiler-response fixture; native MinGW uses installed acceptance")
class RuntimeInstallContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="preview-runtime-install-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.compiler_bin = self.root / "toolchain with spaces/bin"
        self.compiler_bin.mkdir(parents=True)
        self.compiler = self.compiler_bin / "g++"
        self.responses = self.compiler_bin / "responses.json"
        self.files = {}
        for name in NAMES:
            path = self.compiler_bin / name
            path.write_bytes(("runtime fixture: " + name).encode())
            self.files[name] = str(path)
        for member in NOTICES:
            path = self.compiler_bin.parent / "share/licenses" / member
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("notice fixture: " + member)
        self.compiler.write_text(
            f"#!{sys.executable}\n"
            "import json,pathlib,sys\n"
            "responses=json.loads(pathlib.Path(__file__).with_name('responses.json').read_text())\n"
            "name=sys.argv[1].removeprefix('-print-file-name=')\n"
            "print(responses[name])\n"
            "sys.exit(responses.get('exit_code',0))\n")
        self.compiler.chmod(0o755)

    def configure(self, *, mingw=True, expected=0):
        self.responses.write_text(json.dumps(self.files))
        source = self.root / "source"
        source.mkdir()
        (source / "marker.txt").write_text("unrelated install survives")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(RuntimeInstallContract NONE)\n"
            f"set(MINGW {'TRUE' if mingw else 'FALSE'})\n"
            f"set(CMAKE_CXX_COMPILER [==[{self.compiler}]==])\n"
            f"include([==[{HELPER}]==])\n"
            "luminumbra_install_static_preview_mingw_runtime()\n"
            "install(FILES marker.txt DESTINATION share COMPONENT StaticPreview)\n")
        result = subprocess.run([shutil.which("cmake"), "-S", str(source), "-B", str(self.root / "build")],
                                capture_output=True, text=True, timeout=25)
        if expected == 0:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def install(self):
        sdk = self.root / "sdk"
        result = subprocess.run([shutil.which("cmake"), "--install", str(self.root / "build"),
                                 "--prefix", str(sdk), "--component", "StaticPreview"],
                                capture_output=True, text=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return {str(p.relative_to(sdk)): p.read_bytes() for p in sdk.rglob("*") if p.is_file()}

    def test_exact_compiler_files_and_notices_installed_without_path_lookup(self):
        self.configure()
        actual = self.install()
        expected = {"share/marker.txt": b"unrelated install survives"}
        expected.update({"bin/" + name: Path(self.files[name]).read_bytes() for name in NAMES})
        expected.update({"share/luminumbra-static/licenses/" + member:
                         (self.compiler_bin.parent / "share/licenses" / member).read_bytes()
                         for member in NOTICES})
        self.assertEqual(actual, expected)

    def test_non_mingw_never_invokes_compiler_and_preserves_install(self):
        self.compiler.unlink()
        self.files = {}
        self.configure(mingw=False)
        self.assertEqual(self.install(), {"share/marker.txt": b"unrelated install survives"})

    def test_unresolved_bare_filename_refused(self):
        self.files[NAMES[0]] = NAMES[0]
        self.assertIn("cannot resolve", self.configure(expected=1))

    def test_missing_file_refused(self):
        Path(self.files[NAMES[1]]).unlink()
        self.assertIn("cannot resolve", self.configure(expected=1))

    def test_outside_compiler_directory_refused(self):
        outside = self.root / NAMES[0]
        outside.write_bytes(b"another toolchain")
        self.files[NAMES[0]] = str(outside)
        self.assertIn("must resolve beside", self.configure(expected=1))

    def test_wrong_runtime_name_refused(self):
        self.files[NAMES[0]] = self.files[NAMES[1]]
        self.assertIn("must resolve beside", self.configure(expected=1))

    def test_multiple_paths_refused(self):
        self.files[NAMES[0]] += ";" + self.files[NAMES[1]]
        self.assertIn("cannot resolve", self.configure(expected=1))

    def test_directory_refused(self):
        path = Path(self.files[NAMES[2]])
        path.unlink()
        path.mkdir()
        self.assertIn("cannot resolve", self.configure(expected=1))

    def test_linked_runtime_refused(self):
        path = Path(self.files[NAMES[0]])
        path.unlink()
        path.symlink_to(self.files[NAMES[1]])
        self.assertIn("cannot resolve", self.configure(expected=1))

    def test_failed_compiler_refused(self):
        self.files["exit_code"] = 1
        self.assertIn("cannot resolve", self.configure(expected=1))

    def test_missing_runtime_notice_refused(self):
        (self.compiler_bin.parent / "share/licenses/gcc-libs/COPYING.RUNTIME").unlink()
        self.assertIn("runtime notice is missing", self.configure(expected=1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
