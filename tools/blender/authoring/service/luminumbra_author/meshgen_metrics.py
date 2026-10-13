"""Portable, bounded geometry observations and finisher decisions (metric v1).

Coordinates share the exported metres/Y-up frame. No Blender or research metric
module is imported. Native sampling/ray casting remains a game responsibility.
"""
import math
import struct

from .contracts import require, Refusal
from .formats import validate_glb
from .meshgen_backend import bounded_json
from .meshgen_jobs import raw_glb_container

METRIC_VERSION = 'luminumbra.meshgen.guard.v1'
UV_PAIR_LIMIT = 200000
METRIC_ELEMENT_LIMIT = 1000000  # fixed portable measurement work bound, not a quality threshold


def inspect_artifact(data, effective):
    """Inspect every consumed accessor; exported nodes must have applied transforms."""
    limits = effective['limits']
    require(len(data) <= limits['result_bytes'], 'glb.limit', 'Artifact exceeds its byte cap.')
    size = raw_glb_container(data, limits)
    bounded_json(data[20:20+size], depth=limits['json_depth'], items=limits['collection_items'],
                 max_bytes=limits['metadata_bytes'])
    doc = validate_glb(data)
    require(not any(doc.get(k) for k in ('skins', 'animations', 'extensionsUsed', 'extensionsRequired')),
            'glb.encoding', 'Only static uncompressed geometry is supported.')
    require(not doc.get('images') and not doc.get('textures'), 'glb.encoding', 'Geometry-only output must strip textures/images.')
    require(len(doc.get('materials', [])) <= (0 if effective['finish']['material']['mode'] == 'none' else 1),
            'glb.encoding', 'Geometry-only output permits only the neutral material.')
    pending = [doc]
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            require(not value.get('extensions'), 'glb.encoding', 'Extensions are unsupported.')
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    def collection(name):
        value = doc.get(name, [])
        require(isinstance(value, list) and len(value) <= limits['collection_items']
                and all(isinstance(v, dict) for v in value), 'glb.structure', 'Invalid collection.')
        return value
    accessors, views = collection('accessors'), collection('bufferViews')
    require(28+size <= len(data), 'glb.geometry', 'Missing binary geometry.')
    binary = memoryview(data)[28+size:]
    buffers = collection('buffers')
    require(len(buffers) == 1 and type(buffers[0].get('byteLength')) is int
            and 0 < buffers[0]['byteLength'] <= len(binary)
            and len(binary)-buffers[0]['byteLength'] <= 3, 'glb.bounds', 'Invalid buffer length.')
    binary = binary[:buffers[0]['byteLength']]
    expanded = 0
    def read(index, components, index_data=False):
        nonlocal expanded
        require(type(index) is int and 0 <= index < len(accessors), 'glb.bounds', 'Invalid accessor reference.')
        item = accessors[index]
        require('sparse' not in item and not item.get('normalized', False), 'glb.encoding', 'Unsupported accessor.')
        component = item.get('componentType')
        fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(component) if index_data else ('f' if component == 5126 else None)
        require(fmt is not None and item.get('type') == ('SCALAR' if index_data else 'VEC'+str(components)),
                'glb.encoding', 'Invalid accessor encoding.')
        n, view_id = item.get('count'), item.get('bufferView')
        require(type(n) is int and 0 < n <= limits['glb_accessor_elements']
                and type(view_id) is int and 0 <= view_id < len(views), 'glb.bounds', 'Invalid accessor bounds.')
        expanded += n
        require(expanded <= min(limits['glb_accessor_elements'], METRIC_ELEMENT_LIMIT), 'glb.limit', 'Aggregate accessor cap exceeded.')
        view = views[view_id]
        width = struct.calcsize('<'+fmt*components)
        start, length, offset, stride = view.get('byteOffset', 0), view.get('byteLength'), item.get('byteOffset', 0), view.get('byteStride', width)
        require(type(view.get('buffer')) is int and view['buffer'] == 0
                and all(type(v) is int and v >= 0 for v in (start, length, offset, stride))
                and width <= stride <= 252 and start+length <= len(binary)
                and offset+(n-1)*stride+width <= length, 'glb.bounds', 'Accessor escapes buffer.')
        values = []
        for i in range(n):
            value = struct.unpack_from('<'+fmt*components, binary, start+offset+i*stride)
            require(all(math.isfinite(v) for v in value), 'glb.nonfinite', 'Non-finite exported attribute.')
            values.append(value)
        return values
    positions, faces, uv_faces = [], [], []
    has_uv = has_normals = True
    meshes = collection('meshes')
    mesh_data = []
    for mesh in meshes:
        primitives = mesh.get('primitives')
        require(isinstance(primitives, list) and len(primitives) <= limits['collection_items'], 'glb.structure', 'Invalid primitives.')
        parts = []
        for primitive in primitives:
            require(isinstance(primitive, dict) and type(primitive.get('mode', 4)) is int and primitive.get('mode', 4) == 4 and not primitive.get('targets'),
                    'glb.encoding', 'Only static triangles are supported.')
            attrs = primitive.get('attributes')
            require(isinstance(attrs, dict) and 'POSITION' in attrs and set(attrs) <= {'POSITION', 'NORMAL', 'TEXCOORD_0'},
                    'glb.encoding', 'Unsupported attribute.')
            points = read(attrs['POSITION'], 3)
            uv = read(attrs['TEXCOORD_0'], 2) if 'TEXCOORD_0' in attrs else None
            normals = read(attrs['NORMAL'], 3) if 'NORMAL' in attrs else None
            require((uv is None or len(uv) == len(points)) and (normals is None or len(normals) == len(points)),
                    'glb.bounds', 'Attribute counts differ.')
            if normals is not None:
                require(all(sum(v*v for v in normal) > 0 for normal in normals), 'finish.gate_failed', 'Zero-length exported normal.')
            has_uv &= uv is not None
            has_normals &= normals is not None
            indices = [v[0] for v in read(primitive['indices'], 1, True)] if 'indices' in primitive else list(range(len(points)))
            require(len(indices) % 3 == 0 and all(v < len(points) for v in indices), 'glb.bounds', 'Invalid triangle indices.')
            parts.append((points, uv, indices))
        mesh_data.append(parts)
    nodes, scenes = collection('nodes'), collection('scenes')
    scene = doc.get('scene', 0)
    require(type(scene) is int and 0 <= scene < len(scenes), 'glb.bounds', 'Invalid scene.')
    roots = scenes[scene].get('nodes', [])
    require(isinstance(roots, list) and len(roots) <= limits['collection_items'], 'glb.bounds', 'Invalid roots.')
    pending, seen = [(i, 0) for i in roots], set()
    while pending:
        index, depth = pending.pop()
        require(type(index) is int and 0 <= index < len(nodes) and index not in seen and depth <= limits['json_depth'],
                'glb.bounds', 'Invalid scene tree.')
        seen.add(index)
        node = nodes[index]
        for key, identity in (('translation', [0, 0, 0]), ('scale', [1, 1, 1]), ('rotation', [0, 0, 0, 1]),
                              ('matrix', [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])):
            require(key not in node or (node[key] == identity and all(type(v) in (int, float) for v in node[key])),
                    'glb.encoding', 'Export must apply node transforms in metres/Y-up.')
        require('skin' not in node, 'glb.encoding', 'Skinned node is unsupported.')
        if 'mesh' in node:
            mi = node['mesh']
            require(type(mi) is int and 0 <= mi < len(mesh_data), 'glb.bounds', 'Invalid mesh index.')
            for points, uv, indices in mesh_data[mi]:
                base = len(positions)
                require(base+len(points) <= min(limits['glb_accessor_elements'], METRIC_ELEMENT_LIMIT) and len(faces)+len(indices)//3 <= effective['budgets']['max_tris_prefinish'],
                        'glb.limit', 'Instanced expansion cap exceeded.')
                positions.extend(points)
                for at in range(0, len(indices), 3):
                    face = indices[at:at+3]
                    faces.append(tuple(base+i for i in face))
                    if uv is not None:
                        uv_faces.append(tuple(uv[i] for i in face))
        children = node.get('children', [])
        require(isinstance(children, list) and len(children) <= limits['collection_items'], 'glb.bounds', 'Invalid children.')
        pending.extend((i, depth+1) for i in children)
    require(positions and faces, 'glb.geometry', 'Empty output is refused.')
    # Connectivity is by exact exported positions, accounting for split UV seams.
    parents = list(range(len(positions)))
    def find(i):
        while parents[i] != i:
            parents[i] = parents[parents[i]]; i = parents[i]
        return i
    unique, edges = {}, {}
    for i, p in enumerate(positions):
        if p in unique: parents[find(i)] = find(unique[p])
        else: unique[p] = i
    for a, b, c in faces:
        for x, y in ((a, b), (b, c), (c, a)):
            parents[find(x)] = find(y)
            edge = tuple(sorted((positions[x], positions[y])))
            edges[edge] = edges.get(edge, 0)+1
    minimum = [min(p[i] for p in positions) for i in range(3)]
    maximum = [max(p[i] for p in positions) for i in range(3)]
    bounds = [maximum[i]-minimum[i] for i in range(3)]
    overlap = uv_overlap(uv_faces) if has_uv else None
    return {'triangles': len(faces), 'vertices': len(positions), 'bodies': len({find(i) for f in faces for i in f}),
            'watertight': all(n == 2 for n in edges.values()), 'has_uv': has_uv, 'has_normals': has_normals,
            'bounds_m': bounds, 'bounds_min_m': minimum, 'bounds_max_m': maximum, 'budget_ok': len(faces) <= effective['budgets']['max_tris_final'],
            'uv_overlap_zero': overlap}


