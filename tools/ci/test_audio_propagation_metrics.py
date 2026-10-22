"""Check production metrics lock coverage; native cases exercise public behavior."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/luminumbra_client/audio/AudioPropagationSystem.h"
SOURCE = HEADER.with_suffix(".cpp")


def code(path):
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                  "", path.read_text(encoding="utf-8"), flags=re.DOTALL)


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1:end - 1]


def scopes(source):
    """Each position's lexical scope; guards live until that scope closes."""
    stack = []
    result = []
    for position, char in enumerate(source):
        result.append(tuple(stack))
        if char == "{":
            stack.append(position)
        elif char == "}":
            stack.pop()
    return result


class AudioPropagationMetricsContract(unittest.TestCase):
    def test_every_metrics_access_is_dominated_by_a_live_guard(self):
        header, source = code(HEADER), code(SOURCE)
        self.assertRegex(header, r'mutable\s+std::mutex\s+m_metrics_mutex\s*;')
        self.assertNotIn("recursive_mutex", header)
        self.assertNotRegex(header + source, r'm_metrics_mutex\.(lock|unlock|try_lock)')
        # Check the whole implementation, including future member methods. Merely
        # mentioning a mutex or locking a sibling scope cannot protect an access.
        for text in (header, source):
            lexical = scopes(text)
            guards = list(re.finditer(
                r'std::lock_guard<std::mutex>\s+\w+\(m_metrics_mutex\);', text))
            for access in re.finditer(r'\bm_metrics\b', text):
                if re.search(r'PerformanceMetrics\s+$', text[:access.start()]):
                    continue  # storage declaration, not an access
                with self.subTest(access=text[access.start():access.start() + 60]):
                    self.assertTrue(any(
                        guard.end() <= access.start()
                        and lexical[guard.start()] == lexical[access.start()][:len(lexical[guard.start()])]
                        for guard in guards), "unguarded production metrics access")

    def test_snapshot_is_a_value_and_reset_preserves_in_flight_gauge(self):
        header = code(HEADER)
        self.assertRegex(header, r'\bPerformanceMetrics\s+GetMetrics\(\)\s+const\s*\{')
        self.assertIn("return m_metrics;", body(header, "GetMetrics()"))
        reset = body(header, "ResetMetrics()")
        self.assertRegex(reset, r'const int active_calculations = m_metrics.active_calculations;\s*'
                               r'm_metrics = \{\};\s*'
                               r'm_metrics.active_calculations = active_calculations;')

    def test_worker_locks_end_before_audio_work_and_callbacks(self):
        worker = body(code(SOURCE), "void AudioPropagationSystem::WorkerThreadFunction()")
        lexical = scopes(worker)
        guards = list(re.finditer(r'std::lock_guard<std::mutex>\s+\w+\(m_metrics_mutex\);', worker))
        self.assertEqual(len(guards), 2)
        for operation in ("CalculatePropagation(", "task.callback(result)", "m_task_cv.wait("):
            position = worker.index(operation)
            for guard in guards:
                self.assertFalse(guard.end() < position and
                                 lexical[guard.start()] == lexical[position][:len(lexical[guard.start()])])
        # The worker's task lock and both gauge updates have disjoint scopes.
        task_guard = worker.index("std::unique_lock<std::mutex>")
        for guard in guards:
            self.assertNotEqual(lexical[task_guard], lexical[guard.start()])
            self.assertNotIn(lexical[task_guard][-1], lexical[guard.start()])
        self.assertLess(worker.index("m_metrics.active_calculations++"), worker.index("CalculatePropagation("))
        self.assertGreater(worker.index("m_metrics.active_calculations--"), worker.index("task.callback(result)"))

    def test_native_cases_and_python_guard_are_registered(self):
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        target = body(cmake.replace("(", "{").replace(")", "}"),
                      "add_executable{audio_propagation_metrics_test")
        self.assertIn("audio/audio_propagation_metrics_test.cpp", target)
        self.assertIn("../src/luminumbra_client/audio/AudioPropagationSystem.cpp", target)
        self.assertIn("    audio_propagation_metrics_test\n", cmake)
        self.assertIn("add_test(NAME AudioPropagationMetricsContract", cmake)
        self.assertIn("tools/ci/test_audio_propagation_metrics.py", cmake)
        self.assertIn('"tools/ci/test_audio_propagation_metrics.py"',
                      (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8"))
        native = (ROOT / "test/audio/audio_propagation_metrics_test.cpp").read_text(encoding="utf-8")
        for name in ("SnapshotsOwnAllFieldsAndResetClearsStatistics",
                     "ConcurrentUpdatesAndReadsKeepExactTotalsAndCoherentSnapshots",
                     "ResetPreservesInFlightCallbackAndCountsLaterUpdates"):
            self.assertIn(f"TEST(AudioPropagationMetrics, {name})", native)
        self.assertNotIn("#define private", native)
        self.assertNotIn("sleep", code(ROOT / "test/audio/audio_propagation_metrics_test.cpp"))


if __name__ == "__main__":
    unittest.main()
