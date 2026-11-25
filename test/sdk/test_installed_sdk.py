"""Installed header contracts; --consume also builds and runs the C/C++ fixture."""

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PUBLIC_HEADERS = {
    "include/luminumbra/abi/luminumbra_abi.h": "include/luminumbra/abi/luminumbra_abi.h",
    "include/luminumbra/modules/ModuleRegistry.h": "src/luminumbra_common/core/ModuleRegistry.h",
    "include/luminumbra/modules/PhaseScheduler.h": "src/luminumbra_common/core/PhaseScheduler.h",
}
PACKAGE_FILES = {
    "lib/cmake/Luminumbra/LuminumbraTargets.cmake",
    "lib/cmake/Luminumbra/LuminumbraConfig.cmake",
    "lib/cmake/Luminumbra/LuminumbraConfigVersion.cmake",
}


def run(*args, cwd):
    return subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True,
                          timeout=15)


def install_relocated(root):
    """Use a source tree containing only the package recipe and three headers."""
    source = root / "producer source"
    build = root / "producer build"
    original = root / "original prefix"
    relocated = root / "relocated prefix"
    # The fixture copies a fixed six files; it never scans engine or content trees.
    for relative in ("CMakeLists.txt", "tools/sdk/CMakeLists.txt",
                     "tools/sdk/LuminumbraConfig.cmake.in", *PUBLIC_HEADERS.values()):
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / relative, target)
    run("cmake", "-S", str(source / "tools/sdk"), "-B", str(build), cwd=root)
    run("cmake", "--install", str(build), "--prefix", str(original),
        "--component", "SDK", cwd=root)
    original.rename(relocated)
    shutil.rmtree(source)
    shutil.rmtree(build)
    return relocated


def probe(root, prefix, name, package_request, assertions=""):
    source = root / name
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(InstalledSdkProbe LANGUAGES NONE)\n"
        "set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)\n"
        "set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)\n"
        f"find_package(Luminumbra {package_request})\n{assertions}\n", encoding="utf-8")
    return subprocess.run(
        ["cmake", "-S", str(source), "-B", str(root / (name + " build")),
         f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}"], cwd=root, capture_output=True, text=True,
        timeout=15)


class InstalledSdkPackage(unittest.TestCase):
    def test_isolated_relocated_install_and_components(self):
        with tempfile.TemporaryDirectory(prefix="installed-sdk-") as temporary:
            root = Path(temporary)
            prefix = install_relocated(root)
            files = {path.relative_to(prefix).as_posix() for path in prefix.rglob("*")
                     if path.is_file()}
            self.assertEqual(files, set(PUBLIC_HEADERS) | PACKAGE_FILES)
            for installed, source in PUBLIC_HEADERS.items():
                self.assertEqual((prefix / installed).read_bytes(), (ROOT / source).read_bytes())
            for relative in PACKAGE_FILES:
                text = (prefix / relative).read_text(encoding="utf-8")
                for private in (str(ROOT), ROOT.as_posix(), str(root), root.as_posix()):
                    self.assertNotIn(private, text)
                self.assertNotIn("find_dependency(", text)

            result = probe(root, prefix, "valid", "0.3.0 EXACT CONFIG REQUIRED COMPONENTS ABI ModuleContracts", r'''
get_target_property(abi_imported Luminumbra::ABI IMPORTED)
get_target_property(modules_imported Luminumbra::ModuleContracts IMPORTED)
get_target_property(abi_type Luminumbra::ABI TYPE)
get_target_property(modules_type Luminumbra::ModuleContracts TYPE)
get_target_property(includes Luminumbra::ABI INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(abi_links Luminumbra::ABI INTERFACE_LINK_LIBRARIES)
get_target_property(abi_features Luminumbra::ABI INTERFACE_COMPILE_FEATURES)
get_target_property(links Luminumbra::ModuleContracts INTERFACE_LINK_LIBRARIES)
get_target_property(features Luminumbra::ModuleContracts INTERFACE_COMPILE_FEATURES)
if(NOT abi_imported OR NOT modules_imported OR NOT abi_type STREQUAL "INTERFACE_LIBRARY"
        OR NOT modules_type STREQUAL "INTERFACE_LIBRARY")
    message(FATAL_ERROR "Expected imported header contract targets")
endif()
if(NOT includes STREQUAL "${CMAKE_PREFIX_PATH}/include" OR abi_links OR abi_features
        OR NOT links STREQUAL "Luminumbra::ABI" OR NOT features STREQUAL "cxx_std_20")
    message(FATAL_ERROR "Unexpected public dependency graph")
endif()
''')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = probe(root, prefix, "unknown", "CONFIG REQUIRED COMPONENTS Renderer")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Luminumbra_FOUND", result.stdout + result.stderr)
            result = probe(root, prefix, "wrong version", "1.0 CONFIG REQUIRED")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("version", (result.stdout + result.stderr).lower())
            result = probe(root, prefix, "optional", "CONFIG REQUIRED OPTIONAL_COMPONENTS Renderer",
                           'if(Luminumbra_Renderer_FOUND)\nmessage(FATAL_ERROR "Unknown component found")\nendif()')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_registration_and_standalone_version(self):
        main_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        sdk_cmake = (ROOT / "tools/sdk/CMakeLists.txt").read_text(encoding="utf-8")
        pattern = r"project\(Luminumbra VERSION ([0-9.]+) "
        self.assertEqual(re.search(pattern, main_cmake)[1], re.search(pattern, sdk_cmake)[1])
        self.assertLess(main_cmake.index("add_subdirectory(tools/sdk)"),
                        main_cmake.index("include(cmake/dependencies.cmake)"))
        test_cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_test(NAME InstalledSdkPackageContract", test_cmake)
        for target, source, capability in (("abi", "abi.c", "ABI"),
                                            ("modules", "main.cpp", "ModuleContracts")):
            self.assertIn(f"add_library(installed_sdk_{target}_compile OBJECT EXCLUDE_FROM_ALL "
                          f"sdk/installed_consumer/{source})", test_cmake)
            self.assertIn(f"target_link_libraries(installed_sdk_{target}_compile PRIVATE "
                          f"Luminumbra::{capability})", test_cmake)
        self.assertRegex(test_cmake, r'if\(CMAKE_SYSTEM_NAME STREQUAL "Linux"\)\s+'
                                    r'add_test\(NAME InstalledSdkConsumer')
        self.assertIn('"test/sdk/test_installed_sdk.py",',
                      (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8"))


def consume():
    with tempfile.TemporaryDirectory(prefix="installed-sdk-consumer-") as temporary:
        root = Path(temporary)
        prefix = install_relocated(root)
        source = root / "consumer source"
        shutil.copytree(ROOT / "test/sdk/installed_consumer", source)
        build = root / "consumer build"
        run("cmake", "-S", str(source), "-B", str(build),
            f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}", "-DCMAKE_BUILD_TYPE=Release", cwd=root)
        run("cmake", "--build", str(build), "--config", "Release", "--parallel", "2", cwd=root)
        result = run("ctest", "--test-dir", str(build), "-C", "Release",
                     "--output-on-failure", cwd=root)
        print(result.stdout, end="")


if __name__ == "__main__":
    if sys.argv[1:] == ["--consume"]:
        consume()
    else:
        unittest.main()
