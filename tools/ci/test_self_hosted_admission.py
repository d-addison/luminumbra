#!/usr/bin/env python3
"""Worker admission policy and process regressions; no host qualification claimed."""

from contextlib import ExitStack
from dataclasses import replace
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("self_hosted_admission", Path(__file__).with_name("self_hosted_admission.py"))
ADMISSION = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ADMISSION
SPEC.loader.exec_module(ADMISSION)


class Channel:
    family = socket.AF_UNIX
    type = socket.SOCK_STREAM

    def __init__(self, data, credentials=(123, 0, 0), chunk=65536):
        self.data = data
        self.credentials = struct.pack("3i", *credentials)
        self.chunk = chunk
        self.timeouts = []

    def getsockopt(self, *_args):
        return self.credentials

    def settimeout(self, value):
        self.timeouts.append(value)

    def recv(self, size):
        block = self.data[:min(size, self.chunk)]
        self.data = self.data[len(block):]
        return block


class AdmissionTests(unittest.TestCase):
    def setUp(self):
        self.plan = ADMISSION.LaunchPlan(("/host/worker",), 123, 0, 2001, 2001,
                                        "12345678-1234-1234-1234-123456789abc", 7, 2, "true")
        self.number = 8
        self.assignment = dict(job_id=self.plan.job_id, run_id=7, run_attempt=2,
                               repository=ADMISSION.REPOSITORY, head_repository=ADMISSION.REPOSITORY,
                               base_repository=ADMISSION.REPOSITORY, event="pull_request", revision="a" * 40,
                               head_revision="b" * 40, ref=f"refs/pull/{self.number}/merge", pull_request=self.number, cancelled=False)
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(ADMISSION.socket, "SO_PEERCRED", 17, create=True))

    def data(self):
        return json.dumps(self.assignment, separators=(",", ":")).encode("utf-8")

    def channel(self, data=None, **kwargs):
        data = self.data() if data is None else data
        return Channel(struct.pack("!I", len(data)) + data, **kwargs)

    def receive(self, channel=None, deadline=12):
        with patch.object(ADMISSION.time, "monotonic", return_value=10):
            return ADMISSION.receive_assignment(channel or self.channel(), self.plan, deadline)

    def test_same_repository_pull_request_and_push(self):
        self.assertEqual(self.receive(self.channel(chunk=1)), self.data())
        self.assignment.update(event="push", ref="refs/heads/devel", pull_request=None, head_revision="a" * 40)
        self.assertEqual(self.receive(), self.data())
        self.assignment["ref"] = "refs/tags/v0.3.1"
        self.assertEqual(self.receive(), self.data())

    def test_channel_accepts_only_one_assignment(self):
        channel = self.channel()
        self.assertEqual(self.receive(channel), self.data())
        with self.assertRaises(ADMISSION.Denied):
            self.receive(channel)

    def test_frame_bound_refuses_before_reading_body(self):
        data = self.data()
        maximum = data + b" " * (ADMISSION.MAX_JSON - len(data))
        self.assertEqual(self.receive(self.channel(maximum)), maximum)
        for size in (0, ADMISSION.MAX_JSON + 1, 2**32 - 1):
            channel = Channel(struct.pack("!I", size) + b"unread-body")
            with self.subTest(size=size), self.assertRaises(ADMISSION.Denied):
                self.receive(channel)
            self.assertEqual(channel.data, b"unread-body")

    def test_every_missing_and_unknown_field_refuses(self):
        for key in self.assignment:
            partial = dict(self.assignment)
            del partial[key]
            with self.subTest(key=key), self.assertRaises(ADMISSION.Denied):
                ADMISSION.validate_assignment(json.dumps(partial).encode(), self.plan)
        with self.assertRaisesRegex(ADMISSION.Denied, "unknown"):
            self.receive(self.channel(self.data()[:-1] + b',"trusted":true}'))

    def test_external_metadata_and_binding_refuse(self):
        changes = {
            "job_id": ["", "00000000-0000-0000-0000-000000000000", "another-job"],
            "run_id": [True, 0, -1, 8, 2**63, "7"], "run_attempt": [True, 0, 1, "2"],
            "repository": [None, "", "D-Addison/luminumbra", "fork/repo"],
            "head_repository": [None, "", "fork/repo"], "base_repository": [None, "fork/repo"],
            "revision": [None, "a", "0" * 40, "A" * 40], "head_revision": [None, "b", "0" * 40],
            "event": [None, "", "pull_request_target", "workflow_dispatch", "schedule", "workflow_run", "unknown"],
            "cancelled": [True, None, 0, "false"], "pull_request": [True, 0, 9, None],
            "ref": [None, "", f"refs/pull/{self.number}/head", f"refs/pull/{self.number + 1}/merge"],
        }
        for key, values in changes.items():
            for value in values:
                changed = dict(self.assignment, **{key: value})
                with self.subTest(key=key, value=value), self.assertRaises(ADMISSION.Denied):
                    ADMISSION.validate_assignment(json.dumps(changed).encode(), self.plan)
        # Equality with a bool must not satisfy a trusted numeric binding.
        for key in ("run_id", "run_attempt"):
            changed = dict(self.assignment, **{key: True})
            with self.assertRaises(ADMISSION.Denied):
                ADMISSION.validate_assignment(json.dumps(changed).encode(), replace(self.plan, **{key: 1}))
        for job_id in ("", "00000000-0000-0000-0000-000000000000", "invalid"):
            with self.assertRaises(ADMISSION.Denied):
                ADMISSION.validate_assignment(json.dumps(dict(self.assignment, job_id=job_id)).encode(),
                                                replace(self.plan, job_id=job_id))

    def test_invalid_push_cross_field_identity_refuses(self):
        self.assignment.update(event="push", ref="refs/heads/devel", pull_request=None, head_revision="a" * 40)
        for key, values in {"ref": ["devel", "refs/heads/", "refs/heads/a..b", "refs/heads/a/", "refs/heads/a.",
                                            "refs/heads/a\n", "refs/heads/a//b", "refs/heads/a/.b", "refs/heads/a.lock/b",
                                            "refs/heads/" + "x" * 257],
                            "head_revision": ["b" * 40], "pull_request": [0, 8]}.items():
            for value in values:
                with self.subTest(key=key, value=value), self.assertRaises(ADMISSION.Denied):
                    ADMISSION.validate_assignment(json.dumps(dict(self.assignment, **{key: value})).encode(), self.plan)

    def test_malformed_duplicate_partial_and_oversized_input_refuses(self):
        for data in (b"", b"{", b"[]", b"null", b"\xff", b'{"x":1,"x":2}',
                     b'{"x":NaN}', b'{"x":Infinity}', b"[" * 1500, b" " * (ADMISSION.MAX_JSON + 1)):
            with self.subTest(data=data[:30]), self.assertRaises(ADMISSION.Denied):
                self.receive(self.channel(data))
        for frame in (b"", b"\0", struct.pack("!I", 8) + b"{", struct.pack("!I", 0),
                      struct.pack("!I", ADMISSION.MAX_JSON + 1), self.channel().data + b"x"):
            with self.subTest(frame=frame[:20]), self.assertRaises(ADMISSION.Denied):
                self.receive(Channel(frame))
        with self.assertRaises(ADMISSION.Denied):
            self.receive(self.channel(b" " * ADMISSION.MAX_JSON))

    def test_peer_authentication_transport_failure_and_deadline_refuse(self):
        for credentials in ((123, 2001, 0), (999, 0, 0), (123, 1, 0), (123, 0, 2001)):
            channel = self.channel(credentials=credentials)
            with self.assertRaisesRegex(ADMISSION.Denied, "unauthenticated"):
                self.receive(channel)
            self.assertEqual(len(channel.timeouts), 0)
        for attribute, value in (("family", socket.AF_INET), ("type", socket.SOCK_DGRAM)):
            channel = self.channel()
            setattr(channel, attribute, value)
            with self.assertRaises(ADMISSION.Denied):
                self.receive(channel)
        for deadline in (10, 9, float("nan"), float("inf")):
            with self.assertRaises(ADMISSION.Denied):
                self.receive(deadline=deadline)
        for error in (TimeoutError(), PermissionError(), ConnectionResetError()):
            with patch.object(Channel, "recv", side_effect=error), self.assertRaises(OSError):
                self.receive()
        with patch.object(Channel, "getsockopt", side_effect=PermissionError()), self.assertRaises(OSError):
            self.receive()
        with patch.object(Channel, "getsockopt", return_value=b""), self.assertRaises(struct.error):
            self.receive()
        channel = self.channel(chunk=1)
        with patch.object(ADMISSION.time, "monotonic", side_effect=[10, 11, 12]), self.assertRaises(ADMISSION.Denied):
            ADMISSION.receive_assignment(channel, self.plan, 12)
        self.assertEqual(channel.timeouts, [2, 1])

    def test_kill_switch_and_host_launch_policy_refuse(self):
        for enabled in ("", "false", "0", "true ", True, None):
            with self.subTest(enabled=enabled), self.assertRaises(ADMISSION.Denied):
                ADMISSION.validate_plan(replace(self.plan, enabled=enabled))
        ADMISSION.validate_plan(replace(self.plan, enabled="TRUE"))
        changes = {"listener_pid": [0, True], "listener_uid": [-1, True, 2**31],
                   "worker_uid": [0, True, 2**31], "worker_gid": [0, True, 2**31],
                   "job_id": [None, "", "invalid", "00000000-0000-0000-0000-000000000000"],
                   "run_id": [True, 0, -1, 2**63, "7"], "run_attempt": [True, 0, 2**63, "2"],
                   "command": [(), ("worker",), ("/host/../worker",), ("/host//worker",), ("//host/worker",), ("/host/worker", ""),
                               ("/host/worker", "a\0b"), ("/host/worker", "x" * 4097), ("/worker",) * 65]}
        for key, values in changes.items():
            for value in values:
                with self.subTest(key=key, value=str(value)[:40]), self.assertRaises(ADMISSION.Denied):
                    ADMISSION.validate_plan(replace(self.plan, **{key: value}))
        with self.assertRaises(ADMISSION.Denied):
            ADMISSION.validate_plan(replace(self.plan, listener_uid=self.plan.worker_uid))

    def test_unsupported_platform_and_unprivileged_supervisor_refuse(self):
        for platform, uid in (("win32", 0), ("darwin", 0), ("linux", 1000)):
            with patch.object(ADMISSION.sys, "platform", platform), \
                    patch.object(ADMISSION.os, "geteuid", return_value=uid, create=True), \
                    patch.object(ADMISSION.subprocess, "Popen") as start, self.assertRaises(ADMISSION.Denied):
                ADMISSION.launch(self.channel(), self.plan)
            start.assert_not_called()

    def host_fixture(self):
        self.stack.enter_context(patch.object(ADMISSION.sys, "platform", "linux"))
        self.stack.enter_context(patch.object(ADMISSION.os, "geteuid", return_value=0, create=True))
        self.stack.enter_context(patch.object(ADMISSION, "open_worker", side_effect=lambda _: os.open(__file__, os.O_RDONLY)))
        file = self.stack.enter_context(tempfile.TemporaryFile())
        self.stack.enter_context(patch.object(ADMISSION.os, "memfd_create", side_effect=lambda *_: os.dup(file.fileno()), create=True))
        for name in ("MFD_CLOEXEC", "MFD_ALLOW_SEALING"):
            self.stack.enter_context(patch.object(ADMISSION.os, name, 1, create=True))
        seals = SimpleNamespace(fcntl=Mock(), F_ADD_SEALS=1, F_SEAL_WRITE=2, F_SEAL_GROW=4,
                                F_SEAL_SHRINK=8, F_SEAL_SEAL=16)
        self.stack.enter_context(patch.dict(sys.modules, fcntl=seals))
        return seals

    def test_denial_never_creates_worker_or_status_and_startup_processes(self):
        self.host_fixture()
        with tempfile.TemporaryDirectory() as directory:
            sentinels = [Path(directory) / name for name in ("always", "failure", "cancelled")]
            command = ("/host/worker", "-I", "-B", "-c",
                       "import subprocess,sys; from pathlib import Path; "
                       "status=sys.argv[2]; conditions={'always':True,'failure':status=='failure','cancelled':status=='cancelled'}; "
                       "[subprocess.run([sys.executable,'-I','-B','-c',"
                       "'from pathlib import Path; import sys; Path(sys.argv[1]).touch()',"
                       "str(Path(sys.argv[1])/name)],check=True) for name,enabled in conditions.items() if enabled]",
                       directory, "failure")
            plan = replace(self.plan, command=command)
            original = subprocess.Popen

            def spawn(args, **kwargs):
                # Credential transitions and descriptor execution are modelled
                # in this interpreter; the sentinel child is a real process.
                for key in ("user", "group", "extra_groups", "pass_fds", "executable"):
                    kwargs.pop(key)
                return original((sys.executable, *args[1:]), **kwargs)

            with patch.object(ADMISSION.subprocess, "Popen", side_effect=spawn) as start:
                for cancelled in (False, True):
                    self.assignment.update(head_repository="fork/repo", cancelled=cancelled)
                    with self.assertRaises(ADMISSION.Denied):
                        ADMISSION.launch(self.channel(), plan)
                    self.assertTrue(all(not sentinel.exists() for sentinel in sentinels))
                start.assert_not_called()
                self.assignment.update(head_repository=ADMISSION.REPOSITORY, cancelled=False)
                process = ADMISSION.launch(self.channel(), plan)
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual([sentinel.exists() for sentinel in sentinels], [True, True, False])

    def test_admitted_worker_receives_exact_bytes_and_fresh_environment(self):
        seals = self.host_fixture()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "worker.json"
            body = "import json,os,sys; from pathlib import Path; Path(" + repr(str(output)) + ").write_text(json.dumps([dict(os.environ),sys.stdin.read()]))"
            plan = replace(self.plan, command=("/host/worker", "-I", "-B", "-c", body))
            original = subprocess.Popen

            def spawn(args, **kwargs):
                self.assertEqual(kwargs["env"], {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"})
                self.assertEqual((kwargs.pop("user"), kwargs.pop("group"), kwargs.pop("extra_groups")), (2001, 2001, ()))
                self.assertTrue(kwargs["close_fds"] and kwargs["start_new_session"])
                self.assertTrue(kwargs.pop("executable").startswith("/proc/self/fd/"))
                self.assertEqual(len(kwargs.pop("pass_fds")), 1)
                return original((sys.executable, *args[1:]), **kwargs)

            poison = {"BASH_ENV": "untrusted-startup", "ENV": "untrusted-startup", "LD_PRELOAD": "untrusted-loader",
                      "PYTHONPATH": "untrusted-imports", "PATH": ".", "GITHUB_REPOSITORY": "forged/repo",
                      "GITHUB_EVENT_PATH": "untrusted.json", "BASH_FUNC_start%%": "() { :; }"}
            with patch.dict(os.environ, poison), patch.object(ADMISSION.subprocess, "Popen", side_effect=spawn):
                process = ADMISSION.launch(self.channel(), plan)
                self.assertEqual(process.wait(timeout=5), 0)
            environment, received = json.loads(output.read_text())
            self.assertEqual(received.encode(), self.data())
            self.assertTrue(set(environment) <= {"PATH", "LANG", "LC_CTYPE"})
            seals.fcntl.assert_called_once()

    def test_launch_failures_close_descriptors_without_starting_worker(self):
        self.host_fixture()
        for target in ("memfd_create", "write", "lseek"):
            with self.subTest(target=target), patch.object(ADMISSION.os, target, side_effect=OSError("denied")), \
                    patch.object(ADMISSION.os, "close", wraps=os.close) as close, \
                    patch.object(ADMISSION.subprocess, "Popen") as start, self.assertRaises(OSError):
                ADMISSION.launch(self.channel(), self.plan)
            self.assertGreaterEqual(close.call_count, 1)
            start.assert_not_called()
        with patch.object(ADMISSION.os, "write", return_value=0), \
                patch.object(ADMISSION.subprocess, "Popen") as start, self.assertRaises(ADMISSION.Denied):
            ADMISSION.launch(self.channel(), self.plan)
        start.assert_not_called()
        with patch.object(ADMISSION.subprocess, "Popen", side_effect=OSError("exec denied")), \
                patch.object(ADMISSION.os, "close", wraps=os.close) as close, self.assertRaises(OSError):
            ADMISSION.launch(self.channel(), self.plan)
        self.assertEqual(close.call_count, 2)
        with patch.object(ADMISSION, "remaining", side_effect=ADMISSION.Denied("expired")), \
                patch.object(ADMISSION, "receive_assignment", return_value=self.data()), \
                patch.object(ADMISSION.subprocess, "Popen") as start, self.assertRaises(ADMISSION.Denied):
            ADMISSION.launch(self.channel(), self.plan)
        start.assert_not_called()
        seals = sys.modules["fcntl"]
        with patch.object(seals, "fcntl", side_effect=OSError("seals denied")), \
                patch.object(ADMISSION.subprocess, "Popen") as start, self.assertRaises(OSError):
            ADMISSION.launch(self.channel(), self.plan)
        start.assert_not_called()

    def test_linux_descriptor_execution_seals_and_shell_startup(self):
        if sys.platform != "linux":
            with self.assertRaises(ADMISSION.Denied):
                ADMISSION.launch(self.channel(), self.plan)
            return
        # Real Linux descriptors, executable walk, seals and shell startup. Only
        # privileged ownership and account transitions are modelled in-process.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            startup, sentinel, status = root / "startup.sh", root / "startup", root / "status"
            startup.write_text('printf executed > "$STARTUP_SENTINEL"\n', encoding="utf-8")
            bash = shutil.which("bash")
            self.assertIsNotNone(bash)
            environment = dict(os.environ, BASH_ENV=str(startup), STARTUP_SENTINEL=str(sentinel))
            subprocess.run([bash, "--noprofile", "--norc", "-c", "exit 0"], env=environment, check=True, timeout=5)
            self.assertEqual(sentinel.read_text(), "executed")
            sentinel.unlink()
            body = (
                "import fcntl,json,os,subprocess,sys; from pathlib import Path; "
                "data=sys.stdin.buffer.read(); "
                "assert fcntl.fcntl(0,fcntl.F_GET_SEALS) & fcntl.F_SEAL_WRITE; "
                "subprocess.run([" + repr(bash) + ",'--noprofile','--norc','-c','exit 0'],check=True); "
                "Path(" + repr(str(status)) + ").write_bytes(data)"
            )
            plan = replace(self.plan, command=(str(Path(sys.executable).resolve()), "-I", "-B", "-c", body))
            original = subprocess.Popen
            original_stat = os.fstat

            def host_stat(descriptor):
                fields = list(original_stat(descriptor))
                fields[4] = 0
                return os.stat_result(fields)

            def spawn(args, **kwargs):
                for key in ("user", "group", "extra_groups"):
                    kwargs.pop(key)
                return original(args, **kwargs)

            with patch.dict(os.environ, environment), patch.object(ADMISSION.os, "geteuid", return_value=0), \
                    patch.object(ADMISSION.os, "fstat", side_effect=host_stat), \
                    patch.object(ADMISSION.subprocess, "Popen", side_effect=spawn) as start:
                self.assignment["head_repository"] = "fork/repo"
                with self.assertRaises(ADMISSION.Denied):
                    ADMISSION.launch(self.channel(), plan)
                start.assert_not_called()
                self.assertFalse(status.exists() or sentinel.exists())
                self.assignment["head_repository"] = ADMISSION.REPOSITORY
                process = ADMISSION.launch(self.channel(), plan)
                self.assertEqual(process.wait(timeout=5), 0)
            self.assertEqual(status.read_bytes(), self.data())
            self.assertFalse(sentinel.exists())

    def test_worker_path_walk_uses_retained_descriptors_and_refuses_untrusted_files(self):
        directory = SimpleNamespace(st_uid=0, st_mode=stat.S_IFDIR | 0o755)
        executable = SimpleNamespace(st_uid=0, st_mode=stat.S_IFREG | 0o755)
        for name in ("O_DIRECTORY", "O_NOFOLLOW", "O_NONBLOCK", "O_CLOEXEC"):
            self.stack.enter_context(patch.object(ADMISSION.os, name, 1, create=True))
        for info in (executable, SimpleNamespace(st_uid=2001, st_mode=stat.S_IFREG | 0o755),
                     SimpleNamespace(st_uid=0, st_mode=stat.S_IFREG | 0o775),
                     SimpleNamespace(st_uid=0, st_mode=stat.S_IFREG | 0o4755),
                     SimpleNamespace(st_uid=0, st_mode=stat.S_IFIFO | 0o755)):
            with patch.object(ADMISSION.os, "open", side_effect=[10, 11, 12]) as opened, \
                    patch.object(ADMISSION.os, "fstat", side_effect=[directory, directory, info]), \
                    patch.object(ADMISSION.os, "read", return_value=b"\x7fELF"), \
                    patch.object(ADMISSION.os, "close") as close:
                if info is executable:
                    self.assertEqual(ADMISSION.open_worker("/host/worker"), 12)
                    self.assertEqual(close.call_count, 2)
                else:
                    with self.assertRaises(ADMISSION.Denied):
                        ADMISSION.open_worker("/host/worker")
                    self.assertEqual(close.call_count, 3)
                self.assertEqual(opened.call_args.kwargs["dir_fd"], 11)
        for data in (b"", b"#!sh", b"\x7fEL"):
            with patch.object(ADMISSION.os, "open", side_effect=[10, 11, 12]), \
                    patch.object(ADMISSION.os, "fstat", side_effect=[directory, directory, executable]), \
                    patch.object(ADMISSION.os, "read", return_value=data), patch.object(ADMISSION.os, "close") as close, \
                    self.assertRaises(ADMISSION.Denied):
                ADMISSION.open_worker("/host/worker")
            self.assertEqual(close.call_count, 3)
        for error in (PermissionError(), FileNotFoundError(), OSError("symlink refused")):
            with patch.object(ADMISSION.os, "open", side_effect=[10, error]), \
                    patch.object(ADMISSION.os, "fstat", return_value=directory), patch.object(ADMISSION.os, "close") as close, \
                    self.assertRaises(OSError):
                ADMISSION.open_worker("/host/worker")
            close.assert_called_once_with(10)
        for uid, mode in ((2001, 0o755), (0, 0o775), (0, 0o757)):
            with patch.object(ADMISSION.os, "open", return_value=10) as opened, \
                    patch.object(ADMISSION.os, "fstat", return_value=SimpleNamespace(st_uid=uid, st_mode=stat.S_IFDIR | mode)), \
                    patch.object(ADMISSION.os, "close") as close, self.assertRaises(ADMISSION.Denied):
                ADMISSION.open_worker("/host/worker")
            opened.assert_called_once()
            close.assert_called_once_with(10)
        for operation in ("read", "fstat"):
            with patch.object(ADMISSION.os, "open", side_effect=[10, 11, 12]), \
                    patch.object(ADMISSION.os, "fstat", side_effect=[directory, directory, executable]), \
                    patch.object(ADMISSION.os, operation, side_effect=OSError("unreadable")), \
                    patch.object(ADMISSION.os, "close") as close, self.assertRaises(OSError):
                ADMISSION.open_worker("/host/worker")
            self.assertEqual(close.call_args.args[0], 10 if operation == "fstat" else 12)

    def test_suite_registration_and_no_linux_routing(self):
        spec = importlib.util.spec_from_file_location("admission_ci_paths", ROOT / "tools/ci/ci_paths.py")
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        self.assertEqual(gate.PYTHON_TESTS.count("tools/ci/test_self_hosted_admission.py"), 1)
        for path in ("tools/ci/self_hosted_admission.py", "tools/ci/test_self_hosted_admission.py"):
            self.assertTrue(gate.needs_python(path))
            self.assertTrue(gate.needs_cpp(path))
        cmake = (ROOT / "tools/CMakeLists.txt").read_text()
        self.assertIn('NAME SelfHostedAdmissionContract\n', cmake)
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/ci/test_self_hosted_admission.py"', cmake)
        for name in ("ci.yml", "asan.yml", "performance.yml"):
            workflow = (ROOT / ".github/workflows" / name).read_text()
            self.assertNotIn("LUM_SELF_HOSTED", workflow)
            self.assertNotIn("lum-vq1", workflow)

    def test_standalone_invocation_cannot_report_admission(self):
        result = subprocess.run([sys.executable, "-I", "-B", str(Path(ADMISSION.__file__))],
                                capture_output=True, text=True, timeout=5, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("Standalone execution denied", result.stderr)


if __name__ == "__main__":
    unittest.main()
