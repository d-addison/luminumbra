"""Guard native fixture capture wiring without running the capture executable."""

import ast
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CONTRACT = "test/tools/test_region_fixture_capture_contract.py"


def source(path):
    text = (ROOT / path).read_text(encoding="utf-8")
    return re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                  lambda match: "" if match[0].startswith(("//", "/*")) else match[0],
                  text, flags=re.DOTALL)


class RegionFixtureCaptureContract(unittest.TestCase):
    def test_publication_regressions_are_unconditional_and_registered(self):
        native = source("test/common/RegionFixtureCapture_test.cpp")
        publication = source("test/support/RegionFixtureCapturePublication.h")
        for text in (native, publication):
            self.assertNotRegex(text, r"GTEST_SKIP|DISABLED_|getenv|RequestedCapture")
            self.assertNotIn("LUMINUMBRA_REGION_FIXTURE_CAPTURE", text)
        self.assertEqual(re.findall(r"TEST\(RegionFixtureCapture, (\w+)\)", native), [
            "PublicationRefusesExistingDestinationAndCleansStaging",
            "PublicationCreatesNewDestinationAndCleansStaging",
            "PublicationFailuresLeaveNoStagingOrDestination"])
        cmake = re.sub(r"#[^\n]*", "", source("test/CMakeLists.txt"))
        executables = re.findall(r"add_executable\(\s*(\w+)\s+([^)]*)\)", cmake)
        self.assertEqual([name for name, files in executables
                          if "common/RegionFixtureCapture_test.cpp" in files], ["common_tests"])
        roster = re.search(r"set\(LUMINUMBRA_GTEST_TARGETS\s+([^)]*)\)", cmake)[1]
        self.assertIn("common_tests", roster.split())
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_capture_executable_is_built_separately_and_never_discovered(self):
        cmake = re.sub(r"#[^\n]*", "", source("test/CMakeLists.txt"))
        executables = re.findall(r"add_executable\(\s*(\w+)\s+([^)]*)\)", cmake)
        self.assertEqual([name for name, files in executables
                          if "tools/RegionFixtureCapture.cpp" in files], ["region_fixture_capture"])
        self.assertIn(("region_fixture_capture", "tools/RegionFixtureCapture.cpp"), executables)
        for command in re.findall(r"\b(?:add_test|gtest_discover_tests|set|list)\s*\(([^)]*)\)",
                                  cmake):
            self.assertNotRegex(command, r"\bregion_fixture_capture\b")
            self.assertNotIn("RegionFixtureCapture.Emit", command)
        native = source("test/tools/RegionFixtureCapture.cpp")
        self.assertNotRegex(native, r"\bTEST(?:_F|_P)?\s*\(|GTEST_SKIP|DISABLED_")
        self.assertIn("int main()", native)
        self.assertIn("return EXIT_FAILURE;", native)

    def test_capture_target_has_test_include_and_link_interface(self):
        cmake = re.sub(r"#[^\n]*", "", source("test/CMakeLists.txt"))
        self.assertRegex(cmake, r"target_include_directories\(region_fixture_capture\s+PRIVATE\s+"
                         r"\.\./src\s*\)")
        self.assertRegex(cmake, r"target_link_libraries\(region_fixture_capture\s+PRIVATE\s+"
                         r"gtest\s+luminumbra_common\s*\)")
        self.assertIn("luminumbra_configure_first_party_target(region_fixture_capture)", cmake)
        self.assertRegex(cmake, r"target_compile_definitions\(region_fixture_capture\s+PRIVATE\s+"
                         r'LUMINUMBRA_SOURCE_ROOT="\$\{CMAKE_SOURCE_DIR\}"\s*\)')

    def test_contract_runs_in_ctest_and_standalone_ci(self):
        cmake = re.sub(r"#[^\n]*", "", source("test/CMakeLists.txt"))
        self.assertRegex(cmake, r"add_test\(NAME RegionFixtureCaptureContract\s+"
                         r'COMMAND "\$\{Python3_EXECUTABLE\}" -I -B\s+"\$\{CMAKE_SOURCE_DIR\}/'
                         + re.escape(CONTRACT) + r'"\)')
        tree = ast.parse((ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8"))
        roster = next(ast.literal_eval(node.value) for node in tree.body
                      if isinstance(node, ast.Assign)
                      and any(isinstance(target, ast.Name) and target.id == "PYTHON_TESTS"
                              for target in node.targets))
        self.assertEqual(roster.count(CONTRACT), 1)

    def test_capture_admission_platform_branches_and_inventory(self):
        native = source("test/tools/RegionFixtureCapture.cpp")
        for condition, message in (
                ("!RequestedCapture(destination)",
                 "set LUMINUMBRA_REGION_FIXTURE_CAPTURE to request a capture"),
                ("!Luminumbra::Filesystem::IsAbsolutePath(destination)",
                 "capture destination must be absolute"),
                ("IsWithinSourceTree(parent)", "capture destination must be outside source tree"),
                ("fs::exists(destination)", "capture destination must be new"),
                ("payload.size() > (16U << 20)", "capture payload exceeds size limit")):
            with self.subTest(condition=condition):
                self.assertIn('if (' + condition + ') {\n        throw std::runtime_error("'
                              + message + '");\n    }', native)
        for admission in ("!RequestedCapture(destination)", "IsWithinSourceTree(parent)",
                          "fs::exists(destination)"):
            self.assertLess(native.index(admission), native.index("Capture(false)"))
        self.assertLess(native.index("payload.size() >"),
                        native.index("PublishCapture(destination, payload)"))
        for reader in ('_wdupenv_s(&value, &size, L"LUMINUMBRA_REGION_FIXTURE_CAPTURE")',
                       '_wgetenv(L"LUMINUMBRA_REGION_FIXTURE_CAPTURE")',
                       'std::getenv("LUMINUMBRA_REGION_FIXTURE_CAPTURE")'):
            self.assertIn(reader, native)
        for member in ("plant-entities.json", "r.0.0.lmr", "snapshot.json", "world-manifest.json"):
            self.assertIn('"chunks/region/' + member + '"', native)
        self.assertIn('"devel-off", Capture(false)', native)
        self.assertIn('"devel-off-plant-edit", Capture(true)', native)
        world = source("test/support/RegionFixtureWorld.h")
        for flag in ("ActiveRegions", "AetherState", "WaterHighRes", "WeatherRain",
                     "WeatherEvents", "FiniteHydrology"):
            self.assertIn("session.Set" + flag + "Enabled(false);", world)


if __name__ == "__main__":
    unittest.main()
