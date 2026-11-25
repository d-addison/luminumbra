"""Qualify packaged inline texture planning without a native compiler or renderer."""

import copy
import math
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SERVICE = ROOT / "tools/blender/authoring/service"
sys.path.insert(0, str(SERVICE))
sys.path.insert(0, str(SERVICE / "tests"))

from luminumbra_author.contracts import Refusal
from luminumbra_author.formats import texture_info
from luminumbra_author.prefab import PrefabPlan, encode_glb
from prefab_fixture import fixture


def plan_for(document, binary):
    return PrefabPlan(encode_glb(document, binary), "fixture.prefab")


class InlineTextureQualification(unittest.TestCase):
    def test_cutoff_and_factor_preserve_material_and_non_cutout_policies(self):
        # The fixture is bounded to two draws and two embedded 8x8 images.
        for mode in ("OPAQUE", "MASK", "BLEND"):
            for cutoff in (None, 0, .25, .5, .75, 1):
                for factor in (.8, 1):
                    if mode == "MASK" and cutoff is not None and cutoff > factor:
                        continue
                    with self.subTest(mode=mode, cutoff=cutoff, factor=factor):
                        document, binary = fixture()
                        source = document["materials"][1]
                        source["alphaMode"] = mode
                        source["pbrMetallicRoughness"]["baseColorFactor"][3] = factor
                        if cutoff is None:
                            source.pop("alphaCutoff")
                        else:
                            source["alphaCutoff"] = cutoff
                        binding = source["pbrMetallicRoughness"]["baseColorTexture"]
                        source["pbrMetallicRoughness"]["metallicRoughnessTexture"] = (
                            copy.deepcopy(binding))
                        source["occlusionTexture"] = copy.deepcopy(binding)
                        source["emissiveTexture"] = copy.deepcopy(binding)
                        original = copy.deepcopy(document)
                        plan = plan_for(document, binary)
                        repeated = plan_for(document, binary)
                        self.assertEqual(plan.tasks, repeated.tasks)
                        self.assertEqual(plan.description, repeated.description)
                        self.assertEqual(document, original)
                        material = plan.materials["fixture.leaf"]
                        declared = .5 if cutoff is None else cutoff
                        self.assertEqual(material["alpha_mode"], mode)
                        self.assertEqual(material["alpha_cutoff"], declared)
                        self.assertEqual(material["base_color"], [1, 1, 1, factor])
                        self.assertTrue(material["double_sided"])
                        for role, policy in (("baseColorTexture", "--srgb"),
                                             ("normalTexture", "--normal-map"),
                                             ("metallicRoughnessTexture", "--linear"),
                                             ("occlusionTexture", "--linear"),
                                             ("emissiveTexture", "--srgb")):
                            texture = material["textures"][role]
                            expected = [policy]
                            if role == "baseColorTexture" and mode == "MASK":
                                expected += ["--alpha-cutoff", str(declared / factor)]
                            task = plan.tasks[texture["file"]]
                            self.assertEqual(task["arguments"], expected)
                            self.assertEqual(task["dimensions"], [8, 8])
                            self.assertEqual(texture["encoding"],
                                             "srgb" if policy == "--srgb" else "linear")
                            self.assertEqual(texture["coordinates"],
                                             {"set": 1, "offset": [.125, .25],
                                              "scale": [.5, .75], "rotation": 0})
                        self.assertEqual(material["textures"]["baseColorTexture"]["sampler"],
                                         {"wrapS": 33071, "wrapT": 33648,
                                          "magFilter": 9728, "minFilter": 9984})

    def test_shared_image_deduplicates_only_equal_effective_cutoffs(self):
        document, binary = fixture()
        leaf = document["materials"][1]
        leaf["pbrMetallicRoughness"]["baseColorFactor"][3] = 1
        paint = copy.deepcopy(leaf)
        paint["extras"]["luminumbra.material_id"] = "fixture.paint"
        document["materials"][0] = paint
        baseline = plan_for(document, binary)
        baseline_meshes = {key: task for key, task in baseline.tasks.items()
                           if task["kind"] == "mesh"}
        for cutoff, factor, equal in ((.5, 1, True), (.25, 1, False), (.5, .8, False),
                                      (.4, .8, True), (0, 1, False), (1, 1, False)):
            with self.subTest(cutoff=cutoff, factor=factor):
                leaf["alphaCutoff"] = cutoff
                leaf["pbrMetallicRoughness"]["baseColorFactor"][3] = factor
                plan = plan_for(document, binary)
                first = plan.materials["fixture.paint"]["textures"]["baseColorTexture"]["file"]
                second = plan.materials["fixture.leaf"]["textures"]["baseColorTexture"]["file"]
                self.assertEqual(first == second, equal)
                self.assertEqual(plan.tasks[first]["payload"], plan.tasks[second]["payload"])
                self.assertEqual(plan.materials["fixture.paint"],
                                 baseline.materials["fixture.paint"])
                self.assertEqual({key: task for key, task in plan.tasks.items()
                                  if task["kind"] == "mesh"},
                                 baseline_meshes)

    def test_invalid_cutoffs_and_fully_discarded_factors_are_refused(self):
        cases = [(value, 1, "prefab.number") for value in
                 (-.01, 1.01, float("nan"), float("inf"), "0.5", True)]
        cases += [(.5, factor, "prefab.alpha") for factor in (0, .25)]
        cases += [(1, .8, "prefab.alpha")]
        for cutoff, factor, rule in cases:
            with self.subTest(cutoff=cutoff, factor=factor):
                document, binary = fixture()
                document["materials"][1]["alphaCutoff"] = cutoff
                document["materials"][1]["pbrMetallicRoughness"]["baseColorFactor"][3] = factor
                # Nonfinite JSON is rejected by the canonical encoder before planning.
                if isinstance(cutoff, float) and not math.isfinite(cutoff):
                    with self.assertRaises(ValueError):
                        plan_for(document, binary)
                else:
                    before = encode_glb(document, binary)
                    with self.assertRaises(Refusal) as error:
                        plan_for(document, binary)
                    self.assertEqual(error.exception.finding["rule_id"], rule)
                    self.assertEqual(encode_glb(document, binary), before)

    def test_cutoff_bytes_use_the_existing_complete_ltex_v1_layout(self):
        pixels = bytes((255, 0, 0, 0, 255, 0, 0, 64, 255, 0, 0, 128, 255, 0, 0, 255))
        for alpha in (112, 64, 128, 191, 254):
            payload = (struct.pack("<4sHHIIB", b"LTEX", 1, 2, 2, 2, 4)
                       + pixels + bytes((255, 0, 0, alpha)))
            with self.subTest(alpha=alpha):
                info = texture_info(payload)
                for field, value in (("format", "LTEX"), ("width", 2), ("height", 2),
                                     ("channels", 4), ("mip_levels", 2)):
                    self.assertEqual(info[field], value)
                for invalid in (b"", payload[:16], payload[:-1], payload + b"x",
                                payload[:6] + b"\x01\0" + payload[8:],
                                payload[:6] + b"\x03\0" + payload[8:]):
                    with self.assertRaises(Refusal):
                        texture_info(invalid)


if __name__ == "__main__":
    unittest.main()
