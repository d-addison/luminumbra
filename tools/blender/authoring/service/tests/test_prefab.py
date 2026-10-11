import copy
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from luminumbra_author.contracts import Refusal, atomic_json, read_json
from luminumbra_author.formats import texture_info, validate_glb
from luminumbra_author.prefab import PrefabPlan, PROFILE, encode_glb, multiply
from prefab_fixture import fixture
import test_service as helpers


class PrefabContracts(unittest.TestCase):
    def test_hierarchy_shared_geometry_materials_uv_selection_and_samplers(self):
        document, binary = fixture()
        original = copy.deepcopy(document)
        plan = PrefabPlan(encode_glb(document, binary), "fixture.prefab")
        self.assertEqual(document, original)
        self.assertEqual(len(plan.tasks), 4) # Two primitive draws, two shared images.
        nodes = {node["id"]: node for node in plan.description["nodes"]}
        self.assertEqual(nodes["fixture.first"]["parent"], "fixture.root")
        self.assertEqual(nodes["fixture.first"]["mesh"], nodes["fixture.second"]["mesh"])
        self.assertTrue(nodes["fixture.second"]["reverse_front_face"])
        self.assertEqual(multiply(nodes["fixture.root"]["local_matrix"], nodes["fixture.first"]["local_matrix"])[12:15], [11,2,3])
        leaf = plan.materials["fixture.leaf"]
        self.assertEqual(leaf["alpha_cutoff"], .5)
        texture = leaf["textures"]["baseColorTexture"]
        self.assertEqual(texture["sampler"]["wrapS"], 33071)
        self.assertEqual(texture["coordinates"]["set"], 1)
        self.assertEqual(plan.tasks[texture["file"]]["arguments"], ["--srgb", "--alpha-cutoff", "0.625"])
        for task in plan.tasks.values():
            if task["kind"] == "mesh":
                derived = validate_glb(encode_glb(task["payload"], plan.binary))
                self.assertEqual(derived["nodes"], [{"mesh":0}])
                selector = derived["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]
                self.assertEqual(selector["texCoord"], 1)
                self.assertNotIn("extensions", selector)

    def test_refuses_ambiguous_or_unfaithful_authored_content(self):
        def bad_uv(d):
            d["materials"][1]["normalTexture"]["extensions"]["KHR_texture_transform"]["offset"]=[.5,0]
        cases = [
            (lambda d: d["nodes"][2]["extras"].update({"luminumbra.object_id":"fixture.first"}), "prefab.identity"),
            (lambda d: d["nodes"][2].update(children=[0]), "prefab.hierarchy"),
            (lambda d: d["nodes"][2].update(mesh=99), "prefab.index"),
            (lambda d: d["nodes"][2].update(scale=[0,1,1]), "prefab.transform"),
            (lambda d: d["nodes"][2].update(camera=0), "prefab.component"),
            (lambda d: d["materials"][0].update(extensions={"KHR_materials_unlit":{}}), "prefab.extension"),
            (lambda d: d["materials"][0]["normalTexture"].update(extensions={"KHR_texture_transform":None}), "prefab.extension"),
            (lambda d: d["meshes"][0]["primitives"][0].update(targets=[{}]), "prefab.primitive"),
            (lambda d: d["nodes"][2].update(rotation=[0,0,0,2]), "prefab.number"),
            (lambda d: d["images"][0].update(bufferView=999), "prefab.index"),
            (bad_uv, "prefab.uv_mismatch"),
        ]
        for change, rule in cases:
            with self.subTest(rule=rule):
                document, binary = fixture()
                change(document)
                with self.assertRaises(Refusal) as error:
                    PrefabPlan(encode_glb(document, binary), "fixture.prefab")
                self.assertEqual(error.exception.finding["rule_id"], rule)

    def test_same_mesh_id_cannot_hide_different_geometry(self):
        document, binary = fixture()
        other = copy.deepcopy(document["meshes"][0])
        other["primitives"][0]["indices"] = 0
        document["meshes"].append(other)
        document["nodes"][2]["mesh"] = 1
        with self.assertRaises(Refusal) as error:
            PrefabPlan(encode_glb(document, binary), "fixture.prefab")
        self.assertEqual(error.exception.finding["rule_id"], "prefab.identity")

    def test_shared_geometry_can_use_distinct_exporter_buffer_offsets(self):
        document, binary = fixture()
        other = copy.deepcopy(document["meshes"][0])
        source = document["accessors"][0]
        view = document["bufferViews"][source["bufferView"]]
        duplicate = copy.deepcopy(source)
        duplicate["bufferView"] = len(document["bufferViews"])
        offset = len(binary) + (-len(binary) % 4)
        binary += b"\0" * (-len(binary) % 4) + binary[view["byteOffset"]:view["byteOffset"]+view["byteLength"]]
        document["buffers"][0]["byteLength"] = len(binary)
        document["bufferViews"].append({"buffer":0,"byteOffset":offset,"byteLength":view["byteLength"]})
        document["accessors"].append(duplicate)
        for primitive in other["primitives"]:
            primitive["attributes"]["POSITION"] = len(document["accessors"])-1
        document["meshes"].append(other)
        document["nodes"][2]["mesh"] = 1
        plan = PrefabPlan(encode_glb(document, binary), "fixture.prefab")
        self.assertEqual(len(plan.meshes),1)
        self.assertEqual(len(plan.tasks),4)

    def test_texture_output_requires_exact_complete_mip_bytes(self):
        good = struct.pack("<4sHHIIB", b"LTEX",1,2,2,2,4) + bytes(20)
        self.assertEqual(texture_info(good)["mip_levels"], 2)
        for payload in (good[:-1], good+b"x", good[:6]+b"\x01\0"+good[8:]):
            with self.assertRaises(Refusal):
                texture_info(payload)


class PrefabPublication(unittest.TestCase):
    setUp, open, wait = helpers.Project.setUp, helpers.Project.open, helpers.Project.wait

    def prepare(self):
        document, binary = fixture()
        self.asset["profile"] = PROFILE
        atomic_json(self.project / "asset.json", self.asset)
        (self.project / "triangle.glb").write_bytes(encode_glb(document, binary))
        return self.open()

    def compile_part(self, job_id, source, output, arguments):
        if output.suffix == ".lmesh":
            output.write_bytes(helpers.triangle())
        else:
            output.write_bytes(struct.pack("<4sHHIIB",b"LTEX",1,4,8,8,4)+bytes((64+16+4+1)*4))

    def test_complete_generation_and_partial_failure_cancellation_retention(self):
        service = self.prepare()
        with patch.object(service, "_invoke", side_effect=self.compile_part):
            first = self.wait(service, service.submit("asset.json",1))
        self.assertEqual(first["status"],"succeeded",first)
        pointer = service.current()
        generation = service.root / "generations" / first["job_id"]
        self.assertEqual(read_json(generation/"prefab.json")["schema"],"luminumbra.asset.prefab.v1")
        self.assertEqual(len(first["outputs"]),5)
        self.assertEqual(list((service.root/"jobs"/first["job_id"]).glob("part-*")),[])
        for action in ("fail", "cancel", "source", "truncate"):
            calls = []
            original = (self.project/"source.blend").read_bytes()
            def interrupted(job_id, source, output, arguments):
                self.compile_part(job_id,source,output,arguments)
                calls.append(output.name)
                if len(calls)==2:
                    if action=="fail":
                        raise OSError("Injected later-part compiler failure")
                    if action=="cancel":
                        service.cancel(job_id)
                    if action=="source":
                        (self.project/"source.blend").write_bytes(b"edited")
                    if action=="truncate":
                        output.write_bytes(b"invalid")
            with patch.object(service,"_invoke",side_effect=interrupted):
                result=self.wait(service,service.submit("asset.json",1))
            self.assertEqual(result["status"],{"cancel":"cancelled","source":"stale"}.get(action,"failed"),result)
            self.assertEqual(service.current(),pointer)
            self.assertEqual(service.generation(first["job_id"])["outputs"],first["outputs"])
            (self.project/"source.blend").write_bytes(original)
