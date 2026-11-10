#!/usr/bin/env python3
"""Performance floor verdict tests, run in the existing cross-platform Python CI lane."""
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))

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

    def test_window_visibility_evidence_for_both_schemas(self):
        script = FIXTURES / 'save-load.traversal'
        for schema in contract.SCHEMAS:
            for mode in ('windowed', 'borderless', 'fullscreen', 'headless'):
                for visible in (True, False, None, 0, 1, 'true', 'false', [], {}, 'missing'):
                    data = (base.workload_artifact('save_load', script)
                            if schema == contract.WORKLOAD_SCHEMA else base.artifact())
                    render = data['settings']['effective_render']
                    render.update(window_mode=mode, window_visible=visible)
                    if visible == 'missing':
                        del render['window_visible']
                    with self.subTest(schema=schema, mode=mode, visible=visible):
                        if mode == 'headless' and schema == contract.WORKLOAD_SCHEMA:
                            with self.assertRaisesRegex(budget.BudgetInputError, 'headless'):
                                budget.evaluate(data)
                        elif mode == 'headless' or visible is True:
                            self.assertTrue(budget.verdict_passes(budget.evaluate(data)))
                        else:
                            message = 'hidden-window' if visible is False else 'window_visible'
                            with self.assertRaisesRegex(budget.BudgetInputError, message):
                                budget.evaluate(data)

    def test_missing_or_malformed_visibility_blocks(self):
        for schema in contract.SCHEMAS:
            for settings in (None, [], {}, {'effective_render': None}, {'effective_render': []}, 'missing'):
                data = base.artifact()
                data['schema'] = schema
                data['settings'] = settings
                if settings == 'missing':
                    del data['settings']
                with self.subTest(schema=schema, settings=settings), self.assertRaisesRegex(budget.BudgetInputError, 'window_visible'):
                    budget.evaluate(data)

    def test_cli_refuses_hidden_override_and_missing_visibility(self):
        script = FIXTURES / 'save-load.traversal'
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'capture.json'
            verdict = Path(tmp) / 'verdict.json'
            for schema in contract.SCHEMAS:
                for visible in (False, 'missing'):
                    data = (base.workload_artifact('save_load', script)
                            if schema == contract.WORKLOAD_SCHEMA else base.artifact())
                    render = data['settings']['effective_render']
                    if visible is False:
                        render['window_visible'] = False
                        override = {'cli:--hidden-window': 'true', 'cli:--window-mode': 'windowed'}
                        data['debug_overrides'] = {'active': override, 'declared': dict(override)}
                        data['settings']['render_overrides'] = dict(override)
                    else:
                        del render['window_visible']
                    if schema == contract.WORKLOAD_SCHEMA:
                        data['render_identity'] = contract.render_identity(data['settings'])
                    path.write_text(json.dumps(data), encoding='utf-8')
                    for flags in ((), ('--enforce-floor', '--verdict-out', str(verdict))):
                        extra = ('--traversal', str(script)) if schema == contract.WORKLOAD_SCHEMA else ()
                        with self.subTest(schema=schema, visible=visible, flags=flags):
                            result = self.run_cli(path, *extra, *flags)
                            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                            message = 'hidden-window' if visible is False else 'window_visible'
                            self.assertIn(message, result.stdout + result.stderr)
                            self.assertFalse(verdict.exists())

    def test_v4_headless_cannot_pass_floor(self):
        script = FIXTURES / 'save-load.traversal'
        data = base.workload_artifact('save_load', script)
        self.assertTrue(budget.verdict_passes(budget.evaluate(data)))
        data['settings']['effective_render']['window_mode'] = 'headless'
        data['render_identity'] = contract.render_identity(data['settings'])
        with self.assertRaisesRegex(budget.BudgetInputError, 'headless'):
            budget.evaluate(data)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'capture.json'
            path.write_text(json.dumps(data), encoding='utf-8')
            for flags in ((), ('--enforce-floor',)):
                with self.subTest(flags=flags):
                    result = self.run_cli(path, '--traversal', str(script), *flags)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn('headless', result.stdout + result.stderr)

    def test_target_flags_do_not_fail(self):
        slow = budget.evaluate(with_wall([9.0, 9.0, 9.0, 9.0]))
        self.assertEqual(slow['floor']['verdict'], 'pass')
        self.assertFalse(slow['target']['p50_met'])
        self.assertNotIn('p99_met', slow['target'])  # quality reports p50 only
        fast = budget.evaluate(with_wall([8.0, 8.0, 8.0, 8.0]))
        self.assertEqual(fast['floor']['verdict'], 'pass')
        self.assertTrue(fast['target']['p50_met'])

    def test_performance_profile_reports_p50_and_p99_target(self):
        block = budget.target_block('performance', 8.0, 9.0)
        self.assertTrue(block['p50_met'])
        self.assertFalse(block['p99_met'])
        self.assertEqual(block['p99_ms'], 9.0)
        self.assertNotIn('p99_met', budget.target_block('quality', 8.0, 9.0))

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
        return subprocess.run(base.isolated_script(capture.__file__, str(path),
                                        '--qualified-renderer', base.QUALIFIED, *extra),
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
        result = subprocess.run(base.isolated_script(capture.__file__, str(FIXTURES / 'adapter-mismatch.v3.json'),
                                      '--qualified-renderer', base.QUALIFIED, '--enforce-floor'),
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)


if __name__ == '__main__':
    unittest.main()
