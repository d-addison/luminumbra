"""Neutral authored prefab data, generated without Blender or image dependencies."""
import struct
import zlib


def png(width, height, pixels):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    rows = b"".join(b"\0" + pixels[y*width*4:(y+1)*width*4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def fixture():
    binary = bytearray()
    views, accessors = [], []
    def view(payload):
        binary.extend(b"\0" * (-len(binary) % 4))
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(payload)})
        binary.extend(payload)
        return len(views) - 1
    def accessor(payload, component, count, kind, **extra):
        accessors.append({"bufferView": view(payload), "componentType": component, "count": count, "type": kind, **extra})
        return len(accessors)-1
    position = accessor(struct.pack("<9f", 0, 0, 0, 2, 0, 0, 0, 1, 0), 5126, 3, "VEC3", min=[0,0,0], max=[2,1,0])
    normal = accessor(struct.pack("<9f", *(0,0,1)*3), 5126, 3, "VEC3")
    uv0 = accessor(struct.pack("<6f", 0,0,1,0,0,1), 5126, 3, "VEC2")
    uv1 = accessor(struct.pack("<6f", .2,.3,.8,.3,.2,.9), 5126, 3, "VEC2")
    indices = accessor(struct.pack("<3H", 0,1,2), 5123, 3, "SCALAR")
    color = png(8, 8, b"".join(bytes((20,100,40,230 if i%8<4 else 30)) for i in range(64)))
    normal_map = png(8, 8, bytes((128,128,255,255))*64)
    images = [{"bufferView": view(p), "mimeType": "image/png"} for p in (color, normal_map)]
    coords = {"texCoord": 1, "offset": [.125,.25], "scale": [.5,.75], "rotation": 0.0}
    def binding(index):
        return {"index": index, "extensions": {"KHR_texture_transform": dict(coords)}}
    attributes = {"POSITION": position, "NORMAL": normal, "TEXCOORD_0": uv0, "TEXCOORD_1": uv1}
    document = {"asset": {"version":"2.0"}, "buffers": [{"byteLength":len(binary)}],
                "bufferViews": views, "accessors": accessors, "images": images,
                "textures": [{"source": 0, "sampler": 0}, {"source": 1}],
                "samplers": [{"wrapS":33071,"wrapT":33648,"magFilter":9728,"minFilter":9984}],
                "materials": [
                    {"extras":{"luminumbra.material_id":"fixture.paint"},
                     "pbrMetallicRoughness":{"baseColorFactor":[.8,.2,.1,1],"metallicFactor":.2,"roughnessFactor":.4},
                     "normalTexture":binding(1)},
                    {"extras":{"luminumbra.material_id":"fixture.leaf"}, "alphaMode":"MASK", "alphaCutoff":.5,
                     "doubleSided":True, "normalTexture":binding(1),
                     "pbrMetallicRoughness":{"baseColorTexture":binding(0),"baseColorFactor":[1,1,1,.8]}}],
                "meshes": [{"extras":{"luminumbra.asset_id":"fixture.mesh"}, "primitives":[
                    {"attributes":attributes,"indices":indices,"material":i} for i in range(2)]}],
                "nodes":[
                    {"name":"Root", "extras":{"luminumbra.object_id":"fixture.root"},"translation":[10,2,3],"children":[1,2]},
                    {"name":"Instance", "extras":{"luminumbra.object_id":"fixture.first"},"mesh":0,"translation":[1,0,0]},
                    {"name":"Mirrored", "extras":{"luminumbra.object_id":"fixture.second"},"mesh":0,"scale":[-1,2,1]}],
                "scenes":[{"nodes":[0]}],"scene":0,"extensionsUsed":["KHR_texture_transform"]}
    return document, bytes(binary)
