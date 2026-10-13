"""Blender 5.1 OpenGL presentation probe; run in an owned factory-startup GUI.

blender --factory-startup --python native_viewport_presentation.py -- \
    --modules PATH_TO_EXTENSION_MODULES --output FRESH_EVIDENCE_DIRECTORY

An external owner must impose an 80-second process deadline and check the final
receipt, including its passed flag. This script quits Blender from its main
thread timer. The 70-second emergency watchdog is a last resort, not proof that
the owner reaped the GUI. No bpy/gpu mocks or NumPy are used.

Inputs are declared synthetic synchronized frame planes. Actual Blender GPU
uploads, production FrameDraw shaders and framebuffer readbacks qualify only
the presentation boundary; native host IPC, RenderEngine integration, selection
and gizmo UI, and native GPU performance remain separate acceptance work.
"""
import argparse
import array
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import stat
import sys
import threading
import time
import traceback
import types

import bpy


MODULES = ('viewport_math.py', 'viewport_adapter_state.py', 'viewport_draw.py')
WIDTH, HEIGHT = 160, 120
STARTED = time.monotonic()
IDENTITY = (1.,0.,0.,0., 0.,1.,0.,0., 0.,0.,1.,0., 0.,0.,0.,1.)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def regular(path):
    path = path.absolute()
    for part in (*reversed(path.parents), path):
        info = part.lstat()
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                'Probe inputs cannot traverse symlinks or reparse points')
        require(stat.S_ISDIR(info.st_mode) if part != path else stat.S_ISREG(info.st_mode),
                'Probe input must be a regular file')
    return path


