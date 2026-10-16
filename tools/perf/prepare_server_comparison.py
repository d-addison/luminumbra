#!/usr/bin/env python3
"""Prepare an explicit historical workload in isolated CI base/head checkouts.

Both binaries receive the same guarded fixture adapter. Only roots containing
.perf-controlled use it; ordinary shipping and qualification roots are untouched.
The receipt records the source adaptation and hashes actual common data/presets.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil

from fixture_hash import fixture_hash
from fixture_paths import open_regular, read_text, resolve_reference

GATES = ("ActiveRegions", "WaterHighRes", "WeatherRain", "WeatherEvents", "AetherState")
TUNINGS = ("Ecology", "WildlifeFoliage", "Thirst", "Scavenging", "Foraging", "Reproduction")
MARKER = ".perf-controlled"


def identity(root: Path) -> str:
    data = resolve_reference(root, "data")
    inputs = {"common": fixture_hash(resolve_reference(root, "data/common"), scope="headless-server"),
              "presets": fixture_hash(resolve_reference(root, "worlds/atlas/presets"),
                                      reference_root=data),
              "configuration": read_text(root.resolve(), resolve_reference(root, MARKER))}
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


def prepare(base: Path, head: Path, receipt: Path) -> None:
    # Take the base content once. The source adapters pin lower-layer defaults,
    # including finite hydrology, which systems.json alone cannot configure.
    common = resolve_reference(base, "data/common")
    presets = resolve_reference(base, "worlds/atlas/presets")
    # Refuse unsafe source trees before copytree follows links or adapters edit
    # either checkout. Retain dependencies outside common in the copied fixture.
    fixture_hash(common, scope="headless-server")
    dependencies: dict[str, Path] = {}
    fixture_hash(presets, reference_root=resolve_reference(base, "data"),
                 dependencies=dependencies)
    systems = json.loads(read_text(common, common / "systems.json", encoding="utf-8-sig"))
    schema = json.loads((head / "src/luminumbra_common/core/ConfigSchema.json").read_text(encoding="utf-8-sig"))
    # Registry fields are authored in the schema; do not omit an absent base key.
    for key in schema["keys"]:
        systems.setdefault(key["section"], {}).setdefault(key["json_name"], {})["enabled"] = False
    records = []
    for checkout in (base, head):
        root = checkout / "perf-common"
        if root.exists():
            raise ValueError(f"refusing to replace existing fixture: {root}")
        def copy_fixture(source, destination):
            source = Path(source)
            source_root = common if source.is_relative_to(common) else presets
            with open_regular(source_root, source) as stream, Path(destination).open("wb") as output:
                shutil.copyfileobj(stream, output)
            return destination

        shutil.copytree(common, root / "data/common", copy_function=copy_fixture)
        shutil.copytree(presets, root / "worlds/atlas/presets", copy_function=copy_fixture)
        for name, path in sorted(dependencies.items()):
            destination = resolve_reference(root / "data", name)
            if not destination.exists() and path.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                with open_regular(resolve_reference(base, "data"), path) as stream, destination.open("wb") as output:
                    shutil.copyfileobj(stream, output)
        (root / "data/common/systems.json").write_text(json.dumps(systems, indent=2) + "\n")
        (root / MARKER).write_text(json.dumps(dict(schema="luminumbra.controlled_server.v1",
            gates=False, finite_hydrology=False, autosave_ticks=0), sort_keys=True) + "\n")
        source = checkout / "src/luminumbra_server/ServerWorldRunner.cpp"
        before = source.read_bytes()
        text = before.decode("utf-8-sig")
        boot_anchor = "    bool world_ready = false;"
        water_anchor = "    world_system->debug_set_streaming_radius_cap(m_config.test_streaming_radius_cap);"
        if text.count(boot_anchor) != 1 or text.count(water_anchor) != 1:
            raise ValueError(f"comparison adapter anchors changed: {source}")
        guard = f'    if (std::filesystem::exists(std::filesystem::path(m_config.root_path) / "{MARKER}")) {{\n'
        config = guard + "        m_config.autosave_interval_ticks = 0;\n" + ''.join(f"        m_session->Set{gate}Enabled(false);\n" for gate in GATES)
        session_header = checkout / "src/luminumbra_common/world/GameSession.h"
        if session_header.exists() and "SetFiniteHydrologyEnabled" in session_header.read_text():
            config += "        m_session->SetFiniteHydrologyEnabled(false);\n"
        config += ''.join(f"        m_session->Set{tuning}Tuning({{}});\n" for tuning in TUNINGS)
        config += "        m_session->SetCircadianAmplitude(1.0f);\n        m_session->SetPlantMutationRate(0.05f);\n    }\n\n"
        text = text.replace(boot_anchor, config + boot_anchor)
        text = text.replace(water_anchor, water_anchor + "\n" + guard +
                            "        world_system->SetWaterHydrology(false, 0, 0);\n    }")
        source.write_text(text, encoding="utf-8")
        records.append(dict(checkout=str(checkout), fixture_hash=identity(root),
                            source=str(source.relative_to(checkout)),
                            source_before_sha256=hashlib.sha256(before).hexdigest(),
                            source_after_sha256=hashlib.sha256(source.read_bytes()).hexdigest()))
    if records[0]["fixture_hash"] != records[1]["fixture_hash"]:
        raise ValueError("controlled fixtures differ")
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.write_text(json.dumps(dict(schema="luminumbra.server_comparison_fixture.v1",
        purpose="common historical workload; new shipping workload qualifies separately",
        runs=records), indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path)
    parser.add_argument("--head", type=Path)
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--hash", type=Path, dest="hash_root")
    args = parser.parse_args()
    if args.hash_root:
        print(identity(args.hash_root))
    else:
        if not all((args.base, args.head, args.receipt)):
            parser.error("--base, --head and --receipt are required for preparation")
        prepare(args.base, args.head, args.receipt)


if __name__ == "__main__":
    main()