def uv_overlap(triangles, *, pair_limit=UV_PAIR_LIMIT):
    """Sweep broad phase plus positive-area convex clipping; shared edges pass.

    Returns None if the fixed work budget is exhausted (unavailable evidence).
    """
    from fractions import Fraction
    def finite_coordinate(x):
        return (type(x) is int and x.bit_length() <= 1023 or
                type(x) is float and math.isfinite(x))
    def cross(a, b, c): return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
    require(len(triangles) <= METRIC_ELEMENT_LIMIT, 'metric.limit', 'UV triangle work cap exceeded.')
    records = []
    for tri in triangles:
        require(len(tri) == 3 and all(len(v) == 2 and all(finite_coordinate(x) for x in v) for v in tri),
                'finish.gate_failed', 'Invalid UV triangle.')
        # Exact arithmetic on the accepted binary floats, including clipping.
        tri = tuple(tuple(Fraction(x) for x in v) for v in tri)
        area = cross(*tri)
        if area == 0: return False
        tri = tuple(tri if area > 0 else reversed(tri))
        records.append((min(v[0] for v in tri), max(v[0] for v in tri), min(v[1] for v in tri), max(v[1] for v in tri), tri))
    records.sort(key=lambda r: r[:4])
    active, work = [], 0
    for record in records:
        active = [r for r in active if r[1] > record[0]]
        for other in active:
            work += 1
            if work > pair_limit: return None
            if other[3] <= record[2] or record[3] <= other[2]: continue
            polygon = list(record[4])
            clip = other[4]
            for i in range(3):
                a, b = clip[i], clip[(i+1)%3]
                old, polygon = polygon, []
                if not old: break
                last, dl = old[-1], cross(a, b, old[-1])
                for point in old:
                    dp = cross(a, b, point)
                    if (dp >= 0) != (dl >= 0):
                        t = dl/(dl-dp)
                        polygon.append(tuple(last[k]+t*(point[k]-last[k]) for k in range(2)))
                    if dp >= 0: polygon.append(point)
                    last, dl = point, dp
            area = abs(sum(polygon[i][0]*polygon[(i+1)%len(polygon)][1]-polygon[(i+1)%len(polygon)][0]*polygon[i][1] for i in range(len(polygon))))
            if area > 0: return False
        active.append(record)
    return True


