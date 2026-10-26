#!/usr/bin/env python3
"""Tests for the GL KHR_debug log verdict. Synthetic logs in a temporary directory."""
import codecs
import contextlib
import io
import json
import os
import stat
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_gl_debug_log as checker  # noqa: E402

MARKER = 'GL KHR_debug output installed (synchronous); routing driver messages\n'
HEADER = 'Engine start\n'
INFO = '[GL][API][Performance][MEDIUM] id=131 buffer copy is slow\n'
NOTE = '[GL][API][Other][NOTIFICATION] id=131185 buffer usage hint\n'
BAD_TYPE = '[GL][API][Error][?] id=1280 invalid enum value\n'
BAD_SEVERITY = '[GL][Shader][Performance][HIGH] id=7 shader spills registers\n'
UNSET = 'LUMIN_GL_DEBUG set but build lacks GL 4.3 KHR_debug headers\n'
RENDER_BENCHMARK_V3 = 'luminumbra.render_benchmark.v3'


class GlDebugLogTest(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)

    def tearDown(self):
        self._temp.cleanup()

    def write_log(self, text, name='client.log', encoding='utf-8'):
        path = self.root / name
        path.write_bytes(text.encode(encoding))
        return path

    def run_cli(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = checker.main(argv)
        return code, out.getvalue(), err.getvalue()

    def verdict(self, text, extra=()):
        log = self.write_log(text)
        code, out, _ = self.run_cli(['--log', str(log), '--format', 'json', *extra])
        return code, json.loads(out) if out else None

    def test_pass_with_warnings_and_notifications_only(self):
        code, document = self.verdict(HEADER + MARKER + INFO + NOTE)
        self.assertEqual(code, 0)
        self.assertEqual(document['schema'], 'luminumbra.gl_debug_verdict.v1')
        self.assertEqual(document['verdict'], 'pass')
        self.assertTrue(document['installed'])
        self.assertEqual(document['error_count'], 0)
        self.assertEqual(document['warning_count'], 1)
        self.assertEqual(document['notification_count'], 1)
        self.assertEqual(document['errors'], [])

    def test_verified_sources_are_never_reopened_or_closed_by_readers(self):
        path = self.root / 'handle-only'
        for reader, payload, expected in (
                (checker.bounded_bytes, b'{"value":1}', b'{"value":1}'),
                (lambda path, **kwargs: list(checker.read_log(path, **kwargs)),
                 b'first\r\nsecond\n', ['first', 'second'])):
            with self.subTest(reader=reader):
                source = io.BytesIO(payload)
                with mock.patch.object(checker, 'open_input',
                                       side_effect=AssertionError('source reopened')):
                    self.assertEqual(reader(path, source=source), expected)
                self.assertFalse(source.closed)

    def test_replaced_input_refuses_for_both_log_and_artifact_readers(self):
        original_stat = Path.lstat
        for reader in (checker.bounded_bytes, lambda path: list(checker.read_log(path))):
            with self.subTest(reader=reader):
                path = self.write_log(MARKER)
                retired = self.root / 'retired.log'
                retired.unlink(missing_ok=True)
                observed = []

                def replace(candidate, *args, **kwargs):
                    info = original_stat(candidate, *args, **kwargs)
                    if candidate == path and not observed:
                        observed.append(candidate)
                        path.replace(retired)
                        path.write_bytes(b'replacement bytes')
                    return info

                with mock.patch.object(Path, 'lstat', replace):
                    with self.assertRaises(checker.InputError):
                        reader(path)
                self.assertEqual(observed, [path])
                self.assertEqual(path.read_bytes(), b'replacement bytes')

    def test_input_open_cannot_block_on_a_replacement_fifo(self):
        path = self.write_log(MARKER)
        original_open = os.open
        flags_seen = []

        def opened(candidate, flags, *args, **kwargs):
            flags_seen.append(flags)
            return original_open(candidate, flags, *args, **kwargs)

        with mock.patch.object(checker.os, 'open', opened):
            self.assertEqual(checker.bounded_bytes(path), MARKER.encode())
        self.assertEqual(len(flags_seen), 1)
        for name in ('O_NONBLOCK', 'O_NOFOLLOW', 'O_BINARY'):
            flag = getattr(os, name, 0)
            self.assertEqual(flags_seen[0] & flag, flag)

    def test_invalid_opened_metadata_refuses_and_closes_descriptor(self):
        path = self.write_log(MARKER)
        original_fstat = os.fstat
        for changes in ({'st_mode': stat.S_IFIFO}, {'st_ino': 0},
                        {'st_size': checker.MAX_FILE_BYTES + 1}):
            with self.subTest(changes=changes):
                descriptors = []

                def metadata(descriptor):
                    descriptors.append(descriptor)
                    info = original_fstat(descriptor)
                    return mock.Mock(**{name: changes.get(name, getattr(info, name))
                                        for name in ('st_mode', 'st_ino', 'st_dev', 'st_size')})

                with mock.patch.object(checker.os, 'fstat', metadata):
                    with self.assertRaises(checker.InputError):
                        checker.bounded_bytes(path)
                self.assertEqual(len(descriptors), 1)
                with self.assertRaises(OSError):
                    original_fstat(descriptors[0])

    def test_failed_stream_creation_closes_descriptor(self):
        path = self.write_log(MARKER)
        original_open = os.open
        descriptors = []

        def opened(*args, **kwargs):
            descriptor = original_open(*args, **kwargs)
            descriptors.append(descriptor)
            return descriptor

        with mock.patch.object(checker.os, 'open', opened), mock.patch.object(
                checker.os, 'fdopen', side_effect=OSError('unreadable stream')):
            with self.assertRaises(checker.InputError):
                checker.bounded_bytes(path)
        self.assertEqual(len(descriptors), 1)
        with self.assertRaises(OSError):
            os.fstat(descriptors[0])

    def test_link_metadata_refuses_before_open_without_symlink_privilege(self):
        path = self.write_log(MARKER)
        for mode, attributes in ((stat.S_IFLNK, 0),
                                 (stat.S_IFREG, stat.FILE_ATTRIBUTE_REPARSE_POINT)):
            with self.subTest(mode=mode, attributes=attributes):
                info = mock.Mock(st_mode=mode, st_file_attributes=attributes)
                with mock.patch.object(Path, 'lstat', return_value=info), mock.patch.object(
                        checker.os, 'open') as opened:
                    with self.assertRaises(checker.InputError):
                        checker.bounded_bytes(path)
                    opened.assert_not_called()

    def test_unknown_original_identity_refuses_and_stat_failure_closes_stream(self):
        path = self.write_log(MARKER)
        info = path.lstat()
        unknown = mock.Mock(st_mode=info.st_mode, st_file_attributes=0,
                            st_ino=0, st_dev=info.st_dev)
        with mock.patch.object(Path, 'lstat', return_value=unknown):
            with self.assertRaises(checker.InputError):
                checker.bounded_bytes(path)
        original_fstat = os.fstat
        descriptors = []

        def unreadable(descriptor):
            descriptors.append(descriptor)
            raise OSError('metadata unavailable')

        with mock.patch.object(checker.os, 'fstat', unreadable):
            with self.assertRaises(checker.InputError):
                checker.bounded_bytes(path)
        self.assertEqual(len(descriptors), 1)
        with self.assertRaises(OSError):
            original_fstat(descriptors[0])

    def test_fail_by_error_type(self):
        code, document = self.verdict(MARKER + INFO + BAD_TYPE)
        self.assertEqual(code, 1)
        self.assertEqual(document['verdict'], 'fail')
        self.assertEqual(document['error_count'], 1)
        self.assertEqual(document['errors'], [BAD_TYPE.rstrip('\n')])

    def test_fail_by_high_severity(self):
        code, document = self.verdict(MARKER + BAD_SEVERITY)
        self.assertEqual(code, 1)
        self.assertEqual(document['verdict'], 'fail')
        self.assertEqual(document['errors'], [BAD_SEVERITY.rstrip('\n')])

    def test_unevaluated_without_install_marker(self):
        code, document = self.verdict(HEADER + INFO + NOTE)
        self.assertEqual(code, 3)
        self.assertEqual(document['verdict'], 'unevaluated')
        self.assertFalse(document['installed'])

    def test_unevaluated_when_debug_was_not_installed(self):
        code, document = self.verdict(MARKER + UNSET)
        self.assertEqual(code, 3)
        self.assertEqual(document['verdict'], 'unevaluated')
        self.assertFalse(document['installed'])

    def test_utf16_log_with_bom_is_decoded(self):
        text = MARKER + INFO + BAD_TYPE
        path = self.root / 'utf16.log'
        path.write_bytes(codecs.BOM_UTF16_LE + text.encode('utf-16-le'))
        code, out, _ = self.run_cli(['--log', str(path)])
        self.assertEqual(code, 1)
        document = json.loads(out)
        self.assertEqual(document['error_count'], 1)
        self.assertEqual(document['warning_count'], 1)

    def test_utf16_big_endian_pass(self):
        path = self.root / 'utf16be.log'
        path.write_bytes(codecs.BOM_UTF16_BE + (MARKER + INFO).encode('utf-16-be'))
        code, out, _ = self.run_cli(['--log', str(path)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)['verdict'], 'pass')

    def test_unreadable_path_exits_2(self):
        code, out, err = self.run_cli(['--log', str(self.root / 'missing.log')])
        self.assertEqual(code, 2)
        self.assertEqual(out, '')
        self.assertIn('unreadable', err)

    def test_directory_as_log_exits_2(self):
        code, _, _ = self.run_cli(['--log', str(self.root)])
        self.assertEqual(code, 2)

    def test_bad_artifact_not_json_exits_2(self):
        log = self.write_log(MARKER)
        artifact = self.root / 'capture.json'
        artifact.write_text('{not json', encoding='utf-8')
        code, out, _ = self.run_cli(['--log', str(log), '--artifact', str(artifact)])
        self.assertEqual(code, 2)
        self.assertEqual(out, '')

    def test_bad_artifact_wrong_schema_exits_2(self):
        log = self.write_log(MARKER)
        artifact = self.root / 'capture.json'
        artifact.write_text(json.dumps({'schema': 'luminumbra.other.v1'}), encoding='utf-8')
        code, _, _ = self.run_cli(['--log', str(log), '--artifact', str(artifact)])
        self.assertEqual(code, 2)

    def test_artifact_that_is_not_an_object_exits_2(self):
        log = self.write_log(MARKER)
        artifact = self.root / 'capture.json'
        artifact.write_text('[1, 2]', encoding='utf-8')
        code, _, _ = self.run_cli(['--log', str(log), '--artifact', str(artifact)])
        self.assertEqual(code, 2)

    def test_valid_artifact_does_not_change_verdict(self):
        log = self.write_log(MARKER + INFO)
        artifact = self.root / 'capture.json'
        artifact.write_text(json.dumps({'schema': RENDER_BENCHMARK_V3}), encoding='utf-8')
        code, out, _ = self.run_cli(['--log', str(log), '--artifact', str(artifact)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)['verdict'], 'pass')

    def test_errors_are_capped_and_truncated(self):
        long_line = '[GL][API][Error][HIGH] id=9 ' + 'x' * 500 + '\n'
        text = MARKER + long_line * 25
        code, document = self.verdict(text)
        self.assertEqual(code, 1)
        self.assertEqual(document['error_count'], 25)
        self.assertEqual(len(document['errors']), 20)
        self.assertTrue(all(len(line) <= 300 for line in document['errors']))

    def test_truncated_or_malformed_record_fails(self):
        for record in ('[GL][API][Error][HIGH', '[GL][API][Other][LOW]',
                       '[GL][API][Other][LOW] id=oops message',
                       '[GL][API][Other][LOW] id=123', '[GL]garbled', '[GL',
                       '[GL[API][Other][LOW] id=123 message'):
            with self.subTest(record=record):
                code, document = self.verdict(MARKER + record)
                self.assertEqual(code, 1)
                self.assertEqual(document['verdict'], 'fail')
                self.assertEqual(document['malformed_count'], 1)
                self.assertEqual(document['error_count'], 1)

    def test_malformed_record_fails_even_without_install_marker(self):
        code, document = self.verdict('[GL][API][Error][HIGH')
        self.assertEqual(code, 1)
        self.assertEqual(document['verdict'], 'fail')

    def test_oversize_log_and_artifact_refuse(self):
        log = self.write_log(MARKER)
        large = self.root / 'oversize'
        with large.open('wb') as output:
            output.truncate(checker.MAX_FILE_BYTES + 1)
        for args in (['--log', str(large)],
                     ['--log', str(log), '--artifact', str(large)]):
            with self.subTest(args=args):
                code, out, err = self.run_cli(args)
                self.assertEqual(code, 2)
                self.assertEqual(out, '')
                self.assertIn('byte cap', err)

    def test_line_and_record_count_caps_refuse(self):
        with mock.patch.object(checker, 'MAX_RECORDS', 2):
            for text in (MARKER + INFO * 2, MARKER + INFO.rstrip() * 3):
                with self.subTest(text=text):
                    code, document = self.verdict(text)
                    self.assertEqual(code, 2)
                    self.assertIsNone(document)

    def test_overlong_line_refuses(self):
        code, document = self.verdict(MARKER + 'x' * (checker.MAX_LINE_CHARS + 1))
        self.assertEqual(code, 2)
        self.assertIsNone(document)

    def test_invalid_utf8_and_utf16_refuse(self):
        for payload in (MARKER.encode() + b'\xff', b'\xff\xfeA'):
            with self.subTest(payload=payload):
                log = self.root / 'invalid.log'
                log.write_bytes(payload)
                code, out, err = self.run_cli(['--log', str(log)])
                self.assertEqual(code, 2)
                self.assertEqual(out, '')
                self.assertIn('text', err)

    def test_nul_path_refuses_without_traceback(self):
        for option in ('--log', '--artifact'):
            args = ['--log', str(self.write_log(MARKER))]
            if option == '--log':
                args = ['--log', 'bad\x00.log']
            else:
                args += ['--artifact', 'bad\x00.json']
            with self.subTest(option=option):
                code, out, err = self.run_cli(args)
                self.assertEqual(code, 2)
                self.assertEqual(out, '')
                self.assertNotIn('Traceback', err)



if __name__ == '__main__':
    unittest.main()
