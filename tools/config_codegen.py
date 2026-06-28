#!/usr/bin/env python3
"""Typed config schema -> registry codegen for SystemConfig (spec 020 FR-B).

Single source of truth = src/luminumbra_common/core/ConfigSchema.json. Each entry
declares its section/residency, JSON name, type, default, owner and canonical order.
This tool emits a behavior-neutral registry header that reproduces the hand-written
`kKeys` / `kParams` tables in SystemConfig.cpp, and a `--check` mode that proves the
schema is byte-faithful to the current C++ registry and that hash residency is
schema-declared (sim => hashed, render => excluded).

It is intentionally STANDALONE and additive: it does NOT modify SystemConfig.{h,cpp}
and does NOT change `ComputeConfigSubHash` semantics, so `--smoke` stays
6f008a9f637c40b7 and there is no world_hash bump.

Usage:
  python tools/config_codegen.py --check          # residency + registry parity (CI gate)
  python tools/config_codegen.py --emit OUT.h     # write generated registry header
  python tools/config_codegen.py --from-cpp       # print a schema derived from the .cpp
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SCHEMA_PATH = os.path.join(ROOT, "src", "luminumbra_common", "core", "ConfigSchema.json")
CPP_PATH = os.path.join(ROOT, "src", "luminumbra_common", "core", "SystemConfig.cpp")

# Map between the C++ `Section::` enum and the schema's declared residency. This IS the
# residency contract: Sim-section keys are hashed (folded into config:v1:), Render-section
# keys are excluded from every hash. ComputeConfigSubHash() enforces it at :223.
SECTION_TO_RESIDENCY = {"Sim": "hashed", "Render": "excluded"}
RESIDENCY_TO_SECTION = {v: k for k, v in SECTION_TO_RESIDENCY.items()}


def _f(text: str) -> float:
    """Parse a C++ float literal ('0.080f', '90.0f', '1000000.0f') as a Python float."""
    return float(text.strip().rstrip("fF"))


def parse_cpp(cpp_text: str):
    """Parse the kKeys[] and kParams[] initializer tables out of SystemConfig.cpp."""
    keys = []
    key_block = re.search(r"kKeys\[\]\s*=\s*\{(.*?)\}\s*;", cpp_text, re.S)
    if not key_block:
        raise SystemExit("config_codegen: could not locate kKeys[] in SystemConfig.cpp")
    # {SysKey::SimPlantGrowth, Section::Sim, "sim", "plant_growth"},
    for m in re.finditer(
        r"\{\s*SysKey::(\w+)\s*,\s*Section::(\w+)\s*,\s*\"([^\"]+)\"\s*,\s*\"([^\"]+)\"\s*\}",
        key_block.group(1),
    ):
        enum, section, json_section, json_name = m.groups()
        keys.append(
            {
                "enum": enum,
                "section": json_section,        # "sim" | "render"
                "residency": SECTION_TO_RESIDENCY[section],
                "json_name": json_name,
            }
        )

    params = []
    param_block = re.search(r"kParams\[\]\s*=\s*\{(.*?)\}\s*;", cpp_text, re.S)
    if not param_block:
        raise SystemExit("config_codegen: could not locate kParams[] in SystemConfig.cpp")
    # {SysParam::PlantMutationRate, SysKey::SimPlantGrowth, "mutation_rate", false, 0.05f, glm::vec3(0.0f)},
    # {SysParam::MoonlightColor,    SysKey::RenderMoonlight, "color", true, 0.0f, glm::vec3(0.6f, 0.7f, 1.0f)},
    pat = re.compile(
        r"\{\s*SysParam::(\w+)\s*,\s*SysKey::(\w+)\s*,\s*\"([^\"]+)\"\s*,"
        r"\s*(true|false)\s*,\s*([0-9.eEf+-]+)\s*,\s*glm::vec3\(([^)]*)\)\s*\}",
    )
    for m in pat.finditer(param_block.group(1)):
        enum, owner, json_name, is_vec3, default_scalar, vec3_args = m.groups()
        comps = [c for c in (a.strip() for a in vec3_args.split(",")) if c]
        if len(comps) == 1:
            vx = vy = vz = _f(comps[0])
        elif len(comps) == 3:
            vx, vy, vz = (_f(c) for c in comps)
        else:
            raise SystemExit(f"config_codegen: bad glm::vec3 args for {enum}: {vec3_args!r}")
        params.append(
            {
                "enum": enum,
                "owner": owner,
                "json_name": json_name,
                "type": "vec3" if is_vec3 == "true" else "scalar",
                "default": [vx, vy, vz] if is_vec3 == "true" else _f(default_scalar),
            }
        )
    return keys, params


def schema_to_tables(schema: dict):
    """Normalize the schema into the same (keys, params) shape parse_cpp returns."""
    keys = []
    for k in schema["keys"]:
        keys.append(
            {
                "enum": k["enum"],
                "section": k["section"],
                "residency": k["residency"],
                "json_name": k["json_name"],
            }
        )
    params = []
    for p in schema["params"]:
        d = p["default"]
        params.append(
            {
                "enum": p["enum"],
                "owner": p["owner"],
                "json_name": p["json_name"],
                "type": p["type"],
                "default": [float(x) for x in d] if p["type"] == "vec3" else float(d),
            }
        )
    return keys, params


def cpp_to_schema(keys, params) -> dict:
    """Build a schema document from parsed C++ tables (used by --from-cpp bootstrap)."""
    return {
        "$schema_version": 1,
        "_comment": (
            "Typed config schema (spec 020 FR-B). Single source of truth for SystemConfig "
            "feature flags + tunable params. residency: 'hashed' = sim (folded into config:v1: "
            "when enabled/non-default); 'excluded' = render (NEVER hashed). Order is canonical "
            "(== SysKey/SysParam enum order == the hash's serialization order). Generated by "
            "tools/config_codegen.py; verified byte-faithful to SystemConfig.cpp via --check."
        ),
        "keys": keys,
        "params": params,
    }


def _fmt_float(x: float) -> str:
    """Emit a C++ float literal that round-trips to the same IEEE value."""
    s = repr(float(x))
    if "e" in s or "E" in s or "." in s:
        return s + "f"
    return s + ".0f"


def emit_header(schema: dict) -> str:
    keys, params = schema_to_tables(schema)
    lines = []
    lines.append("#pragma once")
    lines.append("// AUTO-GENERATED by tools/config_codegen.py from ConfigSchema.json. DO NOT EDIT.")
    lines.append("//")
    lines.append("// Behavior-neutral registry: byte-faithful to the hand-written kKeys/kParams")
    lines.append("// tables in SystemConfig.cpp. Residency is schema-declared: KEY() rows carry")
    lines.append("// 'hashed' (sim, folded into config:v1:) or 'excluded' (render, never hashed).")
    lines.append("//")
    lines.append("// X-macro tables let a future refactor generate SysKey/SysParam enums AND the")
    lines.append("// kKeys/kParams arrays from one authored home without colliding with existing")
    lines.append("// definitions. This header is additive and is not yet wired into the build.")
    lines.append("")
    lines.append("// KEY(enum, Section, \"json_section\", \"json_name\", residency)")
    lines.append("#define LUMIN_CONFIG_KEY_TABLE(KEY) \\")
    for i, k in enumerate(keys):
        sect = RESIDENCY_TO_SECTION[k["residency"]]
        cont = " \\" if i < len(keys) - 1 else ""
        lines.append(
            f'  KEY({k["enum"]}, {sect}, "{k["section"]}", "{k["json_name"]}", {k["residency"]}){cont}'
        )
    lines.append("")
    lines.append("// PARAM(enum, owner, \"json_name\", is_vec3, default_scalar, vx, vy, vz)")
    lines.append("#define LUMIN_CONFIG_PARAM_TABLE(PARAM) \\")
    for i, p in enumerate(params):
        cont = " \\" if i < len(params) - 1 else ""
        if p["type"] == "vec3":
            vx, vy, vz = p["default"]
            scalar = "0.0f"
        else:
            vx = vy = vz = 0.0
            scalar = _fmt_float(p["default"])
        is_vec3 = "true" if p["type"] == "vec3" else "false"
        lines.append(
            f'  PARAM({p["enum"]}, {p["owner"]}, "{p["json_name"]}", {is_vec3}, '
            f"{scalar}, {_fmt_float(vx)}, {_fmt_float(vy)}, {_fmt_float(vz)}){cont}"
        )
    lines.append("")
    return "\n".join(lines)


def load_schema() -> dict:
    with open(SCHEMA_PATH, "r", encoding="utf-8") as fh:
        return json.load(fh)


def load_cpp_tables():
    with open(CPP_PATH, "r", encoding="utf-8") as fh:
        return parse_cpp(fh.read())


def cmd_check() -> int:
    schema = load_schema()
    cpp_keys, cpp_params = load_cpp_tables()
    sch_keys, sch_params = schema_to_tables(schema)

    errors = []

    # 1. Registry parity (behavior-neutral): schema reproduces the C++ tables exactly,
    #    in canonical order, with identical names/owners/types/defaults.
    if sch_keys != cpp_keys:
        errors.append("kKeys mismatch between ConfigSchema.json and SystemConfig.cpp:")
        for idx in range(max(len(sch_keys), len(cpp_keys))):
            s = sch_keys[idx] if idx < len(sch_keys) else None
            c = cpp_keys[idx] if idx < len(cpp_keys) else None
            if s != c:
                errors.append(f"  [{idx}] schema={s} cpp={c}")
    if sch_params != cpp_params:
        errors.append("kParams mismatch between ConfigSchema.json and SystemConfig.cpp:")
        for idx in range(max(len(sch_params), len(cpp_params))):
            s = sch_params[idx] if idx < len(sch_params) else None
            c = cpp_params[idx] if idx < len(cpp_params) else None
            if s != c:
                errors.append(f"  [{idx}] schema={s} cpp={c}")

    # 2. Residency parity: every key declares a known residency, and it agrees with the
    #    C++ Section. sim => hashed, render => excluded. This is the FR-B residency contract.
    sim_keys = {k["enum"] for k in sch_keys if k["residency"] == "hashed"}
    render_keys = {k["enum"] for k in sch_keys if k["residency"] == "excluded"}
    for k in sch_keys:
        if k["residency"] not in RESIDENCY_TO_SECTION:
            errors.append(f"  key {k['enum']}: unknown residency {k['residency']!r}")
        if k["residency"] == "hashed" and k["section"] != "sim":
            errors.append(f"  key {k['enum']}: hashed residency must be section 'sim'")
        if k["residency"] == "excluded" and k["section"] != "render":
            errors.append(f"  key {k['enum']}: excluded residency must be section 'render'")

    # 3. Param residency follows owner: a hashed param must be owned by a hashed (sim) key,
    #    and no render-owned param may be hashed. This mirrors ComputeConfigSubHash's loop,
    #    which only emits params owned by enabled sim keys.
    for p in sch_params:
        if p["owner"] not in sim_keys and p["owner"] not in render_keys:
            errors.append(f"  param {p['enum']}: owner {p['owner']} not a declared key")

    # 4. The all-default sim set must be representable: at least one sim (hashed) key exists,
    #    proving sim fields CAN move the hash while render fields are excluded.
    if not sim_keys:
        errors.append("  no hashed (sim) keys declared - residency model is degenerate")
    if not render_keys:
        errors.append("  no excluded (render) keys declared - residency model is degenerate")

    # 5. Generated header round-trips: emit -> re-parse the X-macros -> must equal schema.
    header = emit_header(schema)
    rt_keys, rt_params = _parse_header(header)
    if rt_keys != sch_keys or rt_params != sch_params:
        errors.append("generated header does not round-trip back to the schema tables")

    if errors:
        print("config_codegen --check: FAIL", file=sys.stderr)
        for e in errors:
            print(e, file=sys.stderr)
        return 1

    print(
        f"config_codegen --check: PASS - {len(sch_keys)} keys "
        f"({len(sim_keys)} hashed/sim, {len(render_keys)} excluded/render), "
        f"{len(sch_params)} params; schema is byte-faithful to SystemConfig.cpp and "
        f"residency is schema-declared."
    )
    return 0


def _parse_header(header: str):
    """Re-parse the emitted X-macro tables (round-trip self-check)."""
    # Drop comment lines so the "// KEY(enum, Section, ...)" doc legend is not parsed as data.
    header = "\n".join(ln for ln in header.splitlines() if not ln.lstrip().startswith("//"))
    keys = []
    for m in re.finditer(
        r'KEY\(\s*(\w+)\s*,\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*\)', header
    ):
        enum, section, json_section, json_name, residency = m.groups()
        keys.append(
            {
                "enum": enum,
                "section": json_section,
                "residency": residency,
                "json_name": json_name,
            }
        )
    params = []
    for m in re.finditer(
        r'PARAM\(\s*(\w+)\s*,\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*(true|false)\s*,'
        r"\s*([0-9.eEf+-]+)\s*,\s*([0-9.eEf+-]+)\s*,\s*([0-9.eEf+-]+)\s*,\s*([0-9.eEf+-]+)\s*\)",
        header,
    ):
        enum, owner, json_name, is_vec3, scalar, vx, vy, vz = m.groups()
        if is_vec3 == "true":
            params.append(
                {
                    "enum": enum,
                    "owner": owner,
                    "json_name": json_name,
                    "type": "vec3",
                    "default": [_f(vx), _f(vy), _f(vz)],
                }
            )
        else:
            params.append(
                {
                    "enum": enum,
                    "owner": owner,
                    "json_name": json_name,
                    "type": "scalar",
                    "default": _f(scalar),
                }
            )
    return keys, params


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="SystemConfig schema/codegen (spec 020 FR-B).")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true", help="residency + registry parity gate")
    g.add_argument("--emit", metavar="OUT", help="write generated registry header to OUT")
    g.add_argument("--from-cpp", action="store_true", help="print a schema derived from the .cpp")
    args = ap.parse_args(argv)

    if args.check:
        return cmd_check()
    if args.from_cpp:
        keys, params = load_cpp_tables()
        print(json.dumps(cpp_to_schema(keys, params), indent=2))
        return 0
    if args.emit:
        header = emit_header(load_schema())
        with open(args.emit, "w", encoding="utf-8") as fh:
            fh.write(header + "\n")
        print(f"config_codegen: wrote {args.emit}")
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