def guard_decision(observation, decimate):
    """Consume matched component/projected areas from fixed common X/Y/Z views."""
    def finite(value): return type(value) in (int, float) and math.isfinite(value)
    rates = [observation.get(k) for k in ('hit_rate_before', 'hit_rate_after')]
    if all(finite(r) and decimate['min_ray_hit_rate'] <= r <= 1 for r in rates):
        before, after = observation.get('p10_before'), observation.get('p10_after')
        ok = finite(before) and finite(after) and before > 0 and after >= before*decimate['thin_shell_p10_min_ratio']
        ok &= observation.get('components_survived') is True
        return {'protocol': METRIC_VERSION, 'mode': 'p10', 'pass': ok}
    components = observation.get('components')
    raw_ids = observation.get('raw_component_ids')
    ok = isinstance(components, list) and 0 < len(components) <= 65536 and isinstance(raw_ids, list)
    if ok:
        ok = all(isinstance(i, str) for i in raw_ids) and len(set(raw_ids)) == len(raw_ids)
        ok &= all(isinstance(c, dict) and isinstance(c.get('raw_id'), str) and isinstance(c.get('matched_id'), str) for c in components)
    if ok:
        ok = set(raw_ids) == {c['raw_id'] for c in components} and len({c['raw_id'] for c in components}) == len(components)
        ok &= len({c['matched_id'] for c in components}) == len(components)
    if ok:
        for component in components:
            raw, final = component.get('raw_projected_xyz'), component.get('matched_projected_xyz')
            if not (isinstance(raw, list) and isinstance(final, list) and len(raw) == len(final) == 3
                    and all(finite(v) and v >= 0 for v in raw+final) and max(raw) > 0):
                ok = False; break
            view = max(range(3), key=lambda i: raw[i])
            if final[view] < raw[view]*decimate['open_sheet_min_retention']: ok = False
    return {'protocol': METRIC_VERSION, 'mode': 'open_sheet' if components else 'unavailable', 'pass': bool(ok)}


