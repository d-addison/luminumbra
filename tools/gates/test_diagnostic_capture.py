#!/usr/bin/env python3
"""Synthetic schema-1 bundles: no renderer, network, or third-party modules."""
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import shutil
import stat
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest import mock

CHECKER = Path(__file__).with_name('check-diagnostic-capture.py')
SPEC = importlib.util.spec_from_file_location('diagnostic_capture', CHECKER)
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)
PREFIX = 'view.png'


class Bundle:
    def __init__(self, root):
        self.root = root
        self.meta = dict(schema=1, status='complete', frame=7, width=2, height=2,
                         origin='top_left', surface='deferred_only', identity_complete=True,
                         coverage_complete=True, unsupported_producers=[],
                         capture_manifest='view.manifest.json', encodings=checker.ENCODINGS.copy(),
                         camera=dict(view=[0.0] * 16, projection=[0.0] * 16,
                                     matrix_order='column_major', reversed_z=True))
        self.table = dict(schema=1, meshes=[dict(id=1, path='res/mesh.glb', provenance=1)],
                          draws=[dict(id=1, mesh=1, lod=2, ownership=3, **{'pass': 1},
                                      chunk=[-1, 0, 1], shader_variant='default',
                                      lod_convention='zero_for_non_lod', supported=True)])
        self.ids = [(1, 1, 2, 259), (0, 0, 0, 0), (1, 1, 2, 259), (1, 1, 2, 259)]
        self.values = dict(albedo=[(float(i), i + .25, i + .5) for i in range(4)],
                           normal=[(-1., 0., 1.), (0., 1., 0.), (1., 0., -1.), (0., -1., 0.)],
                           depth=[(.25,), (.5,), (.75,), (1.,)],
                           material=[(1.,), (2.,), (3.,), (255.,)])
        self.probe = dict(schema=1, origin='top_left', width=2, height=2, x=1, y=0,
                          background=True, id=[0, 0, 0, 0], draw=0, mesh=0, lod=0,
                          ownership=0, **{'pass': 0}, non_finite=False,
                          albedo_linear=list(self.values['albedo'][1]),
                          normal_view=list(self.values['normal'][1]), depth_hardware=.5, material=2)
        self.meta['files'] = [PREFIX + '.' + p + '.pfm' for p in checker.PLANES]
        self.meta['files'] += [PREFIX + s for s in ('.ids.bin', '.draws.json',
                                                   '.probe-1-0.json', checker.SUFFIX)]
        self.write()

    def path(self, suffix):
        return self.root / (PREFIX + suffix)

    def json(self, suffix, value):
        self.path(suffix).write_text(json.dumps(value, allow_nan=False), encoding='utf-8')

    def write(self):
        (self.root / PREFIX).write_bytes(b'image')
        (self.root / 'view.manifest.json').write_text(json.dumps(dict(
            schema=1, status='complete', resolution=dict(width=2, height=2),
            camera={k: self.meta['camera'][k] for k in ('view', 'projection')})), encoding='utf-8')
        for plane, channels in checker.PLANES.items():
            rows = self.values[plane][2:] + self.values[plane][:2]
            self.path('.' + plane + '.pfm').write_bytes(
                (b'PF\n' if channels == 3 else b'Pf\n') + b'2 2\n-1.0\n' +
                b''.join(struct.pack('<' + 'f' * channels, *pixel) for pixel in rows))
        self.path('.ids.bin').write_bytes(struct.pack('<4sIII', b'LVID', 1, 2, 2) +
                                         b''.join(struct.pack('<4I', *p) for p in self.ids))
        self.json('.draws.json', self.table)
        self.json('.probe-1-0.json', self.probe)
        self.finish()

    def finish(self):
        self.json(checker.SUFFIX, self.meta)
        # Fixed times avoid dependence on filesystem clock resolution or run order.
        for path in self.root.iterdir():
            os.utime(path, ns=(1000000000, 1000000000))
        os.utime(self.path(checker.SUFFIX), ns=(2000000000, 2000000000))

    def edit_bytes(self, suffix, transform):
        path = self.path(suffix)
        path.write_bytes(transform(path.read_bytes()))
        self.finish()

    def check(self):
        return checker.validate(self.path(checker.SUFFIX),
                                [p.name for p in self.root.iterdir() if p.name.startswith(PREFIX + '.')])


