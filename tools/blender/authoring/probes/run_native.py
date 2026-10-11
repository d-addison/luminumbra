#!/usr/bin/env python3
"""Launch only owned, isolated Windows Blender qualification probes from WSL."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('suite', choices=('prop', 'plant', 'character', 'extension', 'extension_ui', 'service'))
    parser.add_argument('--python', type=Path, required=True, help='Native Windows Python executable accessible from WSL')
    parser.add_argument('--blender', help='Windows-native path to Blender 5.1.0; required except for service')
    args = parser.parse_args()
    if args.suite != 'service' and not args.blender:
        parser.error('--blender is required for this suite')
    filename = {'extension': 'queue_extension.py', 'extension_ui': 'queue_extension.py', 'service': 'native_service_tests.py'}.get(args.suite, 'queue_blender.py')
    launcher = subprocess.check_output(['wslpath', '-w', str(ROOT / 'probes' / filename)], text=True).strip()
    command = [str(args.python), launcher]
    if args.suite != 'service':
        command.extend(['--blender', args.blender, '--execute-coordinated'])
    if args.suite == 'extension_ui':
        command.append('--interactive')
    if args.suite not in ('extension', 'extension_ui', 'service'):
        command.extend(['--suite', args.suite])
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    evidence = ROOT / 'evidence' / ('native-' + args.suite + '-' + stamp)
    evidence.mkdir(parents=True)
    (evidence / 'launch.json').write_text(json.dumps({'command': command, 'cwd': str(ROOT),
        'scope': 'owned Windows Blender qualification process; no release source/build modifications'}, indent=2))
    # Windows launcher owns and bounds Blender lifetime. Outer timeout is longer
    # so ordinary failure/timeout receipts can be flushed through native Python.
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=240)
        (evidence / 'stdout.log').write_text(result.stdout)
        (evidence / 'stderr.log').write_text(result.stderr)
        print(result.stdout)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        print('Native launch evidence:', evidence)
        return result.returncode
    except subprocess.TimeoutExpired as error:
        (evidence / 'timeout.json').write_text(json.dumps({'status': 'TIMEOUT', 'seconds': 240}))
        print(str(error), file=sys.stderr)
        return 124


if __name__ == '__main__':
    raise SystemExit(main())
