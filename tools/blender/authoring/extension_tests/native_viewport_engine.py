"""Qualify the installed extension against a real installed host in Blender's GUI.

Run only in an owned factory-startup process with isolated user directories and
an external deadline. Receipts remain failed until all checks and cleanup pass.
"""
import argparse
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback

import bpy
from mathutils import Euler


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(args, report, check):
    import gpu
    check('actual Blender GUI profile', bpy.app.version == (5, 1, 0) and not bpy.app.background and
          gpu.platform.backend_type_get() == 'OPENGL')
    report['gpu'] = dict(backend=gpu.platform.backend_type_get(), vendor=gpu.platform.vendor_get(),
                         renderer=gpu.platform.renderer_get(), version=gpu.platform.version_get())
    check('native NVIDIA adapter', report['gpu']['vendor'] == 'NVIDIA Corporation' and
          report['gpu']['renderer'] == 'NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2')
    resources = (bpy.utils.user_resource('CONFIG'), bpy.utils.user_resource('SCRIPTS'),
                 bpy.utils.user_resource('DATAFILES'), bpy.app.tempdir)
    check('isolated Blender resource directories', all(Path(path).resolve().is_relative_to(args.isolation_root)
                                                       for path in resources))
    bpy.context.preferences.use_preferences_save = False
    bpy.context.preferences.filepaths.use_auto_save_temporary_files = False
    repositories = bpy.context.preferences.extensions.repos
    while repositories:
        repositories.remove(repositories[0])
    repository = args.output / 'repository'
    repository.mkdir()
    repo = repositories.new(name='Installed viewport qualification', module='viewport_acceptance',
                            custom_directory=str(repository), remote_url='', source='USER')
    check('install actual extension archive', bpy.ops.extensions.package_install_files('EXEC_DEFAULT',
          filepath=str(args.archive), repo=repo.module, enable_on_install=True) == {'FINISHED'})
    module_name = 'bl_ext.viewport_acceptance.luminumbra_geometry_author'
    extension = importlib.import_module(module_name)
    engine_module = importlib.import_module(module_name + '.viewport_engine')
    installation = importlib.import_module(module_name + '.viewport_installation')
    check('register actual engine and redraw timer', bpy.app.timers.is_registered(engine_module.redraw) and
          hasattr(bpy.types, 'LUMINUMBRA_RenderEngine'))
    before = {path.relative_to(args.host.parent.parent).as_posix(): sha(path)
              for path in args.host.parent.parent.rglob('*') if path.is_file()}
    report['sdk_before'] = before
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    scene = bpy.context.scene
    collection = bpy.data.collections.new('ViewportAcceptance')
    scene.collection.children.link(collection)
    layer = next(child for child in bpy.context.view_layer.layer_collection.children if child.collection == collection)
    bpy.context.view_layer.active_layer_collection = layer
    bpy.ops.mesh.primitive_cube_add(size=1)
    obj = bpy.context.object
    obj.name = 'AsymmetricAuthoredBlock'
    obj.scale = (1.3, .7, 1.8)
    material = bpy.data.materials.new('AuthoredPaint')
    material.use_nodes = True
    principled = next(node for node in material.node_tree.nodes if node.type == 'BSDF_PRINCIPLED')
    principled.inputs['Base Color'].default_value = (.8, .08, .02, 1)
    principled.inputs['Roughness'].default_value = .6
    obj.data.materials.append(material)
    project = args.output / 'project'
    project.mkdir()
    scene.lum_author_project = str(project)
    scene.lum_author_python = str(args.python)
    scene.lum_author_service = str(args.service)
    scene.lum_author_toolchain = str(args.toolchain)
    scene.lum_author_preview_host = str(args.host)
    scene.lum_author_preview_manifest = str(args.manifest)
    scene.lum_author_prefab = True
    scene.lum_author_auto = False
    check('mark static prefab', bpy.ops.luminumbra.mark_geometry() == {'FINISHED'})

    def build(previous=None):
        check('submit geometry build', bpy.ops.luminumbra.geometry(operation='build') == {'FINISHED'})
        deadline = time.monotonic() + 100
        while extension._session.state.busy and time.monotonic() < deadline:
            yield .03
        generation = extension._session.state.generation
        check('published distinct native generation', not extension._session.state.busy and generation and
              generation != previous, status=extension._session.state.status)
        return generation

    generation = yield from build()
    document = project / 'viewport-acceptance.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(document))
    report['blend_sha256'] = sha(document)
    window = bpy.context.window_manager.windows[0]
    area = next(area for area in window.screen.areas if area.type == 'VIEW_3D')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces.active
    space.clip_start, space.clip_end = .1, 100.
    space.region_3d.view_location = (0, 0, 0)
    space.region_3d.view_rotation = Euler((math.pi / 2, 0, 0)).to_quaternion()
    space.region_3d.view_distance = 6.
    space.region_3d.view_perspective = 'PERSP'
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('open installed RenderEngine', bpy.ops.luminumbra.viewport() == {'FINISHED'})
    report['captures'] = []

    def frame(name, previous=None, expected_generation=None):
        deadline = time.monotonic() + 45
        found = None
        while time.monotonic() < deadline:
            for engine in tuple(engine_module._engines):
                if engine.presented_frame is not None and engine.presentation is not None:
                    header = engine.presented_frame[0]
                    identity = (header['session'], header['sequence'])
                    if identity != previous and (expected_generation is None or
                                                 header['state']['generation_id'] == expected_generation):
                        found = engine
                        break
            if found:
                break
            area.tag_redraw()
            yield .03
        errors = [engine.error for engine in tuple(engine_module._engines)]
        check('present actual frame: ' + name, found is not None, errors=errors)
        receipt = found.capture_frame(args.output / name)
        check('native frame has coverage: ' + name, any(found.presented_frame[1][8 * (
              found.presented_frame[0]['state']['width'] * found.presented_frame[0]['state']['height']):]))
        report['captures'].append({'name': name, 'frame': receipt, 'client': found.client.status})
        with bpy.context.temp_override(window=window, area=area, region=region):
            bpy.ops.wm.screenshot(filepath=str(args.output / (name + '-editor.png')))
        check('original editor capture: ' + name, (args.output / (name + '-editor.png')).is_file())
        return found, (receipt['header']['session'], receipt['header']['sequence'])

    engine, identity = yield from frame('initial', expected_generation=generation)
    first = engine.presented_frame[0]['planes_sha256']
    obj.location.x = .4
    bpy.context.view_layer.update()
    engine, identity = yield from frame('transform', identity)
    check('transform changes rendered planes without a generation rebuild',
          engine.presented_frame[0]['planes_sha256'] != first and
          engine.presented_frame[0]['state']['generation_id'] == generation)
    moved = engine.presented_frame[0]['planes_sha256']
    space.region_3d.view_distance = 5.
    engine, identity = yield from frame('camera', identity)
    check('camera changes rendered planes', engine.presented_frame[0]['planes_sha256'] != moved)
    space.region_3d.view_perspective = 'ORTHO'
    engine, identity = yield from frame('orthographic', identity)
    check('actual orthographic host projection', engine.presented_frame[0]['state']['projection'][15] == 1)
    space.region_3d.view_perspective = 'PERSP'
    engine, identity = yield from frame('perspective-restored', identity)
    before_material = engine.presented_frame[0]['planes_sha256']
    principled.inputs['Base Color'].default_value = (.02, .12, .8, 1)
    bpy.context.view_layer.update()
    generation = yield from build(generation)
    engine, identity = yield from frame('material-refresh', identity, generation)
    check('material edit changes installed engine output', engine.presented_frame[0]['planes_sha256'] != before_material)
    before_geometry = engine.presented_frame[0]['planes_sha256']
    for vertex in obj.data.vertices:
        if vertex.co.z > 0:
            vertex.co.x += .35
    obj.data.update()
    bpy.context.view_layer.update()
    generation = yield from build(generation)
    engine, identity = yield from frame('geometry-refresh', identity, generation)
    check('geometry edit changes installed engine output', engine.presented_frame[0]['planes_sha256'] != before_geometry)
    client = engine.client
    old_session = client.status['session']
    broker_pid = client.status['broker_pid']
    check('crash target is the owned viewport broker', type(broker_pid) is int and broker_pid > 0)
    subprocess.run(['C:/Windows/System32/taskkill.exe', '/PID', str(broker_pid), '/F'],
                   check=True, timeout=10, stdout=subprocess.DEVNULL)
    engine, identity = yield from frame('crash-recovery', identity, generation)
    check('crash recovery creates a fresh authenticated session', client.status['session'] != old_session)
    report['feedback_samples_ms'] = list(engine.presentation_samples)
    report['feedback_scope'] = 'Submission to texture upload; compositor presentation timing remains unqualified'
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('close installed viewport', bpy.ops.luminumbra.viewport(operation='close') == {'FINISHED'})
    check('owned client finishes cleanup', client.close(wait=True))
    report['host_session'] = client.session_receipt()
    check('clean host receipt preserved after close', report['host_session']['status'] == 'complete')
    check('SDK bytes unchanged across all authored changes', before == {
        path.relative_to(args.host.parent.parent).as_posix(): sha(path)
        for path in args.host.parent.parent.rglob('*') if path.is_file()})
    scene.render.engine = 'BLENDER_EEVEE'
    bpy.ops.preferences.addon_disable(module=module_name)
    check('extension unload removes viewport timer', not bpy.app.timers.is_registered(engine_module.redraw))
    check('extension unload removes viewport properties', not hasattr(bpy.types.Scene, 'lum_author_preview_host'))
    report['passed'] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('archive', 'output', 'python', 'service', 'toolchain', 'host', 'manifest', 'isolation-root'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(exist_ok=False)
    report = {'passed': False, 'visual_approved': False, 'performance_qualified': False,
              'scope': 'Installed Blender static RenderEngine and actual host; additional lifecycle/multiview gates remain',
              'blender': bpy.app.version_string, 'blender_sha256': sha(bpy.app.binary_path),
              'archive_sha256': sha(args.archive), 'probe_sha256': sha(__file__), 'checks': []}
    def save():
        (args.output / 'receipt.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    def check(name, value, **details):
        report['checks'].append({'name': name, 'passed': bool(value), **details})
        save()
        if not value:
            raise AssertionError(name)
    save()
    steps = run(args, report, check)
    def tick():
        try:
            return next(steps)
        except StopIteration:
            save()
        except BaseException:
            report['error'] = traceback.format_exc()
            save()
            print(report['error'], flush=True)
        bpy.ops.wm.quit_blender()
        return None
    bpy.app.timers.register(tick, first_interval=.5)


if __name__ == '__main__':
    main()
