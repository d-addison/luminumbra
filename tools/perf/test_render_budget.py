#!/usr/bin/env python3
"""Performance floor verdict tests, run in the existing cross-platform Python CI lane."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import perf
import render_budget as budget
import render_contract as contract
import test_render_contract as base
import validate_render_capture as capture

FIXTURES = Path(__file__).parent / 'fixtures'


def with_wall(wall_values):
    """Synthetic v3 artifact whose frame_wall_ms samples are exactly wall_values."""
    data = copy.deepcopy(base.artifact())
    for row, value in zip(data['frame_samples'], wall_values):
        row['frame_wall_ms'] = value
    summary = perf.summarize(list(wall_values), 'ms')
    block = {key: summary[key] for key in contract.STATS}
    block.update(unit='ms', sample_count=len(wall_values), unavailable_count=0)
    data['distribution']['frame_wall_ms'] = block
    data['avg']['frame_wall_ms'] = sum(wall_values) / len(wall_values)
    return data


class RenderBudgetTests(unittest.TestCase):
    def test_floor_pass_at_exact_limit(self):
        data = with_wall([16.67, 16.67, 16.67, 16.67])
        verdict = budget.evaluate(data)
        self.assertEqual(verdict['floor']['p99_ms'], 16.67)
        self.assertEqual(verdict['floor']['verdict'], 'pass')
        self.assertTrue(budget.verdict_passes(verdict))
        self.assertEqual(capture.validate(data, base.args())['verdict'], 'PASS')

    def test_floor_fail_just_above(self):
        verdict = budget.evaluate(with_wall([16.68, 16.68, 16.68, 16.68]))
        self.assertEqual(verdict['floor']['verdict'], 'fail')
        self.assertFalse(budget.verdict_passes(verdict))

    def test_target_flags_do_not_fail(self):
        slow = budget.evaluate(with_wall([9.0, 9.0, 9.0, 9.0]))
        self.assertEqual(slow['floor']['verdict'], 'pass')
        self.assertFalse(slow['target']['p50_met'])
        self.assertFalse(slow['target']['p99_met'])
        fast = budget.evaluate(with_wall([8.0, 8.0, 8.0, 8.0]))
        self.assertEqual(fast['floor']['verdict'], 'pass')
        self.assertTrue(fast['target']['p50_met'])
        self.assertTrue(fast['target']['p99_met'])

    def test_verdict_shape(self):
        verdict = budget.evaluate(base.artifact())
        self.assertEqual(verdict['schema'], budget.VERDICT_SCHEMA)
        self.assertEqual(verdict['frames'], 4)
        self.assertEqual(verdict['adapter']['renderer'], base.QUALIFIED)
        self.assertEqual(verdict['floor']['limit_ms'], budget.FLOOR_MS)
        self.assertEqual(verdict['target']['limit_ms'], budget.TARGET_MS)

    def test_input_errors(self):
        data = with_wall([5.0, 6.0, 8.0, 14.0])
        data['schema'] = 'luminumbra.render_benchmark.v2'
        with self.assertRaises(budget.BudgetInputError):
            budget.evaluate(data)

        data = with_wall([5.0, 6.0, 8.0, 14.0])
        data['distribution']['frame_wall_ms']['unavailable_count'] = 1
        with self.assertRaises(budget.BudgetInputError):
            budget.evaluate(data)

        data = with_wall([5.0, 6.0, 8.0, 14.0])
        data['distribution']['frame_wall_ms']['sample_count'] = 3
        with self.assertRaises(budget.BudgetInputError):
            budget.evaluate(data)

        data = with_wall([5.0, 6.0, 8.0, 14.0])
        del data['distribution']['frame_wall_ms']['p99']
        with self.assertRaises(budget.BudgetInputError):
            budget.evaluate(data)

    def run_cli(self, path, *extra):
        return subprocess.run([sys.executable, str(Path(capture.__file__)), str(path),
                               '--qualified-renderer', base.QUALIFIED, *extra],
                              capture_output=True, text=True, check=False)

    def test_cli_floor_flag_is_opt_in(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'slow.json'
            path.write_text(json.dumps(with_wall([16.68, 16.68, 16.68, 16.68])), encoding='utf-8')
            result = self.run_cli(path)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertNotIn('floor', json.loads(result.stdout))

            result = self.run_cli(path, '--enforce-floor')
            self.assertEqual(result.returncode, 4, result.stdout)
            self.assertEqual(json.loads(result.stdout)['floor']['floor']['verdict'], 'fail')

    def test_cli_passing_verdict_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'fast.json'
            verdict_path = Path(tmp) / 'verdict.json'
            path.write_text(json.dumps(with_wall([5.0, 6.0, 8.0, 14.0])), encoding='utf-8')
            result = self.run_cli(path, '--enforce-floor', '--verdict-out', str(verdict_path))
            self.assertEqual(result.returncode, 0, result.stdout)
            written = json.loads(verdict_path.read_text(encoding='utf-8'))
            self.assertEqual(written['schema'], budget.VERDICT_SCHEMA)
            self.assertEqual(written['floor']['verdict'], 'pass')

    def test_cli_verdict_out_requires_enforce_floor(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'fast.json'
            path.write_text(json.dumps(with_wall([5.0, 6.0, 8.0, 14.0])), encoding='utf-8')
            result = self.run_cli(path, '--verdict-out', str(Path(tmp) / 'verdict.json'))
            self.assertEqual(result.returncode, 2)

    def test_cli_adapter_mismatch_with_enforce_floor(self):
        result = subprocess.run([sys.executable, str(Path(capture.__file__)),
                                 str(FIXTURES / 'adapter-mismatch.v3.json'),
                                 '--qualified-renderer', base.QUALIFIED, '--enforce-floor'],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)


if __name__ == '__main__':
    unittest.main()
