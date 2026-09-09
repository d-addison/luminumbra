"""Blender main-thread UI for the installed geometry build service."""
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import time
import uuid

import bpy
from bpy.app.handlers import persistent

from .blender_asset import assign_ids, capture, validate_collection
from .logic import BuildState, atomic_json, digest, read_json

_session = None
_capturing = False


def configuration(scene):
    return tuple(bpy.path.abspath(getattr(scene, "lum_author_" + name))
                 for name in ("project", "python", "service", "toolchain"))


class Session:
    def __init__(self, scene, collection):
        self.config = configuration(scene)
        project, python, service, toolchain = self.config
        self.project = Path(project).resolve(strict=True)
        if not self.project.is_dir() or not all(Path(value).is_file() for value in (python, service, toolchain)):
            raise ValueError("Configure an existing project, Python, installed service and toolchain manifest")
        self.asset_id = collection["luminumbra.asset_id"]
        self.prefab = scene.lum_author_prefab
        self.scene = scene
        self.state = BuildState()
        self.sequence = 0
        self.pending = None
        self.suppressed = False
        self.nonce = uuid.uuid4().hex
        self.guard = ".luminumbra-author-source/scene-" + digest(self.asset_id.encode()) + ".json"
        directory = self.project / ".luminumbra-author-source"
        if directory.is_symlink():
            raise ValueError("Authoring snapshot directory cannot be a symlink")
        directory.mkdir(exist_ok=True)
        self.write_guard()
        self.directory = Path(tempfile.mkdtemp(prefix="luminumbra-geometry-"))
        self.response = self.directory / "response.json"
        self.token = secrets.token_hex(32)
        self.log = (self.directory / "broker.log").open("wb")
        try:
            self.process = subprocess.Popen(
                [python, "-B", str(Path(__file__).with_name("broker.py")),
                 "--project", str(self.project), "--service", service, "--toolchain", toolchain,
                 "--blender", bpy.app.binary_path, "--response", str(self.response)],
                stdin=subprocess.PIPE, stdout=self.log, stderr=self.log,
                env={**os.environ, "LUMINUMBRA_ADAPTER_TOKEN": self.token})
        except OSError:
            self.log.close()
            raise
        self.timing = self.scene_timing()
        self.structure = self.collection_structure(collection)

    @staticmethod
    def collection_structure(collection):
        return tuple(sorted((obj.session_uid, obj.parent.session_uid if obj.parent else 0,
                             obj.data.session_uid if obj.data else 0)
                            for obj in collection.all_objects))

    def scene_timing(self):
        scene = self.scene
        return (scene.frame_start, scene.frame_end, scene.render.fps,
                scene.render.fps_base, scene.unit_settings.scale_length)

    def write_guard(self):
        path = self.project / self.guard
        atomic_json(path, {"asset_id": self.asset_id, "session": self.nonce,
                           "revision": self.state.revision})
        return digest(path.read_bytes())

    def changed(self):
        self.state.changed(time.monotonic())
        self.suppressed = False
        self.write_guard()
        if self.state.busy:
            self.cancel(suppress=False)

    def send(self, value):
        value["token"] = self.token
        self.process.stdin.write((json.dumps(value) + "\n").encode())
        self.process.stdin.flush()

    def build(self, collection):
        global _capturing
        self.suppressed = False
        if self.state.busy:
            return
        _capturing = True
        try:
            guard_hash = self.write_guard()
            request = capture(self.scene, collection, self.project, self.state.revision,
                              self.guard, guard_hash, prefab=self.prefab)
            self.state.submitted()
            self.sequence += 1
            self.pending = self.sequence
            self.send({"id": self.sequence, "op": "build", "params": request})
        except Exception:
            self.state.busy = False
            self.pending = None
            raise
        finally:
            _capturing = False

    def poll(self):
        if self.process.poll() is not None:
            self.state.busy = False
            self.state.status = "Geometry worker exited; see " + str(self.directory / "broker.log")
            self.pending = None
            self.suppressed = True
            return
        if self.pending is None or not self.response.is_file():
            return
        response = read_json(self.response)
        if response.get("id") != self.pending:
            return
        if response.get("pending"):
            self.state.status = response["stage"]
            return
        self.pending = None
        self.state.completed(response)

    def cancel(self, suppress=True):
        self.suppressed = suppress
        if self.state.busy:
            self.send({"op": "cancel"})
            self.state.status = "Cancelling geometry build"

    def close(self):
        # Invalidate already-captured snapshots even if a worker is just about
        # to submit them, then close the owned broker/service pipes.
        self.state.revision += 1
        try:
            self.write_guard()
        except OSError:
            pass
        try:
            self.process.stdin.close()
        except BrokenPipeError:
            pass
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
        session, _session = _session, None
        session.close()


@persistent
def file_changed(_):
    stop()


@persistent
def undone(_):
    if _session:
        try:
            _session.changed()
        except (OSError, ReferenceError):
            stop()


@persistent
def edited(scene, depsgraph):
    if _session and not _capturing and scene.original == _session.scene:
        collection = _session.scene.lum_author_collection
        if not collection:
            return
        objects = set(collection.all_objects)
        related = objects | {obj.data for obj in objects if obj.data}
        if any(update.id.original in related or isinstance(update.id, (bpy.types.Material, bpy.types.NodeTree,
                                                              bpy.types.Action, bpy.types.Image))
               for update in depsgraph.updates):
            try:
                _session.changed()
            except (OSError, BrokenPipeError):
                stop()


