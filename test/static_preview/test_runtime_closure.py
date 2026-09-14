"""Runtime path/hash joins; Windows also snapshots an actual owned child."""
import copy
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import installed_viewport as probe


class RuntimeClosureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.sdk = self.root / 'sdk with spaces'
        self.bin = self.sdk / 'bin'
        self.bin.mkdir(parents=True)
        self.host = self.bin / 'luminumbra_static_preview.exe'
        self.names = (self.host.name, 'libluminumbra_render_static.dll', *probe.SDK_RUNTIME_DLLS)
        for name in self.names:
            (self.bin / name).write_bytes(('fixture bytes for ' + name).encode())
        self.before = probe.inventory(self.sdk)
        self.loaded = {name: [str(self.bin / name)] for name in self.names}
        self.checks = []

    def qualify(self):
        def check(name, condition):
            self.checks.append((name, bool(condition)))
            probe.wire.require(condition, name)
        return probe.qualify_sdk_runtime(self.host, self.sdk, self.before, self.loaded, check)

    def test_all_five_loaded_modules_bind_to_prelaunch_roster(self):
        result = self.qualify()
        self.assertEqual(set(result), set(self.names))
        self.assertEqual(len(self.checks), 5)
        self.assertTrue(all(value for _, value in self.checks))
        for row in result.values():
            self.assertEqual(row['sha256'], self.before[row['relative_path']])
            self.assertEqual(Path(row['path']).parent, self.bin)

    def test_identical_runtime_bytes_outside_sdk_are_refused(self):
        name = probe.SDK_RUNTIME_DLLS[0]
        external = self.root / name
        external.write_bytes((self.bin / name).read_bytes())
        self.loaded[name] = [str(external)]
        with self.assertRaisesRegex(probe.wire.Refusal, 'outside installed bin'):
            self.qualify()

    def test_runtime_changed_after_prelaunch_inventory_is_refused(self):
        (self.bin / probe.SDK_RUNTIME_DLLS[1]).write_bytes(b'changed after inventory')
        with self.assertRaisesRegex(probe.wire.Refusal, 'loaded SDK runtime pin'):
            self.qualify()

    def test_unlisted_loaded_file_is_refused(self):
        del self.before['bin/' + probe.SDK_RUNTIME_DLLS[2]]
        with self.assertRaisesRegex(probe.wire.Refusal, 'loaded SDK runtime pin'):
            self.qualify()

    def test_case_ambiguous_inventory_is_refused(self):
        key = 'bin/' + probe.SDK_RUNTIME_DLLS[0]
        self.before[key.upper()] = self.before[key]
        with self.assertRaisesRegex(probe.wire.Refusal, 'loaded SDK runtime pin'):
            self.qualify()

    def test_missing_or_duplicate_module_is_refused(self):
        original = copy.deepcopy(self.loaded)
        for count in (0, 2):
            with self.subTest(count=count):
                self.loaded = copy.deepcopy(original)
                self.loaded[probe.SDK_RUNTIME_DLLS[0]] *= count
                with self.assertRaisesRegex(probe.wire.Refusal, 'Missing or ambiguous'):
                    self.qualify()

    def test_relative_loaded_path_is_refused(self):
        self.loaded[self.host.name] = [self.host.name]
        with self.assertRaisesRegex(probe.wire.Refusal, 'absolute file'):
            self.qualify()

    def test_linked_runtime_is_refused_even_with_matching_bytes(self):
        name = probe.SDK_RUNTIME_DLLS[0]
        target = self.root / name
        path = self.bin / name
        path.rename(target)
        try:
            path.symlink_to(target)
        except OSError as error:
            self.skipTest('symlink creation unavailable: ' + str(error))
        with self.assertRaisesRegex(probe.wire.Refusal, 'Reparse path'):
            self.qualify()

    @unittest.skipUnless(os.name == 'nt', 'requires actual Windows Toolhelp APIs')
    def test_windows_snapshot_enumerates_only_owned_live_child(self):
        child = subprocess.Popen([sys.executable, '-c',
                                  'import sys,time; print("ready",flush=True); time.sleep(20)'],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        try:
            # A separate reader avoids a blocking readiness read on a failed child.
            import queue
            import threading
            ready = queue.Queue(maxsize=1)
            reader = threading.Thread(target=lambda: ready.put(child.stdout.readline()), daemon=True)
            reader.start()
            self.assertEqual(ready.get(timeout=5), 'ready\n')
            modules = probe.windows_loaded_modules(child)
            actual = modules[Path(sys.executable).name.casefold()]
            self.assertEqual(len(actual), 1)
            self.assertTrue(Path(actual[0]).samefile(sys.executable))
            self.assertIn('kernel32.dll', modules)
            self.assertIsNone(child.poll())
        finally:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
            child.stdout.close()
        with self.assertRaisesRegex(probe.wire.Refusal, 'live owned Windows host'):
            probe.windows_loaded_modules(child)


if __name__ == '__main__':
    unittest.main()