def meta(key, value):
    def change(bundle):
        bundle.meta[key] = value
        bundle.finish()
    return change


def table(key, value):
    def change(bundle):
        bundle.table[key] = value
        bundle.json('.draws.json', bundle.table)
        bundle.finish()
    return change


def probe(key, value):
    def change(bundle):
        bundle.probe[key] = value
        bundle.json('.probe-1-0.json', bundle.probe)
        bundle.finish()
    return change


def binary(suffix, transform):
    return lambda b: b.edit_bytes(suffix, transform)


def missing(b):
    b.path(checker.SUFFIX).unlink()


def payload_missing(b):
    b.path('.normal.pfm').unlink()


def newer(b):
    os.utime(b.path('.ids.bin'), ns=(3000000000, 3000000000))


def oversized(b):
    with b.path('.depth.pfm').open('ab') as stream:
        stream.truncate(1024 * 1024)
    b.finish()


def unsupported(b, deferred=False):
    record = copy.deepcopy(b.table['draws'][0])
    record.update(id=2, mesh=2, supported=False)
    record['pass'] = 1 if deferred else 2
    b.table['meshes'].append(dict(id=2, path='', provenance=4))
    b.table['draws'].append(record)
    b.meta.update(coverage_complete=False, unsupported_producers=[record])
    b.write()


def missing_unsupported(b):
    unsupported(b)
    b.meta['unsupported_producers'] = []
    b.finish()


def capture_document(b, key, value):
    path = b.root / 'view.manifest.json'
    document = json.loads(path.read_text(encoding='utf-8'))
    document[key] = value
    path.write_text(json.dumps(document), encoding='utf-8')
    b.finish()


def boolean_resolution(b):
    b.meta.update(width=1, height=4)
    b.meta['files'].remove(PREFIX + '.probe-1-0.json')
    b.path('.probe-1-0.json').unlink()
    for plane in checker.PLANES:
        path = b.path('.' + plane + '.pfm')
        path.write_bytes(path.read_bytes().replace(b'2 2\n', b'1 4\n', 1))
    path = b.path('.ids.bin')
    path.write_bytes(path.read_bytes()[:8] + struct.pack('<II', 1, 4) + path.read_bytes()[16:])
    capture_document(b, 'resolution', dict(width=True, height=4))


def probe_name(b, name):
    b.path('.probe-1-0.json').rename(b.path(name))
    b.meta['files'][6] = PREFIX + name
    b.finish()


def missing_nonfinite_depth(b):
    b.values['depth'][1] = (float('nan'),)
    b.probe['non_finite'] = True
    del b.probe['depth_hardware']
    b.write()