def digest(path):
    value = hashlib.sha256()
    with regular(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def atomic_json(path, value):
    temporary = path.with_name(path.name + '.' + str(threading.get_ident()) + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + '\n', encoding='utf-8')
    temporary.replace(path)


class Probe:
    def __init__(self, args):
        self.args = args
        require(args.output.is_absolute() and '..' not in args.output.parts,
                'Use a fresh absolute evidence directory')
        args.output.mkdir(parents=True, exist_ok=False)
        self.receipt_path = args.output / 'receipt.json'
        self.receipt = {'schema': 'luminumbra.blender_viewport_presentation.v1',
                        'passed': False, 'status': 'not_completed', 'failure': 'Probe has not completed',
                        'scope': 'Actual Blender GPU FrameDraw presentation of synthetic planes; not native host or RenderEngine acceptance',
                        'visual_approved': False, 'performance_qualified': False,
                        'process_id': os.getpid(), 'checks': [], 'cases': [], 'artifacts': {}}
        atomic_json(self.receipt_path, self.receipt)
        self.finished = threading.Event()
        self.watchdog = threading.Timer(70., self.timeout)
        self.watchdog.daemon = True
        self.watchdog.start()

    def timeout(self):
        if not self.finished.is_set():
            value = dict(self.receipt, passed=False, status='timeout', failure='70 second probe watchdog',
                         elapsed_seconds=time.monotonic() - STARTED)
            atomic_json(self.receipt_path, value)
            os._exit(124)

    def check(self, name, condition, **details):
        self.receipt['checks'].append({'name': name, 'passed': bool(condition), **details})
        require(condition, name)

    def artifact(self, name, payload, **description):
        (self.args.output / name).write_bytes(payload)
        result = {'path': name, 'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest(),
                  **description}
        self.receipt['artifacts'][name] = result
        return result

    def stage(self):
        self.check('GUI factory context and supported Blender version',
                   not bpy.app.background and bpy.app.version == (5, 1, 0) and
                   '--factory-startup' in sys.argv and threading.current_thread() is threading.main_thread())
        self.check('little-endian frame representation', sys.byteorder == 'little')
        import gpu
        backend = gpu.platform.backend_type_get()
        self.receipt['blender'] = {'version': list(bpy.app.version), 'version_string': bpy.app.version_string,
                                   'build_hash': bpy.app.build_hash.decode('utf-8', errors='replace'),
                                   'executable': bpy.app.binary_path,
                                   'executable_sha256': digest(Path(bpy.app.binary_path)),
                                   'backend': backend, 'vendor': gpu.platform.vendor_get(),
                                   'renderer': gpu.platform.renderer_get(), 'graphics_version': gpu.platform.version_get()}
        self.check('actual OpenGL backend has GPU identity', backend == 'OPENGL' and
                   all(self.receipt['blender'][name] for name in ('vendor', 'renderer', 'graphics_version')))
        self.receipt['probe_sha256'] = digest(Path(__file__))
        self.receipt['source_modules'] = {}
        staged = self.args.output / 'modules'
        staged.mkdir()
        for name in MODULES:
            source = regular(self.args.modules / name)
            require(source.stat().st_size <= 1024 * 1024, 'Bounded presentation module source')
            raw = source.read_bytes()
            (staged / name).write_bytes(raw)
            self.receipt['source_modules'][name] = {'path': str(source), 'sha256': hashlib.sha256(raw).hexdigest()}
        # A synthetic package path permits only these actual relative imports;
        # it never executes the extension's registration-heavy __init__.py.
        package = types.ModuleType('luminumbra_gpu_presentation_fixture')
        package.__path__ = [str(staged)]
        sys.modules[package.__name__] = package
        self.adapter = importlib.import_module(package.__name__ + '.viewport_adapter_state')
        self.draw_module = importlib.import_module(package.__name__ + '.viewport_draw')
        return staged

    def planes(self, width, height):
        rgba, depth, coverage = bytearray(), array.array('f'), bytearray()
        for y in range(height):
            for x in range(width):
                covered = not (x < width // 4 and y < height // 4)
                rgba.extend((32 + 48 * (x * 4 // width), 23 + 51 * (y * 4 // height),
                             191 - 27 * ((x * 4 // width + y * 4 // height) % 4), 255 if covered else 0))
                depth.append((.25 if x < width // 2 else .75) if covered else 0.)
                coverage.append(int(covered))
        return bytes(rgba), depth, bytes(coverage)

    @staticmethod
    def read(framebuffer, width, height):
        color = framebuffer.read_color(0, 0, width, height, 4, 0, 'UBYTE')
        color.dimensions = width * height * 4
        depth = framebuffer.read_depth(0, 0, width, height)
        depth.dimensions = width * height
        color, depth = bytes(color), array.array('f', depth)
        require(len(color) == width*height*4 and len(depth) == width*height and
                all(math.isfinite(value) and 0 <= value <= 1 for value in depth),
                'Framebuffer returned malformed or nonfinite planes')
        return color, depth

    def save_frame(self, name, color, depth, width, height):
        common = dict(width=width, height=height, origin='lower_left')
        self.artifact(name + '.rgba8', color, format='RGBA8', **common)
        self.artifact(name + '.depth.f32le', depth.tobytes(), format='float32_le_window_depth', **common)
        # A lossless RGB preview in normal top-left file order; the RGBA original
        # above retains every byte, including alpha and its native row order.
        rgb = bytearray()
        for y in range(height - 1, -1, -1):
            row = color[y*width*4:(y+1)*width*4]
            for x in range(width):
                rgb.extend(row[x*4:x*4+3])
        self.artifact(name + '.ppm', f'P6\n{width} {height}\n255\n'.encode() + rgb,
                      format='P6_RGB8', width=width, height=height, origin='upper_left')

    @staticmethod
    def expected_depth(projection, camera, host_depth):
        if host_depth == 0:
            return 1.
        # Independent scalar inversion for these axis-aligned fixtures. It does
        # not reuse adapter.depth_transform or the presentation shader formula.
        native = camera['projection']
        host_z = (-native[14] / (host_depth + native[10]) if native[11] == -1 else
                  (host_depth - native[14]) / native[10])
        original_z = host_z - camera['view'][14]
        return .5 * (projection[10] * original_z + projection[14]) / (
                     projection[11] * original_z + projection[15]) + .5

    def overlay_shader(self):
        import gpu
        info = gpu.types.GPUShaderCreateInfo()
        info.push_constant('MAT4', 'pixelToClip')
        info.push_constant('FLOAT', 'windowDepth')
        info.push_constant('VEC4', 'color')
        info.vertex_in(0, 'VEC2', 'pos')
        info.fragment_out(0, 'VEC4', 'outputColor')
        info.vertex_source('''void main() {
            gl_Position = pixelToClip * vec4(pos, 0.0, 1.0);
            gl_Position.z = (windowDepth * 2.0 - 1.0) * gl_Position.w;
        }''')
        info.fragment_source('void main() { outputColor = color; }')
        return gpu.shader.create_from_info(info)

    def overlay(self, shader, x, y, depth, color):
        import gpu
        from gpu_extras.batch import batch_for_shader
        shader.bind()
        shader.uniform_float('pixelToClip', gpu.matrix.get_projection_matrix() @ gpu.matrix.get_model_view_matrix())
        shader.uniform_float('windowDepth', depth)
        shader.uniform_float('color', tuple(v/255 for v in color))
        batch = batch_for_shader(shader, 'TRIS',
            {'pos': ((x-2, y-2), (x+3, y-2), (x+3, y+3), (x-2, y+3))},
            indices=((0,1,2), (2,3,0)))
        batch.draw(shader)

    def case(self, name, projection, width=WIDTH, height=HEIGHT):
        import gpu
        from mathutils import Matrix
        camera = self.adapter.camera(IDENTITY, projection, width, height)
        host_width, host_height = camera['width'], camera['height']
        rgba, host_depth, coverage = self.planes(host_width, host_height)
        payload = rgba + host_depth.tobytes() + coverage
        self.artifact(name + '-input.rgba8', rgba, width=host_width, height=host_height, origin='lower_left')
        self.artifact(name + '-input.depth.f32le', host_depth.tobytes(), width=host_width, height=host_height,
                      origin='lower_left', format='float32_le_reversed_zero_to_one')
        self.artifact(name + '-input.coverage.u8', coverage, width=host_width, height=host_height, origin='lower_left')
        result = {'name': name, 'original_projection': projection, 'camera': camera,
                  'fixture': 'Synthetic synchronized RGBA/depth/coverage planes', 'samples': []}
        self.receipt['cases'].append(result)
        offscreen = gpu.types.GPUOffScreen(width, height, format='RGBA8')
        draw = None
        try:
            with offscreen.bind(), gpu.matrix.push_pop(), gpu.matrix.push_pop_projection():
                gpu.state.viewport_set(0, 0, width, height)
                gpu.matrix.load_matrix(Matrix.Identity(4))
                gpu.matrix.load_projection_matrix(Matrix(((2/width,0,0,-1), (0,2/height,0,-1),
                                                          (0,0,1,0), (0,0,0,1))))
                framebuffer = gpu.state.active_framebuffer_get()
                gpu.state.color_mask_set(True, True, True, True)
                gpu.state.depth_mask_set(True)
                framebuffer.clear(color=(.1,.2,.3,1), depth=.125)
                initialized = framebuffer.read_depth(0, 0, 1, 1)
                initialized.dimensions = 1
                self.check(name + ': known preexisting depth initialized', initialized[0] == .125)
                gpu.state.blend_set('ALPHA')
                gpu.state.depth_test_set('LESS_EQUAL')
                gpu.state.depth_mask_set(False)
                before = (gpu.state.blend_get(), gpu.state.depth_test_get(), gpu.state.depth_mask_get(),
                          tuple(gpu.state.viewport_get()), gpu.matrix.get_projection_matrix().copy(),
                          gpu.matrix.get_model_view_matrix().copy())
                draw = self.draw_module.FrameDraw()
                draw.update({'state': camera}, payload)
                draw.draw(camera['rectangle'], camera['depth_transform'])
                after = (gpu.state.blend_get(), gpu.state.depth_test_get(), gpu.state.depth_mask_get(),
                         tuple(gpu.state.viewport_get()), gpu.matrix.get_projection_matrix().copy(),
                         gpu.matrix.get_model_view_matrix().copy())
                self.check(name + ': blend/depth/viewport/matrices restored', before == after,
                           state_before=list(before[:3]), state_after=list(after[:3]))
                color, depth = self.read(framebuffer, width, height)
                self.save_frame(name + '-presented', color, depth, width, height)
                rx, ry, rw, rh = camera['rectangle']
                points = [(width * a // 8, height * b // 8) for a in (1,3,5,7) for b in (1,3,5,7)]
                maximum_color_error, maximum_depth_error = 0, 0.
                for x,y in points:
                    sx = min(host_width-1, max(0, int(((x+.5-rx)/rw) * host_width)))
                    sy = min(host_height-1, max(0, int(((y+.5-ry)/rh) * host_height)))
                    source, target = sy*host_width+sx, y*width+x
                    expected_color = rgba[source*4:source*4+4]
                    expected_depth = self.expected_depth(projection, camera, host_depth[source])
                    actual_color = color[target*4:target*4+4]
                    maximum_color_error = max(maximum_color_error, *(abs(a-b) for a,b in zip(actual_color, expected_color)))
                    maximum_depth_error = max(maximum_depth_error, abs(depth[target]-expected_depth))
                    result['samples'].append({'pixel': [x,y], 'source_pixel': [sx,sy],
                                               'coverage': coverage[source], 'expected_rgba': list(expected_color),
                                               'actual_rgba': list(actual_color), 'expected_depth': expected_depth,
                                               'actual_depth': depth[target]})
                self.check(name + ': exact color orientation and output-space conversion', maximum_color_error <= 1,
                           maximum_byte_error=maximum_color_error, samples=len(points))
                self.check(name + ': reprojected depth and clear coverage', maximum_depth_error <= 2e-5,
                           maximum_absolute_error=maximum_depth_error, samples=len(points))
                if (host_width,host_height) == (width,height):
                    self.check(name + ': complete unscaled color plane joins input', color == rgba,
                               checked_pixels=width*height)
                # Clear pixels must overwrite the initial .125 depth with 1.
                clear_point = (width//8, height//8)
                covered_points = [(3*width//8, 3*height//8), (5*width//8, 5*height//8)]
                shader = self.overlay_shader()
                gpu.state.blend_set('NONE')
                gpu.state.depth_test_set('LESS')
                gpu.state.depth_mask_set(True)
                behind_color, front_color = (255,0,255,255), (0,255,255,255)
                expected_overlays = []
                for x,y in covered_points:
                    at = y*width+x
                    behind = min(.99, depth[at] + .05)
                    self.overlay(shader, x,y,behind,behind_color)
                    expected_overlays.append({'pixel': [x,y], 'surface_depth': depth[at], 'behind_depth': behind,
                                              'front_depth': max(.001, depth[at]-.05)})
                self.overlay(shader, *clear_point, .5, behind_color)
                behind_rgba, behind_depth = self.read(framebuffer, width, height)
                self.save_frame(name + '-behind-control', behind_rgba, behind_depth, width, height)
                for x,y in covered_points:
                    at=y*width+x
                    self.check(name + ': geometry behind surface remains occluded ' + str((x,y)),
                               behind_rgba[at*4:at*4+4] == color[at*4:at*4+4] and behind_depth[at] == depth[at])
                at=clear_point[1]*width+clear_point[0]
                self.check(name + ': geometry in clear coverage remains visible',
                           behind_rgba[at*4:at*4+4] == bytes(behind_color) and abs(behind_depth[at]-.5) <= 1e-6)
                for entry in expected_overlays:
                    self.overlay(shader, *entry['pixel'], entry['front_depth'], front_color)
                front_rgba, front_depth = self.read(framebuffer, width, height)
                self.save_frame(name + '-front-control', front_rgba, front_depth, width, height)
                for entry in expected_overlays:
                    x,y=entry['pixel']; at=y*width+x
                    self.check(name + ': geometry in front remains visible ' + str((x,y)),
                               front_rgba[at*4:at*4+4] == bytes(front_color) and
                               abs(front_depth[at]-entry['front_depth']) <= 1e-6)
                result['overlay_controls'] = expected_overlays
                # The previous overlay draws also verify all four color masks
                # were restored, since FrameDraw's depth pass disables them.
                draw = None
                shader = None
        finally:
            draw = None
            offscreen.free()

    def run(self):
        try:
            staged = self.stage()
            near,far,scale=.1,100.,2.414213562373095
            def perspective(width,height):
                return (scale*height/width,0,0,0,0,scale,0,0,0,0,-(far+near)/(far-near),-1,
                        0,0,-2*near*far/(far-near),0)
            self.case('perspective', perspective(WIDTH,HEIGHT))
            self.case('orthographic-negative-near',
                      (.5*HEIGHT/WIDTH,0,0,0,0,.5,0,0,0,0,-.002,0,-.2,.15,0,1))
            self.case('odd-downsample', perspective(1281,721),1281,721)
            self.check('source and staged module hashes stayed fixed', all(
                digest(Path(item['path'])) == item['sha256'] == digest(staged/name)
                for name,item in self.receipt['source_modules'].items()))
            self.check('executable and probe hashes stayed fixed',
                       digest(Path(bpy.app.binary_path)) == self.receipt['blender']['executable_sha256'] and
                       digest(Path(__file__)) == self.receipt['probe_sha256'])
            self.check('bounded completion', time.monotonic()-STARTED < 65.)
            self.receipt.update(passed=True, status='passed', failure='')
        except BaseException as error:
            self.receipt.update(passed=False, status='failed', failure=str(error), traceback=traceback.format_exc())
        finally:
            self.receipt['elapsed_seconds'] = time.monotonic()-STARTED
            self.receipt['exit_requested'] = 'bpy.ops.wm.quit_blender from main-thread timer'
            atomic_json(self.receipt_path, self.receipt)
            self.finished.set()
            self.watchdog.cancel()
            bpy.ops.wm.quit_blender()
        return None


if __name__ == '__main__':
    try:
        probe = Probe(arguments())
        bpy.app.timers.register(probe.run, first_interval=.25)
    except BaseException:
        traceback.print_exc()
        bpy.ops.wm.quit_blender()
