"""Guard replay fixture symbol ownership without claiming native link coverage."""

import ast
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = "test/persistence/host_scheduler_fault_test.cpp"
MODES = "src/luminumbra_server/modes/"
ENTRY_POINTS = {
    "RunRecord": "RunSchedulerFaultRecord",
    "RunReplay": "RunSchedulerFaultReplay",
    "RunMutateReplayFixture": "RunSchedulerFaultMutateReplayFixture",
    "RunServer": "RunSchedulerFaultServer",
}


def without_comments(source):
    # Keep strings intact so commented-out directives cannot satisfy the check.
    return re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                  lambda match: "" if match[0].startswith(("//", "/*")) else match[0],
                  source, flags=re.DOTALL)


def read_source(path):
    return without_comments((ROOT / path).read_text(encoding="utf-8"))


class ReplaySourceContract(unittest.TestCase):
    def assert_fixture_ownership(self, fixture):
        includes = list(re.finditer(
            r'^#include "../../src/luminumbra_server/modes/(Replay|Server)\.cpp"$',
            fixture, re.MULTILINE))
        self.assertEqual([match[1] for match in includes], ["Replay", "Server"])
        first, last = includes[0].start(), includes[-1].end()
        prefix, suffix = fixture[:first], fixture[last:]
        self.assertIn('#include "luminumbra_server/modes/Modes.h"', prefix)
        # Discover every external mode definition, including the fixture mutator
        # even though the fault cases do not call it. All need distinct symbols.
        exported = set()
        for mode in ("Replay", "Server"):
            exported.update(re.findall(
                r'^int (Run\w+)\(const ServerCliOptions& options\)\s*\{',
                read_source(MODES + mode + ".cpp"), re.MULTILINE))
        self.assertEqual(exported, set(ENTRY_POINTS))
        self.assertEqual(len(set(ENTRY_POINTS.values())), len(exported))
        substitutions = {"ServerWorldRunner": "SchedulerFaultTestRunner", **ENTRY_POINTS}
        for original, adapted in substitutions.items():
            define = f"#define {original} {adapted}"
            undef = f"#undef {original}"
            self.assertEqual(fixture.count(define + "\n"), 1)
            self.assertEqual(fixture.count(undef + "\n"), 1)
            self.assertIn(define + "\n", prefix)
            self.assertIn(undef + "\n", suffix)
            # No intervening condition, undef or replacement may leave one
            # include with shipping names, or leak a macro into the test cases.
            scope = fixture[prefix.index(define):suffix.index(undef) + last]
            directives = re.findall(r'^#(\w+).*$', scope, re.MULTILINE)
            self.assertTrue(all(word in ("define", "include", "undef")
                                for word in directives))
            self.assertNotIn(undef, fixture[:last])
            self.assertLess(suffix.index(undef), suffix.index("TEST_F("))
        for original in ENTRY_POINTS:
            self.assertNotRegex(suffix, rf'\b{original}\s*\(')
        self.assertEqual(len(re.findall(r'\bRunSchedulerFaultRecord\(options\)', suffix)), 3)
        self.assertEqual(len(re.findall(r'\bRunSchedulerFaultServer\(options\)', suffix)), 1)

    def test_fixture_compiles_real_modes_with_distinct_entry_points(self):
        self.assert_fixture_ownership(read_source(FIXTURE))

    def test_every_missing_or_ineffective_substitution_is_detected(self):
        fixture = read_source(FIXTURE)
        for original, adapted in ENTRY_POINTS.items():
            define = f"#define {original} {adapted}\n"
            undef = f"#undef {original}\n"
            for mutation, changed in enumerate((
                fixture.replace(define, ""),
                fixture.replace(define, f"#define {original} {original}\n"),
                fixture.replace(undef, ""),
                fixture.replace(define, "").replace(undef, define + undef),
            )):
                with self.subTest(symbol=original, mutation=mutation), self.assertRaises(AssertionError):
                    self.assert_fixture_ownership(changed)

    def test_fault_cases_cannot_silently_call_unadapted_modes(self):
        fixture = read_source(FIXTURE)
        for original in ("RunRecord", "RunServer"):
            with self.subTest(symbol=original), self.assertRaises(AssertionError):
                self.assert_fixture_ownership(fixture.replace(
                    ENTRY_POINTS[original] + "(options)", original + "(options)", 1))

    def assert_shipping_owner(self, cmake, identity):
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)',
                           cmake, re.DOTALL)
        self.assertIsNotNone(target)
        for source in ("persistence/host_scheduler_fault_test.cpp",
                       "common/EffectivePresetIdentity_test.cpp",
                       "../" + MODES + "Replay.cpp"):
            self.assertEqual(target[1].split().count(source), 1)
        # The identity roundtrip must keep using the unadapted production bodies.
        self.assertRegex(identity, r'ASSERT_EQ\(RunRecord\(options\), 0\)')
        self.assertRegex(identity, r'ASSERT_EQ\(RunReplay\(options\), 0\)')
        self.assertNotIn("#define Run", identity)
        shipping = read_source("src/luminumbra_server/sources.cmake")
        for mode in ("Replay", "Server"):
            self.assertEqual(shipping.count(f'/modes/{mode}.cpp"'), 1)

    def test_shipping_replay_and_identity_roundtrip_keep_their_owner(self):
        self.assert_shipping_owner(read_source("test/CMakeLists.txt"),
                                   read_source("test/common/EffectivePresetIdentity_test.cpp"))

    def test_removing_shipping_replay_coverage_is_detected(self):
        cmake = read_source("test/CMakeLists.txt")
        identity = read_source("test/common/EffectivePresetIdentity_test.cpp")
        with self.assertRaises(AssertionError):
            self.assert_shipping_owner(cmake.replace("../" + MODES + "Replay.cpp", ""), identity)
        with self.assertRaises(AssertionError):
            self.assert_shipping_owner(cmake, identity.replace("RunReplay(options)", "FakeReplay(options)"))

    def test_contract_is_registered_in_ctest_and_both_python_platform_plans(self):
        path = "tools/ci/test_replay_source_contract.py"
        self.assertIn('add_test(NAME ReplaySourceContract COMMAND "${Python3_EXECUTABLE}" -I -B\n'
                      f'    ${{CMAKE_SOURCE_DIR}}/{path})', read_source("test/CMakeLists.txt"))
        plan = ast.parse(read_source("tools/ci/ci_paths.py"))
        tests = next(ast.literal_eval(node.value) for node in plan.body
                     if isinstance(node, ast.Assign)
                     and any(isinstance(name, ast.Name) and name.id == "PYTHON_TESTS"
                             for name in node.targets))
        self.assertEqual(tests.count(path), 1)
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("tools/ci/ci_paths.py python-tests", workflow)


if __name__ == "__main__":
    unittest.main()