# Each refusal has a separately named unittest and a unique expected finding.
INVALID = [
    ('missing_completion', 'missing member', missing),
    ('missing_payload', 'manifest members', payload_missing),
    ('incomplete_status', 'completion status', meta('status', 'pending')),
    ('incomplete_identity', 'completion status', meta('identity_complete', False)),
    ('metadata_version', 'unknown schema version', meta('schema', 2)),
    ('table_version', 'unknown schema version', table('schema', 2)),
    ('probe_version', 'unknown schema version', probe('schema', 2)),
    ('boolean_version', 'unknown schema version', meta('schema', True)),
    ('zero_dimension', 'dimension bound', meta('width', 0)),
    ('huge_dimension', 'dimension bound', meta('width', 16385)),
    ('pixel_bound', 'dimension bound', lambda b: (b.meta.update(width=16384, height=16384), b.finish())),
    ('fractional_dimension', 'dimension bound', meta('height', 1.5)),
    ('boolean_dimension', 'dimension bound', meta('width', True)),
    ('encoding', 'attachment encodings', meta('encodings', {})),
    ('origin', 'attachment encodings', meta('origin', 'bottom_left')),
    ('camera', 'frame camera', meta('camera', {})),
    ('frame', 'frame camera', meta('frame', -1)),
    ('huge_camera_number', 'JSON integer bound', lambda b: (
        b.meta['camera']['view'].__setitem__(0, 10**400), b.finish())),
    ('unsafe_filename', 'manifest filenames', meta('files', ['../escape'] * 7)),
    ('duplicate_filename', 'manifest filenames', meta('files', [PREFIX + '.ids.bin'] * 7)),
    ('manifest_names', 'manifest members', meta('files', [PREFIX + s for s in
        ('.normal.pfm', '.albedo.pfm', '.depth.pfm', '.material.pfm', '.ids.bin',
         '.draws.json', '.probe-1-0.json', checker.SUFFIX)])),
    ('publication_order', 'completion written last', newer),
    ('capture_manifest_name', 'capture manifest name', meta('capture_manifest', 'C:escape')),
    ('empty_capture', 'empty capture', binary('', lambda _: b'')),
    ('capture_mismatch', 'capture manifest mismatch', lambda b: (
        (b.root / 'view.manifest.json').write_text('{"schema":1,"status":"pending"}', encoding='utf-8'),
        b.finish())),
    ('capture_boolean_matrix', 'capture manifest mismatch', lambda b: capture_document(
        b, 'camera', dict(view=[False] * 16, projection=[0.0] * 16))),
    ('capture_boolean_resolution', 'capture manifest mismatch', boolean_resolution),
    ('malformed_json', 'malformed JSON', binary('.draws.json', lambda _: b'{')),
    ('duplicate_json_key', 'duplicate JSON key', binary('.draws.json', lambda _: b'{"schema":1,"schema":1}')),
    ('nonstandard_json', 'nonstandard JSON number', binary('.probe-1-0.json', lambda _: b'{"x":NaN}')),
    ('invalid_utf8', 'malformed JSON', binary('.draws.json', lambda _: b'\xff')),
    ('json_array', 'JSON object required', binary('.draws.json', lambda _: b'[]')),
    ('json_bound', 'JSON size bound', binary('.draws.json', lambda _: b' ' * (checker.MAX_JSON + 1))),
    ('empty_json', 'JSON size bound', binary('.draws.json', lambda _: b'')),
    ('completion_bound', 'JSON size bound', lambda b: (
        b.path(checker.SUFFIX).write_bytes(b' ' * (checker.MAX_COMPLETION + 1)))),
    ('table_bound', 'table entry bound', table('meshes', [{}] * 65537)),
    ('mesh_record', 'mesh record', table('meshes', [dict(id=3, path='res/x', provenance=1)])),
    ('mesh_path', 'mesh path', table('meshes', [dict(id=1, path='../x', provenance=1)])),
    ('draw_identity', 'draw identity', table('draws', [dict(id=0)])),
    ('draw_metadata', 'draw metadata', lambda b: (
        b.table['draws'][0].update(chunk=[]), b.json('.draws.json', b.table), b.finish())),
    ('provenance', 'unsupported provenance', lambda b: (
        b.table['draws'][0].update(supported=False), b.json('.draws.json', b.table), b.finish())),
    ('unsupported_omitted', 'unsupported records', missing_unsupported),
    ('unsupported_deferred', 'unsupported deferred producer', lambda b: unsupported(b, True)),
    ('coverage_flag', 'unsupported records', meta('coverage_complete', False)),
    ('pfm_channels', 'PFM channel header', binary('.albedo.pfm', lambda d: d.replace(b'PF\n', b'Pf\n', 1))),
    ('pfm_dimensions', 'PFM dimensions', binary('.normal.pfm', lambda d: d.replace(b'2 2', b'1 4', 1))),
    ('pfm_endian', 'PFM little endian scale', binary('.depth.pfm', lambda d: d.replace(b'-1.0', b'+1.0', 1))),
    ('pfm_truncated', 'PFM payload size', binary('.normal.pfm', lambda d: d[:-1])),
    ('pfm_trailing', 'PFM payload size', binary('.depth.pfm', lambda d: d + b'x')),
    ('pfm_oversized', 'PFM payload size', oversized),
    ('id_version', 'unknown ID format version', binary('.ids.bin', lambda d: d[:4] + struct.pack('<I', 2) + d[8:])),
    ('id_magic', 'unknown ID format version', binary('.ids.bin', lambda d: b'BAD!' + d[4:])),
    ('id_dimensions', 'ID dimensions', binary('.ids.bin', lambda d: d[:8] + struct.pack('<II', 1, 4) + d[16:])),
    ('id_partial_header', 'partial payload read', binary('.ids.bin', lambda d: d[:12])),
    ('id_truncated', 'ID payload size', binary('.ids.bin', lambda d: d[:-1])),
    ('id_trailing', 'ID payload size', binary('.ids.bin', lambda d: d + b'x')),
    ('id_unresolved', 'ID draw join', binary('.ids.bin', lambda d: d[:16] + struct.pack('<I', 9) + d[20:])),
    ('id_mesh', 'ID draw join', binary('.ids.bin', lambda d: d[:20] + struct.pack('<I', 2) + d[24:])),
    ('id_lod', 'ID draw join', binary('.ids.bin', lambda d: d[:24] + struct.pack('<I', 256) + d[28:])),
    ('id_reserved', 'ID draw join', binary('.ids.bin', lambda d: d[:28] + struct.pack('<I', 0x10103) + d[32:])),
    ('id_unknown_owner', 'ID draw join', binary('.ids.bin', lambda d: d[:28] + struct.pack('<I', 260) + d[32:])),
    ('id_unknown_pass', 'ID draw join', binary('.ids.bin', lambda d: d[:28] + struct.pack('<I', 1283) + d[32:])),
    ('id_partial_background', 'ID draw join', binary('.ids.bin', lambda d: d[:32] + struct.pack('<4I', 0, 1, 0, 0) + d[48:])),
    ('probe_identity', 'probe identity or coordinates', probe('draw', 1)),
    ('probe_boolean_id', 'probe identity or coordinates', probe('id', [False, 0, 0, 0])),
    ('probe_coordinate', 'probe identity or coordinates', probe('y', 1)),
    ('probe_filename', 'probe filename', lambda b: probe_name(b, '.probe-bad.json')),
    ('probe_outside', 'probe coordinate bound', lambda b: probe_name(b, '.probe-2-0.json')),
    ('probe_albedo', 'probe plane albedo', probe('albedo_linear', [2, 2.25, 2.5])),
    ('probe_normal', 'probe plane normal', probe('normal_view', [1, 0, -1])),
    ('probe_depth', 'probe plane depth', probe('depth_hardware', .75)),
    ('probe_missing_null', 'probe plane depth', missing_nonfinite_depth),
    ('probe_huge_number', 'JSON integer bound', probe('depth_hardware', 10**400)),
    ('probe_material', 'probe plane material', probe('material', 3)),
    ('probe_nonfinite', 'probe nonfinite flag', probe('non_finite', True)),
    ('material_fractional', 'material finite integral range', binary('.material.pfm', lambda d: d[:-4] + struct.pack('<f', 1.5))),
    ('material_range', 'material finite integral range', binary('.material.pfm', lambda d: d[:-4] + struct.pack('<f', 256))),
    ('material_nan', 'material finite integral range', binary('.material.pfm', lambda d: d[:-4] + struct.pack('<f', float('nan')))),
]


class DiagnosticCaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = Bundle(self.root)

    def test_valid(self):
        self.assertEqual(self.bundle.check(), [])
        self.assertEqual(checker.discover(self.root)[0][0], self.bundle.path(checker.SUFFIX))
        self.assertEqual(checker.discover(self.bundle.path(checker.SUFFIX))[0][0],
                         self.bundle.path(checker.SUFFIX))

    def test_no_probe_and_empty_tables(self):
        b = self.bundle
        b.meta['files'].remove(PREFIX + '.probe-1-0.json')
        b.path('.probe-1-0.json').unlink()
        b.ids = [(0, 0, 0, 0)] * 4
        b.table.update(draws=[], meshes=[])
        b.write()
        b.path('.probe-1-0.json').unlink()
        self.assertEqual(b.check(), [])

    def test_equal_timestamps(self):
        os.utime(self.bundle.path(checker.SUFFIX), ns=(1000000000, 1000000000))
        self.assertEqual(self.bundle.check(), [])

    def test_float32_roundtrip(self):
        b = self.bundle
        b.values['depth'][1] = (.1,)
        b.probe['depth_hardware'] = .1
        b.write()
        self.assertEqual(b.check(), [])

    def test_completion_overhead_beyond_table_bound(self):
        # Completion repeats the bounded unsupported draw subset plus its own
        # filenames/camera fields, so its bound must exceed the table's bound.
        self.bundle.meta['padding'] = 'x' * checker.MAX_JSON
        self.bundle.finish()
        self.assertEqual(self.bundle.check(), [])

    def test_explicit_unsupported_forward(self):
        unsupported(self.bundle)
        self.assertEqual(self.bundle.check(), [])

    def test_nonfinite_per_plane_and_probe_null(self):
        b = self.bundle
        for plane in ('albedo', 'normal', 'depth'):
            values = list(b.values[plane][1])
            values[0] = float('nan') if plane == 'normal' else float('inf')
            b.values[plane][1] = tuple(values)
        b.probe.update(albedo_linear=[None, 1.25, 1.5], normal_view=[None, 1, 0],
                       depth_hardware=None, non_finite=True)
        b.write()
        self.assertEqual(b.check(), ['albedo: 1 nonfinite samples', 'normal: 1 nonfinite samples',
                                     'depth: 1 nonfinite samples'])

    def test_nonfinite_including_material(self):
        b = self.bundle
        for plane in checker.PLANES:
            values = list(b.values[plane][0])
            values[0] = float('nan')
            b.values[plane][0] = tuple(values)
        b.write()
        with self.assertRaisesRegex(checker.Invalid, 'albedo: 1.*normal: 1.*depth: 1.*material: 1'):
            b.check()

    def test_ppm_tree(self):
        child = self.root / 'nested'
        child.mkdir()
        Bundle(child)
        for path in list(child.iterdir()):
            path.rename(child / path.name.replace('view.png', 'view.ppm'))
        metadata = json.loads((child / 'view.ppm.diagnostics.json').read_text())
        metadata['files'] = [name.replace('view.png', 'view.ppm') for name in metadata['files']]
        (child / 'view.ppm.diagnostics.json').write_text(json.dumps(metadata), encoding='utf-8')
        found = checker.discover(self.root)
        self.assertEqual(len(found), 2)
        for path, members in found:
            self.assertEqual(checker.validate(path, members), [])

    def test_partial_tree(self):
        self.bundle.path(checker.SUFFIX).unlink()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main([str(self.root)]), 1)

    def test_pending_member(self):
        self.bundle.path(checker.SUFFIX + '.pending').write_bytes(b'')
        with self.assertRaisesRegex(checker.Invalid, 'manifest members'):
            self.bundle.check()

    def test_empty_tree(self):
        with tempfile.TemporaryDirectory() as empty:
            with self.assertRaisesRegex(checker.Invalid, 'no diagnostic bundles'):
                checker.discover(Path(empty))

    def test_tree_bound(self):
        with mock.patch.object(checker, 'MAX_ENTRIES', 1):
            with self.assertRaisesRegex(checker.Invalid, 'capture tree entry bound'):
                checker.discover(self.root)

    def test_io_and_usage(self):
        result = subprocess.run([sys.executable, '-I', '-B', str(CHECKER)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        result = subprocess.run([sys.executable, '-I', '-B', str(CHECKER), str(self.root / 'absent')],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        result = subprocess.run([sys.executable, '-I', '-B', str(CHECKER), str(self.root / PREFIX)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        with mock.patch.object(checker, 'open_regular', side_effect=PermissionError('denied')):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(checker.main([str(self.root)]), 2)
        with mock.patch.object(Path, 'resolve', side_effect=RuntimeError('symlink loop')):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(checker.main([str(self.root)]), 2)

    def test_cli_valid_invalid(self):
        for valid in (True, False):
            if not valid:
                self.bundle.path('.ids.bin').write_bytes(b'LVID')
            result = subprocess.run([sys.executable, '-I', '-B', str(CHECKER), str(self.root)],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0 if valid else 1, result.stderr + result.stdout)

    def test_special_file_refused_without_read(self):
        # Mock the common owning-handle helper rather than requiring symlink rights.
        with mock.patch.object(checker, 'open_regular', side_effect=ValueError('not a regular file')):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(checker.main([str(self.root)]), 1)

    def test_fifo_or_directory_refused(self):
        path = self.bundle.path('.ids.bin')
        path.unlink()
        if hasattr(os, 'mkfifo'):
            os.mkfifo(path)
        else:
            path.mkdir()
        with self.assertRaisesRegex(ValueError, 'not a regular file'):
            self.bundle.check()

    def test_link_refusal_including_denied_creation(self):
        self.check_link_refusal()
        with mock.patch.object(Path, 'symlink_to', side_effect=PermissionError('denied')):
            self.check_link_refusal()

    def check_link_refusal(self):
        link = self.root / 'linked'
        try:
            link.symlink_to(self.bundle.path('.ids.bin'))
        except OSError:
            # Windows accounts may lack link privilege: exercise discovery's
            # actual link refusal through the enumeration boundary in that case.
            entry = mock.Mock(path=str(link))
            entry.name = 'linked'
            original = os.lstat

            def link_status(path, *args, **kwargs):
                if Path(path) == link:
                    return SimpleNamespace(st_mode=stat.S_IFLNK, st_file_attributes=0)
                return original(path, *args, **kwargs)

            with mock.patch.object(checker.os, 'scandir') as scan, \
                    mock.patch.object(checker.os, 'lstat', side_effect=link_status):
                scan.return_value.__enter__.return_value = iter([entry])
                with self.assertRaisesRegex(checker.Invalid, 'linked tree entry'):
                    checker.discover(self.root)
        else:
            with self.assertRaisesRegex(checker.Invalid, 'linked tree entry'):
                checker.discover(self.root)
            link.unlink()

    def test_windows_reparse_directory(self):
        # Junctions are not symlinks on older Python/Windows versions. Exercise
        # the Windows lstat attribute on every host, without link privileges.
        directory = self.root / 'junction'
        directory.mkdir()
        original = os.lstat

        def reparse_status(path, *args, **kwargs):
            if Path(path) == directory:
                return SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=0x400)
            return original(path, *args, **kwargs)

        with mock.patch.object(checker.os, 'lstat', side_effect=reparse_status):
            with self.assertRaisesRegex(checker.Invalid, 'linked tree entry'):
                checker.discover(self.root)

    def test_selected_bundle_ignores_unrelated_links(self):
        unrelated = self.root / 'other.png.depth.pfm'
        unrelated.write_bytes(b'other capture')
        original = os.lstat

        def linked_status(path, *args, **kwargs):
            if Path(path) == unrelated:
                return SimpleNamespace(st_mode=stat.S_IFLNK, st_file_attributes=0)
            return original(path, *args, **kwargs)

        with mock.patch.object(checker.os, 'lstat', side_effect=linked_status):
            self.assertEqual(len(checker.discover(self.bundle.path(checker.SUFFIX))), 1)
            with self.assertRaisesRegex(checker.Invalid, 'linked tree entry'):
                checker.discover(self.root)

    def test_completion_alias_uses_resolved_long_filename(self):
        alias = self.root / 'VIEW~1.JSO'
        completion = self.bundle.path(checker.SUFFIX)
        original = Path.resolve

        def resolved(path, *args, **kwargs):
            return completion if path == alias else original(path, *args, **kwargs)

        with mock.patch.object(Path, 'resolve', resolved), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main([str(alias)]), 0)

    def test_partial_read_and_trailing_growth(self):
        with self.assertRaisesRegex(checker.Invalid, 'partial payload read'):
            checker.read_exact(io.BytesIO(b'x'), 2)
        with self.assertRaisesRegex(checker.Invalid, 'trailing payload bytes'):
            checker.aov(io.BytesIO(struct.pack('<f', 0) + b'x'), 0, 1, 1, 1, [])

    def test_changed_member(self):
        original = checker.identities

        def change(stream, *args):
            result = original(stream, *args)
            # Mutate after it has been read, through the selected artifact path.
            os.utime(self.bundle.path('.ids.bin'), ns=(4000000000, 4000000000))
            return result

        with mock.patch.object(checker, 'identities', side_effect=change):
            with self.assertRaisesRegex(checker.Invalid, 'member changed during validation'):
                self.bundle.check()

    def test_ci_and_optional_hook_registration(self):
        root = CHECKER.parents[2]
        registry = importlib.util.spec_from_file_location('capture_ci_paths', root / 'tools/ci/ci_paths.py')
        module = importlib.util.module_from_spec(registry)
        registry.loader.exec_module(module)
        self.assertIn('tools/gates/test_diagnostic_capture.py', module.PYTHON_TESTS)
        self.assertIn('tools/gates/check-diagnostic-capture.py', module.PYTHON_MODULES)
        self.assertIn('tools/gates/check-diagnostic-capture.py', module.CI_ENTRYPOINTS)
        cmake = (root / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('add_test(NAME DiagnosticCapturePython COMMAND "${Python3_EXECUTABLE}" -I -B', cmake)
        self.assertIn('"${CMAKE_SOURCE_DIR}/tools/gates/test_diagnostic_capture.py")', cmake)
        script = CHECKER.with_name('diff-render-health.ps1')
        source = script.read_text(encoding='utf-8')
        self.assertIn('if ($CapturePath)', source)
        self.assertIn('if (Test-Path -LiteralPath $bundlePath)', source)
        self.assertIn('& python3 -I -B $checker $bundlePath', source)
        self.assertIn('if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }', source)
        shell = shutil.which('pwsh') or shutil.which('powershell')
        if shell:
            baseline = self.root / 'health.json'
            baseline.write_text('{"healthy":true}', encoding='utf-8')
            command = [shell, '-NoProfile', '-File', str(script), '-BaselinePath', str(baseline),
                       '-CurrentPath', str(baseline)]
            for capture in (None, self.root / 'absent.png', self.root / PREFIX):
                args = command + (['-CapturePath', str(capture)] if capture else [])
                result = subprocess.run(args, capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.bundle.path('.ids.bin').write_bytes(b'LVID')
            result = subprocess.run(command + ['-CapturePath', str(self.root / PREFIX)],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

def invalid_test(reason, mutate):
    def test(self):
        mutate(self.bundle)
        with self.assertRaisesRegex(checker.Invalid, reason):
            self.bundle.check()
    return test


for name, reason, mutate in INVALID:
    setattr(DiagnosticCaptureTests, 'test_invalid_' + name, invalid_test(reason, mutate))


class MetricIntegration(unittest.TestCase):
    def test_default_rois_are_advisory_and_missing_weather_is_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle = Bundle(root)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(checker.main([str(root)]), 0)
            records = [json.loads(line) for line in output.getvalue().splitlines() if line.startswith('{')]
            self.assertEqual(len(records), 9)
            self.assertTrue(all(r['qualification'] == 'advisory' for r in records))
            weather = [r for r in records if r['metric']['kind'] == 'weather']
            self.assertEqual(len(weather), 3)
            self.assertTrue(all(r['status'] == 'unavailable' and r['value'] is None for r in weather))

    def test_qualified_unavailable_is_a_cli_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            Bundle(root)
            config = json.loads(checker.DEFAULT_ROIS.read_text(encoding='utf-8'))
            config['rois'][1]['enforce'] = True
            path = root / 'rois.json'
            path.write_text(json.dumps(config), encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(checker.main([str(root), '--rois', str(path)]), 1)

    def test_roi_io_and_invalid_configuration_keep_exit_classification(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'rois.json'
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(checker.main([directory, '--rois', str(path)]), 2)
                path.write_text('{', encoding='utf-8')
                self.assertEqual(checker.main([directory, '--rois', str(path)]), 1)


if __name__ == '__main__':
    unittest.main()
