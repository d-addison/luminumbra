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
import zipfile

import bpy
from mathutils import Euler


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def remember(owned):
    module = owned.get('engine_module')
    for engine in tuple(module._engines) if module else ():
        if engine.client:
            owned['clients'][id(engine.client)] = engine.client
        if engine.reader:
            owned['readers'][id(engine.reader)] = engine.reader
    extension = owned.get('extension')
    if extension and extension._session:
        session = extension._session
        owned['builders'][session.process.pid] = session


def retain(client, check, report):
    status, retained = client.status, client.session_receipt()
    captured = [entry['frame']['header'] for entry in report['captures']
                if entry['frame']['header']['session'] == status['session']]
    if retained is None and not captured and status['state'] == 'closed' and status['error'] is None and status['last_result'] is None:
        check('unused client closed before host startup', status['closed'] and
              status['broker_pid'] is None and status['session_directory'] is None)
        return {'status': 'closed_before_host_start', 'session': status['session']}
    check('closed owned client has reaped broker/host', status['closed'] and
          status['broker_pid'] is None and status['session_directory'] is None and
          status['last_result'] is not None and status['last_result'].get('child_reaped') is True and
          status['last_result'].get('status') == 'stopped', status=status)
    check('retained authenticated host receipt is complete', retained is not None and
          retained['status'] == 'complete' and retained['failure'] is None)
    native, raw = retained['receipt'], retained['receipt_raw']
    check('retained raw receipt hash and session join',
          hashlib.sha256(raw.encode('utf-8')).hexdigest() == retained['receipt_sha256'] and
          json.loads(raw) == native and native['session'] == retained['session'] == status['session'] and
          native['shutdown_complete'] is True and native['source_dirty'] is False)
    if captured:
        last = max(captured, key=lambda header: header['sequence'])
        check('retained native receipt includes final captured frame', any(
            item['wire_header'] == last and item['planes_sha256'] == last['planes_sha256'] and
            item['actual_view'] == last['state']['view'] and
            item['actual_projection'] == last['state']['projection']
            for item in native.get('recent_frames', [])) and native['last_sequence'] > last['sequence'])
    report.setdefault('host_sessions', {})[status['session']] = retained
    return retained


