"""Static prefab extraction: preserved instances and explicit metallic/roughness bindings."""
import math
import struct
import hashlib

from .contracts import ID, MAX_DOCUMENT, MAX_OUTPUT, MAX_SNAPSHOT, PREFAB_PROFILE, canonical, digest, require
from .formats import validate_glb

PROFILE = PREFAB_PROFILE
SCHEMA = "luminumbra.asset.prefab.v1"
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def number(value, field, minimum=-1e30, maximum=1e30):
    require(type(value) in (int, float) and math.isfinite(value) and minimum <= value <= maximum,
            "prefab.number", "Expected a finite number within the supported range.", field)
    return value


def vector(value, size, field, minimum=-1e30, maximum=1e30):
    require(isinstance(value, list) and len(value) == size,
            "prefab.vector", f"Expected {size} numeric components.", field)
    return [number(item, field, minimum, maximum) for item in value]


def item(document, name, index):
    values = document.get(name, [])
    require(isinstance(values, list) and type(index) is int and 0 <= index < len(values)
            and isinstance(values[index], dict), "prefab.index", "Invalid referenced element.", name)
    return values[index]


def identity(value, key):
    extras = value.get("extras", {})
    result = extras.get(key) if isinstance(extras, dict) else None
    require(isinstance(result, str) and ID.fullmatch(result), "prefab.identity",
            "Assign a persistent correlation ID before compiling a prefab.", key)
    return result


def multiply(a, b):
    return [sum(a[k * 4 + row] * b[column * 4 + k] for k in range(4))
            for column in range(4) for row in range(4)]


def determinant(m):
    return (m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
            + m[8] * (m[1] * m[6] - m[5] * m[2]))


def transform(node):
    if "matrix" in node:
        require(not {"translation", "rotation", "scale"} & node.keys(),
                "prefab.transform", "Use a matrix or TRS, not both.")
        result = vector(node["matrix"], 16, "matrix")
    else:
        t = vector(node.get("translation", [0, 0, 0]), 3, "translation")
        x, y, z, w = vector(node.get("rotation", [0, 0, 0, 1]), 4, "rotation", -1, 1)
        require(abs(x*x + y*y + z*z + w*w - 1) <= 1e-5,
                "prefab.rotation", "Normalize the authored rotation.")
        s = vector(node.get("scale", [1, 1, 1]), 3, "scale")
        result = [(1-2*(y*y+z*z))*s[0], 2*(x*y+z*w)*s[0], 2*(x*z-y*w)*s[0], 0,
                  2*(x*y-z*w)*s[1], (1-2*(x*x+z*z))*s[1], 2*(y*z+x*w)*s[1], 0,
                  2*(x*z+y*w)*s[2], 2*(y*z-x*w)*s[2], (1-2*(x*x+y*y))*s[2], 0,
                  *t, 1]
    require([result[i] for i in (3, 7, 11, 15)] == [0, 0, 0, 1]
            and abs(determinant(result)) > 1e-12,
            "prefab.transform", "Expected an invertible affine transform.")
    lengths = [math.sqrt(sum(result[c*4+r]**2 for r in range(3))) for c in range(3)]
    for a, b in ((0, 1), (0, 2), (1, 2)):
        require(abs(sum(result[a*4+r] * result[b*4+r] for r in range(3))) <= lengths[a]*lengths[b]*1e-5,
                "prefab.shear", "Bake static shear before compiling this prefab profile.")
    return result


def encode_glb(document, binary):
    encoded = canonical(document)
    encoded += b" " * (-len(encoded) % 4)
    binary += b"\0" * (-len(binary) % 4)
    return (struct.pack("<4sII", b"glTF", 2, 28 + len(encoded) + len(binary))
            + struct.pack("<II", len(encoded), 0x4e4f534a) + encoded
            + struct.pack("<II", len(binary), 0x004e4942) + binary)


