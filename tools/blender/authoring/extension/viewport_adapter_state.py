"""Main-thread snapshot math; no Blender, GPU, process or filesystem access."""
from . import viewport_math as vm


def camera(view, projection, width, height):
    """Convert an observed OpenGL Blender camera to the finite host profile.

    A subpixel presentation rectangle compensates integer downsampling without
    moving authored geometry relative to Blender's selection and gizmo pixels.
    Orthographic Blender clip intervals may straddle the camera origin; translate
    the host camera along its view axis to retain that same visible interval.
    """
    view, original = vm.matrix(view), vm.matrix(projection)
    if original[0] <= 0 or original[5] <= 0:
        raise vm.UnsupportedView("Blender camera requires positive projection scales")
    if type(width) is not int or type(height) is not int or not 1 <= width <= 32768 or not 1 <= height <= 32768:
        raise vm.UnsupportedView("Invalid Blender region extent")
    scale = min(1., 1280 / width, 720 / height)
    target_width, target_height = max(1, round(width * scale)), max(1, round(height * scale))
    converted = list(original)
    if original[11] == 0 and original[15] == 1:
        if original[10] >= 0:
            raise vm.UnsupportedView("Expected Blender OpenGL orthographic depth")
        near = (original[14] + 1) / original[10]
        far = (original[14] - 1) / original[10]
        shift = max(0., .001 - near)
    elif original[11] == -1 and original[15] == 0:
        if original[10] >= -1:
            raise vm.UnsupportedView("Expected finite Blender OpenGL perspective depth")
        near = original[14] / (original[10] - 1)
        far = original[14] / (original[10] + 1)
        shift = 0.
    else:
        raise vm.UnsupportedView("Unsupported Blender camera projection")
    host_near, host_far = max(.001, near + shift) if shift else near, far + shift
    if not .001 <= host_near < host_far <= 1e6:
        raise vm.UnsupportedView("Blender clip interval exceeds installed viewport limits")
    converted[0] = original[5] / (target_width / target_height)
    ratio = converted[0] / original[0]
    converted[12] *= ratio
    adjusted = list(vm.to_engine_view(view))
    adjusted[14] -= shift
    host_projection = vm.projection(converted, host_near, host_far, target_width, target_height)
    host_view = vm.float_matrix(adjusted)
    # Map a host homogeneous depth sample to Blender's actual clip coordinates.
    depth_transform = vm.multiply(vm.multiply(original, vm.to_engine_view(view)),
                                  vm.inverse(vm.multiply(host_projection, host_view)))
    drawn_width = width / ratio
    return dict(width=target_width, height=target_height, near_plane=host_near,
                far_plane=host_far, view=list(host_view), projection=list(host_projection),
                rectangle=((width - drawn_width) / 2, 0., drawn_width, float(height)),
                depth_transform=depth_transform)


class Revisions:
    """Session-monotonic revisions survive generation replacement and coalescing."""
    def __init__(self):
        self.previous = None
        self.scene = self.camera = 0

    def state(self, generation, camera, locals_):
        result = {name: camera[name] for name in
                  ('width', 'height', 'near_plane', 'far_plane', 'view', 'projection')}
        result.update(generation_id=generation['generation_id'], manifest_sha256=generation['manifest_sha256'],
                      locals=locals_)
        if self.previous:
            self.scene += any(result[name] != self.previous[name] for name in
                              ('generation_id', 'manifest_sha256', 'locals'))
            self.camera += any(result[name] != self.previous[name] for name in
                               ('width', 'height', 'near_plane', 'far_plane', 'view', 'projection'))
        self.previous = result
        return dict(result, scene_revision=self.scene, camera_revision=self.camera)
