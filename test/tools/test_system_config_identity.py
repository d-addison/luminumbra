"""Independent stdlib computation of SystemConfig's canonical session identity."""

import copy
import json
import math
from pathlib import Path
import struct
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCHEMA = json.loads(
    (ROOT / "src/luminumbra_common/core/ConfigSchema.json").read_text(encoding="utf-8")
)
SHIPPED = json.loads((ROOT / "data/common/systems.json").read_text(encoding="utf-8-sig"))


def canonical_bytes(document):
    result = bytearray(b"system-config:v2:")
    for key_id, key in enumerate(SCHEMA["keys"]):
        if key["section"] != "sim" and key["enum"] != "RenderCircadian":
            continue
        entry = document.get(key["section"], {}).get(key["json_name"], {})
        enabled = entry.get("enabled", True)
        # Only these flags gate session simulation; other flags select fallback tuning.
        gates = {"SimHydrologyWeather", "SimWeatherEvents", "SimAetherState",
                 "SimWaterHighRes", "SimActiveRegions"}
        result.extend((key_id, int(enabled if key["enum"] in gates else True)))
        for param_id, param in enumerate(SCHEMA["params"]):
            if param["owner"] != key["enum"]:
                continue
            vector = param["type"] == "vec3"
            binary64 = param["owner"] == "SimForaging"
            ticks = param["enum"] in {
                "ReproMaturityTicks", "ReproCooldownTicks", "ReproCourtshipTicks"
            }
            result.extend((param_id, 1 if vector else 2 if binary64 else 3 if ticks else 0))
            value = param["default"]
            if enabled:
                value = entry.get("params", {}).get(param["json_name"], value)
            for component in value if vector else [value]:
                # SystemConfig stores JSON scalar inputs as binary32; ForagingParams
                # widens that resolved value to double, and tick tuning truncates to u32.
                resolved = struct.unpack("<f", struct.pack("<f", component))[0]
                if param["enum"] == "PlantMutationRate" and resolved < 0:
                    resolved = 0.0
                result.extend(struct.pack("<d" if binary64 else "<I" if ticks else "<f",
                                          int(resolved) if ticks else resolved))
    return bytes(result)


def identity(document):
    value = 14695981039346656037
    for byte in canonical_bytes(document):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{value:016x}"


class SystemConfigIdentityTest(unittest.TestCase):
    def test_historical_workload_explicitly_covers_every_registered_flag(self):
        historical = json.loads(
            (ROOT / "test/fixtures/historical-defaults-off.json").read_text(encoding="utf-8")
        )
        render_on = {"far_world_ladder", "plant_procgen", "sky_lut_gpu",
                     "live_weather", "aether_tap", "snow_cover"}
        for section in ("sim", "render"):
            keys = [key["json_name"] for key in SCHEMA["keys"] if key["section"] == section]
            self.assertEqual(set(historical[section]), set(keys))
            for name in keys:
                with self.subTest(section=section, name=name):
                    self.assertIs(historical[section][name]["enabled"],
                                  section == "render" and name in render_on)
        expected = {"sim": {key["json_name"]: {"enabled": False}
                            for key in SCHEMA["keys"] if key["section"] == "sim"},
                    "render": {"circadian": {"enabled": False}}}
        self.assertEqual(canonical_bytes(historical), canonical_bytes(expected))
        self.assertNotEqual(identity(historical), identity(SHIPPED))

    def test_exact_shipping_pin(self):
        self.assertEqual(len(canonical_bytes(SHIPPED)), 279)
        self.assertEqual(identity(SHIPPED), "da24cea9dbc5159b")
        self.assertEqual(canonical_bytes({}), canonical_bytes(SHIPPED))

    def test_every_applied_flag_and_parameter(self):
        baseline = identity(SHIPPED)
        for key in SCHEMA["keys"]:
            if key["section"] != "sim" and key["enum"] != "RenderCircadian":
                continue
            with self.subTest(flag=key["enum"]):
                changed = copy.deepcopy(SHIPPED)
                changed[key["section"]][key["json_name"]]["enabled"] = False
                if key["enum"] in {"SimHydrologyWeather", "SimWeatherEvents", "SimAetherState",
                                   "SimWaterHighRes", "SimActiveRegions"}:
                    self.assertNotEqual(identity(changed), baseline)
                else:
                    self.assertEqual(identity(changed), baseline)
            for param in SCHEMA["params"]:
                if param["owner"] != key["enum"]:
                    continue
                with self.subTest(param=param["enum"]):
                    changed = copy.deepcopy(SHIPPED)
                    value = param["default"]
                    value = [value[0] + 1, *value[1:]] if isinstance(value, list) else value + 1
                    changed[key["section"]][key["json_name"]].setdefault("params", {})[
                        param["json_name"]
                    ] = value
                    self.assertNotEqual(identity(changed), baseline)

    def test_exact_effective_precision(self):
        self.assertNotEqual(struct.pack("<d", 1.0), struct.pack("<d", math.nextafter(1.0, 2.0)))
        self.assertEqual(struct.pack("<f", 1.0), struct.pack("<f", math.nextafter(1.0, 2.0)))
        self.assertNotEqual(struct.pack("<I", (1 << 32) - 1), struct.pack("<I", (1 << 32) - 2))
        fractional_ticks = {"sim": {"reproduction": {"params": {"maturity_ticks": 90.75}}}}
        self.assertEqual(identity(fractional_ticks), identity({}))

    def test_presentation_does_not_change_identity(self):
        changed = copy.deepcopy(SHIPPED)
        changed["render"]["moonlight"]["enabled"] = False
        changed["render"]["moonlight"]["params"]["strength"] = 0.75
        changed["user"] = {"video": {"fov": 90}}
        self.assertEqual(identity(changed), identity(SHIPPED))

    def test_disabled_tuning_uses_compiled_default(self):
        disabled = {"sim": {"ecology": {"enabled": False}}}
        changed = copy.deepcopy(disabled)
        changed["sim"]["ecology"]["params"] = {"herd_weight": 0.9}
        self.assertEqual(identity(disabled), identity(changed))


if __name__ == "__main__":
    unittest.main()
