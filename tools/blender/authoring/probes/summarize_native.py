"""Consolidate completed native receipts without promoting mock/runtime support."""
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess

from common import ROOT, SOURCE, file_digest, read_json


def newest(paths):
    return max(paths, key=lambda path: path.stat().st_mtime)


def main():
    native = ROOT / 'probes/native-runs'
    report = {'created': datetime.now(timezone.utc).isoformat(),
              'qualification': 'Recorded small Windows Blender/exporter and MOCK profile only',
              'production_authoring_gate_complete': False, 'engine_viewport_implemented': False,
              'exports': {}, 'historical_attempts': []}
    for suite in ('prop', 'plant', 'character'):
        receipt = newest(list(native.glob(suite + '-*/command-receipt.json')))
        data = read_json(receipt)
        exported = read_json(receipt.parent / 'outputs/export-receipt.json')
        report['exports'][suite] = {'receipt': str(receipt), 'status': data['status'],
            'exact_repeat': data['comparison']['exact_bytes_equal'],
            'canonical_repeat': data['comparison']['canonical_document_and_buffer_equal'],
            'facts': exported['facts'], 'blender_sha256': data['blender_sha256']}
    receipts = list(native.glob('extension-*/outputs/extension-receipt.json'))
    interactive = [path for path in receipts if not read_json(path)['background']]
    latest_ui = newest(interactive)
    ui = read_json(latest_ui)
    report['interactive_extension'] = {'receipt': str(latest_ui), 'passed': ui.get('passed', False),
        'checks': len(ui['checks']), 'capture': str(latest_ui.with_name('mock-panel.png')),
        'visual_review': 'REQUIRED: inspect the capture separately; automated checks do not establish rendered fidelity'}
    windows = newest(list(native.glob('service-*/service-receipt.json')))
    report['windows_service'] = {'receipt': str(windows), **read_json(windows)}
    cpu_pointer = read_json(ROOT / 'evidence/latest.json')
    cpu = read_json(cpu_pointer['report'])
    report['linux_cpu'] = {'receipt': cpu_pointer['report'], 'tests': cpu['tests'], 'package': cpu['package'],
                           'note': 'Native queued labels in this CPU-only invocation are superseded by this report'}
    compiled = newest(list((ROOT / 'evidence').glob('compiled-readback-*/report.json')))
    report['compiled_readback'] = {'receipt': str(compiled), 'probe_completed': read_json(compiled)['probe_completed'],
                                  'engine_fidelity': 'Read per-fixture findings; probe completion does not imply fidelity'}
    for receipt in sorted(native.glob('*/command-receipt.json')):
        report['historical_attempts'].append({'receipt': str(receipt), 'status': read_json(receipt)['status']})
    for name, path in [('source_checkout', SOURCE)]:
        report[name] = {'commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=path, text=True).strip(),
                        'status': subprocess.check_output(['git', 'status', '--short'], cwd=path, text=True)}
    report['remaining'] = ['Installed production SDK/service and module contracts', 'Faithful prefab/material compiler',
        'Engine viewport color/depth/picking/performance', 'Executable visual language and components',
        'General morph/retarget/IK/physical-animation systems', 'Game-owned production acceptance and v0.4 integration']
    report['completed_checks_passed'] = all(x['status'] == 'EXPORT_COMPLETED' for x in report['exports'].values()) \
        and ui.get('passed', False) and report['windows_service']['passed'] and cpu['passed']
    output = ROOT / 'evidence/native-qualification.json'
    output.write_text(json.dumps(report, indent=2))
    sources = [ROOT / 'README.md', *(ROOT / 'probes').rglob('*')]
    sources = [p for p in sources if p.is_file() and '__pycache__' not in p.parts and 'native-runs' not in p.parts]
    (ROOT / 'evidence/native-delivery-hashes.json').write_text(json.dumps({str(p.relative_to(ROOT)): file_digest(p) for p in sources}, indent=2, sort_keys=True))
    print(json.dumps({'receipt': str(output), 'completed_checks_passed': report['completed_checks_passed'],
                      'production_authoring_gate_complete': False,
                      'windows_tests': report['windows_service']['tests'],
                      'linux_tests': cpu['tests']['run'], 'interactive_checks': len(ui['checks'])}, indent=2))


if __name__ == '__main__':
    main()
