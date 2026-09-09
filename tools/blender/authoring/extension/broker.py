"""External-only IO broker: owns background export and installed build service."""
import argparse
import hmac
import json
import os
from pathlib import Path
import queue
import secrets
import subprocess
import sys
import threading
import time

from logic import atomic_json, read_json


class Broker:
    def __init__(self, args):
        self.args = args
        self.project = args.project.resolve(strict=True)
        self.cancelled = threading.Event()
        self.disconnected = threading.Event()
        self.requests = queue.Queue(maxsize=2)
        self.active = threading.Event()
        self.parent_token = os.environ.pop("LUMINUMBRA_ADAPTER_TOKEN", "")
        if len(self.parent_token) < 32:
            raise ValueError("Missing adapter session token")
        self.token = secrets.token_hex(32)
        self.sequence = 0
        self.service = subprocess.Popen(
            [sys.executable, str(args.service), "--project", str(self.project),
             "--toolchain", str(args.toolchain), "serve"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            env={**os.environ, "LUMINUMBRA_AUTHOR_TOKEN": self.token})
        self.exporter = None
        threading.Thread(target=self.read_requests, daemon=True).start()

    def read_requests(self):
        # This thread lives in external Python, never in Blender.
        try:
            pending = bytearray()
            while True:
                block = os.read(sys.stdin.fileno(), 4096)
                if not block:
                    break
                pending.extend(block)
                if len(pending) > 65536:
                    break
                while b"\n" in pending:
                    line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    request = json.loads(line)
                    token = request.get("token")
                    if not isinstance(token, str) or not hmac.compare_digest(token, self.parent_token):
                        continue
                    if request.get("op") == "cancel":
                        self.cancelled.set()
                    else:
                        if self.active.is_set():
                            raise ValueError("Only one adapter build may be outstanding")
                        self.active.set()
                        self.cancelled.clear()
                        self.requests.put_nowait(request)
        except (ValueError, TypeError, queue.Full):
            pass
        finally:
            self.disconnected.set()
            self.cancelled.set()

    def rpc(self, op, **params):
        self.sequence += 1
        request = {"id": self.sequence, "token": self.token, "op": op, "params": params}
        self.service.stdin.write((json.dumps(request) + "\n").encode())
        self.service.stdin.flush()
        line = self.service.stdout.readline(1024 * 1024 + 1)
        if not line or len(line) > 1024 * 1024:
            raise RuntimeError("Installed authoring service exited or returned an oversized response")
        response = json.loads(line)
        if not response.get("ok"):
            raise RuntimeError(response["findings"][0]["message"])
        if response.get("id") != self.sequence:
            raise RuntimeError("Authoring response ID did not match")
        return response["result"]

    def export(self, request):
        relative = request["snapshot"]
        snapshot = (self.project / relative).resolve(strict=True)
        if not snapshot.is_relative_to(self.project) or snapshot.suffix != ".blend":
            raise ValueError("Snapshot must be a project-owned blend library")
        directory = snapshot.parent
        config = directory / "export-request.json"
        atomic_json(config, {**request, "project": str(self.project), "snapshot": str(snapshot)})
        result = directory / "export-process.json"
        env = os.environ.copy()
        for name in ("BLENDER_USER_RESOURCES", "BLENDER_USER_CONFIG", "BLENDER_USER_SCRIPTS",
                     "BLENDER_USER_DATAFILES", "BLENDER_USER_EXTENSIONS"):
            path = directory / "user" / name.lower()
            path.mkdir(parents=True, exist_ok=True)
            env[name] = str(path)
        temporary = directory / "temp"
        temporary.mkdir(exist_ok=True)
        env.update(TEMP=str(temporary), TMP=str(temporary), OMP_NUM_THREADS="2")
        command = [sys.executable, str(self.args.service), "_compiler", str(result), "180",
                   str(self.args.blender), "--background", "--factory-startup", "--disable-autoexec",
                   "--threads", "2", "--python-exit-code", "23", "--python",
                   str(Path(__file__).with_name("export_worker.py")), "--", str(config)]
        self.exporter = subprocess.Popen(command, stdin=subprocess.PIPE,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        try:
            deadline = time.monotonic() + 195
            while self.exporter.poll() is None:
                if self.cancelled.wait(0.02) or time.monotonic() > deadline:
                    self.exporter.stdin.close()
                    self.exporter.wait(timeout=10)
                    raise RuntimeError("Geometry export cancelled" if self.cancelled.is_set() else "Geometry export timed out")
            outcome = read_json(result)
            if self.exporter.returncode or outcome["returncode"]:
                raise RuntimeError(outcome.get("log", "Background export failed")[-4000:])
            return str((directory / "asset.json").relative_to(self.project)).replace("\\", "/")
        finally:
            self.exporter.stdin.close()
            if self.exporter.poll() is None:
                self.exporter.wait(timeout=10)
            self.exporter = None

    def build(self, request, request_id):
        if self.disconnected.is_set():
            raise RuntimeError("Blender session closed")
        capabilities = self.rpc("capabilities")
        if not capabilities.get("source_revision_guards"):
            raise RuntimeError("Install authoring service 0.2 or newer with source revision guards")
        if request.get("build_profile", "glb-geometry-v1") not in capabilities["profiles"]:
            raise RuntimeError("Install an authoring service supporting the selected asset profile")
        if self.cancelled.is_set():
            raise RuntimeError("Geometry build cancelled")
        source = self.export(request)
        if self.cancelled.is_set():
            raise RuntimeError("Geometry build cancelled")
        atomic_json(self.args.response, {"id": request_id, "pending": True,
                                         "stage": "Compiling and validating geometry"})
        job = self.rpc("build", source=source, revision=request["revision"])
        sent_cancel = False
        while job["status"] not in ("succeeded", "failed", "stale", "cancelled"):
            if self.cancelled.is_set() and not sent_cancel:
                self.rpc("job.cancel", job_id=job["job_id"])
                sent_cancel = True
            time.sleep(0.05)
            job = self.rpc("job.status", job_id=job["job_id"])
        return job

    def run(self):
        try:
            while not self.disconnected.is_set():
                try:
                    request = self.requests.get(timeout=0.1)
                except queue.Empty:
                    continue
                try:
                    if request.get("op") != "build":
                        raise ValueError("Unsupported adapter operation")
                    response = {"id": request["id"], "ok": True, "result": self.build(request["params"], request["id"])}
                except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
                    response = {"id": request.get("id"), "ok": False, "message": str(error)}
                self.active.clear()
                atomic_json(self.args.response, response)
        finally:
            self.service.stdin.close()
            try:
                self.service.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.service.kill()
                self.service.wait()
            self.service.stdout.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("project", "service", "toolchain", "blender", "response"):
        parser.add_argument("--" + name, type=Path, required=True)
    Broker(parser.parse_args()).run()