def image_size(data, mime):
    if mime == "image/png":
        require(len(data) >= 33 and data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR",
                "prefab.image", "Expected a complete PNG header.")
        width, height = struct.unpack_from(">II", data, 16)
    else:
        require(mime == "image/jpeg" and data[:2] == b"\xff\xd8", "prefab.image", "Use embedded PNG or JPEG textures.")
        at, width, height = 2, 0, 0
        while at + 4 <= len(data):
            require(data[at] == 255, "prefab.image", "Malformed JPEG marker.")
            while at < len(data) and data[at] == 255:
                at += 1
            require(at + 3 <= len(data), "prefab.image", "Truncated JPEG marker.")
            marker = data[at]
            at += 1
            size = struct.unpack_from(">H", data, at)[0]
            require(size >= 2 and at + size <= len(data), "prefab.image", "Truncated JPEG segment.")
            if marker in (0xc0, 0xc1, 0xc2):
                require(size >= 8, "prefab.image", "Truncated JPEG dimensions.")
                height, width = struct.unpack_from(">HH", data, at + 3)
                break
            require(marker not in (0xda, 0xd9), "prefab.image", "JPEG has no supported frame header.")
            at += size
    require(1 <= width <= 4096 and 1 <= height <= 4096,
            "prefab.image_budget", "Textures must be between 1 and 4096 pixels per dimension.")
    return width, height


