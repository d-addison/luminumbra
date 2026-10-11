"""Validate interchange containment and actual compiled geometry/clip bytes."""
import math
import struct

from .contracts import parse_json, require


def validate_glb(data):
    require(len(data) >= 20, "glb.header", "Truncated GLB header.")
    magic, version, length = struct.unpack_from("<4sII", data)
    require(magic == b"glTF" and version == 2 and length == len(data),
            "glb.header", "Expected a complete glTF 2.0 binary snapshot.")
    offset, chunks = 12, []
    while offset < len(data):
        require(offset + 8 <= len(data), "glb.chunk", "Truncated GLB chunk.")
        size, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        require(size % 4 == 0 and offset + size <= len(data), "glb.chunk", "Invalid chunk extent.")
        chunks.append((kind, data[offset:offset + size]))
        offset += size
    require([kind for kind, _ in chunks] in ([0x4e4f534a], [0x4e4f534a, 0x004e4942]),
            "glb.chunk", "This profile supports JSON followed by an optional BIN chunk.")
    document = parse_json(chunks[0][1])
    require(isinstance(document, dict) and isinstance(document.get("asset"), dict)
            and document["asset"].get("version") == "2.0",
            "glb.version", "Expected glTF 2.0.")
    for collection in ("buffers", "images"):
        items = document.get(collection, [])
        require(isinstance(items, list) and all(isinstance(x, dict) for x in items),
                "glb.structure", f"Invalid {collection} collection.")
        for item in items:
            require("uri" not in item, "glb.external", "Embed all buffer/image data in the GLB.")
    required = document.get("extensionsRequired", [])
    require(isinstance(required, list) and all(x == "KHR_texture_transform" for x in required),
            "glb.extension", "Required extension is unavailable in the geometry profile.")
    # Extras carry correlation IDs only. Required engine metadata belongs in the
    # validated sidecar; never execute unknown namespaced payloads in GLB extras.
    pending = [document]
    while pending:
        value = pending.pop()
        if isinstance(value, list):
            pending.extend(value)
        elif isinstance(value, dict):
            for key, item in value.items():
                require(key != "luminumbra", "glb.metadata",
                        "Engine metadata must use the validated sidecar.", key)
                if key.startswith("luminumbra."):
                    require(key in ("luminumbra.object_id", "luminumbra.asset_id", "luminumbra.material_id")
                            and isinstance(item, str),
                            "glb.metadata", "Only correlation IDs are accepted in GLB extras.", key)
                pending.append(item)
    return document


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, fmt):
        fmt = "<" + fmt
        size = struct.calcsize(fmt)
        require(size <= len(self.data) - self.at, "output.truncated", "Compiled output is truncated.")
        value = struct.unpack_from(fmt, self.data, self.at)
        self.at += size
        return value

    def extent(self, size):
        require(size == len(self.data) - self.at, "output.size", "Compiled output size/count mismatch.")


def finite(values):
    require(all(math.isfinite(x) for x in values), "output.nonfinite", "Nonfinite compiled attribute.")


def texture_info(data):
    reader = Reader(data)
    magic, version, count, width, height, channels = reader.take("4sHHIIB")
    require(magic == b"LTEX" and version == 1 and channels == 4
            and 1 <= width <= 4096 and 1 <= height <= 4096,
            "output.texture", "Expected bounded LTEX v1 RGBA8 data.")
    w, h, levels, size = width, height, 1, width * height * 4
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        size += w * h * 4
        levels += 1
    require(count == levels, "output.texture_mips", "Expected a complete halving mip chain.")
    reader.extent(size)
    return {"format": "LTEX", "width": width, "height": height, "channels": channels,
            "mip_levels": count, "bytes": len(data)}


def mesh_info(data):
    reader = Reader(data)
    magic, = reader.take("4s")
    require(magic in (b"LMSH", b"LMS2"), "output.format", "Unknown mesh format.")
    skinned = magic == b"LMS2"
    if skinned:
        version, vertices, indices, joints = reader.take("4I")
        require(version == 1 and 1 <= joints <= 256, "output.skin", "Unsupported LMS2 palette.")
    else:
        vertices, indices = reader.take("2I")
        joints = 0
    sphere = reader.take("4f")
    finite(sphere)
    require(sphere[3] >= 0 and vertices > 0 and indices > 0 and indices % 3 == 0,
            "output.mesh", "Invalid mesh counts or bounding sphere.")
    reader.extent(vertices * (40 if skinned else 32) + indices * 4 + joints * 112)
    for _ in range(vertices):
        finite(reader.take("8f"))
        if skinned:
            lanes = reader.take("8B")
            require(max(lanes[:4]) < joints and sum(lanes[4:]) == 255,
                    "output.weights", "Invalid compiled influence indices/weights.")
    for _ in range(indices):
        require(reader.take("I")[0] < vertices, "output.index", "Mesh index is out of bounds.")
    names = set()
    for index in range(joints):
        name, parent = reader.take("Ii")
        require(name not in names and -1 <= parent < index,
                "output.hierarchy", "Invalid joint identity or parent ordering.")
        names.add(name)
        bind = reader.take("16f")
        pose = reader.take("10f")
        finite(bind + pose)
        require((bind[3], bind[7], bind[11], bind[15]) == (0, 0, 0, 1)
                and sum(x * x for x in pose[3:7]) > 0,
                "output.bind", "Invalid inverse bind or local rotation.")
    return {"format": magic.decode(), "vertices": vertices, "triangles": indices // 3,
            "joints": joints, "joint_ids": sorted(names), "bytes": len(data)}


def clip_info(data, joint_ids):
    reader = Reader(data)
    magic, version, tracks, duration = reader.take("4sIIf")
    require(magic == b"LANM" and version in (1, 2) and math.isfinite(duration) and duration >= 0,
            "output.clip", "Unknown or invalid animation clip header.")
    require(0 < tracks <= (len(data) - 16) // (20 if version == 2 else 16),
            "output.tracks", "Invalid animation track count.")
    targets = set()
    modes = set()
    for _ in range(tracks):
        joint, target, count, components = reader.take("4I")
        mode = reader.take("I")[0] if version == 2 else 0
        require(joint in joint_ids and target <= 2 and components == (4 if target == 1 else 3)
                and count > 0 and (joint, target) not in targets,
                "output.track", "Invalid or duplicate animation target.")
        require(mode in ((1, 2, 3) if version == 2 else (0,)) and (mode != 3 or count >= 2),
                "output.interpolation", "Invalid interpolation mode/key count.")
        targets.add((joint, target))
        modes.add(mode)
        stride = components * (3 if mode == 3 else 1)
        require(count * (stride + 1) * 4 <= len(data) - reader.at,
                "output.truncated", "Animation counts exceed available bytes.")
        previous = -1
        for _ in range(count):
            time, = reader.take("f")
            require(math.isfinite(time) and previous < time <= duration and time >= 0,
                    "output.times", "Invalid animation time sequence.")
            previous = time
        for _ in range(count):
            values = reader.take(f"{stride}f")
            finite(values)
            if target == 1:
                key = values[components:components * 2] if mode == 3 else values
                norm = sum(x * x for x in key)
                require(norm > 0 and (version == 1 or abs(norm - 1) <= 0.001),
                        "output.rotation", "Invalid animation rotation key.")
    reader.extent(0)
    return {"format": "LANM", "version": version, "tracks": tracks,
            "duration": duration, "interpolation_modes": sorted(modes), "bytes": len(data)}
