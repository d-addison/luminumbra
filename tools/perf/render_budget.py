#!/usr/bin/env python3
"""Render performance floor verdict over an already validated v3/v4 capture.

The floor is a hard gate on frame_wall_ms p99. The target is reported only and
never fails. Measurement validity lives in render_contract.py; this module only
judges a capture that has already passed it.
"""
import math

import render_contract

FLOOR_MS = 16.67
TARGET_MS = 8.33
VERDICT_SCHEMA = 'luminumbra.render_verdict.v1'


class BudgetInputError(ValueError):
    pass


def _finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def target_block(profile, p50, p99):
    """Quality reports p50 against the target; every other profile reports p50 and p99."""
    block = {'limit_ms': TARGET_MS, 'p50_ms': p50, 'p50_met': p50 <= TARGET_MS}
    if profile != 'quality':
        block.update(p99_ms=p99, p99_met=p99 <= TARGET_MS)
    return block


def evaluate(data):
    if data.get('schema') not in render_contract.SCHEMAS:
        raise BudgetInputError('floor requires a v3/v4 render capture')
    if (data['schema'] == render_contract.WORKLOAD_SCHEMA and
            data['settings']['effective_render']['window_mode'] == 'headless'):
        raise BudgetInputError('headless v4 captures cannot qualify the performance floor')
    block = data['distribution']['frame_wall_ms']
    p50 = block.get('p50')
    p99 = block.get('p99')
    if not _finite_number(p50) or not _finite_number(p99):
        raise BudgetInputError('frame_wall_ms distribution lacks numeric p50/p99')
    if block.get('unavailable_count') != 0:
        raise BudgetInputError('frame_wall_ms has unavailable samples')
    if block.get('sample_count') != data['frames']:
        raise BudgetInputError('frame_wall_ms sample_count does not match frames')
    return {
        'schema': VERDICT_SCHEMA,
        'workload_kind': data['workload']['kind'],
        'profile': data['profile']['name'],
        'adapter': {key: data['adapter'][key] for key in ('vendor', 'renderer', 'version')},
        'frames': data['frames'],
        'floor': {'limit_ms': FLOOR_MS, 'p99_ms': p99,
                  'verdict': 'pass' if p99 <= FLOOR_MS else 'fail'},
        'target': target_block(data['profile']['name'], p50, p99),
    }


def verdict_passes(verdict):
    return verdict['floor']['verdict'] == 'pass'