def run(args, report, check, owned):
    import gpu
    report['inputs_before'] = {name: sha(getattr(args, name)) for name in
                              ('archive', 'python', 'service', 'toolchain', 'host', 'manifest')}
    report['archive_sha256'] = report['inputs_before']['archive']
    report['blender_sha256'], report['probe_sha256'] = sha(bpy.app.binary_path), sha(__file__)
    report['blender_build_hash'] = bpy.app.build_hash.decode('ascii')
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
    bpy.context.preferences.edit.use_global_undo = True
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
    owned.update(extension=extension, engine_module=engine_module, module_name=module_name)
    installed = Path(extension.__file__).parent.resolve()
    with zipfile.ZipFile(args.archive) as archive:
        members = {name: hashlib.sha256(archive.read(name)).hexdigest() for name in archive.namelist()
                   if not name.endswith('/')}
    check('installed extension files join actual archive', installed.is_relative_to(repository) and
          all(sha(installed / name) == digest for name, digest in members.items()))
    report['installed_extension'] = {'directory': str(installed), 'files': members}
    _, report['installation'] = installation.verify_installation(args.manifest, args.host, time.monotonic() + 30)
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
        remember(owned)
        deadline = time.monotonic() + 100
        while extension._session.state.busy and time.monotonic() < deadline:
            yield .03
        generation = extension._session.state.generation
        check('published distinct native generation', not extension._session.state.busy and generation and
              generation != previous, status=extension._session.state.status)
        directory = project / '.luminumbra-author/generations' / generation
        report.setdefault('generations', {})[generation] = {
            path.name: sha(path) for path in directory.iterdir() if path.is_file()}
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

    def current(engine, target):
        if engine.client is None or engine.presented_frame is None or engine.presentation is None:
            return False
        actual_scene = bpy.context.scene
        actual_collection = actual_scene.lum_author_collection
        configured = (actual_collection.as_pointer(), actual_collection['luminumbra.asset_id'],
            *(bpy.path.abspath(getattr(actual_scene, 'lum_author_' + name)) for name in
              ('project', 'python', 'preview_host', 'preview_manifest')), actual_scene.lum_author_generation)
        if engine.config != configured:
            return False
        target_region = next(part for part in target.regions if part.type == 'WINDOW')
        target_view = target.spaces.active.region_3d
        camera = engine_module.camera(engine_module.matrix(target_view.view_matrix),
                                      engine_module.matrix(target_view.window_matrix),
                                      target_region.width, target_region.height)
        header, status = engine.presented_frame[0], engine.client.status
        state = header['state']
        if (status['state'] != 'ready' or status['session'] != header['session'] or
                status['sequence'] != header['sequence'] or engine.desired != state or
                any(state[key] != camera[key] for key in
                    ('width', 'height', 'near_plane', 'far_plane', 'view', 'projection')) or
                engine.presentation != camera):
            return False
        metadata = engine.reader.poll()
        if metadata is None or any(state[key] != metadata[key] for key in ('generation_id', 'manifest_sha256')):
            return False
        with bpy.context.temp_override(window=window, area=target, region=target_region):
            return state['locals'] == engine.snapshot(bpy.context.scene.lum_author_collection,
                                                       metadata, bpy.context.evaluated_depsgraph_get())

    def frame(name, previous=None, expected_generation=None, target=None):
        target = target or area
        deadline = time.monotonic() + 45
        found = None
        while time.monotonic() < deadline:
            remember(owned)
            for engine in tuple(engine_module._engines):
                if current(engine, target):
                    header = engine.presented_frame[0]
                    identity = (header['session'], header['sequence'])
                    if identity != previous and (expected_generation is None or
                                                 header['state']['generation_id'] == expected_generation):
                        found = engine
                        break
            if found:
                break
            target.tag_redraw()
            yield .03
        errors = [engine.error for engine in tuple(engine_module._engines)]
        check('present actual frame: ' + name, found is not None, errors=errors)
        receipt = found.capture_frame(args.output / name)
        check('native frame has coverage: ' + name, any(found.presented_frame[1][8 * (
              found.presented_frame[0]['state']['width'] * found.presented_frame[0]['state']['height']):]))
        report['captures'].append({'name': name, 'frame': receipt, 'client': found.client.status})
        target_region = next(part for part in target.regions if part.type == 'WINDOW')
        report['captures'][-1]['area'] = {'pointer': target.as_pointer(),
            'rectangle': [target.x, target.y, target.width, target.height],
            'region': [target_region.x, target_region.y, target_region.width, target_region.height],
            'presentation': found.presentation}
        with bpy.context.temp_override(window=window, area=target, region=target_region):
            bpy.ops.wm.screenshot(filepath=str(args.output / (name + '-editor.png')))
        check('original editor capture: ' + name, (args.output / (name + '-editor.png')).is_file())
        report['captures'][-1]['editor_sha256'] = sha(args.output / (name + '-editor.png'))
        return found, (receipt['header']['session'], receipt['header']['sequence'])

    engine, identity = yield from frame('initial', expected_generation=generation)
    first = engine.presented_frame[0]['planes_sha256']
    original_id = obj['luminumbra.object_id']
    obj['luminumbra.object_id'] = 'temporarily.invalid'
    bpy.context.view_layer.update()
    deadline = time.monotonic() + 3
    while engine.presentation is not None and time.monotonic() < deadline:
        yield .03
    check('invalid snapshot clears presentation', engine.presentation is None)
    obj['luminumbra.object_id'] = original_id
    bpy.context.view_layer.update()
    engine, identity = yield from frame('unchanged-state-recovery', identity, generation)
    check('identical restored snapshot is requested again without camera motion',
          engine.presented_frame[0]['planes_sha256'] == first)
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

    # Split the actual editor; each RenderEngine must own a different client and camera.
    remember(owned)
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('split actual Blender viewport', bpy.ops.screen.area_split(
            'EXEC_DEFAULT', direction='VERTICAL', factor=.5) == {'FINISHED'})
    yield .2
    views = sorted((item for item in window.screen.areas if item.type == 'VIEW_3D'), key=lambda item: item.x)
    check('two simultaneous viewport areas exist', len(views) == 2)
    area, other = views
    region = next(part for part in area.regions if part.type == 'WINDOW')
    space, other_space = area.spaces.active, other.spaces.active
    other_space.region_3d.view_rotation = Euler((math.pi / 2, 0, math.pi / 4)).to_quaternion()
    other_space.region_3d.view_distance = 7.
    other_space.region_3d.view_location = (.1, 0, 0)
    other_space.region_3d.view_perspective = 'PERSP'
    other_space.shading.type = 'RENDERED'
    engine, identity = yield from frame('parallel-left', expected_generation=generation)
    right, right_identity = yield from frame('parallel-right', expected_generation=generation, target=other)
    check('simultaneous viewports own independent clients and cameras', engine is not right and
          engine.client is not right.client and engine.reader is not right.reader and
          identity[0] != right_identity[0] and
          engine.client.status['broker_pid'] != right.client.status['broker_pid'] and
          engine.presented_frame[0]['state']['view'] != right.presented_frame[0]['state']['view'])
    left_before = engine.presented_frame[0].copy()
    other_space.region_3d.view_location = (.3, -.2, .1)
    other_space.region_3d.view_distance = 5.5
    right, right_identity = yield from frame('parallel-right-pan-zoom', right_identity, generation, other)
    engine, identity = yield from frame('parallel-left-unchanged', expected_generation=generation)
    check('right camera update does not submit or alter left frame', engine.presented_frame[0] == left_before)

    old_extents = [(next(part for part in item.regions if part.type == 'WINDOW').width,
                    next(part for part in item.regions if part.type == 'WINDOW').height) for item in views]
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('move actual shared viewport border', bpy.ops.screen.area_move('EXEC_DEFAULT',
            x=other.x, y=other.y + other.height // 2, delta=64) == {'FINISHED'})
    yield .2
    new_extents = [(next(part for part in item.regions if part.type == 'WINDOW').width,
                    next(part for part in item.regions if part.type == 'WINDOW').height) for item in views]
    check('actual viewport resize changes both region widths',
          all(old[0] != new[0] for old, new in zip(old_extents, new_extents)),
          before=old_extents, after=new_extents)
    engine, identity = yield from frame('parallel-left-resized', identity, generation)
    right, right_identity = yield from frame('parallel-right-resized', right_identity, generation, other)

    # Capture native Blender overlay pixels around a real click-selected object.
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    projected = location_3d_to_region_2d(region, space.region_3d, obj.matrix_world.translation)
    check('authored object projects inside left viewport', projected is not None and
          24 < projected.x < region.width - 24 and 24 < projected.y < region.height - 24)
    point = (round(projected.x), round(projected.y))
    space.overlay.show_overlays = True
    space.show_gizmo = False
    with bpy.context.temp_override(window=window, area=area, region=region):
        bpy.ops.object.select_all(action='DESELECT')

    def editor_pixels(name):
        area.tag_redraw()
        yield .2
        check('overlay capture still joins current native frame: ' + name, current(engine, area))
        path = args.output / (name + '.png')
        with bpy.context.temp_override(window=window, area=area, region=region):
            check('capture actual editor overlay: ' + name, bpy.ops.screen.screenshot_area(
                filepath=str(path), hide_props_region=False) == {'FINISHED'})
        image = bpy.data.images.load(str(path), check_existing=False)
        try:
            import numpy as np
            width, height = image.size
            check('editor capture dimensions join area', (width, height) == (area.width, area.height))
            pixels = np.empty(width * height * 4, dtype=np.float32)
            image.pixels.foreach_get(pixels)
            x, y = point[0] + region.x - area.x, point[1] + region.y - area.y
            box = (max(0, x - 100), max(0, y - 100), min(width, x + 101), min(height, y + 101))
            cropped = pixels.reshape(height, width, 4)[box[1]:box[3], box[0]:box[2]].copy()
        finally:
            bpy.data.images.remove(image)
        report.setdefault('overlay_captures', []).append({'name': name, 'file': path.name,
            'sha256': sha(path), 'crop': box, 'selected_ids': sorted(
                item.get('luminumbra.object_id', item.name) for item in bpy.context.selected_objects),
            'native_header': engine.presented_frame[0]})
        return cropped

    overlay_planes = engine.presented_frame[0]['planes_sha256']
    deselected = yield from editor_pixels('selection-none')
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('Blender selects authored object by viewport hit', bpy.ops.view3d.select(
            'EXEC_DEFAULT', location=point, deselect_all=True) == {'FINISHED'})
    check('selection hit preserves supported object identity', obj.select_get() and
          bpy.context.view_layer.objects.active == obj and obj['luminumbra.object_id'] == original_id)
    selected = yield from editor_pixels('selection-outline')
    check('actual selection overlay changes local editor pixels', bool((selected != deselected).any()))
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('activate native translation gizmo', bpy.ops.wm.tool_set_by_id(name='builtin.move') == {'FINISHED'})
    space.show_gizmo = space.show_gizmo_tool = space.show_gizmo_object_translate = True
    gizmo = yield from editor_pixels('selection-translation-gizmo')
    check('actual translation gizmo changes local editor pixels', bool((gizmo != selected).any()))
    check('selection and gizmos retain the native render planes', engine.presented_frame[0]['planes_sha256'] == overlay_planes)

    # Explicit Blender undo states avoid reliance on a Python operator's implicit undo push.
    before_undo = engine.presented_frame[0]['planes_sha256']
    before_location = tuple(obj.location)
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('push pre-edit Blender undo state', bpy.ops.ed.undo_push(message='Viewport before edit') == {'FINISHED'})
    obj.location.x += .3
    bpy.context.view_layer.update()
    engine, identity = yield from frame('undo-transform-edited', identity, generation)
    check('undo control changes actual installed output', engine.presented_frame[0]['planes_sha256'] != before_undo)
    remember(owned)
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('push edited Blender undo state', bpy.ops.ed.undo_push(message='Viewport edited') == {'FINISHED'})
        check('execute actual Blender undo', bpy.ops.ed.undo() == {'FINISHED'})
    # Blender undo replaces datablocks. Never dereference the old object/material/scene.
    scene = bpy.context.scene
    collection = scene.lum_author_collection
    views = sorted((item for item in window.screen.areas if item.type == 'VIEW_3D'), key=lambda item: item.x)
    check('undo preserves two actual viewport areas', len(views) == 2)
    area, other = views
    region = next(part for part in area.regions if part.type == 'WINDOW')
    space, other_space = area.spaces.active, other.spaces.active
    obj = next(item for item in collection.all_objects if item.get('luminumbra.object_id') == original_id)
    material = obj.data.materials[0]
    principled = next(node for node in material.node_tree.nodes if node.type == 'BSDF_PRINCIPLED')
    bpy.context.view_layer.update()
    check('undo restores actual authored transform', tuple(obj.location) == before_location)
    engine, identity = yield from frame('undo-transform-restored', identity, generation)
    check('undo restores installed output without executable changes', engine.presented_frame[0]['planes_sha256'] == before_undo)

    # Hold A while the service publishes B, then restart hosts and prove A is loaded again.
    remember(owned)
    scene.lum_author_generation = generation
    engine, identity = yield from frame('held-generation-initial', identity, generation)
    right, right_identity = yield from frame('held-generation-right', expected_generation=generation, target=other)
    before_material = engine.presented_frame[0]['planes_sha256']
    principled.inputs['Base Color'].default_value = (.02, .12, .8, 1)
    bpy.context.view_layer.update()
    held_generation = generation
    generation = yield from build(held_generation)
    check('new current pointer differs from held generation',
          json.loads((project / '.luminumbra-author/current.json').read_text())['job_id'] == generation and
          scene.lum_author_generation == held_generation)
    remember(owned)
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('restart hosts while holding old generation', bpy.ops.luminumbra.viewport(operation='reset') == {'FINISHED'})
    engine, identity = yield from frame('held-generation-after-publication', identity, held_generation)
    right, right_identity = yield from frame('held-generation-right-after-publication', right_identity, held_generation, other)
    check('held generation reloaded with original material bytes', engine.presented_frame[0]['planes_sha256'] == before_material)
    remember(owned)
    scene.lum_author_generation = ''
    engine, identity = yield from frame('material-refresh', identity, generation)
    check('material edit changes installed engine output', engine.presented_frame[0]['planes_sha256'] != before_material)
    right, right_identity = yield from frame('parallel-material-refresh', right_identity, generation, other)
    before_geometry = engine.presented_frame[0]['planes_sha256']
    for vertex in obj.data.vertices:
        if vertex.co.z > 0:
            vertex.co.x += .35
    obj.data.update()
    bpy.context.view_layer.update()
    generation = yield from build(generation)
    engine, identity = yield from frame('geometry-refresh', identity, generation)
    check('geometry edit changes installed engine output', engine.presented_frame[0]['planes_sha256'] != before_geometry)
    right, right_identity = yield from frame('parallel-geometry-refresh', right_identity, generation, other)
    client = engine.client
    old_session = client.status['session']
    broker_pid = client.status['broker_pid']
    check('crash target is the owned viewport broker', type(broker_pid) is int and broker_pid > 0)
    report['deliberate_crash'] = {'session': old_session, 'broker_pid': broker_pid,
                                'last_valid_header': engine.presented_frame[0],
                                'expected_outcome': 'owned broker termination and fresh-session recovery'}
    subprocess.run(['C:/Windows/System32/taskkill.exe', '/PID', str(broker_pid), '/F'],
                   check=True, timeout=10, stdout=subprocess.DEVNULL)
    engine, identity = yield from frame('crash-recovery', identity, generation)
    check('crash recovery creates a fresh authenticated session', client.status['session'] != old_session)
    report['feedback_samples_ms'] = list(engine.presentation_samples)
    report['feedback_scope'] = 'Submission to texture upload; compositor presentation timing remains unqualified'
    remember(owned)
    with bpy.context.temp_override(window=window, area=area, region=region):
        check('close installed viewport', bpy.ops.luminumbra.viewport(operation='close') == {'FINISHED'})
    check('owned client finishes cleanup', client.close(wait=True))
    report['host_session'] = retain(client, check, report)
    check('clean host receipt preserved after close', report['host_session']['status'] == 'complete')
    check('SDK bytes unchanged across all authored changes', before == {
        path.relative_to(args.host.parent.parent).as_posix(): sha(path)
        for path in args.host.parent.parent.rglob('*') if path.is_file()})
    # Reopen two live hosts, then disable the extension while both still render.
    for target in views:
        part = next(item for item in target.regions if item.type == 'WINDOW')
        with bpy.context.temp_override(window=window, area=target, region=part):
            check('reopen viewport before active unload', bpy.ops.luminumbra.viewport() == {'FINISHED'})
    engine, identity = yield from frame('before-active-unload-left', expected_generation=generation)
    right, right_identity = yield from frame('before-active-unload-right', expected_generation=generation, target=other)
    remember(owned)
    active = (engine.client, right.client)
    check('two authenticated sessions are active at extension unload',
          all(item.status['state'] == 'ready' and item.status['broker_pid'] for item in active) and
          active[0].status['session'] != active[1].status['session'] and
          all(item.spaces.active.shading.type == 'RENDERED' for item in views))
    check('disable actual installed extension while rendering',
          bpy.ops.preferences.addon_disable(module=module_name) == {'FINISHED'})
    owned['disabled'] = True
    check('extension unload removes viewport timer', not bpy.app.timers.is_registered(engine_module.redraw))
    check('extension unload removes viewport properties', not hasattr(bpy.types.Scene, 'lum_author_preview_host'))
    check('extension unload removes authoring timer and callbacks',
          not bpy.app.timers.is_registered(extension.timer) and
          extension.file_changed not in bpy.app.handlers.load_pre and
          extension.edited not in bpy.app.handlers.depsgraph_update_post and
          extension.undone not in bpy.app.handlers.undo_post and extension.undone not in bpy.app.handlers.redo_post)
    check('extension unload clears all presented frames', all(
        item.client is None and item.reader is None and item.presentation is None and item.presented_frame is None
        for item in tuple(engine_module._engines)))
    for item in owned['clients'].values():
        check('retired viewport client cleanup completes', item.close(wait=True, timeout=15))
        retain(item, check, report)
    check('metadata readers and geometry brokers stop on unload',
          all(item.status()['state'] == 'closed' and not item._thread.is_alive() for item in owned['readers'].values()) and
          extension._session is None and all(item.process.poll() is not None for item in owned['builders'].values()))
    check('published immutable generations remain byte-identical', all(
        sha(project / '.luminumbra-author/generations' / generation / name) == digest
        for generation, files in report['generations'].items() for name, digest in files.items()))
    report['checks_complete'] = True


def cleanup(owned):
    errors = []
    try:
        remember(owned)
    except Exception:
        errors.append(traceback.format_exc())
    module, extension = owned.get('engine_module'), owned.get('extension')
    if module:
        try:
            module.stop_all()
        except Exception:
            errors.append(traceback.format_exc())
    for client in owned['clients'].values():
        client.close()
    if extension and not owned.get('disabled'):
        try:
            bpy.ops.preferences.addon_disable(module=owned['module_name'])
        except Exception:
            errors.append(traceback.format_exc())
    deadline = time.monotonic() + 15
    for client in owned['clients'].values():
        if not client.close(wait=True, timeout=max(0, deadline - time.monotonic())):
            errors.append('Owned viewport client did not stop within cleanup deadline')
    for reader in owned['readers'].values():
        reader.close()
        if reader._thread.is_alive():
            errors.append('Owned metadata reader did not stop')
    for builder in owned['builders'].values():
        if builder.process.poll() is None:
            try:
                builder.close()
            except Exception:
                errors.append(traceback.format_exc())
    return {'passed': not errors, 'errors': errors, 'clients': [item.status for item in owned['clients'].values()],
            'retained_sessions': [item.session_receipt() for item in owned['clients'].values()],
            'geometry_brokers': {str(pid): item.process.poll() for pid, item in owned['builders'].items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('archive', 'output', 'python', 'service', 'toolchain', 'host', 'manifest', 'isolation-root'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=420,
                        help='Main-thread campaign deadline; external owned-process deadline must exceed this by 60 seconds')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    if not 30 <= args.timeout <= 900:
        parser.error('--timeout must be in 30..900 seconds')
    for name in vars(args).keys() - {'timeout'}:
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(exist_ok=False)
    report = {'passed': False, 'visual_approved': False, 'performance_qualified': False,
              'schema': 'luminumbra.blender_viewport_engine.v1',
              'scope': 'Installed static RenderEngine with two viewports, native overlays, undo and generation lifecycle',
              'blender': bpy.app.version_string, 'blender_executable': bpy.app.binary_path,
              'checks': [], 'timeout_seconds': args.timeout, 'process_id': os.getpid()}
    def save():
        (args.output / 'receipt.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    def check(name, value, **details):
        report['checks'].append({'name': name, 'passed': bool(value), **details})
        save()
        if not value:
            raise AssertionError(name)
    save()
    started = time.monotonic()
    owned = {'clients': {}, 'readers': {}, 'builders': {}}
    steps = run(args, report, check, owned)
    def tick():
        try:
            remember(owned)
            if time.monotonic() - started >= args.timeout:
                raise TimeoutError('Installed Blender acceptance campaign deadline')
            return next(steps)
        except StopIteration:
            pass
        except BaseException:
            report['error'] = traceback.format_exc()
            save()
            print(report['error'], flush=True)
        try:
            report['cleanup'] = cleanup(owned)
        except BaseException:
            report['cleanup'] = {'passed': False, 'error': traceback.format_exc()}
        try:
            after = {name: sha(getattr(args, name)) for name in report.get('inputs_before', {})}
            check('installed input identities remain fixed through cleanup', after == report.get('inputs_before') and
                  report.get('blender_sha256') == sha(bpy.app.binary_path) and report.get('probe_sha256') == sha(__file__))
            check('installed extension members remain fixed', all(
                sha(Path(report['installed_extension']['directory']) / name) == digest
                for name, digest in report['installed_extension']['files'].items()))
            report['sdk_after'] = {path.relative_to(args.host.parent.parent).as_posix(): sha(path)
                for path in args.host.parent.parent.rglob('*') if path.is_file()}
            check('complete SDK roster is unchanged after unload and cleanup', report['sdk_after'] == report['sdk_before'])
            report['inputs_after'] = after
        except BaseException:
            report['error'] = report.get('error', '') + traceback.format_exc()
        report['elapsed_seconds'] = time.monotonic() - started
        report['passed'] = (report.get('checks_complete') is True and not report.get('error') and
                            report['cleanup']['passed'] and report['elapsed_seconds'] < args.timeout)
        save()
        bpy.ops.wm.quit_blender()
        return None
    bpy.app.timers.register(tick, first_interval=.5)


if __name__ == '__main__':
    main()
