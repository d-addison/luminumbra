"""Actual package and held-reader publication contracts, without Blender/GPU."""
import hashlib
from contextlib import contextmanager
import importlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
AUTHORING = ROOT / 'tools/blender/authoring'
sys.path.insert(0, str(AUTHORING / 'service'))
from luminumbra_author import contracts, file_io


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generation_module():
    name = 'shared_identity_extension'
    if name not in sys.modules:
        package = types.ModuleType(name)
        package.__path__ = [str(AUTHORING / 'extension'), str(AUTHORING)]
        sys.modules[name] = package
    return importlib.import_module(name + '.viewport_generation')


def change_metadata_time(path, previous):
    if os.name != 'nt':
        os.chmod(path, path.stat().st_mode ^ stat.S_IWUSR)
        return
    # Actual metadata-only Windows mutation: preserve creation/access/write
    # times and bytes, and set ChangeTime through a separately owned handle.
    import ctypes
    from ctypes import wintypes as w
    class Basic(ctypes.Structure):
        _fields_ = [('creation', ctypes.c_int64), ('access', ctypes.c_int64),
                    ('write', ctypes.c_int64), ('change', ctypes.c_int64), ('attributes', w.DWORD)]
    kernel = file_io._windows_reader_api()
    handle = kernel.CreateFileW(str(path), 0x180, 7, None, 3, 0x00200000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        value = Basic(change=previous.changed + 10_000_000)
        if not kernel.SetFileInformationByHandle(handle, 0, ctypes.byref(value), ctypes.sizeof(value)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)


class SharedFileIOTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='shared-publication-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.target = self.root / 'current.json'
        self.old, self.new = {'revision': 1}, {'revision': 2}
        contracts.atomic_json(self.target, self.old)

    def test_actual_held_service_reader_keeps_old_bytes_and_new_reader_gets_replacement(self):
        before = self.target.read_bytes()
        with file_io.open_regular_reader(self.target) as reader:
            self.assertFalse(os.get_inheritable(reader.fileno()))
            contracts.atomic_json(self.target, self.new)
            self.assertEqual(reader.read(), before)
            self.assertEqual(contracts.read_json(self.target), self.new)
        self.assertEqual(list(self.root.glob('.tmp-*')), [])

    def test_published_metadata_path_and_reader_identity_agree(self):
        # Publication changes metadata time even when the creation timestamp
        # survives replacement. Ordinary write-only fixtures hid this defect.
        contracts.atomic_json(self.target, self.new)
        identity = file_io.file_identity(self.target)
        with file_io.open_regular_reader(self.target) as held:
            self.assertEqual(identity, file_io.reader_identity(held))
            if os.name == 'nt':
                birth = os.fstat(held.fileno()).st_birthtime_ns // 100 + 116444736000000000
                self.assertNotEqual(identity.changed, birth)
                if 'GCC UCRT' in sys.version:
                    # This is the exact old failing join on the qualified
                    # UCRT interpreter, retained as an actual negative control.
                    self.assertNotEqual(self.target.lstat().st_ctime_ns,
                                        os.fstat(held.fileno()).st_ctime_ns)
        self.assertEqual(generation_module()._read_regular(self.target, 1024), self.target.read_bytes())

    def replace_same_bytes_and_mtime(self):
        before = self.target.stat()
        temporary = self.target.with_name('replacement.tmp')
        temporary.write_bytes(self.target.read_bytes())
        os.utime(temporary, ns=(before.st_atime_ns, before.st_mtime_ns))
        file_io.replace_file(temporary, self.target)

    def test_identical_same_size_replacement_between_baseline_and_open_is_refused(self):
        generation = generation_module()
        original = generation.open_record_reader
        @contextmanager
        def swapped(path):
            self.replace_same_bytes_and_mtime()
            with original(path) as held:
                yield held
        with mock.patch.object(generation, 'open_record_reader', side_effect=swapped):
            with self.assertRaisesRegex(generation.MetadataError, 'changed while opening'):
                generation._read_regular(self.target, 1024)

    def test_identical_same_size_replacement_during_read_is_refused(self):
        generation = generation_module()
        original = generation.open_record_reader
        @contextmanager
        def replaced(path):
            with original(path) as held:
                def read(limit):
                    value = held.read(limit)
                    self.replace_same_bytes_and_mtime()
                    return value
                yield types.SimpleNamespace(fileno=held.fileno, read=read)
        with mock.patch.object(generation, 'open_record_reader', side_effect=replaced):
            with self.assertRaisesRegex(generation.MetadataError, '(changed|replaced) while reading'):
                generation._read_regular(self.target, 1024)

    def test_metadata_only_change_during_read_is_refused(self):
        generation = generation_module()
        original = generation.open_record_reader
        before = file_io.file_identity(self.target)
        raw = self.target.read_bytes()
        @contextmanager
        def changed(path):
            with original(path) as held:
                def read(limit):
                    value = held.read(limit)
                    change_metadata_time(path, before)
                    return value
                yield types.SimpleNamespace(fileno=held.fileno, read=read)
        with mock.patch.object(generation, 'open_record_reader', side_effect=changed):
            with self.assertRaisesRegex(generation.MetadataError, 'changed while reading'):
                generation._read_regular(self.target, 1024)
        after = file_io.file_identity(self.target)
        self.assertEqual(before[:4], after[:4])
        if os.name == 'nt':
            self.assertEqual(before.attributes, after.attributes)
            self.assertNotEqual(before.changed, after.changed)
        else:
            # A coarse filesystem clock can give chmod the same ctime tick;
            # the complete identity still detects the mode change itself.
            self.assertNotEqual(before.attributes, after.attributes)
        self.assertEqual(self.target.read_bytes(), raw)

    def test_failed_publication_keeps_destination_and_cleans_complete_temporary(self):
        before = self.target.read_bytes()
        with mock.patch.object(file_io, 'replace_file', side_effect=OSError('injected replacement failure')):
            with self.assertRaisesRegex(OSError, 'injected'):
                contracts.atomic_json(self.target, self.new)
        self.assertEqual(self.target.read_bytes(), before)
        self.assertEqual(list(self.root.glob('.tmp-*')), [])

    def test_service_fsync_occurs_on_complete_data_before_publication(self):
        events = []
        fsync, replace = os.fsync, file_io.replace_file
        def synced(fd):
            events.append('fsync')
            return fsync(fd)
        def published(temporary, path):
            self.assertEqual(events, ['fsync'])
            self.assertEqual(temporary.read_bytes(), contracts.canonical(self.new) + b'\n')
            events.append('replace')
            return replace(temporary, path)
        with mock.patch.object(os, 'fsync', side_effect=synced), mock.patch.object(file_io, 'replace_file', side_effect=published):
            contracts.atomic_json(self.target, self.new)
        self.assertEqual(events, ['fsync', 'replace'])

    def test_same_directory_and_regular_file_publication_are_required(self):
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        temporary = elsewhere / 'new'
        temporary.write_bytes(b'complete')
        with self.assertRaisesRegex(ValueError, 'same-directory'):
            file_io.replace_file(temporary, self.target)
        self.assertTrue(temporary.exists())
        with self.assertRaisesRegex(ValueError, 'regular file'):
            file_io.replace_file(elsewhere, self.target)
        with self.assertRaisesRegex(ValueError, 'regular file'):
            file_io.open_regular_reader(elsewhere)

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'POSIX FIFO control')
    def test_fifo_reader_is_rejected_without_blocking(self):
        fifo = self.root / 'fifo'
        os.mkfifo(fifo)
        started = time.monotonic()
        with self.assertRaisesRegex(ValueError, 'regular file'):
            file_io.open_regular_reader(fifo)
        self.assertLess(time.monotonic() - started, .5)

    def test_linked_reader_and_publication_are_rejected(self):
        link = self.root / 'link'
        try:
            link.symlink_to(self.target)
        except OSError as error:
            if getattr(error, 'winerror', None) == 1314:
                self.skipTest('Windows symlink creation requires developer mode or privilege')
            raise
        with self.assertRaisesRegex(ValueError, 'Reparse'):
            file_io.open_regular_reader(link)
        with self.assertRaises(contracts.Refusal):
            contracts.atomic_json(link, self.new)
        self.assertEqual(contracts.read_json(self.target), self.old)

    def test_wire_module_uses_canonical_source_and_preserves_its_refusal_type(self):
        wire = load(AUTHORING / 'viewport/protocol.py', 'shared_wire_source')
        self.assertEqual(wire.file_io_path(), AUTHORING / 'service/luminumbra_author/file_io.py')
        with self.assertRaises(wire.Refusal):
            wire.open_record_reader(self.root)
        with self.assertRaises(wire.Refusal):
            wire.read_record_file(self.target, wire.CAPACITY + 1)

    def test_packaged_protocol_refuses_missing_sibling_and_never_uses_ambient_module(self):
        path = self.root / 'protocol.py'
        shutil.copyfile(AUTHORING / 'viewport/protocol.py', path)
        fake = types.ModuleType('file_io')
        previous = sys.modules.get('file_io')
        sys.modules['file_io'] = fake
        try:
            with self.assertRaisesRegex(ValueError, 'module is missing'):
                load(path, 'shared_wire_missing')
        finally:
            if previous is None:
                sys.modules.pop('file_io', None)
            else:
                sys.modules['file_io'] = previous
        (self.root / 'file_io.py').write_text('raise ImportError("inside chosen helper")\n')
        with self.assertRaisesRegex(ImportError, 'inside chosen helper'):
            load(path, 'shared_wire_broken')

    def test_service_build_identity_includes_actual_shared_helper(self):
        # Capture the identity construction under a real service job in the
        # existing fixture; the test compiler writes a deterministic mesh.
        fixture_module = load(AUTHORING / 'service/tests/test_service.py', 'shared_service_fixture')
        fixture = fixture_module.Project()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        builder = fixture.open()
        job = fixture.success(builder)
        self.assertEqual(job['service_hashes']['file_io.py'], hashlib.sha256(
            (AUTHORING / 'service/luminumbra_author/file_io.py').read_bytes()).hexdigest())

    def test_changed_packaged_helper_invalidates_actual_client_session(self):
        from test_client import ClientTests, state, wait_for
        fixture = ClientTests()
        fixture.setUp()
        self.addCleanup(fixture.tearDown)
        client = fixture.client()
        client.submit(state())
        wait_for(client.poll)
        with (fixture.broker_root / 'file_io.py').open('ab') as stream:
            stream.write(b'\n# changed pinned helper\n')
        self.assertTrue(client.close(wait=True, timeout=5))
        self.assertEqual(client.status['state'], 'failed')
        self.assertIn('broker/interpreter changed', client.status['error'])
        self.assertEqual(client.session_receipt()['status'], 'failed')

    def test_packaged_service_and_extension_publish_across_actual_held_generation_reader(self):
        extension = load(AUTHORING / 'package_extension.py', 'shared_extension_package')
        packaging = load(AUTHORING / 'service/package.py', 'shared_service_package')
        archive = packaging.build(self.root / 'service.pyz')
        bundle = extension.build(self.root / 'extension.zip')
        canonical = (AUTHORING / 'service/luminumbra_author/file_io.py').read_bytes()
        with zipfile.ZipFile(archive) as app, zipfile.ZipFile(bundle) as plugin:
            self.assertEqual(app.read('luminumbra_author/file_io.py'), canonical)
            self.assertEqual(plugin.read('viewport/file_io.py'), canonical)
            plugin.extractall(self.root / 'extension')
        package = types.ModuleType('shared_installed_extension')
        package.__path__ = [str(self.root / 'extension')]
        sys.modules[package.__name__] = package
        generation = importlib.import_module(package.__name__ + '.viewport_generation')
        fixtures = load(AUTHORING / 'extension_tests/test_viewport_generation.py', 'shared_generation_fixture')
        fixture = fixtures.GenerationMetadata()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.publish('b' * 32, 8)
        path = fixture.root / 'current.json'
        desired = json.loads(path.read_bytes())
        fixture.pointer(fixture.generation)
        previous = path.read_bytes()
        code = ('import json,sys;sys.path.insert(0,sys.argv[1]);'
                'from luminumbra_author.contracts import atomic_json;'
                'atomic_json(sys.argv[2],json.loads(sys.argv[3]))')
        with generation.open_record_reader(path) as held:
            started = time.monotonic()
            result = subprocess.run([sys.executable, '-I', '-B', '-c', code, str(archive), str(path), json.dumps(desired)],
                                    cwd=self.root, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertLess(time.monotonic() - started, 5)
            self.assertEqual(held.read(), previous)
            actual = generation._read_metadata(fixture.project, fixture.asset, '', {})
            self.assertEqual((actual['generation_id'], actual['revision']), ('b' * 32, 8))
        self.assertEqual(list(fixture.root.glob('.tmp-*')), [])

    @unittest.skipUnless(os.name == 'nt', 'Windows CRT sharing negative control')
    def test_real_windows_crt_reader_refuses_publication_without_partial_update(self):
        before = self.target.read_bytes()
        with self.target.open('rb') as held:
            with self.assertRaises(OSError) as caught:
                contracts.atomic_json(self.target, self.new)
            self.assertIn(caught.exception.winerror, (5, 32))
            self.assertEqual(held.read(), before)
        self.assertEqual(self.target.read_bytes(), before)
        self.assertEqual(list(self.root.glob('.tmp-*')), [])


if __name__ == '__main__':
    unittest.main()
