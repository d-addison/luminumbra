"""Portable camera/basis math for the explicitly qualified static viewport profile.

Matrices are column-major. This module never imports Blender or graphics APIs.
The caller must supply Blender's observed clip convention; it is not guessed.
"""
import math
import struct


class UnsupportedView(ValueError):
    pass


def matrix(values):
    if len(values) != 16 or any(type(x) not in (int, float) or not math.isfinite(x)
                                or abs(x) > 1e30 for x in values):
        raise UnsupportedView("Expected a finite column-major 4x4 matrix")
    return tuple(float(x) for x in values)


def multiply(a, b):
    a, b = matrix(a), matrix(b)
    return matrix(tuple(sum(a[k * 4 + row] * b[col * 4 + k] for k in range(4))
                        for col in range(4) for row in range(4)))


def inverse(value):
    value = matrix(value)
    rows = [[value[col * 4 + row] for col in range(4)] +
            [float(row == col) for col in range(4)] for row in range(4)]
    for col in range(4):
        pivot = max(range(col, 4), key=lambda row: abs(rows[row][col]))
        if abs(rows[pivot][col]) <= 1e-15:
            raise UnsupportedView("Singular viewport matrix")
        rows[col], rows[pivot] = rows[pivot], rows[col]
        divisor = rows[col][col]
        rows[col] = [item / divisor for item in rows[col]]
        for row in range(4):
            if row != col:
                scale = rows[row][col]
                rows[row] = [x - scale * y for x, y in zip(rows[row], rows[col])]
    return matrix(tuple(rows[row][col + 4] for col in range(4) for row in range(4)))


IDENTITY = tuple(float(row == col) for col in range(4) for row in range(4))
# Blender (x,y,z) -> glTF (x,z,-y). Native exporter fixture must qualify this basis.
BASIS = (1., 0., 0., 0., 0., 0., -1., 0., 0., 1., 0., 0., 0., 0., 0., 1.)
BASIS_INVERSE = inverse(BASIS)


def affine(value):
    value = matrix(value)
    if (value[3], value[7], value[11], value[15]) != (0., 0., 0., 1.):
        raise UnsupportedView("Static viewport transforms must be affine")
    inverse(value)
    return value


def to_engine_local(blender_local):
    return multiply(multiply(BASIS, affine(blender_local)), BASIS_INVERSE)


def to_engine_view(blender_view):
    return multiply(affine(blender_view), BASIS_INVERSE)


def local_from_world(world, parent_world=None):
    world = affine(world)
    return world if parent_world is None else multiply(inverse(affine(parent_world)), world)


def float_matrix(value):
    return tuple(struct.unpack("<16f", struct.pack("<16f", *matrix(value))))


def _parameters(near, far, width, height):
    if (type(width) is not int or type(height) is not int or
            not 1 <= width <= 1280 or not 1 <= height <= 720):
        raise UnsupportedView("Static viewport extent exceeds 1280 by 720")
    if (type(near) not in (int, float) or type(far) not in (int, float) or
            not math.isfinite(near) or not math.isfinite(far) or
            not .001 <= near < far <= 1e6):
        raise UnsupportedView("Unsupported viewport clip planes")


def _aspect(p, width, height):
    aspect = width / height
    if abs(p[5] / p[0] - aspect) > max(aspect, 1.) * 1e-6:
        raise UnsupportedView("Viewport projection and extent do not match")


def _orthographic_bounds(p):
    # The host receives float32 matrices. Admit the rounded representation of
    # the inclusive lower bound, matching RenderView exactly.
    lower = struct.unpack('<f', struct.pack('<f', 1e-6))[0]
    for scale, offset in ((p[0], p[12]), (p[5], p[13])):
        if not lower <= scale <= 1000 or abs(offset / scale) > 1e6:
            raise UnsupportedView("Orthographic span or center exceeds the static profile bounds")


def perspective(blender_projection, near, far, width, height):
    """Preserve x/y; replace only depth for an unshifted perspective camera.

    The source depth convention is independently provided to depth_to_blender.
    Orthographic views use orthographic() or the projection() dispatcher.
    """
    p = matrix(blender_projection)
    _parameters(near, far, width, height)
    if (p[11] != -1 or p[15] != 0 or p[0] <= 0 or p[5] <= 0 or
            any(p[i] != 0 for i in (1, 2, 3, 4, 6, 7, 8, 9, 12, 13))):
        raise UnsupportedView("Use an unshifted perspective view for static preview")
    if not (.01 < p[0] < 1000 and .01 < p[5] < 1000):
        raise UnsupportedView("Unsupported perspective field of view")
    _aspect(p, width, height)
    result = list(p)
    result[10] = near / (far - near)
    result[14] = far * near / (far - near)
    result = float_matrix(result)
    _aspect(result, width, height)
    if not (.01 < result[0] < 1000 and .01 < result[5] < 1000):
        raise UnsupportedView("Perspective field of view exceeds float32 profile bounds")
    return result


def orthographic(blender_projection, near, far, width, height):
    """Preserve axis-aligned orthographic scale/offset; replace only depth.

    Each span is 0.002..2000000 metres; each view-space center is within
    +/-1000000 metres. Oblique/sheared/projective cameras are refused.
    """
    p = matrix(blender_projection)
    _parameters(near, far, width, height)
    if (p[11] != 0 or p[15] != 1 or
            any(p[i] != 0 for i in (1, 2, 3, 4, 6, 7, 8, 9))):
        raise UnsupportedView("Use an axis-aligned orthographic view for static preview")
    _orthographic_bounds(p)
    _aspect(p, width, height)
    result = list(p)
    result[10] = 1 / (far - near)
    result[14] = far / (far - near)
    result = float_matrix(result)
    _orthographic_bounds(result)
    _aspect(result, width, height)
    return result


def projection(blender_projection, near, far, width, height):
    """Select the supported camera kind from its observed matrix, never guess depth."""
    p = matrix(blender_projection)
    convert = orthographic if p[11] == 0 and p[15] == 1 else perspective
    return convert(p, near, far, width, height)


def transform(value, point):
    value = matrix(value)
    if len(point) != 4 or not all(math.isfinite(x) for x in point):
        raise UnsupportedView("Invalid homogeneous point")
    return tuple(sum(value[col * 4 + row] * point[col] for col in range(4))
                 for row in range(4))


def depth_to_blender(host_projection, blender_projection, x_ndc, y_ndc, depth,
                     *, clip_zero_to_one):
    """Reference for GPU depth reprojection of a covered sample (not clear depth)."""
    if (type(clip_zero_to_one) is not bool or not all(math.isfinite(x) for x in
            (x_ndc, y_ndc, depth)) or not 0 < depth <= 1):
        raise UnsupportedView("Depth conversion needs a covered sample and declared clip convention")
    view = transform(inverse(host_projection), (x_ndc, y_ndc, depth, 1.))
    clip = transform(blender_projection, view)
    if clip[3] <= 0:
        raise UnsupportedView("Depth sample projects behind the Blender view")
    result = clip[2] / clip[3]
    return result if clip_zero_to_one else .5 * result + .5
