"""Background Blender exporter probe for the viewport's coordinate boundary."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

import bpy
from mathutils import Euler, Matrix, Quaternion, Vector

root = Path(sys.argv[sys.argv.index("--") + 1])
spec = importlib.util.spec_from_file_location("viewport_math", root / "viewport_math.py")
v = importlib.util.module_from_spec(spec); spec.loader.exec_module(v)
checks = []
def check(name, condition):
    checks.append({"name": name, "passed": bool(condition)})
    if not condition: raise AssertionError(name)
def flat(matrix):
    return tuple(matrix[r][c] for c in range(4) for r in range(4))
def node_matrix(node):
    if "matrix" in node: return tuple(node["matrix"])
    q=node.get("rotation", [0,0,0,1])
    return flat(Matrix.LocRotScale(Vector(node.get("translation", [0,0,0])),
               Quaternion((q[3],q[0],q[1],q[2])), Vector(node.get("scale", [1,1,1]))))
def close(a,b): return max(abs(x-y) for x,y in zip(a,b)) < 2e-5

check("qualified Blender 5.1.0", bpy.app.version == (5,1,0))
import io_scene_gltf2
exporter_root = Path(io_scene_gltf2.__file__).resolve().parent
exporter_files = sorted(({"path": p.relative_to(exporter_root).as_posix(),
    "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
    for p in exporter_root.rglob("*.py")), key=lambda item: item["path"])
exporter_hash = hashlib.sha256(json.dumps(exporter_files, sort_keys=True,
    separators=(",", ":"), allow_nan=False).encode()).hexdigest()
check("qualified exporter source tree", exporter_hash ==
      "5fe9f5ede7e5264b0a1045dc3784e243e645a90cb6073fc73ae56bc7a10de447")
bpy.context.preferences.use_preferences_save = False
bpy.ops.object.select_all(action="SELECT"); bpy.ops.object.delete(use_global=False)
bpy.context.scene.unit_settings.scale_length=1
parent=bpy.data.objects.new("axis_parent", None);bpy.context.collection.objects.link(parent)
parent["luminumbra.object_id"]="axis.parent"
parent.location=(2,3,5);parent.rotation_euler=(.2,.3,.4);parent.scale=(-2,3,4)
mesh=bpy.data.meshes.new("asymmetric_triangle")
mesh.from_pydata([(0,0,0),(2,0,0),(0,3,5)],[],[(0,1,2)])
uv=mesh.uv_layers.new(name="UVMap")
for loop,coord in zip(uv.data,((.1,.2),(.9,.3),(.2,.8))):loop.uv=coord
child=bpy.data.objects.new("axis_child",mesh);bpy.context.collection.objects.link(child)
child["luminumbra.object_id"]="axis.child";child.parent=parent
child.matrix_parent_inverse=Matrix.Translation((.4,-.2,.3))
child.location=(1,2,3);child.rotation_euler=(.3,-.1,.2)
bpy.context.view_layer.update()
expected_parent=v.to_engine_local(flat(parent.matrix_world))
expected_child=v.to_engine_local(v.local_from_world(flat(child.matrix_world),flat(parent.matrix_world)))
expected_world=v.to_engine_local(flat(child.matrix_world))
output=root/"basis.glb"
check("actual GLB export", bpy.ops.export_scene.gltf(filepath=str(output),export_format="GLB",
      export_yup=True,export_extras=True,export_apply=True,export_animations=False,
      export_skins=False,export_morph=False,export_normals=True,export_texcoords=True)=={"FINISHED"})
data=output.read_bytes();check("GLB header and extent",data[:4]==b"glTF" and struct.unpack_from("<I",data,8)[0]==len(data))
length,kind=struct.unpack_from("<II",data,12);check("GLB JSON chunk",kind==0x4e4f534a)
gltf=json.loads(data[20:20+length]);binary_start=20+length
binary_length,binary_kind=struct.unpack_from("<II",data,binary_start)
check("GLB binary chunk",binary_kind==0x004e4942 and binary_start+8+binary_length==len(data))
binary=data[binary_start+8:]
nodes={n.get("extras",{}).get("luminumbra.object_id"):n for n in gltf["nodes"]}
check("stable parent and child IDs survive",set(nodes)=={"axis.parent","axis.child"})
p=node_matrix(nodes["axis.parent"]);c=node_matrix(nodes["axis.child"])
check("parent world basis and mirrored scale",close(p,expected_parent))
check("child parent inverse and rotation",close(c,expected_child))
check("composed world basis agrees",close(v.multiply(p,c),expected_world))
primitive=gltf["meshes"][nodes["axis.child"]["mesh"]]["primitives"][0]
accessor=gltf["accessors"][primitive["attributes"]["POSITION"]]
view=gltf["bufferViews"][accessor["bufferView"]]
check("actual float32 triangle position accessor",accessor["componentType"]==5126 and accessor["type"]=="VEC3" and accessor["count"]==3)
offset=view.get("byteOffset",0)+accessor.get("byteOffset",0);stride=view.get("byteStride",12)
positions=[struct.unpack_from("<3f",binary,offset+i*stride) for i in range(3)]
check("mesh positions use the same axis basis", all(any(close(a,b) for b in positions)
      for a in ((0,0,0),(2,0,0),(0,5,-3))))
# The view-space invariant must hold for every actually exported vertex.
blender_view=Matrix.LocRotScale(Vector((4,7,9)),Euler((.1,.4,.7)).to_quaternion(),Vector((1,1,1))).inverted()
engine_view=v.to_engine_view(flat(blender_view))
for i,original in enumerate(mesh.vertices):
    engine_vertex=v.transform(v.BASIS,(*original.co,1))
    observed=v.transform(engine_view,v.transform(v.multiply(p,c),engine_vertex))
    expected=blender_view @ child.matrix_world @ Vector((*original.co,1))
    check("exported vertex %d camera alignment"%i,close(observed,expected))
receipt={"schema":"luminumbra.viewport.basis_probe.v1","passed":True,"checks":checks,
    "blender":bpy.app.version_string,"blender_sha256":hashlib.sha256(Path(bpy.app.binary_path).read_bytes()).hexdigest(),
    "exporter_sha256":exporter_hash,"exporter_python_files":len(exporter_files),
    "probe_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    "math_sha256":hashlib.sha256((root/"viewport_math.py").read_bytes()).hexdigest(),
    "glb_sha256":hashlib.sha256(data).hexdigest(),"engine_rendered":False,"visual_approved":False,
    "scope":"Actual exporter coordinate and hierarchy boundary; no Blender viewport or depth/display qualification"}
(root/"receipt.json").write_text(json.dumps(receipt,indent=2)+"\n")
print(json.dumps({"passed":True,"checks":len(checks)}))
