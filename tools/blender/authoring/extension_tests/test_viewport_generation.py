"""Portable metadata refusal/lifecycle controls; no native or Blender claim."""
import copy
import hashlib
import importlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import threading
import time
from types import ModuleType, SimpleNamespace
import unittest
from unittest import mock

authoring = Path(__file__).parents[1]
package = ModuleType('viewport_generation_contract')
package.__path__ = [str(authoring / 'extension'), str(authoring)]
sys.modules[package.__name__] = package
v = importlib.import_module(package.__name__ + '.viewport_generation')

IDENTITY = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def sha(value):
    return hashlib.sha256(value).hexdigest()


def node(identity='fixture.child', parent='fixture.root'):
    return {'id': identity, 'parent': parent, 'local_matrix': list(IDENTITY),
            'label': identity, 'reverse_front_face': False}


class GenerationMetadata(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.project = Path(self.directory.name) / 'project with spaces'
        self.root = self.project / '.luminumbra-author'
        self.root.mkdir(parents=True)
        self.asset = 'fixture.prefab'
        self.generation = 'a' * 32
        self.descriptor = {'schema': v.PREFAB, 'asset_id': self.asset,
                           'coordinates': 'gltf-rh-y-up-meters', 'runtime_components': False,
                           'nodes': [node(), node('fixture.root', None)],
                           'meshes': {}, 'materials': {}}
        self.publish()

    def publish(self, generation=None, revision=7, descriptor=None):
        generation = generation or self.generation
        descriptor = self.descriptor if descriptor is None else descriptor
        destination = self.root / 'generations' / generation
        destination.mkdir(parents=True, exist_ok=True)
        raw = encoded(descriptor)
        (destination / 'prefab.json').write_bytes(raw)
        # The reader deliberately does not decode mesh/material contents. The
        # host remains the only authority for the full generation contract.
        mesh = 'm-' + 'c' * 32 + '.lmesh'
        (destination / mesh).write_bytes(b'fixture')
        manifest = {'schema': v.GENERATION, 'mode': 'NATIVE', 'job_id': generation,
                    'asset_id': self.asset, 'revision': revision, 'build_identity': 'e' * 64,
                    'input_hashes': {'asset.json': 'f' * 64},
                    'outputs': {mesh: {'bytes': 7, 'sha256': sha(b'fixture')},
                                'prefab.json': {'format': 'PREFAB', 'schema': v.PREFAB,
                                                'bytes': len(raw), 'sha256': sha(raw),
                                                'nodes': len(descriptor['nodes']),
                                                'meshes': len(descriptor['meshes']),
                                                'materials': len(descriptor['materials'])}}}
        (destination / 'manifest.json').write_bytes(encoded(manifest))
        self.pointer(generation)
        return destination, manifest

    def pointer(self, generation):
        temporary = self.root / 'pointer.tmp'
        temporary.write_bytes(encoded({'schema': v.GENERATION, 'job_id': generation,
                                       'build_identity': 'e' * 64}))
        temporary.replace(self.root / 'current.json')

    def read(self, generation='', pins=None):
        return v._read_metadata(self.project, self.asset, generation, pins or {})

    def reader(self, generation=''):
        reader = v.GenerationReader(self.project, self.asset, generation)
        self.addCleanup(reader.close)
        return reader

    def wait(self, reader, predicate):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            status = reader.status()
            if predicate(status):
                return status
            time.sleep(.005)
        self.fail('Reader did not reach expected state: ' + str(reader.status()))

    def test_actual_snapshot_retains_unsorted_parent_ids_and_exact_local_values(self):
        self.descriptor['nodes'][0]['local_matrix'][12:15] = [1.25, -7, 3]
        self.descriptor['nodes'][1]['local_matrix'][0] = -2
        self.publish()
        result = self.read()
        self.assertEqual(result['generation_id'], self.generation)
        self.assertEqual(result['revision'], 7)
        self.assertEqual(result['asset_id'], self.asset)
        self.assertEqual(result['nodes'][0], {'id': 'fixture.child', 'parent': 'fixture.root',
                                             'local_matrix': self.descriptor['nodes'][0]['local_matrix']})
        self.assertEqual(result['manifest_sha256'], sha((self.root / 'generations' /
                                                        self.generation / 'manifest.json').read_bytes()))

    def test_poll_and_status_perform_no_io_and_return_detached_copies(self):
        reader = self.reader()
        self.wait(reader, lambda s: s['state'] == 'ready')
        owner = threading.get_ident()
        original = v._read_regular
        def checked(*args):
            self.assertNotEqual(threading.get_ident(), owner)
            return original(*args)
        with mock.patch.object(v, '_read_regular', side_effect=checked):
            result = reader.poll()
            result['nodes'][0]['local_matrix'][12] = 999
            result['nodes'].clear()
            status = reader.status()
            status['state'] = 'corrupt'
            self.assertEqual(len(reader.poll()['nodes']), 2)
            self.assertEqual(reader.status()['state'], 'ready')
            self.wait(reader, lambda s: s['checks'] > status['checks'])

    def test_worker_advances_generation_without_relabelling_previous_snapshot(self):
        reader = self.reader()
        self.wait(reader, lambda s: s['state'] == 'ready')
        previous = reader.poll()
        descriptor = copy.deepcopy(self.descriptor)
        descriptor['nodes'][0]['local_matrix'][12] = 17
        self.publish('b' * 32, 8, descriptor)
        self.wait(reader, lambda s: s['generation_id'] == 'b' * 32)
        result = reader.poll()
        self.assertEqual((result['revision'], result['nodes'][0]['local_matrix'][12]), (8, 17))
        self.assertEqual((previous['generation_id'], previous['revision'],
                          previous['nodes'][0]['local_matrix'][12]), (self.generation, 7, 0))

    def test_explicit_generation_ignores_current_pointer_and_stays_pinned(self):
        reader = self.reader(self.generation)
        self.wait(reader, lambda s: s['state'] == 'ready')
        before = reader.poll()
        self.publish('b' * 32, 8)
        (self.root / 'current.json').write_bytes(b'malformed')
        checks = reader.status()['checks']
        self.wait(reader, lambda s: s['checks'] > checks)
        self.assertEqual(reader.poll(), before)

    def test_read_failure_clears_result_then_valid_new_generation_recovers(self):
        reader = self.reader()
        self.wait(reader, lambda s: s['state'] == 'ready')
        self.pointer('b' * 32)
        status = self.wait(reader, lambda s: s['state'] == 'error')
        self.assertEqual(status['generation_id'], '')
        self.assertIsNone(reader.poll())
        self.publish('b' * 32, 8)
        self.wait(reader, lambda s: s['state'] == 'ready')
        self.assertEqual(reader.poll()['generation_id'], 'b' * 32)

    def test_immutable_manifest_pin_survives_generation_revisit(self):
        reader = self.reader()
        self.wait(reader, lambda s: s['state'] == 'ready')
        initial = reader.poll()
        self.publish('b' * 32, 8)
        self.wait(reader, lambda s: s['generation_id'] == 'b' * 32)
        self.publish(self.generation, 9)
        status = self.wait(reader, lambda s: s['state'] == 'error')
        self.assertIn('Immutable generation', status['error'])
        self.assertIsNone(reader.poll())
        self.publish(self.generation, 7)
        self.wait(reader, lambda s: s['state'] == 'ready')
        self.assertEqual(reader.poll(), initial)

    def test_manifest_pin_bound_refuses_new_generation_but_admits_existing(self):
        pins = {format(i, '032x'): 'f' * 64 for i in range(v.MAX_PINS)}
        with self.assertRaisesRegex(v.MetadataError, 'session bound'):
            self.read(pins=pins)
        result = self.read()
        pins.pop(next(iter(pins)))
        pins[self.generation] = result['manifest_sha256']
        self.assertEqual(self.read(pins=pins), result)

    def test_changed_pointer_during_read_cannot_publish_mixed_generation(self):
        self.publish('b' * 32, 8)
        self.pointer(self.generation)
        original = v._read_regular
        def switch(path, limit):
            data = original(path, limit)
            if path.name == 'prefab.json':
                self.pointer('b' * 32)
            return data
        with mock.patch.object(v, '_read_regular', side_effect=switch):
            with self.assertRaisesRegex(v.MetadataError, 'Current generation changed'):
                self.read()
        self.assertEqual(self.read()['generation_id'], 'b' * 32)

    def test_changed_manifest_during_read_is_refused(self):
        original = v._read_regular
        def change(path, limit):
            data = original(path, limit)
            if path.name == 'prefab.json':
                target = path.with_name('manifest.json')
                value = json.loads(target.read_bytes())
                value['revision'] += 1
                target.write_bytes(encoded(value))
            return data
        with mock.patch.object(v, '_read_regular', side_effect=change):
            with self.assertRaisesRegex(v.MetadataError, 'manifest changed during'):
                self.read()

    def test_publication_during_open_reader_succeeds_and_invalidates_old_snapshot(self):
        from contextlib import contextmanager
        original = v.open_record_reader
        self.publish('b' * 32, 8)
        self.pointer(self.generation)
        @contextmanager
        def publish_while_open(path):
            with original(path) as stream:
                if path.name == 'current.json':
                    self.pointer('b' * 32)
                yield stream
        with mock.patch.object(v, 'open_record_reader', side_effect=publish_while_open):
            with self.assertRaisesRegex(v.MetadataError, 'Metadata file (changed|was replaced)'):
                self.read()
        self.assertEqual(self.read()['generation_id'], 'b' * 32)

    def test_current_schema_job_and_build_identity_controls(self):
        good = json.loads((self.root / 'current.json').read_bytes())
        for key, value in (('schema', 'future'), ('job_id', '../escape'),
                           ('job_id', 'A'*32), ('build_identity', 'f'*64)):
            with self.subTest(key=key, value=value):
                bad = dict(good, **{key: value})
                (self.root / 'current.json').write_bytes(encoded(bad))
                with self.assertRaises(v.MetadataError): self.read()

    def test_generation_identity_schema_and_revision_controls(self):
        destination, good = self.publish()
        cases = [('schema', 'future'), ('mode', 'MOCK'), ('job_id', 'b'*32),
                 ('asset_id', 'another.asset'), ('revision', True), ('revision', -1),
                 ('revision', 1 << 64), ('build_identity', 'bad')]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                (destination / 'manifest.json').write_bytes(encoded(dict(good, **{key: value})))
                with self.assertRaises(v.MetadataError): self.read()

    def test_descriptor_hash_and_output_byte_count_are_bound_to_exact_bytes(self):
        destination, good = self.publish()
        for change in ('content', 'bytes', 'sha256'):
            with self.subTest(change=change):
                destination, good = self.publish()
                if change == 'content':
                    with (destination / 'prefab.json').open('ab') as stream: stream.write(b' ')
                else:
                    good['outputs']['prefab.json'][change] = 1 if change == 'bytes' else 'f'*64
                    (destination / 'manifest.json').write_bytes(encoded(good))
                with self.assertRaisesRegex(v.MetadataError, 'hash or byte count'): self.read()

    def test_output_names_and_total_bytes_cannot_escape_or_expand_profile(self):
        for name, size in (('../escape', 1), ('C:escape', 1), ('nested/file', 1),
                           ('m-'+'c'*32+'.lmesh', v.MAX_OUTPUT)):
            with self.subTest(name=name):
                destination, manifest = self.publish()
                manifest['outputs'][name] = {'bytes': size, 'sha256': 'f'*64}
                (destination / 'manifest.json').write_bytes(encoded(manifest))
                with self.assertRaises(v.MetadataError): self.read()

    def test_descriptor_schema_asset_and_coordinate_controls(self):
        for key, value in (('schema', 'future'), ('asset_id', 'another.asset'),
                           ('coordinates', 'blender-z-up'), ('runtime_components', True)):
            with self.subTest(key=key):
                bad = copy.deepcopy(self.descriptor)
                bad[key] = value
                self.publish(descriptor=bad)
                with self.assertRaises(v.MetadataError): self.read()

    def test_node_graph_rejects_duplicate_missing_self_and_multi_node_cycles(self):
        cases = [[node(), node()], [node()], [node('fixture.root', 'fixture.root')],
                 [node('fixture.a', 'fixture.b'), node('fixture.b', 'fixture.a')],
                 [node('bad/id', None)], [node('fixture.a', False)]]
        for nodes in cases:
            with self.subTest(nodes=nodes):
                bad = copy.deepcopy(self.descriptor)
                bad['nodes'] = nodes
                self.publish(descriptor=bad)
                with self.assertRaises(v.MetadataError): self.read()

    def test_complete_1024_node_chain_is_iterative_and_1025_is_refused(self):
        descriptor = copy.deepcopy(self.descriptor)
        descriptor['nodes'] = [node('n'+str(i), 'n'+str(i+1) if i<1023 else None)
                               for i in range(1024)]
        self.publish(descriptor=descriptor)
        self.assertEqual(len(self.read()['nodes']), 1024)
        descriptor['nodes'].append(node('overflow', None))
        self.publish(descriptor=descriptor)
        with self.assertRaises(v.MetadataError): self.read()

    def test_bad_matrix_shape_nonaffine_singular_bool_and_overflow_are_refused(self):
        matrices = [IDENTITY[:15], [0]*16]
        for index, value in ((3, .01), (15, 0), (0, True), (0, 1e31)):
            matrix = list(IDENTITY); matrix[index] = value; matrices.append(matrix)
        for matrix in matrices:
            with self.subTest(matrix=matrix):
                bad = copy.deepcopy(self.descriptor)
                bad['nodes'][0]['local_matrix'] = matrix
                self.publish(descriptor=bad)
                with self.assertRaises(v.MetadataError): self.read()

    def test_duplicate_fields_nonfinite_and_oversized_documents_are_refused(self):
        for raw in (b'{"schema":1,"schema":2}', b'{"value":NaN}', b'{"value":1e999}',
                    b'{}' + b' ' * 65535):
            with self.subTest(raw=raw[:64]):
                (self.root / 'current.json').write_bytes(raw)
                with self.assertRaises(ValueError): self.read()

    def test_symlink_files_generation_directories_and_project_ancestors_refused(self):
        destination = self.root / 'generations' / self.generation
        for path in (self.root / 'current.json', destination / 'manifest.json',
                     destination / 'prefab.json', destination, self.project):
            with self.subTest(path=path):
                moved = path.with_name(path.name + '-real')
                path.rename(moved)
                try:
                    try:
                        path.symlink_to(moved, target_is_directory=moved.is_dir())
                    except OSError as error:
                        if getattr(error, 'winerror', None) == 1314:
                            self.skipTest('Windows symlink creation requires developer mode or privilege')
                        raise
                    with self.assertRaisesRegex(v.MetadataError, 'symlinks or reparse'):
                        self.read()
                finally:
                    path.unlink(missing_ok=True)
                    moved.rename(path)

    def test_reparse_attribute_and_nonregular_nodes_refused_without_windows(self):
        for mode, attributes in ((stat.S_IFREG, 0x400), (stat.S_IFDIR, 0x400),
                                 (stat.S_IFIFO, 0), (stat.S_IFSOCK, 0)):
            info = SimpleNamespace(st_mode=mode, st_file_attributes=attributes)
            path = mock.Mock()
            path.lstat.return_value = info
            with self.subTest(mode=mode, attributes=attributes):
                with self.assertRaises(v.MetadataError): v._checked_stat(path, False)

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'POSIX FIFO control')
    def test_fifo_current_file_is_refused_without_blocking(self):
        path = self.root / 'current.json'
        path.unlink(); os.mkfifo(path)
        started = time.monotonic()
        with self.assertRaises(v.MetadataError): self.read()
        self.assertLess(time.monotonic() - started, .5)

    def test_invalid_constructor_identifiers_do_not_start_threads(self):
        with mock.patch.object(v.threading.Thread, 'start') as start:
            for asset, generation in (('../asset', ''), ('a', '../gen'), ('a', None)):
                with self.subTest(asset=asset, generation=generation):
                    with self.assertRaises(v.MetadataError):
                        v.GenerationReader(self.project, asset, generation)
            with self.assertRaises(v.MetadataError):
                v.GenerationReader(self.project / '..', self.asset)
            start.assert_not_called()

    def test_thread_start_failure_propagates_without_leaking_worker(self):
        with mock.patch.object(v.threading.Thread, 'start', side_effect=RuntimeError('injected')):
            with self.assertRaisesRegex(RuntimeError, 'injected'):
                v.GenerationReader(self.project, self.asset)

    def test_close_clears_result_is_idempotent_and_stops_idle_worker(self):
        reader = self.reader()
        self.wait(reader, lambda s: s['state'] == 'ready')
        reader.close(); reader.close()
        self.assertFalse(reader._thread.is_alive())
        self.assertIsNone(reader.poll())
        self.assertEqual(reader.status()['state'], 'closed')

    def test_close_is_bounded_during_blocked_io_and_cannot_publish_late_result(self):
        entered, release = threading.Event(), threading.Event()
        original = v._read_metadata
        def blocked(*args):
            entered.set()
            release.wait(3)
            return original(*args)
        with mock.patch.object(v, '_read_metadata', side_effect=blocked):
            reader = self.reader()
            try:
                self.assertTrue(entered.wait(2))
                started = time.monotonic()
                reader.close()
                self.assertLess(time.monotonic() - started, .6)
                self.assertEqual(reader.status()['state'], 'closed')
            finally:
                release.set()
                reader._thread.join(2)
            self.assertFalse(reader._thread.is_alive())
            self.assertIsNone(reader.poll())
            self.assertEqual(reader.status()['state'], 'closed')


if __name__ == '__main__':
    unittest.main()
