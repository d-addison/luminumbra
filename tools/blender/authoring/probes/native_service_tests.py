"""Run the same lifecycle contracts with native Windows Python and isolated temp."""
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import uuid

from common import file_digest
import test_probes


def main():
    if os.name != 'nt':
        raise RuntimeError('Native Windows qualification requires native Python')
    here = Path(__file__).resolve().parent
    run = here / 'native-runs' / ('service-' + uuid.uuid4().hex)
    temp = run / 'temp'
    temp.mkdir(parents=True)
    os.environ['TEMP'] = os.environ['TMP'] = tempfile.tempdir = str(temp)
    log = io.StringIO()
    result = unittest.TextTestRunner(stream=log, verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromModule(test_probes))
    (run / 'tests.log').write_text(log.getvalue(), encoding='utf-8')
    receipt = {'mode': 'MOCK_WINDOWS_SERVICE_TESTS', 'python': sys.version,
               'timestamp': datetime.now(timezone.utc).isoformat(), 'tests': result.testsRun,
               'failures': len(result.failures), 'errors': len(result.errors), 'skipped': len(result.skipped),
               'passed': result.wasSuccessful(), 'blender_executed': False,
               'hashes': {name: file_digest(here / name) for name in (
                   'mock_service.py', 'project_lock.py', 'contracts.py', 'common.py', 'test_probes.py')}}
    (run / 'service-receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(receipt, indent=2))
    print(log.getvalue())
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