def retry_decimation(baseline, target, max_attempts, collapse, measure, restore):
    """Every attempt starts from baseline. Rejected geometry restores last accepted."""
    require(type(target) is int and target > 0 and type(max_attempts) is int and 1 <= max_attempts <= 8,
            'finish.gate_failed', 'Invalid decimation controls.')
    initial = measure(baseline)
    ratio, accepted, attempts = min(1, target/initial['triangles']), baseline, []
    if initial['triangles'] <= target:
        return baseline, [initial]  # The full-chain guard still ran in measure.
    for _ in range(max_attempts):
        trial = collapse(baseline, ratio)
        result = measure(trial)
        attempts.append(result)
        if result.get('guard_pass') is not True:
            restore(accepted); break
        accepted = trial
        if result['triangles'] <= target: break
        ratio *= target/result['triangles']
    return accepted, attempts


def validate_png(data, limits):
    """Bounded preview PNG grammar/inflate, independent of image metadata policy."""
    import zlib
    require(type(data) is bytes and len(data) <= limits['image_bytes'] and data[:8] == b'\x89PNG\r\n\x1a\n',
            'worker.protocol', 'Invalid PNG preview header or byte bound.')
    at, chunks, width, height, expected = 8, 0, None, None, None
    compressed = bytearray()
    saw_idat = closed_idat = ended = False
    while at < len(data):
        require(at+12 <= len(data), 'worker.protocol', 'Truncated PNG preview.')
        size, kind = struct.unpack_from('>I4s', data, at)
        require(size <= limits['image_bytes'] and at+12+size <= len(data), 'worker.protocol', 'Invalid PNG extent.')
        body = data[at+8:at+8+size]
        require(zlib.crc32(kind+body) & 0xffffffff == struct.unpack_from('>I', data, at+8+size)[0], 'worker.protocol', 'Invalid PNG CRC.')
        chunks += 1
        require(chunks <= limits['collection_items'], 'worker.limit', 'PNG chunk count exceeds bound.')
        if kind == b'IHDR':
            require(chunks == 1 and size == 13, 'worker.protocol', 'Invalid PNG IHDR.')
            width, height, depth, colour, compression, filtering, interlace = struct.unpack('>IIBBBBB', body)
            channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour)
            require(width > 0 and height > 0 and width*height <= limits['image_pixels'] and depth == 8 and channels
                    and compression == filtering == interlace == 0, 'worker.protocol', 'Unsupported PNG preview.')
            expected = height*(1+width*channels)
        elif kind == b'IDAT':
            require(expected is not None and not closed_idat, 'worker.protocol', 'Nonconsecutive PNG IDAT.')
            saw_idat = True; compressed.extend(body)
        elif kind == b'IEND':
            require(size == 0 and saw_idat and at+12 == len(data), 'worker.protocol', 'Incomplete PNG preview.')
            ended = True
        else:
            # Portable preview fixtures deliberately exclude credential/metadata chunks.
            raise Refusal('worker.protocol', 'Preview supports IHDR/IDAT/IEND only.')
        if saw_idat and kind != b'IDAT': closed_idat = True
        at += 12+size
    require(ended and expected is not None, 'worker.protocol', 'PNG preview lacks IEND.')
    inflater, total = zlib.decompressobj(), 0
    try:
        for at in range(0, len(compressed), 8192):
            tail = bytes(compressed[at:at+8192])
            while tail:
                block = inflater.decompress(tail, min(65536, expected-total+1))
                row_bytes = expected//height
                for offset in range((-total) % row_bytes, len(block), row_bytes):
                    require(block[offset] <= 4, 'worker.protocol', 'Invalid PNG scanline filter.')
                total += len(block)
                require(total <= expected, 'worker.limit', 'PNG preview expansion exceeds dimensions.')
                tail = inflater.unconsumed_tail
        require(inflater.eof and not inflater.unused_data and total == expected,
                'worker.protocol', 'PNG preview stream is incomplete.')
    except zlib.error as error:
        raise Refusal('worker.protocol', 'Invalid compressed PNG preview.') from error
    return width, height


