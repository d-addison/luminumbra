#!/usr/bin/env python3
"""Tests for the GL KHR_debug log verdict. Synthetic logs in a temporary directory."""
import codecs
import contextlib
import io
import json
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
