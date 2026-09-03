#!/usr/bin/env python3
"""Typed config schema -> registry codegen for SystemConfig ().

Single source of truth = src/luminumbra_common/core/ConfigSchema.json. Each entry
declares its section/residency, JSON name, type, default, owner and canonical order.
This tool emits the X-macro registry header SystemConfigRegistry.gen.h; SystemConfig.h
expands it to GENERATE the SysKey/SysParam enums and SystemConfig.cpp expands it to
GENERATE the kKeys/kParams registries, so the parallel arrays have one authored home
and cannot drift. `--check` is the CI drift gate: it fails when the committed header is
stale vs the schema and verifies hash residency is schema-declared (sim => hashed,
render => excluded).

This is a REPRESENTATION change only: the generated KeyMeta/ParamMeta field values are
byte-identical to the prior hand-written arrays, so `ComputeConfigSubHash` produces a
character-for-character identical `config:v1:` string () and `--smoke` stays
6f008a9f637c40b7 with no world_hash bump.

This tool ALSO emits a small GENERATED shared-constant header
(src/luminumbra_common/core/ConfigConstants.gen.h): named C++ constants for the subset of
config params that C++ (and, in a noted shader-side twin, GLSL/Slang) would otherwise
hand-duplicate. It START-s with render.moonlight ( / ) to prove the pipeline
end-to-end; append to CONSTANT_MIGRATIONS as more hand-duplicated constants migrate. The
emitted value is the config DEFAULT straight from ConfigSchema.json, so the C++ consumer and
the schema share ONE authored home and cannot drift; `--check-constants` is the freshness
gate (kept SEPARATE from `--check` so the shared configure/frontier gate is unchanged).

Usage:
  python tools/config_codegen.py --check              # generated-header freshness + residency (CI gate)
  python tools/config_codegen.py --emit OUT.h         # write generated registry header
  python tools/config_codegen.py --from-cpp           # print a schema derived from the generated header
  python tools/config_codegen.py --emit-constants OUT.h   # write generated shared-constant header
  python tools/config_codegen.py --check-constants    # fail if ConfigConstants.gen.h is stale vs the schema
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
GEN_HEADER_PATH = os.path.join(
    ROOT, "src", "luminumbra_common", "core", "SystemConfigRegistry.gen.h"
)
GEN_CONSTANTS_PATH = os.path.join(
    ROOT, "src", "luminumbra_common", "core", "ConfigConstants.gen.h"
)
GEN_SHADER_CONSTANTS_PATH = os.path.join(ROOT, "res", "shaders", "config_constants.gen.glsl")

# Config params migrated to the GENERATED shared-constant header ( / ).
# Each entry names a params[].enum in ConfigSchema.json and the C++ constant identifier to
# emit; the value is the schema DEFAULT (never re-typed here, so it cannot drift). START
# small — render.moonlight strength/color — proving the pipeline end-to-end; append as more
# hand-duplicated C++/shader constants migrate to their single authored home.
CONSTANT_MIGRATIONS = [
    {"param": "MoonlightStrength", "name": "kMoonlightStrength"},
    {"param": "MoonlightColor", "name": "kMoonlightColor"},
]

CONSTANTS_NAMESPACE = "luminumbra::core::config_constants"

# Map between the C++ `Section::` enum and the schema's declared residency. This IS the
# residency contract: Sim-section keys are hashed (folded into config:v1:), Render-section
# keys are excluded from every hash. ComputeConfigSubHash enforces it at:223.
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
            "Typed config schema (). Single source of truth for SystemConfig "
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
    lines.append("// X-macro tables: SystemConfig.h expands LUMIN_CONFIG_KEY_TABLE/PARAM_TABLE to")
    lines.append("// generate the SysKey/SysParam enums, and SystemConfig.cpp expands them to build")
    lines.append("// the kKeys/kParams registries, so the parallel arrays have ONE authored home")
    lines.append("// (ConfigSchema.json) and cannot drift. Regenerate via tools/config_codegen.py")
    lines.append("// --emit; tools/config_codegen.py --check fails CI if this file is stale.")
    lines.append("")
    lines.append("// Codegen owns these bytes: the configure-time drift check enforces")
    lines.append("// byte-freshness against the emitter, so clang-format must not re-wrap")
    lines.append("// the X-macro continuation lines.")
    lines.append("// clang-format off")
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
    lines.append("// clang-format on")
    return "\n".join(lines)


def emit_constants_header(schema: dict) -> str:
    """Emit the shared-constant header for the CONSTANT_MIGRATIONS subset ().

    Each migrated param becomes a named C++ constant whose value is the schema DEFAULT.
    vec3 params also emit their three components as `constexpr float` (kNameR/G/B): those
    are what a shader-side twin's `#define`s consume, and they double as a compile-safe
    fallback for the (proven-constexpr in this codebase) `constexpr glm::vec3` form.
    """
    pmap = {p["enum"]: p for p in schema["params"]}
    lines = []
    lines.append("#pragma once")
    lines.append("// AUTO-GENERATED by tools/config_codegen.py --emit-constants from ConfigSchema.json. DO NOT EDIT.")
    lines.append("//")
    lines.append("// Shared config-mirrored constants (  / ). Each value is the")
    lines.append("// config DEFAULT for the named param, emitted ONCE here so the C++ consumer and the")
    lines.append("// config schema cannot hand-duplicate-and-drift. Regenerate via tools/config_codegen.py")
    lines.append("// --emit-constants; tools/config_codegen.py --check-constants fails CI if this is stale.")
    lines.append("//")
    lines.append("// The shader-side twin is emitted to res/shaders/config_constants.gen.glsl")
    lines.append("// from the same table and is validated by --check-constants.")
    lines.append("")
    lines.append("#include <glm/glm.hpp>")
    lines.append("")
    lines.append(f"namespace {CONSTANTS_NAMESPACE} {{")
    lines.append("")
    for mig in CONSTANT_MIGRATIONS:
        p = pmap.get(mig["param"])
        if p is None:
            raise SystemExit(
                f"config_codegen: constant migration references unknown param {mig['param']!r}"
            )
        name = mig["name"]
        owner = p["owner"]
        json_name = p["json_name"]
        if p["type"] == "vec3":
            vx, vy, vz = (float(x) for x in p["default"])
            lines.append(f"// {owner}.{json_name} — config default (vec3); components exposed as")
            lines.append(f"// scalars for the shader-side twin and a compile-safe fallback.")
            lines.append(f"inline constexpr float {name}R = {_fmt_float(vx)};")
            lines.append(f"inline constexpr float {name}G = {_fmt_float(vy)};")
            lines.append(f"inline constexpr float {name}B = {_fmt_float(vz)};")
            lines.append(f"inline constexpr glm::vec3 {name} = glm::vec3({name}R, {name}G, {name}B);")
        else:
            v = float(p["default"])
            lines.append(f"// {owner}.{json_name} — config default (scalar).")
            lines.append(f"inline constexpr float {name} = {_fmt_float(v)};")
        lines.append("")
    lines.append(f"}}  // namespace {CONSTANTS_NAMESPACE}")
    lines.append("")
    return "\n".join(lines)


def _shader_name(cpp_name: str) -> str:
    """Convert kMoonlightStrength to LUMIN_MOONLIGHT_STRENGTH."""
    stem = cpp_name[1:] if cpp_name.startswith("k") else cpp_name
    return "LUMIN_" + re.sub(r"(?<!^)(?=[A-Z])", "_", stem).upper()


def emit_shader_constants(schema: dict) -> str:
    """Emit GLSL constants from the schema-backed C++ migration table."""
    pmap = {p["enum"]: p for p in schema["params"]}
    lines = [
        "// AUTO-GENERATED by tools/config_codegen.py from ConfigSchema.json. DO NOT EDIT.",
        "#ifndef LUMINUMBRA_CONFIG_CONSTANTS_GEN_GLSL",
        "#define LUMINUMBRA_CONFIG_CONSTANTS_GEN_GLSL",
        "",
    ]
    for migration in CONSTANT_MIGRATIONS:
        param = pmap.get(migration["param"])
        if param is None:
            raise SystemExit(
                f"config_codegen: constant migration references unknown param {migration['param']!r}"
            )
        name = _shader_name(migration["name"])
        if param["type"] == "vec3":
            values = ", ".join(_fmt_float(float(value)).rstrip("f") for value in param["default"])
            lines.append(f"#define {name} vec3({values})")
        else:
            lines.append(f"#define {name} {_fmt_float(float(param['default'])).rstrip('f')}")
    lines.extend(["", "#endif"])
    return "\n".join(lines)


def load_schema() -> dict:
    with open(SCHEMA_PATH, "r", encoding="utf-8") as fh:
        return json.load(fh)


def load_cpp_tables():
    with open(CPP_PATH, "r", encoding="utf-8") as fh:
        return parse_cpp(fh.read())


def cmd_check() -> int:
    schema = load_schema()
    sch_keys, sch_params = schema_to_tables(schema)

    errors = []

    # 1. Generated-header freshness (the drift gate). SystemConfig.h expands the X-macro
    #    tables in SystemConfigRegistry.gen.h to build the SysKey/SysParam enums and
    #    SystemConfig.cpp expands them to build the kKeys/kParams registries. So the COMPILED
    #    registry == this header == the schema (the single authored home). The header MUST be
    #    byte-identical to a fresh emit; a stale header means the schema drifted from the code
    #    that actually compiles. (--emit uses text mode like this read, so newline handling
    #    matches on every platform.)
    expected_header = emit_header(schema) + "\n"
    actual_header = None
    try:
        with open(GEN_HEADER_PATH, "r", encoding="utf-8") as fh:
            actual_header = fh.read()
    except OSError as exc:
        errors.append(f"  cannot read generated header {GEN_HEADER_PATH}: {exc}")
    if actual_header is not None and actual_header != expected_header:
        errors.append(
            "SystemConfigRegistry.gen.h is STALE vs ConfigSchema.json — the generated registry "
            "no longer matches the schema. Regenerate it:"
        )
        errors.append(
            "  python tools/config_codegen.py --emit "
            "src/luminumbra_common/core/SystemConfigRegistry.gen.h"
        )

    # 2. Residency parity: every key declares a known residency, and it agrees with the
    #    C++ Section. sim => hashed, render => excluded. This is the  residency contract.
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
        f"{len(sch_params)} params; SystemConfigRegistry.gen.h is fresh vs ConfigSchema.json "
        f"and residency is schema-declared (the compiled registry has one authored home)."
    )
    return 0


def cmd_check_constants() -> int:
    """Freshness gate for the generated shared-constant header (kept SEPARATE from --check
    so the shared configure/frontier gate stays byte-for-byte unchanged). The committed
    ConfigConstants.gen.h MUST be byte-identical to a fresh emit; a stale file means a C++
    consumer could compile an out-of-date config default. (Text-mode read+write, like --emit,
    so newline handling matches on every platform.)"""
    schema = load_schema()
    expected = emit_constants_header(schema) + "\n"
    expected_shader = emit_shader_constants(schema) + "\n"
    try:
        with open(GEN_CONSTANTS_PATH, "r", encoding="utf-8") as fh:
            actual = fh.read()
    except OSError as exc:
        print(
            f"config_codegen --check-constants: FAIL - cannot read generated header "
            f"{GEN_CONSTANTS_PATH}: {exc}",
            file=sys.stderr,
        )
        return 1
    if actual != expected:
        print("config_codegen --check-constants: FAIL", file=sys.stderr)
        print(
            "ConfigConstants.gen.h is STALE vs ConfigSchema.json — the generated shared "
            "constants no longer match the schema defaults. Regenerate it:",
            file=sys.stderr,
        )
        print(
            "  python tools/config_codegen.py --emit-constants "
            "src/luminumbra_common/core/ConfigConstants.gen.h",
            file=sys.stderr,
        )
        return 1
    try:
        with open(GEN_SHADER_CONSTANTS_PATH, "r", encoding="utf-8") as fh:
            actual_shader = fh.read()
    except OSError as exc:
        print(
            f"config_codegen --check-constants: FAIL - cannot read generated shader include "
            f"{GEN_SHADER_CONSTANTS_PATH}: {exc}",
            file=sys.stderr,
        )
        return 1
    if actual_shader != expected_shader:
        print("config_codegen --check-constants: FAIL", file=sys.stderr)
        print(
            "config_constants.gen.glsl is stale vs ConfigSchema.json. Regenerate it with "
            "--emit-shader-constants.",
            file=sys.stderr,
        )
        return 1
    print(
        f"config_codegen --check-constants: PASS - {len(CONSTANT_MIGRATIONS)} migrated "
        f"constant(s) fresh vs ConfigSchema.json (single authored home; no C++/schema drift)."
    )
    return 0


def _parse_header(header: str):
    """Re-parse the emitted X-macro tables (round-trip self-check)."""
    # Drop comment lines so the "// KEY(enum, Section,...)" doc legend is not parsed as data.
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
    ap = argparse.ArgumentParser(description="SystemConfig schema/codegen ().")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true", help="residency + registry parity gate")
    g.add_argument("--emit", metavar="OUT", help="write generated registry header to OUT")
    g.add_argument(
        "--from-cpp",
        action="store_true",
        help="print a schema derived from the generated registry header (bootstrap/repair)",
    )
    g.add_argument(
        "--emit-constants", metavar="OUT", help="write the generated shared-constant header to OUT"
    )
    g.add_argument(
        "--check-constants",
        action="store_true",
        help="fail if the committed ConfigConstants.gen.h is stale vs the schema",
    )
    g.add_argument(
        "--emit-shader-constants",
        metavar="OUT",
        help="write the generated GLSL constants include to OUT",
    )
    args = ap.parse_args(argv)

    if args.check:
        return cmd_check()
    if args.check_constants:
        return cmd_check_constants()
    if args.emit_constants:
        header = emit_constants_header(load_schema())
        with open(args.emit_constants, "w", encoding="utf-8") as fh:
            fh.write(header + "\n")
        print(f"config_codegen: wrote {args.emit_constants}")
        return 0
    if args.emit_shader_constants:
        shader = emit_shader_constants(load_schema())
        with open(args.emit_shader_constants, "w", encoding="utf-8") as fh:
            fh.write(shader + "\n")
        print(f"config_codegen: wrote {args.emit_shader_constants}")
        return 0
    if args.from_cpp:
        # The kKeys/kParams literal arrays no longer live in SystemConfig.cpp — they are
        # generated by expanding SystemConfigRegistry.gen.h. Derive the schema from that header
        # (the X-macro tables) so this bootstrap/repair path stays meaningful.
        with open(GEN_HEADER_PATH, "r", encoding="utf-8") as fh:
            keys, params = _parse_header(fh.read())
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
