"""Qualify packaged service/viewport publication with actual Windows handles.

No renderer, Blender, visual or performance approval is implied by this probe.
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
    authoring = root / 'tools/blender/authoring'
    files = {Path(__file__).resolve(), Path(sys.executable),
             root / 'test/viewport/test_shared_file_io.py', authoring / 'package_extension.py'}
    for directory in ('service', 'extension', 'viewport'):
        files.update((authoring / directory).rglob('*.py'))
    files.add(authoring / 'extension/blender_manifest.toml')
    files.add(authoring / 'extension_tests/test_viewport_generation.py')
    files.add(root / 'LICENSE')
    files.update((root / 'test/viewport').glob('*.py'))
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(files)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipt', required=True, type=Path)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Windows sharing qualification must run on Windows')
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    with args.receipt.open('x') as stream:
        json.dump({'schema': 'luminumbra.authoring.file_io.windows.v1',
                   'passed': False, 'state': 'started'}, stream)
    before, started = inputs(), time.monotonic()
    suite = unittest.defaultTestLoader.discover(str(Path(__file__).parent), pattern='test_shared_file_io.py')
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    after = inputs()
    allowed = {
        'test_fifo_reader_is_rejected_without_blocking': 'POSIX FIFO control',
        'test_linked_reader_and_publication_are_rejected':
            'Windows symlink creation requires developer mode or privilege'}
    report = {'schema': 'luminumbra.authoring.file_io.windows.v1',
        'scope': 'packaged service and extension held-reader publication contracts',
        'renderer_qualified': False, 'blender_qualified': False, 'visual_approval': False,
        'tests_run': result.testsRun, 'failures': [str(test) for test, _ in result.failures],
        'errors': [str(test) for test, _ in result.errors],
        'skips': [{'test': str(test), 'reason': reason} for test, reason in result.skipped],
        'inputs_before': before, 'inputs_after': after,
        'python': sys.version, 'platform': platform.platform(), 'executable': sys.executable,
        'elapsed_seconds': time.monotonic() - started,
        'passed': result.wasSuccessful() and result.testsRun == 12 and before == after and
                  all(allowed.get(test.id().rsplit('.', 1)[1]) == reason for test, reason in result.skipped)}
    args.receipt.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
