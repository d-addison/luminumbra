"""UI mock for the qualified-version experiment; no engine RenderEngine adapter."""
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import time

import bpy
from bpy.app.handlers import persistent

_session = None


class Session:
    def __init__(self, python, project):
        self.directory = Path(tempfile.mkdtemp(prefix="luminumbra-blender-MOCK-"))
        self.token = secrets.token_hex(32)
        self.sequence = 0
        self.pending = None
        self.job = None
        self.preview = None
        self.status = "MOCK — connected; no engine rendering"
        self.log = (self.directory / "worker.log").open("wb")
        self.process = subprocess.Popen([
            python, "-B", str(Path(__file__).parent / "worker" / "mock_service.py"),
            "--project", str(project), "--responses", str(self.directory / "responses")],
            stdin=subprocess.PIPE, stdout=self.log, stderr=self.log,
            env={**os.environ, "LUMINUMBRA_MOCK_TOKEN": self.token})

    def send(self, op, **params):
        if self.pending:
            return
        self.sequence += 1
        self.process.stdin.write((json.dumps({"op": op, "params": params, "token": self.token}) + "\n").encode())
        self.process.stdin.flush()
        self.pending = (self.sequence, op, time.monotonic())

    def poll(self):
        if self.process.poll() is not None:
            self.status = "MOCK — worker exited; see " + str(self.directory / "worker.log")
            self.job = None
            self.pending = None
            return
        if not self.pending:
            return
        sequence, op, started = self.pending
        path = self.directory / "responses" / f"{sequence}.json"
        if not path.exists():
            if time.monotonic() - started > 10:
                self.status = "MOCK — response timeout; stop and restart the service"
            return
        response = json.loads(path.read_text(encoding="utf-8"))
        self.pending = None
        if not response["ok"]:
            self.status = "MOCK — " + response["findings"][0]["message"]
            self.job = None
            return
        result = response["result"]
        if op in ("build", "job.status", "job.cancel"):
            self.job = result["job_id"]
            self.status = "MOCK — " + result["status"]
        elif op == "preview.open":
            self.preview = result["preview_id"]
            self.status = "MOCK — preview metadata opened; no rendered image"
        else:
            self.status = "MOCK — draft sidecar validated"

    def close(self):
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=0.3)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=0.3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=0.3)
        self.log.close()


def stop():
    global _session
    if _session:
        _session.close()
        _session = None


@persistent
def file_changed(_):
    stop()


def timer():
    if _session:
        try:
            _session.poll()
        except (OSError, ValueError, KeyError) as error:
            _session.status = "MOCK — " + str(error)
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == "VIEW_3D":
                    area.tag_redraw()
    return 0.1


class LUMINUMBRA_OT_mock(bpy.types.Operator):
    bl_idname = "luminumbra.mock"
    bl_label = "Luminumbra MOCK operation"
    operation: bpy.props.StringProperty()

    def execute(self, context):
        global _session
        try:
            scene = context.scene
            if self.operation == "start":
                stop()
                _session = Session(bpy.path.abspath(scene.lum_mock_python),
                                   Path(bpy.path.abspath(scene.lum_mock_project)))
            elif self.operation == "stop":
                stop()
            elif _session:
                if self.operation in ("validate", "build"):
                    params = {"source": scene.lum_mock_source}
                    if self.operation == "build":
                        params["revision"] = scene.lum_mock_revision
                    _session.send(self.operation, **params)
                elif self.operation == "preview.open" and _session.job:
                    _session.send(self.operation, job_id=_session.job)
                elif self.operation in ("job.status", "job.cancel") and _session.job:
                    _session.send(self.operation, job_id=_session.job)
            return {"FINISHED"}
        except (OSError, ValueError) as error:
            self.report({"ERROR"}, "MOCK — " + str(error))
            return {"CANCELLED"}


class LUMINUMBRA_PT_mock(bpy.types.Panel):
    bl_label = "Luminumbra Author MOCK"
    bl_idname = "LUMINUMBRA_PT_mock"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Luminumbra MOCK"
    bl_ui_units_x = 24

    def draw(self, context):
        layout = self.layout
        layout.label(text="MOCK — no engine assets or rendering", icon="INFO")
        for name in ("python", "project", "source", "revision"):
            layout.prop(context.scene, "lum_mock_" + name)
        def button(text, op):
            layout.operator("luminumbra.mock", text=text).operation = op
        if not _session:
            button("Start MOCK service", "start")
        else:
            layout.label(text=_session.status)
            button("Stop MOCK service", "stop")
            if not _session.pending:
                button("Validate sidecar", "validate")
                button("Build MOCK snapshot", "build")
                if _session.job:
                    button("Advance MOCK job", "job.status")
                    button("Cancel MOCK job", "job.cancel")
                    button("Open MOCK preview metadata", "preview.open")


CLASSES = (LUMINUMBRA_OT_mock, LUMINUMBRA_PT_mock)


def register():
    if bpy.app.version != (5, 1, 0):
        raise RuntimeError("MOCK experiment pinned to Blender 5.1.0; qualify another profile first.")
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.lum_mock_python = bpy.props.StringProperty(name="External Python 3.11+", subtype="FILE_PATH")
    bpy.types.Scene.lum_mock_project = bpy.props.StringProperty(name="Fixture project", subtype="DIR_PATH")
    bpy.types.Scene.lum_mock_source = bpy.props.StringProperty(name="Sidecar", default="asset.json")
    bpy.types.Scene.lum_mock_revision = bpy.props.IntProperty(name="Expected revision", default=1, min=0)
    bpy.app.handlers.load_pre.append(file_changed)
    bpy.app.timers.register(timer, persistent=True)


def unregister():
    stop()
    if bpy.app.timers.is_registered(timer):
        bpy.app.timers.unregister(timer)
    if file_changed in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.remove(file_changed)
    for name in ("python", "project", "source", "revision"):
        delattr(bpy.types.Scene, "lum_mock_" + name)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
