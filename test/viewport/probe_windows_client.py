"""Run the portable client and Windows ownership contracts, with pinned receipts.

This uses synthetic protocol children. It does not qualify a native renderer,
Blender adapter, image quality or performance targets.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import sys
import time
import unittest


def inputs():
    root = Path(__file__).resolve().parents[2]
    files = [Path(__file__).resolve(), Path(sys.executable),
        *(root / 'test/viewport' / name for name in (
            'test_client.py', 'test_windows_process.py', 'client_fixture.py')),
        *(root / 'tools/blender/authoring/extension' / name for name in (
            'viewport_client.py', 'viewport_process.py', 'viewport_installation.py')),
        *(root / 'tools/blender/authoring/viewport' / name for name in ('protocol.py', 'broker.py')),
        root / 'tools/blender/authoring/service/luminumbra_author/file_io.py']
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Windows ownership qualification must run on Windows')
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    # A fresh output starts failed, before hashing/importing or launching tests.
    # Aborts cannot leave an earlier successful qualification at this run's path.
    with args.receipt.open('x') as stream:
        json.dump({'schema': 'luminumbra.viewport.client.windows.v1',
                   'passed': False, 'state': 'started'}, stream)
    before, started = inputs(), time.monotonic()
    suite = unittest.TestSuite()
    for name in ('test_client.py', 'test_windows_process.py'):
        suite.addTests(unittest.defaultTestLoader.discover(str(Path(__file__).parent), pattern=name))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    after = inputs()
    receipt = {'schema': 'luminumbra.viewport.client.windows.v1', 'scope': 'synthetic process/protocol contracts',
        'renderer_qualified': False, 'blender_qualified': False, 'visual_approval': False,
        'platform': platform.platform(), 'python': sys.version, 'executable': sys.executable,
        'tests_run': result.testsRun, 'failures': [str(test) for test, _ in result.failures],
        'errors': [str(test) for test, _ in result.errors],
        'skips': [{'test': str(test), 'reason': reason} for test, reason in result.skipped],
        'elapsed_seconds': time.monotonic()-started, 'inputs_before': before, 'inputs_after': after,
        'passed': result.wasSuccessful() and before == after and result.testsRun == 25
            and all(test.id().endswith('test_manifest_escape_and_linked_sdk_inputs_are_refused')
                    and reason == 'Symlink creation unavailable' for test, reason in result.skipped)}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
    return 0 if receipt['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
