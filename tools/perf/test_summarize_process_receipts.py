#!/usr/bin/env python3
"""Tests for the process receipt summary. Synthetic receipts and logs in a temp dir."""
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_gl_debug_log as checker
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
        if data['outcome'] in ('timed_out', 'spawn_error'):
            data.setdefault('exception_message', 'process did not return')
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

    def test_changed_executable_keeps_original_process_classification(self):
        for outcome in summary.OUTCOMES:
            with self.subTest(outcome=outcome):
                path = self.write_receipt(
                    outcome + '.process.json', outcome=outcome,
                    return_code=0 if outcome == 'returned' else None,
                    exception_type='TimeoutExpired' if outcome == 'timed_out' else 'OSError',
                    binary_sha256=None, binary_identity_status='changed',
                    evidence_errors=['executable verification: executable changed during execution'])
                entry = self.document([str(path)])['receipts'][0]
                self.assertEqual(entry['classification'], outcome)
                if outcome == 'returned':
                    self.assertEqual(entry['return_code'], 0)

    def test_null_executable_hash_requires_changed_identity_and_verification_error(self):
        for outcome in ('returned', 'timed_out'):
            for status, errors in ((None, []), ('verified', ['executable verification: changed']),
                                   ('pending', ['executable verification: changed']),
                                   ('changed', []), ('changed', ['capture read: denied'])):
                with self.subTest(outcome=outcome, status=status, errors=errors):
                    path = self.write_receipt(
                        'invalid.process.json', outcome=outcome,
                        return_code=0 if outcome == 'returned' else None,
                        exception_type='TimeoutExpired', binary_sha256=None,
                        binary_identity_status=status, evidence_errors=errors)
                    self.assert_refused([str(path)], 'binary_sha256')

    def test_timed_out_without_breadcrumbs(self):
        self.write_timeout('run2', 'client started\nno stage reached\n')
        entry = self.document([str(self.root)])['receipts'][0]
        self.assertIsNone(entry['last_shutdown_stage'])
        self.assertEqual(entry['placement'], 'before_shutdown_or_no_breadcrumbs')

    def test_unreadable_log_exits_2_naming_the_log(self):
        self.write_timeout('run4', 'Shutdown stage: query_cleanup_started\n')
        original = summary._open_descriptor

        def open_path(path, root):
            if path.suffix == '.log':
                raise PermissionError('denied')
            return original(path, root)

        with mock.patch.object(summary, '_open_descriptor', open_path):
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

    def assert_refused(self, args, diagnostic=None):
        code, out, err = self.run_cli(args)
        self.assertEqual(code, 2)
        self.assertEqual(out, '')
        self.assertNotIn('Traceback', err)
        self.assertLessEqual(len(err), len('summarize_process_receipts: ') + 301)
        if diagnostic:
            self.assertIn(diagnostic, err)

    def test_incomplete_receipt_refuses(self):
        path = self.root / 'incomplete.process.json'
        path.write_text(json.dumps(dict(schema=runner.RECEIPT_SCHEMA, outcome='returned')),
                        encoding='utf-8')
        self.assert_refused([str(path)], 'command')

    def test_each_required_field_is_validated(self):
        path = self.root / 'invalid.process.json'
        for field in RECEIPT:
            with self.subTest(missing=field):
                data = copy.deepcopy(RECEIPT)
                del data[field]
                path.write_text(json.dumps(data), encoding='utf-8')
                self.assert_refused([str(path)])
        invalid = dict(command='client', cwd=None, binary_path=42, binary_sha256='bad',
                       timeout_seconds=True, elapsed_seconds=float('nan'), return_code=False,
                       capture_status=[], capture_path=42, capture_sha256='bad',
                       evidence_errors=[42])
        for field, value in invalid.items():
            with self.subTest(wrong_type=field):
                self.write_receipt(path.name, **{field: value})
                self.assert_refused([str(path)], field)

    def test_exception_fields_are_required_and_typed(self):
        path = self.write_timeout('exception')
        original = json.loads(path.read_text(encoding='utf-8'))
        for field in ('exception_type', 'exception_message'):
            for missing in (True, False):
                with self.subTest(field=field, missing=missing):
                    data = copy.deepcopy(original)
                    if missing:
                        del data[field]
                    else:
                        data[field] = 42
                    path.write_text(json.dumps(data), encoding='utf-8')
                    self.assert_refused([str(path)], field)

    def test_oversize_receipt_and_log_refuse(self):
        path = self.write_timeout('oversize')
        log = path.with_name('oversize.log')
        for target in (log, path):
            with self.subTest(target=target.name):
                with target.open('wb') as output:
                    output.truncate(checker.MAX_FILE_BYTES + 1)
                self.assert_refused([str(path)], 'byte cap')

    def test_receipt_and_discovery_counts_are_capped(self):
        for number in range(3):
            self.write_receipt(f'run{number}.process.json')
        with mock.patch.object(summary, 'MAX_RECEIPTS', 2):
            self.assert_refused([str(self.root)], 'receipt cap')
        with mock.patch.object(summary, 'MAX_DISCOVERY_ENTRIES', 2):
            self.assert_refused([str(self.root)], 'discovery entry cap')

    def test_log_line_count_and_length_are_capped(self):
        path = self.write_timeout('many', 'noise\n' * 3)
        with mock.patch.object(summary, 'MAX_RECORDS', 2):
            self.assert_refused([str(path)], 'line cap')
        path.with_name('many.log').write_text('x' * (checker.MAX_LINE_CHARS + 1),
                                            encoding='utf-8')
        self.assert_refused([str(path)], 'character cap')

    def test_invalid_utf8_and_utf16_log_refuse(self):
        path = self.write_timeout('corrupt')
        for payload in (b'Shutdown stage: world_save_started\n\xff', b'\xff\xfeA'):
            with self.subTest(payload=payload):
                path.with_name('corrupt.log').write_bytes(payload)
                self.assert_refused([str(path)], 'text')

    def test_bad_log_suffix_refuses(self):
        path = self.write_timeout('suffix')
        for suffix in ('/bad', 'nested/bad', 'nested\\bad', 'C:\\bad', 'bad\x00', ''):
            with self.subTest(suffix=suffix):
                self.assert_refused([str(path), '--log-suffix', suffix], '--log-suffix')

    def test_nul_path_refuses(self):
        self.assert_refused(['bad\x00.process.json'], 'NUL')

    def test_symlinked_receipt_refuses_explicit_and_directory_input(self):
        outside = self.root / 'outside'
        receipt = self.write_receipt('target.process.json', directory=outside)
        evidence = (self.root / 'evidence').resolve()
        evidence.mkdir()
        link = evidence / 'link.process.json'
        link.write_bytes(receipt.read_bytes())  # Discoverable without symlink privileges.
        original_lstat = Path.lstat

        def lstat(path, *args, **kwargs):
            if Path(path) == link:
                return mock.Mock(st_mode=stat.S_IFLNK | 0o777)
            return original_lstat(path, *args, **kwargs)

        with mock.patch.object(Path, 'lstat', lstat):
            for item in (link, evidence):
                with self.subTest(item=item):
                    self.assert_refused([str(item)], 'symlink')

    def test_symlinked_log_and_dangling_log_refuse(self):
        path = self.write_timeout('linked')
        target = self.root / 'outside.log'
        target.write_text('Shutdown stage: world_save_started\n', encoding='utf-8')
        log = path.with_name('linked.log').resolve()
        original_lstat = Path.lstat

        def lstat(candidate, *args, **kwargs):
            if Path(candidate) == log:
                return mock.Mock(st_mode=stat.S_IFLNK | 0o777)
            return original_lstat(candidate, *args, **kwargs)

        with mock.patch.object(Path, 'lstat', lstat):
            self.assert_refused([str(path)], 'symlink')
            target.unlink()
            self.assert_refused([str(path)], 'symlink')

    def test_diagnostic_excerpts_are_bounded(self):
        path = self.write_receipt('long.process.json', outcome='spawn_error', return_code=None,
                                  exception_type='E' * 1000, binary_path='x' * 1000)
        entry = self.document([str(path)])['receipts'][0]
        self.assertEqual(len(entry['exception_type']), summary.MAX_EXCERPT_CHARS)
        self.assertEqual(len(entry['binary_path']), summary.MAX_EXCERPT_CHARS)

    def test_receipt_and_log_replacement_before_open_refuse_without_reading(self):
        evidence = self.root / 'evidence'
        evidence.mkdir()
        receipt = self.write_receipt('race.process.json', directory=evidence,
                                     outcome='timed_out', return_code=None,
                                     capture_status='missing', exception_type='TimeoutExpired')
        log = evidence / 'race.log'
        log.write_text('Shutdown stage: world_save_started\n', encoding='utf-8')
        outside = self.write_receipt('outside.process.json')
        original = summary._open_descriptor
        descriptors = []
        for target in (receipt, log):
            with self.subTest(target=target.name):
                def replace_at_open(path, root):
                    if path == target:
                        # Model a symlink/ancestor swap returning an outside handle;
                        # no symlink privileges or renaming open Windows files needed.
                        descriptor = os.open(outside, os.O_RDONLY | getattr(os, 'O_BINARY', 0))
                        descriptors.append(descriptor)
                        return descriptor
                    return original(path, root)

                with mock.patch.object(summary, '_open_descriptor', replace_at_open), \
                        mock.patch.object(summary, 'bounded_bytes', wraps=summary.bounded_bytes) as read_receipt, \
                        mock.patch.object(summary, 'read_log', wraps=summary.read_log) as read_log:
                    self.assert_refused([str(receipt)], 'changed during open')
                self.assertEqual(read_receipt.call_count, int(target == log))
                read_log.assert_not_called()
                with self.assertRaises(OSError):
                    os.fstat(descriptors[-1])

    def test_receipt_and_log_are_read_through_the_verified_handle_only(self):
        receipt = self.write_timeout('handle', 'Shutdown stage: world_save_started\r\n')
        descriptors = []
        original = summary._open_descriptor

        def record_open(path, root):
            descriptor = original(path, root)
            descriptors.append((path, descriptor))
            return descriptor

        # A path reopen would fail even if validation used the correct handle.
        with mock.patch.object(summary, '_open_descriptor', record_open), \
                mock.patch.object(Path, 'open', side_effect=AssertionError('path reopened')):
            entry = self.document([str(receipt)])['receipts'][0]
        self.assertEqual(entry['placement'], 'in_world_save_started')
        self.assertEqual([path for path, _descriptor in descriptors],
                         [receipt, receipt.with_name('handle.log')])
        for _path, descriptor in descriptors:
            with self.assertRaises(OSError):
                os.fstat(descriptor)

    def test_no_follow_flags_cover_ancestor_and_leaf_opens(self):
        if os.name == 'nt':
            # Native reparse-point checks are exercised by every Windows read.
            receipt = self.write_receipt('native.process.json')
            self.document([str(receipt)])
            return
        receipt = self.write_receipt('nofollow.process.json')
        original = os.open
        flags_seen = []

        def record_open(path, flags, *args, **kwargs):
            flags_seen.append(flags)
            return original(path, flags, *args, **kwargs)

        with mock.patch.object(summary.os, 'open', record_open):
            self.document([str(receipt)])
        self.assertGreater(len(flags_seen), 1)
        for flags in flags_seen:
            self.assertEqual(flags & os.O_NOFOLLOW, os.O_NOFOLLOW)

    def test_windows_reparse_and_outside_handles_refuse_before_reading(self):
        import ctypes
        from ctypes import wintypes

        path = self.write_receipt('windows.process.json')
        outside = self.root.parent / 'outside.process.json'
        for reparse in (True, False):
            with self.subTest(reparse=reparse):
                kernel = mock.Mock()
                kernel.CreateFileW.return_value = 123
                crt = types.SimpleNamespace(open_osfhandle=mock.Mock())

                def attributes(_handle, _kind, buffer, _size):
                    flags = ctypes.cast(buffer, ctypes.POINTER(wintypes.DWORD))
                    flags[0] = stat.FILE_ATTRIBUTE_REPARSE_POINT if reparse else 0
                    return 1

                def final_path(_handle, buffer, _size, _flags):
                    if buffer is not None:
                        buffer.value = str(outside)
                    return len(str(outside)) + int(buffer is None)

                kernel.GetFileInformationByHandleEx.side_effect = attributes
                kernel.GetFinalPathNameByHandleW.side_effect = final_path
                with mock.patch.object(ctypes, 'WinDLL', return_value=kernel, create=True), \
                        mock.patch.dict(sys.modules, {'msvcrt': crt}):
                    with self.assertRaisesRegex(checker.InputError,
                                                'reparse point' if reparse else 'escapes'):
                        summary._open_windows_descriptor(path, self.root)
                self.assertTrue(kernel.CreateFileW.call_args.args[5] & 0x00200000)
                kernel.CloseHandle.assert_called_once_with(123)
                crt.open_osfhandle.assert_not_called()

    def test_platform_no_follow_refuses_replacement_at_the_actual_open(self):
        if os.name == 'nt':
            # Exercise Windows refusal without requiring symlink privileges.
            self.test_windows_reparse_and_outside_handles_refuse_before_reading()
            return
        outside = self.write_receipt('outside.process.json')
        evidence = self.root / 'evidence'
        evidence.mkdir()
        receipt = self.write_receipt('swapped.process.json', directory=evidence)
        original = os.open

        def replace_at_open(path, flags, *args, **kwargs):
            if path == receipt.name:
                receipt.unlink()
                receipt.symlink_to(outside)
            return original(path, flags, *args, **kwargs)

        with mock.patch.object(summary.os, 'open', replace_at_open), \
                mock.patch.object(summary, 'bounded_bytes') as read_receipt:
            self.assert_refused([str(receipt)])
        read_receipt.assert_not_called()



if __name__ == '__main__':
    unittest.main()
