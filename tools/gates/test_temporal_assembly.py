#!/usr/bin/env python3
"""Manifest assembly contracts using stdlib image doubles and a real child encoder."""
import contextlib
import copy
import hashlib
import importlib.util
import inspect
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ASSEMBLER = Path(__file__).resolve().parents[1] / 'timelapse.py'
SPEC = importlib.util.spec_from_file_location('temporal_assembly', ASSEMBLER)
assembler = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(assembler)
REAL_RUN = subprocess.run


class ImageDouble:
    """Record exactly which decoded bytes reach the existing GIF/PNG interfaces."""
    BILINEAR = 2

    def __init__(self, data, size=(2, 1)):
        self.data, self.size = data, size

    @classmethod
    def open(cls, filename):
        descriptor = os.open(filename, os.O_RDONLY | getattr(os, 'O_NONBLOCK', 0)
                             | getattr(os, 'O_BINARY', 0))
        with os.fdopen(descriptor, 'rb') as source:
            # The regression also fails promptly on the old assembler, whose
            # decoder would otherwise block forever on a FIFO mutation.
            assert stat.S_ISREG(os.fstat(source.fileno()).st_mode), 'nonregular reached decoder'
            data = source.read()
        width, height = map(int, data.split(b'\n')[1].split())
        return cls(data, (width, height))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        pass

    def convert(self, mode):
        assert mode == 'RGB'
        return ImageDouble(self.data, self.size)

    def resize(self, size, resample):
        assert resample == self.BILINEAR
        return ImageDouble(self.data, size)

    def save(self, filename, **options):
        images = [self] + options.pop('append_images', [])
        Path(filename).write_bytes(json.dumps(dict(
            images=[dict(bytes=image.data.hex(), size=image.size) for image in images],
            options=options), sort_keys=True).encode())


class TemporalAssemblyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='temporal_assembly_test_')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.frames = self.root / 'frames'
        self.frames.mkdir()
        self.gif, self.mp4 = self.root / 'result.gif', self.root / 'result.mp4'
        self.manifest = self.frames / 'arbitrary-sequence-name.json'
        self.names = ['frame-0.color9.ppm', 'frame-1.color2.ppm']
        self.data = [b'P6\n2 1\n255\nabcdef', b'P6\n2 1\n255\nghijkl']
        planned, captured = [], []
        for index, (name, data) in enumerate(zip(self.names, self.data)):
            (self.frames / name).write_bytes(data)
            frame = dict(index=index, tick=100 + index, sub_tick_60=0,
                         kind=0, control=0, dense=False, hold_frame=0)
            planned.append(frame)
            captured.append(dict(frame, payloads=[self.payload(name, data)]))
        sidecar = 'frame-0.ppm.depth.pfm'
        (self.frames / sidecar).write_bytes(b'\x00\xff\r\n')
        captured[0]['payloads'].append(self.payload(sidecar, b'\x00\xff\r\n'))
        # All fields emitted by the version-1 writer, including integer-only plan JSON.
        self.document = dict(schema='luminumbra.temporal_audit_sequence', version=1,
                             status='complete', frames=captured, plan=dict(
                                 span=0, start_tick=100, ticks_per_day=100, ticks_per_second=30,
                                 dawn_tick=20, dusk_tick=80, strike_tick=0,
                                 night_moon_phase_milli=1000, tick_limit=10000000,
                                 frame_limit=4096, end_tick=200, sparse_interval_ticks=1,
                                 boundary_ticks=[100, 200], presentation_steps=400,
                                 max_render_attempts=1000, settling=dict(
                                     warmup_frames=90, initial_readiness_retries=300,
                                     sample_settle_frames=3, sample_readiness_retries=60),
                                 frames=planned))
        self.write_manifest()
        pillow = types.ModuleType('PIL')
        pillow.Image = ImageDouble
        self.pillow = mock.patch.dict(sys.modules, PIL=pillow)
        self.pillow.start()
        self.addCleanup(self.pillow.stop)
        self.output = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.output)
        self.redirect.__enter__()
        self.addCleanup(self.redirect.__exit__, None, None, None)

    @staticmethod
    def payload(name, data):
        return dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())

    def write_manifest(self):
        canonical = json.dumps(self.document['plan'], sort_keys=True, separators=(',', ':'),
                               ensure_ascii=False).encode()
        self.document['plan_sha256'] = hashlib.sha256(canonical).hexdigest()
        self.manifest.write_bytes(json.dumps(self.document).encode() + b'\r\n')

    def build(self, mp4=False, **overrides):
        arguments = dict(frames_dir=str(self.frames), out_gif=str(self.gif), fps=12.0,
                         scale=1.0, pattern='*.ppm', mp4=str(self.mp4) if mp4 else None,
                         manifest=str(self.manifest))
        arguments.update(overrides)
        return assembler.build(**arguments)

    def refused(self, reason, **arguments):
        before = {p.name: p.read_bytes() for p in (self.gif, self.mp4) if p.exists()}
        with self.assertRaisesRegex(SystemExit, reason):
            self.build(**arguments)
        after = {p.name: p.read_bytes() for p in (self.gif, self.mp4) if p.exists()}
        self.assertEqual(before, after)
        self.assertFalse(any(p.name.startswith('.timelapse_') for p in self.root.iterdir()))

    def encoder(self, body):
        script = self.root / 'encoder.py'
        script.write_text('import sys\nfrom pathlib import Path\n' + body, encoding='utf-8')

        def run(command, **options):
            return REAL_RUN([sys.executable, '-I', '-B', str(script), *command[1:]], **options)

        return mock.patch.object(assembler.subprocess, 'run', side_effect=run)

    def test_planned_order_ignores_filename_mtime_and_record_order(self):
        self.document['frames'].reverse()
        self.write_manifest()
        for index, name in enumerate(self.names):
            os.utime(self.frames / name, ns=(1000000000 - index, 1000000000 - index))
        self.assertEqual(self.build(), 2)
        result = json.loads(self.gif.read_bytes())
        self.assertEqual([v['bytes'] for v in result['images']], [v.hex() for v in self.data])

    def test_final_tick_hold_order_is_preserved(self):
        for index, frame in enumerate(self.document['plan']['frames']):
            frame.update(tick=200, kind=1, dense=True, hold_frame=index)
            self.document['frames'][index].update(frame)
        self.write_manifest()
        self.assertEqual(self.build(), 2)
        self.assertEqual([v['bytes'] for v in json.loads(self.gif.read_bytes())['images']],
                         [v.hex() for v in self.data])

    def test_non_json_manifest_corruption_refuses(self):
        self.manifest = self.manifest.rename(self.frames / 'sequence.capture')
        (self.frames / self.names[0]).write_bytes(self.data[0][:-1] + b'z')
        self.refused('hash mismatch')

    def test_manifest_outside_frame_directory_and_output_protection(self):
        self.manifest = self.manifest.rename(self.root / 'sequence.capture')
        self.assertEqual(self.build(), 2)
        self.refused('aliases sequence input', out_gif=str(self.manifest))
        self.assertEqual(json.loads(self.manifest.read_bytes()), self.document)

    def test_manifest_alias_is_excluded_by_file_identity(self):
        self.manifest = self.manifest.rename(self.root / 'sequence.capture')
        # The in-directory name differs from both the locator and its basename.
        os.link(self.manifest, self.frames / 'frame-completion.ppm')
        self.assertEqual(self.build(), 2)
        self.assertEqual([v['bytes'] for v in json.loads(self.gif.read_bytes())['images']],
                         [v.hex() for v in self.data])

    def test_manifest_basename_does_not_exempt_a_different_file(self):
        self.manifest = self.manifest.rename(self.root / 'frame-completion.ppm')
        (self.frames / self.manifest.name).write_bytes(self.manifest.read_bytes())
        self.refused('extra sequence frame')

    def test_unselected_manifests_are_not_discovered(self):
        for name in ('duplicate.json', 'frame-completion.capture'):
            duplicate = self.frames / name
            duplicate.write_bytes(self.manifest.read_bytes())
            if name.startswith('frame-'):
                self.refused('extra sequence frame')
            else:
                self.assertEqual(self.build(), 2)
            duplicate.unlink()

    def test_missing_explicit_manifest_refuses_without_fallback(self):
        self.refused('manifest .*missing.capture', manifest=self.root / 'missing.capture')
        self.manifest.unlink()
        self.refused('manifest .*arbitrary-sequence-name')

    def test_invalid_explicit_manifest_refuses_without_fallback(self):
        for data in (b'', b'{', b'null', b'[]', b'1', b'"manifest"',
                     b'{"schema":"other"}', b'\xff'):
            with self.subTest(data=data):
                self.manifest.write_bytes(data)
                self.refused('manifest ')

    def test_invalid_locator_paths_refuse_without_fallback(self):
        for locator in ('', '.', 'invalid\x00.capture'):
            with self.subTest(locator=locator):
                self.refused('manifest ', manifest=locator)

    def test_manifest_resolution_loop_refuses_clearly(self):
        locator = self.frames / 'loop.capture'
        original_resolve = Path.resolve

        def resolve(path, *args, **kwargs):
            if path == locator:
                raise RuntimeError('Symlink loop from locator')
            return original_resolve(path, *args, **kwargs)

        with mock.patch.object(assembler.Path, 'resolve', resolve):
            self.refused('manifest .*Symlink loop', manifest=locator)

    def test_nonregular_explicit_manifest_refuses(self):
        self.manifest = self.manifest.rename(self.frames / 'sequence.capture')
        self.manifest.unlink()
        self.manifest.mkdir()
        self.refused('regular file')
        self.manifest.rmdir()
        if hasattr(os, 'mkfifo'):
            os.mkfifo(self.manifest)
        else:
            self.manifest.mkdir()
        self.refused('regular file')

    def test_unreadable_or_partial_explicit_manifest_refuses(self):
        self.manifest = self.manifest.rename(self.frames / 'sequence.capture')
        helpers = assembler.runpy.run_path(str(ASSEMBLER.parent / 'perf' / 'fixture_paths.py'))
        original_open = helpers['open_regular']
        for failure in ('denied', 'partial', 'growing'):
            @contextlib.contextmanager
            def changed(root, path):
                if path == self.manifest and failure == 'denied':
                    raise PermissionError('manifest access denied')
                with original_open(root, path) as stream:
                    if path == self.manifest:
                        read = stream.read
                        stream.read = lambda size: (read(size)[:-1] if failure == 'partial'
                                                    else read(size) + b'x')
                    yield stream

            with self.subTest(failure=failure), mock.patch.object(
                    assembler.runpy, 'run_path', return_value=dict(helpers, open_regular=changed)):
                self.refused('access denied' if failure == 'denied' else 'changed during read')

    def test_manifest_metadata_change_during_read_refuses(self):
        helpers = assembler.runpy.run_path(str(ASSEMBLER.parent / 'perf' / 'fixture_paths.py'))
        original_open, original_fstat = helpers['open_regular'], os.fstat
        changed_descriptor = None

        @contextlib.contextmanager
        def changed(root, path):
            nonlocal changed_descriptor
            with original_open(root, path) as stream:
                if path == self.manifest:
                    read = stream.read

                    def read_changed(size):
                        nonlocal changed_descriptor
                        data = read(size)
                        changed_descriptor = stream.fileno()
                        return data

                    stream.read = read_changed
                try:
                    yield stream
                finally:
                    changed_descriptor = None

        def fstat(descriptor):
            status = original_fstat(descriptor)
            if descriptor == changed_descriptor:
                return types.SimpleNamespace(st_size=status.st_size,
                                             st_mtime_ns=status.st_mtime_ns + 1,
                                             st_ctime_ns=status.st_ctime_ns)
            return status

        with mock.patch.object(assembler.runpy, 'run_path', return_value=dict(
                helpers, open_regular=changed)), mock.patch.object(assembler.os, 'fstat', fstat):
            self.refused('manifest changed during read')

    def test_manifest_link_and_relative_locator(self):
        external = self.root / 'sequence.capture'
        external.write_bytes(self.manifest.read_bytes())
        self.manifest.write_bytes(b'{"schema":"other"}')
        link = self.frames / 'locator.capture'
        try:
            link.symlink_to(external)
        except OSError:
            self.assertFalse(link.exists())
            self.refused('manifest ', manifest=link)
        else:
            self.assertEqual(self.build(manifest=link), 2)
        # The checkout and temporary root may be on different Windows drives.
        previous_directory = Path.cwd()
        try:
            os.chdir(self.root)
            self.assertEqual(self.build(manifest=external.name), 2)
        finally:
            os.chdir(previous_directory)
        self.assertEqual([v['bytes'] for v in json.loads(self.gif.read_bytes())['images']],
                         [v.hex() for v in self.data])

    def test_manifest_size_budget_is_inclusive(self):
        size = self.manifest.stat().st_size
        with mock.patch.object(assembler, 'MAX_JSON_BYTES', size):
            self.assertEqual(self.build(), 2)
        with mock.patch.object(assembler, 'MAX_JSON_BYTES', size - 1):
            self.refused('manifest byte budget')

    def test_cli_passes_explicit_manifest_and_defaults_to_legacy(self):
        for locator in ([], ['--manifest', str(self.manifest)]):
            with self.subTest(locator=locator), mock.patch.object(assembler, 'build') as build:
                self.assertEqual(assembler.main([str(self.frames), '-o', str(self.gif), *locator]), 0)
                build.assert_called_once_with(str(self.frames), str(self.gif), 12.0, 1.0,
                                              '*.ppm', None,
                                              manifest=str(self.manifest) if locator else None)

    def test_manifest_name_may_start_with_frame_prefix(self):
        self.manifest = self.manifest.rename(self.frames / 'frame-completion.json')
        self.assertEqual(self.build(), 2)

    def test_image_hash_mismatch_refuses_before_decode(self):
        (self.frames / self.names[0]).write_bytes(self.data[0][:-1] + b'z')
        with mock.patch.object(ImageDouble, 'open', side_effect=AssertionError('decoded input')):
            self.refused('hash mismatch')

    def test_sidecar_hash_mismatch_refuses(self):
        (self.frames / 'frame-0.ppm.depth.pfm').write_bytes(b'\x00\xfe\r\n')
        self.refused('hash mismatch')

    def test_incomplete_status_and_missing_frames_refuse(self):
        original = copy.deepcopy(self.document)
        for status in ('partial', 'failed', None, True):
            with self.subTest(status=status):
                self.document['status'] = status
                self.write_manifest()
                self.refused('not complete')
        self.document = copy.deepcopy(original)
        for key in ('frames',):
            self.document[key].pop()
        self.write_manifest()
        self.refused('missing or extra frame')
        self.document = original
        self.write_manifest()
        (self.frames / self.names[1]).unlink()
        self.refused('missing sequence payload')

    def test_missing_sidecar_refuses(self):
        (self.frames / 'frame-0.ppm.depth.pfm').unlink()
        self.refused('missing sequence payload')

    def test_extra_frames_including_other_extensions_refuse(self):
        for name in ('extra.ppm', 'frame-2.unselected.png', 'unselected.jpg', 'frame-0.extra'):
            with self.subTest(name=name):
                extra = self.frames / name
                extra.write_bytes(self.data[0])
                self.refused('extra sequence frame')
                extra.unlink()

    def test_duplicate_frame_payload_and_file_identity_refuse(self):
        original = copy.deepcopy(self.document)
        self.document['frames'][1] = copy.deepcopy(self.document['frames'][0])
        self.write_manifest()
        self.refused('duplicate frame index')
        self.document = copy.deepcopy(original)
        self.document['frames'][0]['payloads'].append(self.document['frames'][0]['payloads'][0])
        self.write_manifest()
        self.refused('duplicate payload')
        self.document = original
        self.write_manifest()
        alias = self.frames / 'frame-0.alias'
        os.link(self.frames / self.names[0], alias)
        self.document['frames'][0]['payloads'].append(self.payload(alias.name, self.data[0]))
        self.write_manifest()
        self.refused('duplicate payload file identity')

    def test_plan_hash_and_capture_plan_join_refuse(self):
        self.document['plan_sha256'] = '0' * 64
        self.manifest.write_text(json.dumps(self.document), encoding='utf-8')
        self.refused('plan hash mismatch')
        self.document['frames'][0]['tick'] += 1
        self.write_manifest()
        self.refused('differs from planned tick order')

    def test_large_exact_ticks_and_unknown_or_out_of_order_metadata(self):
        for frame in self.document['plan']['frames']:
            frame['tick'] += 1 << 60
        for frame in self.document['frames']:
            frame['tick'] += 1 << 60
        self.document['plan']['start_tick'] += 1 << 60
        self.document['plan']['end_tick'] += 1 << 60
        self.write_manifest()
        self.assertEqual(self.build(), 2)
        self.gif.unlink()
        self.document['frames'][0]['dense'] = 0
        self.write_manifest()
        self.refused('differs from planned tick order')
        self.document['frames'][0]['dense'] = False
        self.document['plan']['frames'][0]['tick'] += 10
        self.write_manifest()
        self.refused('differs from planned tick order')
        self.document['frames'][0]['tick'] += 10
        self.write_manifest()
        self.refused('out-of-order planned ticks')
        self.document['unknown'] = 1
        self.write_manifest()
        self.refused('unknown sequence metadata')

    def test_invalid_schema_metadata_and_empty_input_refuse(self):
        original = copy.deepcopy(self.document)
        changes = [lambda d: d.update(version=2), lambda d: d.update(version=True),
                   lambda d: d.update(plan=[]), lambda d: d.update(frames=[]),
                   lambda d: d['plan'].update(frames=[]),
                   lambda d: d['plan'].update(end_tick=99),
                   lambda d: d['plan']['frames'][0].update(tick=True),
                   lambda d: d['plan']['frames'][0].update(sub_tick_60=60),
                   lambda d: d['plan']['frames'][0].update(control=3),
                   lambda d: d['plan']['frames'][0].update(unknown=0),
                   lambda d: d['frames'][0].update(payloads=[]),
                   lambda d: d['frames'][0]['payloads'][0].update(bytes=0),
                   lambda d: d['frames'][0]['payloads'][0].update(sha256='f'),
                   lambda d: d['frames'][0]['payloads'][0].update(unknown=0)]
        for change in changes:
            with self.subTest(change=changes.index(change)):
                self.document = copy.deepcopy(original)
                change(self.document)
                self.write_manifest()
                self.refused('.')

    def test_invalid_paths_refuse_on_every_host(self):
        original = self.names[0]
        for path in ('../' + original, 'C:' + original, '/outside.ppm',
                     'frame-0.UPPER.ppm', 'frame-0.name.', 'frame-0.a\x00.ppm',
                     'frame-0.a\\b.ppm', 'frame-0.' + 'a' * 121):
            with self.subTest(path=path):
                self.document['frames'][0]['payloads'][0]['path'] = path
                self.write_manifest()
                self.refused('invalid payload path')

    def test_bad_json_and_duplicate_keys_refuse(self):
        original = self.manifest.read_bytes()
        for data in (original[:-3], original.replace(b'"version": 1', b'"version": 1, "version": 1'),
                     original.replace(b'"schema": "luminumbra.temporal_audit_sequence"',
                                      b'"schema": "luminumbra.temporal_audit_sequen\\u0063e", '
                                      b'"schema": "other"'),
                     original.replace(b'"start_tick": 100', b'"start_tick": 1e38'),
                     original.replace(b'"start_tick": 100', b'"start_tick": NaN'),
                     original.replace(b'"start_tick": 100', b'"start_tick": ' + b'9' * 129)):
            with self.subTest(data=data[-50:]):
                self.manifest.write_bytes(data)
                self.refused('.')

    def test_pattern_cannot_hide_missing_or_multiple_images(self):
        self.refused('exactly one image', pattern='*.png')
        self.refused('exactly one image', pattern='*')

    def test_count_payload_and_byte_budgets_refuse_before_encoding(self):
        for constant, limit in (('MAX_FRAMES', 1), ('MAX_PAYLOADS', 1),
                                ('MAX_PAYLOAD_BYTES', len(self.data[0]) - 1),
                                ('MAX_INPUT_BYTES', len(self.data[0]) + 3),
                                ('MAX_JSON_BYTES', 10),
                                ('MAX_ENTRIES', 1)):
            with self.subTest(constant=constant), mock.patch.object(assembler, constant, limit,
                                                                  create=True):
                with mock.patch.object(assembler.subprocess, 'run') as encoder:
                    self.refused('budget')
                    encoder.assert_not_called()

    def test_inclusive_input_budgets_and_decoded_budgets(self):
        total = sum(p['bytes'] for f in self.document['frames'] for p in f['payloads'])
        # Smaller encoded payloads still reserve the complete decoded RGB buffers.
        with mock.patch.object(assembler, 'MAX_INPUT_BYTES', max(total, 24), create=True):
            self.assertEqual(self.build(), 2)
        self.gif.unlink()
        with mock.patch.object(assembler, 'MAX_INPUT_BYTES', total, create=True):
            self.refused('decoded image byte budget', scale=2)
        with mock.patch.object(assembler, 'MAX_PIXELS', 1, create=True):
            self.refused('image dimension budget')
        self.refused('scaled image dimension budget', scale=16384)

    def test_invalid_numeric_options_refuse(self):
        for value in (0, -1, float('inf'), float('nan')):
            for option in ('fps', 'scale'):
                with self.subTest(option=option, value=value):
                    self.refused('finite and positive', **{option: value})
        self.refused('finite and positive', fps=1e-300)

    def test_missing_encoder_preserves_outputs(self):
        self.gif.write_bytes(b'previous gif')
        self.mp4.write_bytes(b'previous video')
        with mock.patch.object(assembler.shutil, 'which', return_value=None):
            self.refused('ffmpeg not found', mp4=True)

    def test_failed_child_encoder_preserves_outputs_and_cleans_partial_video(self):
        self.gif.write_bytes(b'previous gif')
        self.mp4.write_bytes(b'previous video')
        with mock.patch.object(assembler.shutil, 'which', return_value='fake-ffmpeg'):
            with self.encoder("Path(sys.argv[-1]).write_bytes(b'partial')\nsys.exit(7)\n"):
                self.refused('ffmpeg failed', mp4=True)

    def test_encoder_disappears_and_success_without_video_refuse(self):
        with mock.patch.object(assembler.shutil, 'which', return_value='fake-ffmpeg'):
            with mock.patch.object(assembler.subprocess, 'run', side_effect=FileNotFoundError('gone')):
                self.refused('ffmpeg failed', mp4=True)
            with self.encoder('pass\n'):
                self.refused('no video', mp4=True)
            with self.encoder("Path(sys.argv[-1]).write_bytes(b'')\n"):
                self.refused('no video', mp4=True)

    def test_successful_child_encoder_publishes_both_outputs(self):
        body = "assert sys.argv[sys.argv.index('-framerate') + 1] == '12.0'\n"
        body += "frames = Path(sys.argv[sys.argv.index('-i') + 1]).parent\n"
        body += "assert len(list(frames.glob('f_*.png'))) == 2\n"
        body += "Path(sys.argv[-1]).write_bytes(b'complete video')\n"
        with mock.patch.object(assembler.shutil, 'which', return_value='fake-ffmpeg'):
            with self.encoder(body):
                self.assertEqual(self.build(mp4=True), 2)
        self.assertEqual(self.mp4.read_bytes(), b'complete video')
        self.assertEqual(len(json.loads(self.gif.read_bytes())['images']), 2)

    def test_encoder_failure_leaves_no_new_outputs(self):
        with mock.patch.object(assembler.shutil, 'which', return_value='fake-ffmpeg'):
            with self.encoder("Path(sys.argv[-1]).write_bytes(b'partial')\nsys.exit(1)\n"):
                self.refused('ffmpeg failed', mp4=True)

    def test_changed_source_path_after_hash_uses_verified_staged_bytes(self):
        original = assembler.load_sequence_images

        def swap(files, scale):
            (self.frames / self.names[0]).write_bytes(self.data[1])
            return original(files, scale)

        with mock.patch.object(assembler, 'load_sequence_images', side_effect=swap):
            self.build()
        self.assertEqual(json.loads(self.gif.read_bytes())['images'][0]['bytes'], self.data[0].hex())

    def test_output_destinations_cannot_alias(self):
        with mock.patch.object(assembler.shutil, 'which', return_value='fake-ffmpeg'):
            self.refused('destinations alias', mp4=True, out_gif=str(self.mp4))

    def test_repeat_assembly_may_replace_results_inside_capture_directory(self):
        self.gif, self.mp4 = self.frames / 'review.gif', self.frames / 'review.mp4'
        self.assertEqual(self.build(), 2)
        expected = self.gif.read_bytes()
        self.assertEqual(self.build(), 2)
        self.assertEqual(self.gif.read_bytes(), expected)
        self.mp4.write_bytes(b'old video')
        with mock.patch.object(assembler.shutil, 'which', return_value='fake-ffmpeg'):
            with self.encoder("Path(sys.argv[-1]).write_bytes(b'new video')\n"):
                self.assertEqual(self.build(mp4=True), 2)
        self.assertEqual(self.mp4.read_bytes(), b'new video')
        self.assertFalse(any(p.name.startswith('.timelapse_') for p in self.frames.iterdir()))

    def test_output_cannot_replace_input_or_manifest(self):
        for path in (self.manifest, self.frames / self.names[0]):
            before = path.read_bytes()
            self.refused('aliases sequence input', out_gif=str(path))
            self.assertEqual(path.read_bytes(), before)

    def test_gif_failure_cleans_staging(self):
        def fail(images, destination, fps):
            Path(destination).write_bytes(b'partial')
            raise OSError('GIF encode failed')

        with mock.patch.object(assembler, 'write_gif', side_effect=fail):
            self.refused('GIF encode failed')

    def test_payload_size_and_partial_or_growing_reads_refuse(self):
        for data in (b'', self.data[0][:-1], self.data[0] + b'x'):
            (self.frames / self.names[0]).write_bytes(data)
            self.refused('byte size mismatch')
        (self.frames / self.names[0]).write_bytes(self.data[0])
        original_run_path = assembler.runpy.run_path
        helpers = original_run_path(str(ASSEMBLER.parent / 'perf' / 'fixture_paths.py'))
        original_open = helpers['open_regular']

        for replacement, error in ((b'', 'partial payload read'),
                                   (self.data[0] + b'x', 'grew during read')):
            @contextlib.contextmanager
            def changed(root, path):
                with original_open(root, path) as stream:
                    if path.name == self.names[0]:
                        # Resize after fstat, on the first read through the owning handle.
                        read = stream.read
                        mutated = False

                        def read_changed(size):
                            nonlocal mutated
                            if not mutated:
                                path.write_bytes(replacement)
                                mutated = True
                            return read(size)

                        stream.read = read_changed
                    yield stream

            with mock.patch.object(assembler.runpy, 'run_path', return_value=dict(
                    helpers, open_regular=changed)):
                self.refused(error)
            (self.frames / self.names[0]).write_bytes(self.data[0])

    def test_unreadable_payload_refuses(self):
        helpers = assembler.runpy.run_path(str(ASSEMBLER.parent / 'perf' / 'fixture_paths.py'))
        original_open = helpers['open_regular']

        def denied(root, path):
            if path.name == self.names[0]:
                raise PermissionError('payload access denied')
            return original_open(root, path)

        with mock.patch.object(assembler.runpy, 'run_path', return_value=dict(
                helpers, open_regular=denied)):
            self.refused('access denied')

    def test_nonregular_and_escape_inputs_refuse(self):
        target = self.frames / self.names[0]
        target.unlink()
        target.mkdir()
        self.refused('regular file')
        target.rmdir()
        outside = self.root / 'outside.ppm'
        outside.write_bytes(self.data[0])
        try:
            target.symlink_to(outside)
        except OSError:
            # Unprivileged Windows runners deny creation; no input then exists.
            self.assertFalse(target.exists())
            self.refused('missing sequence payload')
        else:
            self.refused('outside root')

    def test_fifo_input_refuses_without_blocking(self):
        if hasattr(os, 'mkfifo'):
            target = self.frames / self.names[0]
            target.unlink()
            os.mkfifo(target)
            self.refused('regular file')
        else:
            # The nonregular directory case exercises the Windows refusal branch.
            target = self.frames / self.names[0]
            target.unlink()
            target.mkdir()
            self.refused('regular file')

    def test_legacy_byte_parity_and_missing_encoder_fallback(self):
        self.manifest.unlink()
        (self.frames / 'unrelated.json').write_text('{"camera": 1.25}', encoding='utf-8')
        # The original build pipeline is the reference, including natural order,
        # GIF bytes, scaling, stdout, and the optional-MP4 missing-encoder fallback.
        reference = self.root / 'reference.gif'
        expected_stdout = io.StringIO()
        with mock.patch.object(assembler.shutil, 'which', return_value=None):
            with contextlib.redirect_stdout(expected_stdout):
                files = assembler.collect_frames(str(self.frames), '*.ppm')
                images = assembler.load_frames(files, .5)
                assembler.write_gif(images, str(reference), 12.0)
                print(f'timelapse: wrote {reference} ({len(images)} frames @ {12.0} fps)')
                if assembler.write_mp4(images, str(self.mp4), 12.0):
                    print(f'timelapse: wrote {self.mp4}')
            self.assertEqual(self.build(mp4=True, scale=.5, manifest=None), len(images))
        self.assertEqual(self.gif.read_bytes(), reference.read_bytes())
        self.assertEqual(self.output.getvalue(), expected_stdout.getvalue().replace(
            str(reference), str(self.gif)))
        self.assertFalse(self.mp4.exists())
        # Pin the byte-for-byte unchanged legacy helpers, rather than comparing
        # two calls to a helper that might have changed together.
        source = ''.join(inspect.getsource(getattr(assembler, name)) for name in (
            'natural_key', 'collect_frames', 'load_frames', 'write_gif', 'write_mp4', 'selftest'))
        self.assertEqual(hashlib.sha256(source.encode()).hexdigest(),
                         '35ff5dd3aa2537936176a508049b2ebbaee96413fda235e017793f66d929b4b6')

    def test_legacy_unrelated_json_entries_are_byte_identical(self):
        self.manifest.unlink()
        reference = self.root / 'reference.gif'
        files = assembler.collect_frames(str(self.frames), '*.ppm')
        assembler.write_gif(assembler.load_frames(files, 1.0), str(reference), 12.0)
        unrelated = self.frames / 'unrelated.json'
        for kind in ('directory', 'file', 'invalid', 'oversized', 'unreadable'):
            with self.subTest(kind=kind):
                if kind == 'directory':
                    unrelated.mkdir()
                else:
                    unrelated.write_bytes(b'{' if kind == 'invalid' else b'{"camera":1.25}')
                try:
                    helpers = assembler.runpy.run_path(str(ASSEMBLER.parent / 'perf' /
                                                           'fixture_paths.py'))
                    original_open = helpers['open_regular']

                    def read_regular(root, path):
                        if path == unrelated and kind == 'unreadable':
                            raise PermissionError('unreadable unrelated entry')
                        return original_open(root, path)

                    # The unreadable seam wraps the exact reader discovery used;
                    # the oversized case lowers the ceiling rather than allocating.
                    limit = 1 if kind == 'oversized' else assembler.MAX_JSON_BYTES
                    with mock.patch.object(assembler, 'MAX_JSON_BYTES', limit), mock.patch.object(
                            assembler.runpy, 'run_path', return_value=dict(
                                helpers, open_regular=read_regular)):
                        self.assertEqual(self.build(manifest=None), 2)
                    self.assertEqual(self.gif.read_bytes(), reference.read_bytes())
                finally:
                    if kind == 'directory':
                        unrelated.rmdir()
                    else:
                        unrelated.unlink()

    def test_unrelated_json_mentioning_schema_is_not_a_sequence(self):
        self.manifest.unlink()
        (self.frames / 'notes.json').write_text(json.dumps(dict(
            schema='other', description='luminumbra.temporal_audit_sequence')),
            encoding='utf-8')
        self.assertEqual(self.build(manifest=None), 2)

    def test_legacy_does_not_discover_valid_sequence(self):
        reference = self.root / 'reference.gif'
        files = assembler.collect_frames(str(self.frames), '*.ppm')
        assembler.write_gif(assembler.load_frames(files, 1.0), str(reference), 12.0)
        with mock.patch.object(assembler, 'read_sequence', side_effect=AssertionError('discovery')):
            self.assertEqual(self.build(manifest=None), 2)
        self.assertEqual(self.gif.read_bytes(), reference.read_bytes())

    def test_legacy_missing_root_and_empty_input_diagnostics(self):
        for root in (self.root / 'missing', self.root / 'empty'):
            if root.name == 'empty':
                root.mkdir()
            with self.subTest(root=root), self.assertRaises(SystemExit) as old:
                assembler.collect_frames(str(root), '*.ppm')
            with self.assertRaises(SystemExit) as current:
                self.build(frames_dir=str(root), manifest=None)
            self.assertEqual(str(current.exception), str(old.exception))


if __name__ == '__main__':
    unittest.main()