def timer():
    if _session:
        try:
            scene = _session.scene
            collection = scene.lum_author_collection
            if (not collection or collection.get("luminumbra.asset_id") != _session.asset_id
                    or configuration(scene) != _session.config or scene.lum_author_prefab != _session.prefab):
                stop()
                return 0.1
            timing = _session.scene_timing()
            structure = _session.collection_structure(collection)
            if timing != _session.timing or structure != _session.structure:
                _session.timing = timing
                _session.structure = structure
                _session.changed()
            _session.poll()
            if scene.lum_author_auto and not _session.suppressed and _session.state.due(time.monotonic()):
                _session.build(collection)
        except (OSError, ValueError, RuntimeError, ReferenceError) as error:
            if _session:
                _session.state.status = str(error)
                _session.suppressed = True
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == "VIEW_3D":
                    area.tag_redraw()
    return 0.1


class LUMINUMBRA_OT_mark_geometry(bpy.types.Operator):
    bl_idname = "luminumbra.mark_geometry"
    bl_label = "Mark / Refresh Asset IDs"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            collection = context.view_layer.active_layer_collection.collection
            validate_collection(context.scene, collection, require_ids=False)
            assign_ids(collection)
            context.scene.lum_author_collection = collection
            return {"FINISHED"}
        except ValueError as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class LUMINUMBRA_OT_geometry(bpy.types.Operator):
    bl_idname = "luminumbra.geometry"
    bl_label = "Geometry Asset Operation"
    operation: bpy.props.StringProperty()

    def execute(self, context):
        global _session
        try:
            if self.operation == "stop":
                stop()
            elif self.operation == "cancel" and _session:
                _session.cancel()
            elif self.operation == "build":
                scene, collection = context.scene, context.scene.lum_author_collection
                validate_collection(scene, collection)
                if _session and (_session.scene != scene or _session.asset_id != collection["luminumbra.asset_id"]
                                 or _session.config != configuration(scene) or _session.prefab != scene.lum_author_prefab):
                    stop()
                if _session is None:
                    _session = Session(scene, collection)
                _session.build(collection)
            return {"FINISHED"}
        except (OSError, ValueError, RuntimeError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class LUMINUMBRA_PT_geometry(bpy.types.Panel):
    bl_label = "Luminumbra Geometry"
    bl_idname = "LUMINUMBRA_PT_geometry"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Luminumbra"

    def draw(self, context):
        layout, scene = self.layout, context.scene
        layout.label(text="Geometry builds with installed engine tools")
        layout.operator("luminumbra.mark_geometry")
        layout.prop(scene, "lum_author_collection")
        layout.prop(scene, "lum_author_prefab")
        for name in ("project", "python", "service", "toolchain"):
            layout.prop(scene, "lum_author_" + name)
        layout.prop(scene, "lum_author_auto")
        row = layout.row()
        row.enabled = not _session or not _session.state.busy
        row.operator("luminumbra.geometry", text="Build Asset").operation = "build"
        if _session:
            layout.label(text=_session.state.status)
            if _session.state.metrics:
                layout.label(text=_session.state.metrics)
            if _session.state.busy:
                layout.operator("luminumbra.geometry", text="Cancel Build").operation = "cancel"
            layout.operator("luminumbra.geometry", text="Stop Service").operation = "stop"
        layout.label(text="Engine viewport and behavior graphs are pending", icon="INFO")


CLASSES = (LUMINUMBRA_OT_mark_geometry, LUMINUMBRA_OT_geometry, LUMINUMBRA_PT_geometry)


def register():
    if bpy.app.version != (5, 1, 0):
        raise RuntimeError("Qualify this extension for the installed Blender version first; expected 5.1.0")
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    scene = bpy.types.Scene
    scene.lum_author_collection = bpy.props.PointerProperty(name="Geometry asset", type=bpy.types.Collection)
    scene.lum_author_project = bpy.props.StringProperty(name="Project", subtype="DIR_PATH")
    scene.lum_author_python = bpy.props.StringProperty(name="External Python 3.11+", subtype="FILE_PATH")
    scene.lum_author_service = bpy.props.StringProperty(name="Installed authoring service", subtype="FILE_PATH")
    scene.lum_author_toolchain = bpy.props.StringProperty(name="Toolchain manifest", subtype="FILE_PATH")
    scene.lum_author_auto = bpy.props.BoolProperty(name="Rebuild after edits", default=True)
    scene.lum_author_prefab = bpy.props.BoolProperty(name="Preserve static prefab and materials", default=False)
    for handlers, callback in ((bpy.app.handlers.load_pre, file_changed),
                               (bpy.app.handlers.depsgraph_update_post, edited),
                               (bpy.app.handlers.undo_post, undone), (bpy.app.handlers.redo_post, undone)):
        handlers.append(callback)
    bpy.app.timers.register(timer, first_interval=0.1, persistent=True)


def unregister():
    stop()
    if bpy.app.timers.is_registered(timer):
        bpy.app.timers.unregister(timer)
    for handlers, callback in ((bpy.app.handlers.load_pre, file_changed),
                               (bpy.app.handlers.depsgraph_update_post, edited),
                               (bpy.app.handlers.undo_post, undone), (bpy.app.handlers.redo_post, undone)):
        if callback in handlers:
            handlers.remove(callback)
    for name in ("collection", "project", "python", "service", "toolchain", "auto", "prefab"):
        delattr(bpy.types.Scene, "lum_author_" + name)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
