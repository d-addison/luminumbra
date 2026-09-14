"""Installed static RenderEngine adapter. Blender and GPU access stay on main thread."""
from pathlib import Path
import threading
import time
import weakref

import bpy

from . import viewport_math as vm
from .viewport_adapter_state import Revisions, camera
from .viewport_client import ViewportClient
from .viewport_generation import GenerationReader

ENGINE_ID = 'LUMINUMBRA_INSTALLED'
_engines = weakref.WeakSet()


def main_thread():
    if threading.current_thread() is not threading.main_thread():
        raise RuntimeError('Blender viewport access must remain on its main thread')


def matrix(value):
    return tuple(float(value[row][col]) for col in range(4) for row in range(4))


def stop_all(wait=False):
    main_thread()
    for engine in tuple(_engines):
        engine.close(wait=wait)


def redraw():
    main_thread()
    if _engines:
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == 'VIEW_3D':
                    area.tag_redraw()
    return 1 / 30


class LUMINUMBRA_RenderEngine(bpy.types.RenderEngine):
    bl_idname = ENGINE_ID
    bl_label = 'Luminumbra Installed'
    bl_use_preview = False
    bl_use_postprocess = False

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        main_thread()
        self.client = self.reader = self.draw_data = None
        self.config = self.desired = self.presentation = None
        self.presented_frame = None
        self.revisions = Revisions()
        self.error = ''
        self.presented_sequence = 0
        self.presentation_samples = []
        _engines.add(self)

    def close(self, wait=False):
        if self.client:
            self.client.close(wait=wait)
        if self.reader:
            self.reader.close()
        self.client = self.reader = None
        self.draw_data = self.desired = self.presentation = self.config = None
        self.presented_frame = None

    def capture_frame(self, directory):
        main_thread()
        if self.presented_frame is None or self.presentation is None:
            raise ValueError('No current presented frame to capture')
        status, header = self.client.status, self.presented_frame[0]
        if (status['state'] != 'ready' or status['session'] != header['session'] or
                status['sequence'] != header['sequence'] or self.desired != header['state']):
            raise ValueError('Presented frame is no longer current')
        from .viewport_capture import capture
        return capture(directory, *self.presented_frame)

    def __del__(self):
        try:
            self.close()
        finally:
            super().__del__()

    def render(self, depsgraph):
        self.error_set('Use a rendered 3D viewport for installed static preview; final rendering is not implemented')

    def configure(self, context):
        scene = context.scene
        collection = scene.lum_author_collection
        if not collection or not collection.get('luminumbra.asset_id'):
            raise ValueError('Mark a geometry collection and build its static prefab first')
        values = tuple(bpy.path.abspath(getattr(scene, 'lum_author_' + name))
                       for name in ('project', 'python', 'preview_host', 'preview_manifest'))
        config = (collection.as_pointer(), collection['luminumbra.asset_id'], *values,
                  scene.lum_author_generation)
        if config == self.config:
            return collection
        self.close()
        project, python, host, manifest = values
        if not all((project, python, host, manifest)):
            raise ValueError('Configure the project, external Python, installed preview host and SDK manifest')
        broker = Path(__file__).parent / 'viewport' / 'broker.py'
        self.reader = GenerationReader(project, collection['luminumbra.asset_id'], scene.lum_author_generation)
        try:
            self.client = ViewportClient(python, str(broker), host, project, manifest)
        except Exception:
            self.reader.close()
            self.reader = None
            raise
        self.revisions = Revisions()
        self.config = config
        self.presented_sequence = 0
        self.presentation_samples.clear()
        return collection

    def view_update(self, context, depsgraph):
        main_thread()
        try:
            self.configure(context)
            self.error = ''
        except (OSError, ValueError, RuntimeError, ReferenceError) as error:
            self.error = str(error)

    def snapshot(self, collection, generation, depsgraph):
        objects = {obj.get('luminumbra.object_id'): obj for obj in collection.all_objects}
        if len(objects) != len(collection.all_objects) or None in objects:
            raise ValueError('Refresh unique asset IDs before opening the viewport')
        if set(objects) != {node['id'] for node in generation['nodes']}:
            raise ValueError('Waiting for a prefab generation matching the edited collection structure')
        result = []
        for node in generation['nodes']:
            obj = objects[node['id']]
            parent = obj.parent
            if (parent.get('luminumbra.object_id') if parent else None) != node['parent']:
                raise ValueError('Waiting for the rebuilt prefab parent hierarchy')
            evaluated = obj.evaluated_get(depsgraph)
            world = matrix(evaluated.matrix_world)
            parent_world = matrix(parent.evaluated_get(depsgraph).matrix_world) if parent else None
            local = vm.to_engine_local(vm.local_from_world(world, parent_world))
            result.append(dict(instance_id='blender.collection', node_id=node['id'], matrix=list(local)))
        return result

    def view_draw(self, context, depsgraph):
        main_thread()
        try:
            collection = self.configure(context)
            generation = self.reader.poll()
            if generation is None:
                status = self.reader.status()
                raise ValueError(status['error'] or 'Waiting for a published static prefab generation')
            region, rv3d = context.region, context.region_data
            if rv3d is None:
                return
            camera_state = camera(matrix(rv3d.view_matrix), matrix(rv3d.window_matrix), region.width, region.height)
            locals_ = self.snapshot(collection, generation, depsgraph)
            desired = self.revisions.state(generation, camera_state, locals_)
            if desired != self.desired:
                self.client.submit(desired)
                self.desired = desired
                self.submitted_at = time.monotonic()
                self.presentation = None
                self.presented_frame = None
            frame = self.client.poll()
            if frame is not None and frame[0]['state'] == desired:
                from .viewport_draw import FrameDraw
                if self.draw_data is None:
                    self.draw_data = FrameDraw()
                self.draw_data.update(*frame)
                self.presentation = camera_state
                self.presented_frame = frame
                self.presented_sequence = frame[0]['sequence']
                self.presentation_samples.append((time.monotonic() - self.submitted_at) * 1000)
                del self.presentation_samples[:-512]
            status = self.client.status
            if status['state'] != 'ready':
                self.presentation = None
                self.presented_frame = None
            if self.draw_data is not None and self.presentation is not None:
                # Downsampling may keep host state identical across a region
                # resize. The matching frame still needs today's pixel rectangle.
                self.presentation = camera_state
                self.draw_data.draw(self.presentation['rectangle'], self.presentation['depth_transform'])
            self.error = status.get('error') or ''
            self.update_stats('Luminumbra', self.error or 'Installed static preview')
        except (OSError, ValueError, RuntimeError, ReferenceError) as error:
            self.error = str(error)
            self.presentation = None
            self.presented_frame = None
            # A temporary descriptor or hierarchy failure can resolve to the
            # identical state. Request it again after recovery even without motion.
            self.desired = None
            self.update_stats('Luminumbra', self.error)


class LUMINUMBRA_OT_viewport(bpy.types.Operator):
    bl_idname = 'luminumbra.viewport'
    bl_label = 'Open Installed Viewport'
    operation: bpy.props.StringProperty(default='open')

    def execute(self, context):
        main_thread()
        if self.operation == 'reset':
            stop_all()
        elif self.operation == 'close':
            stop_all()
            for area in context.screen.areas:
                if area.type == 'VIEW_3D':
                    area.spaces.active.shading.type = 'SOLID'
        else:
            context.scene.render.engine = ENGINE_ID
            context.space_data.shading.type = 'RENDERED'
        context.area.tag_redraw()
        return {'FINISHED'}


CLASSES = (LUMINUMBRA_RenderEngine, LUMINUMBRA_OT_viewport)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.app.timers.register(redraw, first_interval=1 / 30, persistent=True)


def unregister():
    stop_all(wait=True)
    if bpy.app.timers.is_registered(redraw):
        bpy.app.timers.unregister(redraw)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
