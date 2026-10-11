#!/usr/bin/env python3
"""Tests for the process receipt summary. Synthetic receipts and logs in a temp dir."""
import contextlib
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_runner_process as runner  # noqa: E402
import summarize_process_receipts as summary  # noqa: E402

RECEIPT = dict(schema=runner.RECEIPT_SCHEMA, command=['luminumbra_client_app.exe'],
               cwd='/work', binary_path='/bin/luminumbra_client_app.exe',
               binary_sha256='0' * 64, timeout_seconds=900, elapsed_seconds=1.5,
               outcome='returned', return_code=0, capture_status='retained',
               capture_path=None, capture_sha256=None, evidence_errors=[])


class ProcessReceiptSummaryTest(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)

    def tearDown(self):
        self._temp.cleanup()

    def write_receipt(self, name, overrides=None, directory=None, **fields):
        data = copy.deepcopy(RECEIPT)
        data.update(fields)
        if overrides:
            data.update(overrides)
        folder = directory or self.root
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / name
        path.write_text(json.dumps(data), encoding='utf-8')
        return path

    def write_timeout(self, stem, log_text=None):
        receipt = self.write_receipt(stem + '.process.json', outcome='timed_out',
                                     return_code=None, capture_status='missing',
                                     exception_type='TimeoutExpired')
        if log_text is not None:
            receipt.with_name(stem + '.log').write_text(log_text, encoding='utf-8')
        return receipt

    def run_cli(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = summary.main(argv)
        return code, out.getvalue(), err.getvalue()

    def document(self, argv):
        code, out, err = self.run_cli(argv)
        self.assertEqual(code, 0, err)
        return json.loads(out)

    def test_returned_and_spawn_error_are_classified(self):
        self.write_receipt('a.process.json')
        self.write_receipt('b.process.json', outcome='spawn_error', return_code=None,
                           exception_type='FileNotFoundError')
        document = self.document([str(self.root)])
        self.assertEqual(document['schema'], 'luminumbra.process_receipt_summary.v1')
        self.assertEqual(document['counts'], {'returned': 1, 'timed_out': 0,
                                              'spawn_error': 1})
        by_name = {Path(entry['path']).name: entry for entry in document['receipts']}
        returned = by_name['a.process.json']
        self.assertEqual(returned['classification'], 'returned')
        self.assertEqual(returned['return_code'], 0)
        self.assertEqual(returned['capture_status'], 'retained')
        self.assertEqual(by_name['b.process.json']['classification'], 'spawn_error')
        self.assertEqual(by_name['b.process.json']['exception_type'], 'FileNotFoundError')

    def test_timed_out_reports_last_shutdown_stage(self):
        log = ('Shutdown stage: query_cleanup_started\n'
               'Shutdown stage: world_readers_drained\n'
               'noise line\n'
               'Shutdown stage: world_save_started\n'
               'Shutdown stage: world_save_finished_extra\n')
        self.write_timeout('run1', log)
        document = self.document([str(self.root)])
        entry = document['receipts'][0]
        self.assertEqual(entry['classification'], 'timed_out')
        self.assertEqual(entry['last_shutdown_stage'], 'world_save_started')
        self.assertEqual(entry['placement'], 'in_world_save_started')
        self.assertFalse(entry['log_missing'])
        self.assertEqual(document['counts']['timed_out'], 1)

    def test_timed_out_without_breadcrumbs(self):
        self.write_timeout('run2', 'client started\nno stage reached\n')
        entry = self.document([str(self.root)])['receipts'][0]
        self.assertIsNone(entry['last_shutdown_stage'])
        self.assertEqual(entry['placement'], 'before_shutdown_or_no_breadcrumbs')

    def test_unreadable_log_exits_2_naming_the_log(self):
        self.write_timeout('run4', 'Shutdown stage: query_cleanup_started\n')
        original = Path.read_bytes

        def read_bytes(path):
            if path.suffix == '.log':
                raise PermissionError('denied')
            return original(path)

        with mock.patch.object(Path, 'read_bytes', read_bytes):
            code, _out, err = self.run_cli([str(self.root)])
        self.assertEqual(code, 2)
        self.assertIn('run4.log', err)

    def test_timed_out_with_missing_log(self):
        self.write_timeout('run3')
        entry = self.document([str(self.root)])['receipts'][0]
        self.assertTrue(entry['log_missing'])
        self.assertIsNone(entry['last_shutdown_stage'])
        self.assertIsNone(entry['placement'])

    def test_custom_log_suffix(self):
        receipt = self.write_timeout('run4')
        receipt.with_name('run4.txt').write_text('Shutdown stage: far_lod_drained\n',
                                                 encoding='utf-8')
        entry = self.document([str(receipt), '--log-suffix', '.txt'])['receipts'][0]
        self.assertEqual(entry['placement'], 'in_far_lod_drained')

    def test_utf16_log_is_decoded(self):
        receipt = self.write_timeout('run5')
        text = 'Shutdown stage: world_readers_drain_started\n'
        receipt.with_name('run5.log').write_bytes(
            b'\xff\xfe' + text.encode('utf-16-le'))
        entry = self.document([str(receipt)])['receipts'][0]
        self.assertEqual(entry['placement'], 'in_world_readers_drain_started')

    def test_directory_is_searched_recursively_and_sorted(self):
        self.write_receipt('z.process.json')
        self.write_receipt('m.process.json', directory=self.root / 'nested' / 'deep')
        self.write_receipt('not-a-receipt.json')
        document = self.document([str(self.root)])
        paths = [entry['path'] for entry in document['receipts']]
        self.assertEqual(len(paths), 2)
        self.assertEqual(paths, sorted(paths))
        self.assertTrue(paths[0].endswith('m.process.json'))

    def test_explicit_receipt_and_directory_are_deduplicated(self):
        receipt = self.write_receipt('one.process.json')
        document = self.document([str(receipt), str(self.root)])
        self.assertEqual(len(document['receipts']), 1)
        self.assertEqual(document['counts']['returned'], 1)

    def test_wrong_schema_exits_2_naming_file(self):
        bad = self.root / 'bad.process.json'
        bad.write_text(json.dumps({'schema': 'luminumbra.other.v1'}), encoding='utf-8')
        code, out, err = self.run_cli([str(self.root)])
        self.assertEqual(code, 2)
        self.assertEqual(out, '')
        self.assertIn('bad.process.json', err)

    def test_invalid_json_exits_2(self):
        (self.root / 'broken.process.json').write_text('{oops', encoding='utf-8')
        code, _, err = self.run_cli([str(self.root)])
        self.assertEqual(code, 2)
        self.assertIn('broken.process.json', err)

    def test_unknown_outcome_exits_2(self):
        self.write_receipt('odd.process.json', outcome='exploded')
        code, _, err = self.run_cli([str(self.root)])
        self.assertEqual(code, 2)
        self.assertIn('odd.process.json', err)

    def test_missing_input_exits_2(self):
        code, _, _ = self.run_cli([str(self.root / 'absent')])
        self.assertEqual(code, 2)

    def test_explicit_file_without_receipt_suffix_exits_2(self):
        other = self.root / 'plain.json'
        other.write_text(json.dumps(RECEIPT), encoding='utf-8')
        code, _, _ = self.run_cli([str(other)])
        self.assertEqual(code, 2)

    def test_empty_directory_gives_zero_counts(self):
        document = self.document([str(self.root)])
        self.assertEqual(document['receipts'], [])
        self.assertEqual(document['counts'], {'returned': 0, 'timed_out': 0,
                                              'spawn_error': 0})


if __name__ == '__main__':
    unittest.main()