def area_weighted_samples(points, faces, *, count=20000, seed=0):
    """Deterministic samples in the supplied common frame; never independently fit.

    Frozen definition: Python Random(seed), triangle area CDF, sqrt barycentric
    sampling. Native G2 distance queries use these samples with Blender BVH.
    """
    import bisect
    import random
    require(type(count) is int and 1 <= count <= 20000 and len(faces) <= 1000000,
            'metric.limit', 'Sampling work cap exceeded.')
    weights, total = [], 0.0
    for face in faces:
        require(len(face) == 3 and all(type(i) is int and 0 <= i < len(points) for i in face), 'glb.bounds', 'Invalid sample triangle.')
        a, b, c = [points[i] for i in face]
        require(all(len(p) == 3 and all(type(v) in (int, float) and math.isfinite(v) for v in p) for p in (a, b, c)),
                'glb.nonfinite', 'Invalid sample coordinates.')
        ab, ac = [b[i]-a[i] for i in range(3)], [c[i]-a[i] for i in range(3)]
        cross = [ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]]
        total += math.sqrt(sum(v*v for v in cross))/2
        require(math.isfinite(total), 'metric.limit', 'Sampling area overflow.')
        weights.append(total)
    require(total > 0, 'finish.gate_failed', 'No positive-area triangles to sample.')
    rng, samples = random.Random(seed), []
    for _ in range(count):
        index = min(len(faces)-1, bisect.bisect_right(weights, rng.random()*total))
        a, b, c = [points[i] for i in faces[index]]
        u, v = math.sqrt(rng.random()), rng.random()
        samples.append(tuple((1-u)*a[i]+u*(1-v)*b[i]+u*v*c[i] for i in range(3)))
    return samples


def retry_finish_chain(raw, run_chain, measure):
    """Measure the whole chain, then one retry without loose-drop/hole destruction."""
    finished = run_chain(raw, destructive_repairs=True)
    first = measure(raw, finished)
    if first.get('guard_pass') is True: return finished, [first]
    retried = run_chain(raw, destructive_repairs=False)
    second = measure(raw, retried)
    return retried, [first, second]
