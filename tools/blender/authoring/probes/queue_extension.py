"""Native Windows launcher for bounded isolated extension qualification."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

from common import file_digest
from package_mock import build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blender', type=Path, required=True)
    parser.add_argument('--execute-coordinated', action='store_true')
    parser.add_argument('--interactive', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    run = here / 'native-runs' / ('extension-' + uuid.uuid4().hex)
    env = {name: str(run / 'user' / name.lower()) for name in (
        'BLENDER_USER_RESOURCES', 'BLENDER_USER_CONFIG', 'BLENDER_USER_SCRIPTS',
        'BLENDER_USER_DATAFILES', 'BLENDER_USER_EXTENSIONS')}
    env.update(OMP_NUM_THREADS='2', TEMP=str(run / 'temp'), TMP=str(run / 'temp'))
    record = {'status': 'QUEUED', 'environment_overrides': env, 'engine_rendered': False}
    if args.execute_coordinated:
        if os.name != 'nt':
            parser.error('Run using native Windows Python.')
        for name, path in env.items():
            if name != 'OMP_NUM_THREADS':
                Path(path).mkdir(parents=True, exist_ok=True)
        archive = build(run)
        script = here / 'blender_extension_probe.py'
        command = [str(args.blender), *([] if args.interactive else ['--background']), '--factory-startup', '--disable-autoexec',
                   '--threads', '2', '--python-exit-code', '23', '--python', str(script), '--',
                   '--archive', str(archive), '--output', str(run / 'outputs'), '--python', sys.executable]
        record.update(command=command, archive_sha256=file_digest(archive),
                      script_sha256=file_digest(script), blender_sha256=file_digest(args.blender))
        with (run / 'blender.log').open('wb') as log:
            try:
                result = subprocess.run(command, cwd=run, env={**os.environ, **env},
                                        stdout=log, stderr=subprocess.STDOUT, timeout=180)
                receipt = run / 'outputs' / 'extension-receipt.json'
                passed = result.returncode == 0 and receipt.exists() and json.loads(receipt.read_text()).get('passed', False)
                record.update(status='PASSED' if passed else 'FAILED', returncode=0 if passed else (result.returncode or 23))
            except subprocess.TimeoutExpired:
                record.update(status='TIMEOUT', returncode=124)
        (run / 'command-receipt.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps(record, indent=2))
    return record.get('returncode', 0)


if __name__ == '__main__':
    raise SystemExit(main())
