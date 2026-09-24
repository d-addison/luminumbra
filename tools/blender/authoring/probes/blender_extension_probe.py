"""Install the real mock ZIP and exercise its operators in isolated Blender."""
import argparse
import importlib
import json
from pathlib import Path
import sys
import time

import bpy


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--python', required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    args.output.mkdir()
    results = []
    report = {'mode': 'MOCK_NATIVE_BLENDER_QUALIFICATION', 'blender': bpy.app.version_string,
              'background': bpy.app.background, 'engine_rendered': False, 'checks': results}
    def check(name, condition, **details):
        results.append({'check': name, 'passed': bool(condition), **details})
        (args.output / 'extension-receipt.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        if not condition:
            raise AssertionError(name + ': ' + str(details))
    check('exact Blender profile', bpy.app.version == (5, 1, 0))
    repos = bpy.context.preferences.extensions.repos
    # Only transient factory-startup preferences in this isolated process.
    while repos:
        repos.remove(repos[0])
    repository = args.output / 'extension-repository'
    repository.mkdir()
    repo = repos.new(name='Luminumbra isolated probe', module='luminumbra_probe',
                     custom_directory=str(repository), remote_url='', source='USER')
    result = bpy.ops.extensions.package_install_files('EXEC_DEFAULT', filepath=str(args.archive),
                                                       repo=repo.module, enable_on_install=True)
    check('install archive into isolated repository', result == {'FINISHED'})
    name = 'bl_ext.luminumbra_probe.luminumbra_author_mock'
    extension = importlib.import_module(name)
    check('registered panel and timer', hasattr(bpy.types, 'LUMINUMBRA_PT_mock')
          and bpy.app.timers.is_registered(extension.timer))
    project = args.output / 'project'
    project.mkdir()
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    asset = {'schema': 'luminumbra.authoring.asset.v1', 'asset_id': 'fixture.prop', 'revision': 1,
             'profile': 'MOCK-contract-fixture-v1', 'dependencies': [], 'objects': [
                 {'id': 'fixture.root', 'parent': None, 'asset_id': 'fixture.mesh',
                  'transform': identity, 'components': []}]}
    (project / 'asset.json').write_text(json.dumps(asset), encoding='utf-8')
    scene = bpy.context.scene
    scene.lum_mock_python = args.python
    scene.lum_mock_project = str(project)
    def operate(op):
        answer = bpy.ops.luminumbra.mock(operation=op)
        check('operator ' + op, answer == {'FINISHED'})
        if extension._session:
            deadline = time.monotonic() + 8
            while extension._session.pending and time.monotonic() < deadline:
                extension._session.poll()
                time.sleep(0.02)
            check('response ' + op, extension._session.pending is None, status=extension._session.status)
    operate('start')
    first_process = extension._session.process
    operate('validate')
    check('validation succeeded', extension._session.status == 'MOCK — draft sidecar validated')
    operate('build')
    for _ in range(3):
        operate('job.status')
    check('build publishes only MOCK generation', extension._session.status == 'MOCK — succeeded')
    published = json.loads((project / '.mock-author/current.json').read_text())
    operate('preview.open')
    check('preview visibly describes metadata only', 'no rendered image' in extension._session.status)
    operate('build')
    operate('job.cancel')
    check('cancellation preserves current generation', extension._session.status == 'MOCK — cancelled'
          and published == json.loads((project / '.mock-author/current.json').read_text()))
    document = args.output / 'isolated.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(document))
    bpy.ops.wm.open_mainfile(filepath=str(document))
    check('file switch shuts down worker', extension._session is None and first_process.poll() is not None)
    operate('start')
    second_process = extension._session.process
    bpy.ops.preferences.addon_disable(module=name)
    check('unload shuts down worker and unregisters timer', second_process.poll() is not None
          and not bpy.app.timers.is_registered(extension.timer))
    check('unload removes scene properties', not hasattr(bpy.types.Scene, 'lum_mock_project'))
    bpy.ops.preferences.addon_enable(module=name)
    check('reload registers without stale session', extension._session is None
          and bpy.app.timers.is_registered(extension.timer))
    def finish():
        bpy.ops.preferences.addon_disable(module=name)
        report['passed'] = all(item['passed'] for item in results)
        (args.output / 'extension-receipt.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if bpy.app.background:
        finish()
    else:
        # Let the actual UI draw after startup; all Blender calls remain on its main thread.
        def capture():
            try:
                for area in bpy.context.window.screen.areas:
                    if area.type == 'VIEW_3D':
                        area.spaces.active.show_region_ui = True
                        area.tag_redraw()
                        region = next(r for r in area.regions if r.type == 'WINDOW')
                        with bpy.context.temp_override(window=bpy.context.window, area=area, region=region):
                            bpy.ops.wm.call_panel(name='LUMINUMBRA_PT_mock', keep_open=True)
                        break
                def screenshot():
                    try:
                        path = args.output / 'mock-panel.png'
                        bpy.ops.screen.screenshot(filepath=str(path))
                        check('interactive panel capture', path.exists(), path=path.name)
                        finish()
                    finally:
                        bpy.ops.wm.quit_blender()
                    return None
                bpy.app.timers.register(screenshot, first_interval=1.0)
            except Exception as error:
                results.append({'check': 'interactive UI', 'passed': False, 'error': str(error)})
                finish()
                bpy.ops.wm.quit_blender()
            return None
        bpy.app.timers.register(capture, first_interval=1.0)


if __name__ == '__main__':
    main()