class PrefabPlan:
    def __init__(self, data, asset_id):
        self.document = validate_glb(data)
        document = self.document
        json_size = struct.unpack_from("<I", data, 12)[0]
        require(json_size <= MAX_DOCUMENT, "prefab.document_budget", "Prefab JSON exceeds 1 MiB.")
        self.binary = data[28 + json_size:]
        buffers = document.get("buffers", [])
        require(len(buffers) == 1 and type(buffers[0].get("byteLength")) is int
                and 0 <= len(self.binary) - buffers[0]["byteLength"] <= 3,
                "prefab.buffer", "Expected one embedded binary buffer.")
        self.binary = self.binary[:buffers[0]["byteLength"]]
        require(not document.get("skins") and not document.get("animations"),
                "prefab.static", "Use the character profile for skins or animation.")
        pending = [document]
        while pending:
            value = pending.pop()
            if isinstance(value, list):
                pending.extend(value)
            elif isinstance(value, dict):
                extensions = value.get("extensions", {})
                require(isinstance(extensions, dict) and extensions.keys() <= {"KHR_texture_transform"}
                        and all(isinstance(extension, dict) for extension in extensions.values()),
                        "prefab.extension", "This prefab profile supports only texture-transform extensions.")
                pending.extend(value.values())
        self.tasks, self.materials, self.meshes = {}, {}, {}
        self.material_cache, self.mesh_cache = {}, {}
        self.accessor_cache = {}
        self.accessor_bytes = 0
        self.texture_bytes = 0
        self.texture_inputs = 0
        self.asset_id = asset_id
        nodes = self.nodes()
        require(self.meshes and len(nodes) <= 4096 and len(self.tasks) <= 128,
                "prefab.budget", "Prefab needs geometry and must fit node/draw/texture budgets.")
        self.description = {"schema": SCHEMA, "asset_id": asset_id, "coordinates": "gltf-rh-y-up-meters",
                            "nodes": sorted(nodes, key=lambda n: n["id"]),
                            "meshes": dict(sorted(self.meshes.items())),
                            "materials": dict(sorted(self.materials.items())),
                            "runtime_components": False}

    def add_task(self, identity_data, kind, payload, arguments, signature=None):
        key = digest(canonical(identity_data))
        name = ("m-" if kind == "mesh" else "t-") + key[:32] + (".lmesh" if kind == "mesh" else ".ltex")
        if name in self.tasks:
            same_content = self.tasks[name].get("signature") == signature if kind == "mesh" else self.tasks[name]["payload"] == payload
            require(self.tasks[name]["key"] == key and same_content,
                    "prefab.identity", "Distinct source definitions share a derived identity.")
        else:
            require(len(self.tasks) < 128, "prefab.budget", "At most 128 compiled mesh/texture outputs are supported.")
            self.tasks[name] = {"key": key, "kind": kind, "payload": payload, "arguments": arguments, "signature": signature}
        return name

    def accessor_signature(self, index):
        require(type(index) is int, "prefab.index", "Expected an accessor index.")
        if index in self.accessor_cache:
            return self.accessor_cache[index]
        accessor = item(self.document, "accessors", index)
        require("sparse" not in accessor, "prefab.accessor", "Expand sparse attributes for this static prefab profile.")
        view = item(self.document, "bufferViews", accessor.get("bufferView"))
        component = accessor.get("componentType")
        kind = accessor.get("type")
        sizes, counts = {5120:1,5121:1,5122:2,5123:2,5125:4,5126:4}, {"SCALAR":1,"VEC2":2,"VEC3":3,"VEC4":4}
        require(type(component) is int and component in sizes and isinstance(kind, str) and kind in counts,
                "prefab.accessor", "Unsupported static attribute representation.")
        width = sizes[component] * counts[kind]
        stride, count = view.get("byteStride", width), accessor.get("count")
        start, offset, length = view.get("byteOffset", 0), accessor.get("byteOffset", 0), view.get("byteLength")
        require(type(view.get("buffer")) is int and view["buffer"] == 0
                and all(type(v) is int for v in (stride, count, start, offset, length))
                and count > 0 and width <= stride <= 252 and start >= 0 and offset >= 0
                and start + length <= len(self.binary) and offset + (count-1)*stride + width <= length,
                "prefab.accessor", "Attribute data exceeds its embedded buffer view.")
        normalized = accessor.get("normalized", False)
        require(type(normalized) is bool, "prefab.accessor", "normalized must be boolean.")
        self.accessor_bytes += count * width
        require(self.accessor_bytes <= MAX_SNAPSHOT * 4, "prefab.accessor_budget", "Attribute extraction exceeds the processing budget.")
        checksum = hashlib.sha256(canonical([component, kind, count, normalized]))
        data = memoryview(self.binary)
        if width == stride:
            checksum.update(data[start+offset:start+offset+count*width])
        else:
            for row in range(count):
                at = start + offset + row*stride
                checksum.update(data[at:at+width])
        result = checksum.hexdigest()
        self.accessor_cache[index] = result
        return result

    def texture(self, info, role, cutoff):
        document = self.document
        require(isinstance(info, dict), "prefab.texture", "Invalid texture binding.")
        texture = item(document, "textures", info.get("index"))
        image = item(document, "images", texture.get("source"))
        view = item(document, "bufferViews", image.get("bufferView"))
        offset, size = view.get("byteOffset", 0), view.get("byteLength")
        require(view.get("buffer") == 0 and type(offset) is int and type(size) is int
                and offset >= 0 and size > 0 and offset + size <= len(self.binary) and "byteStride" not in view,
                "prefab.image", "Invalid embedded image extent.")
        payload = self.binary[offset:offset+size]
        width, height = image_size(payload, image.get("mimeType"))
        arguments = [{"color": "--srgb", "normal": "--normal-map", "data": "--linear"}[role]]
        if cutoff is not None:
            arguments += ["--alpha-cutoff", str(cutoff)]
        count = len(self.tasks)
        output = self.add_task([digest(payload), role, cutoff], "texture", payload, arguments)
        self.tasks[output]["dimensions"] = [width, height]
        if len(self.tasks) != count:
            # Upper bound for RGBA8 with complete mips, checked before decoding.
            self.texture_bytes += width * height * 8
            self.texture_inputs += len(payload)
            require(self.texture_bytes <= MAX_OUTPUT, "prefab.texture_budget", "Compiled textures exceed the output budget.")
            require(self.texture_inputs <= MAX_SNAPSHOT, "prefab.texture_budget", "Texture inputs exceed the snapshot budget.")
        sampler = item(document, "samplers", texture["sampler"]) if "sampler" in texture else {}
        values = {"wrapS": sampler.get("wrapS", 10497), "wrapT": sampler.get("wrapT", 10497),
                  "magFilter": sampler.get("magFilter", 9729), "minFilter": sampler.get("minFilter", 9987)}
        require(all(type(v) is int for v in values.values())
                and values["wrapS"] in (33071, 33648, 10497) and values["wrapT"] in (33071, 33648, 10497)
                and values["magFilter"] in (9728, 9729) and values["minFilter"] in (9728, 9729, 9984, 9985, 9986, 9987),
                "prefab.sampler", "Unsupported sampler setting.")
        extension = info.get("extensions", {}).get("KHR_texture_transform", {})
        require(extension.keys() <= {"offset", "scale", "rotation", "texCoord", "extras"},
                "prefab.texture_transform", "Unknown texture-transform field.")
        coordinates = {"set": extension.get("texCoord", info.get("texCoord", 0)),
                       "offset": vector(extension.get("offset", [0, 0]), 2, "texture.offset"),
                       "scale": vector(extension.get("scale", [1, 1]), 2, "texture.scale"),
                       "rotation": number(extension.get("rotation", 0), "texture.rotation")}
        require(type(coordinates["set"]) is int and 0 <= coordinates["set"] <= 7,
                "prefab.uv", "This profile supports UV sets 0 through 7.")
        return {"file": output, "encoding": "srgb" if role == "color" else "linear",
                "sampler": values, "coordinates": coordinates}

    def material(self, index):
        require(index is None or type(index) is int, "prefab.index", "Expected a material index.")
        if index in self.material_cache:
            return self.material_cache[index]
        source = item(self.document, "materials", index) if index is not None else {}
        key = identity(source, "luminumbra.material_id") if source else "default"
        pbr = source.get("pbrMetallicRoughness", {})
        require(isinstance(pbr, dict), "prefab.material", "Expected metallic/roughness material fields.")
        base = vector(pbr.get("baseColorFactor", [1, 1, 1, 1]), 4, "material.base_color", 0, 1)
        mode = source.get("alphaMode", "OPAQUE")
        require(mode in ("OPAQUE", "MASK", "BLEND"), "prefab.alpha", "Unknown alpha mode.")
        cutoff = number(source.get("alphaCutoff", 0.5), "material.alpha_cutoff", 0, 1)
        effective_cutoff = None
        if mode == "MASK" and "baseColorTexture" in pbr:
            require(base[3] > 0 and cutoff <= base[3], "prefab.alpha",
                    "Bake a fully discarded cutout material before compiling it.")
            effective_cutoff = cutoff / base[3]
        material = {"base_color": base, "metallic": number(pbr.get("metallicFactor", 1), "metallic", 0, 1),
                    "roughness": number(pbr.get("roughnessFactor", 1), "roughness", 0, 1),
                    "emissive": vector(source.get("emissiveFactor", [0, 0, 0]), 3, "emissive", 0, 1),
                    "alpha_mode": mode, "alpha_cutoff": cutoff,
                    "double_sided": source.get("doubleSided", False), "textures": {}}
        require(type(material["double_sided"]) is bool, "prefab.material", "doubleSided must be boolean.")
        for name, container, role in (("baseColorTexture", pbr, "color"), ("metallicRoughnessTexture", pbr, "data"),
                                      ("normalTexture", source, "normal"), ("occlusionTexture", source, "data"),
                                      ("emissiveTexture", source, "color")):
            if name in container:
                material["textures"][name] = self.texture(container[name], role, effective_cutoff if name == "baseColorTexture" else None)
        material["normal_scale"] = number(source.get("normalTexture", {}).get("scale", 1), "normal.scale", 0)
        material["occlusion_strength"] = number(source.get("occlusionTexture", {}).get("strength", 1), "occlusion.strength", 0, 1)
        coordinates = [value["coordinates"] for value in material["textures"].values()]
        require(not coordinates or all(value == coordinates[0] for value in coordinates),
                "prefab.uv_mismatch", "Bake maps to matching UV sets and transforms for the current vertex layout.")
        require(key not in self.materials or self.materials[key] == material,
                "prefab.identity", "Different material definitions share one persistent ID.")
        self.materials[key] = material
        self.material_cache[index] = key
        return key

    def mesh(self, index):
        require(type(index) is int, "prefab.index", "Expected a mesh index.")
        if index in self.mesh_cache:
            return self.mesh_cache[index]
        source = item(self.document, "meshes", index)
        key = identity(source, "luminumbra.asset_id")
        require(not source.get("weights"), "prefab.morph", "Morph assets need a character profile.")
        primitives = source.get("primitives", [])
        require(isinstance(primitives, list) and 0 < len(primitives) <= 64,
                "prefab.primitive", "Expected between 1 and 64 primitives per mesh.")
        draws = []
        for number, primitive in enumerate(primitives):
            require(isinstance(primitive, dict) and primitive.get("mode", 4) == 4 and not primitive.get("targets"),
                    "prefab.primitive", "Use triangulated static geometry without morph targets.")
            require(isinstance(primitive.get("attributes"), dict) and primitive["attributes"].keys()
                    <= {"POSITION", "NORMAL", *("TEXCOORD_" + str(i) for i in range(8))},
                    "prefab.attributes", "This vertex layout supports position, normal and selected UVs only.")
            material_id = self.material(primitive.get("material"))
            material = self.materials[material_id]
            derived = {name: self.document[name] for name in
                       ("asset", "buffers", "bufferViews", "accessors", "textures", "images", "samplers")
                       if name in self.document}
            selected = dict(primitive)
            selected["material"] = 0
            derived["meshes"] = [{"primitives": [selected]}]
            derived["nodes"], derived["scenes"], derived["scene"] = [{"mesh": 0}], [{"nodes": [0]}], 0
            # Current native layout stores one UV lane. Use an authored map as
            # the compiler's coordinate selector, even when base color is flat.
            original = item(self.document, "materials", primitive["material"]) if "material" in primitive else {}
            original_pbr = original.get("pbrMetallicRoughness", {})
            candidates = [original_pbr.get(name) or original.get(name) for name in material["textures"]]
            # Retain the selected raw UV lane. Applying a texture transform to
            # vertices would also change the derivative normal-map basis. The
            # prefab material carries that transform for texture sampling only.
            coord = next(iter(material["textures"].values()))["coordinates"]["set"] if candidates else 0
            selector = {"index": candidates[0]["index"], "texCoord": coord} if candidates else None
            derived["materials"] = [{"pbrMetallicRoughness": {"baseColorTexture": selector}}] if candidates else [{}]
            if candidates:
                require("TEXCOORD_" + str(coord) in selected["attributes"],
                        "prefab.uv", "The primitive has no UV attribute for its material maps.")
            signature = {name:self.accessor_signature(accessor) for name, accessor in selected["attributes"].items()}
            if "indices" in selected:
                signature["indices"] = self.accessor_signature(selected["indices"])
            # Blender may emit equivalent mesh definitions with different
            # buffer offsets for objects sharing evaluated source geometry.
            output = self.add_task([key, number, material_id], "mesh", derived, [], digest(canonical(signature)))
            draws.append({"file": output, "material": material_id, "uv_transform_baked": False})
        require(key not in self.meshes or self.meshes[key] == draws,
                "prefab.identity", "Different mesh definitions share one persistent ID.")
        self.meshes[key] = draws
        self.mesh_cache[index] = key
        return key

    def nodes(self):
        document = self.document
        scenes = document.get("scenes", [])
        require(isinstance(scenes, list) and ("scene" in document or len(scenes) == 1),
                "prefab.scene", "Select one explicit default scene.")
        scene = item(document, "scenes", document.get("scene", 0))
        roots = scene.get("nodes", [])
        require(isinstance(roots, list), "prefab.hierarchy", "Expected scene root node indices.")
        pending = [(index, None, IDENTITY) for index in reversed(roots)]
        visited, identities, result = set(), set(), []
        while pending:
            index, parent, parent_world = pending.pop()
            node = item(document, "nodes", index)
            require(index not in visited and len(visited) < 4096, "prefab.hierarchy", "Cycle, repeated node or node budget exceeded.")
            visited.add(index)
            key = identity(node, "luminumbra.object_id")
            require(key not in identities, "prefab.identity", "Duplicate persistent object ID.")
            identities.add(key)
            require(not {"skin", "camera", "weights"} & node.keys(),
                    "prefab.component", "Skins, cameras and other runtime components need registered profiles.")
            local = transform(node)
            world = multiply(parent_world, local)
            require(all(math.isfinite(v) and abs(v) <= 1e30 for v in world), "prefab.transform", "World transform overflow.")
            record = {"id": key, "label": node.get("name", ""), "parent": parent, "local_matrix": local,
                      "reverse_front_face": determinant(world) < 0}
            require(isinstance(record["label"], str), "prefab.label", "Node labels must be strings.")
            if "mesh" in node:
                record["mesh"] = self.mesh(node["mesh"])
            result.append(record)
            children = node.get("children", [])
            require(isinstance(children, list), "prefab.hierarchy", "Expected child node indices.")
            pending.extend((child, key, world) for child in reversed(children))
        return result
