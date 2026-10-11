#!/usr/bin/env python3
"""Emit file-only scene-config JSON and client command lines for the issue #64 matrix.

Reads scene_matrix.json (schema luminumbra.scene_matrix.v1). For every resolved
scene and variant it writes `<scene>[.<variant>].scene.json` (with a unique
top-level screenshot path) and one commands.txt listing two command blocks per
entry: the benchmark form plus its validator line, and the plain screenshot form.
Nothing is launched and no capture is run; the commands are NEEDS-NATIVE-RUN.

Scenes whose pose is recorded-evidence (pos null) are unresolved. The generator
exits 2 and lists them unless --allow-unresolved is given. There is no
--world-seed flag: the client implies seed 424242, which is recorded in the matrix
as world_seed_implied and never emitted as a flag.
"""

import argparse
import json
import math
import re
import shlex
import sys
from pathlib import Path


SCHEMA = "luminumbra.scene_matrix.v1"
DEFAULT_MATRIX = Path(__file__).resolve().parent / "scene_matrix.json"
FULL_LAYERS = (
    "terrain",
    "farfield",
    "water",
    "foliage",
    "particles",
    "skinned",
    "skybox",
    "aerial",
    "lightning",
)
WEATHER_TYPES = ("none", "rain", "snow", "fog", "storm")
CLOUD_KEYS = ("coverage", "biome_variation", "plane_height", "shadow", "shadow_strength")
SCENE_KEYS = ("camera", "time_of_day", "moon", "weather", "clouds")
VARIANT_KEYS = ("id", "weather", "layers", "exclude_layers")
POSE_SOURCES = ("explicit", "recorded-evidence")
IMPLIED_SEED = 424242
CLIENT_BINARY = "luminumbra_client_app"
VALIDATOR = "tools/perf/validate_render_capture.py"
NAME_RE = re.compile(r"^[a-z0-9_-]+$")
EXIT_INVALID = 1
EXIT_UNRESOLVED = 2


class MatrixError(ValueError):
    """The matrix is malformed or refers to an unknown layer or key."""


def _number(value, label, low=None, high=None, low_open=False, high_open=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise MatrixError(f"{label}: must be a finite number, got {value!r}")
    if low is not None and (value <= low if low_open else value < low):
        raise MatrixError(f"{label}: {value} below range")
    if high is not None and (value >= high if high_open else value > high):
        raise MatrixError(f"{label}: {value} above range")
    return float(value)


def _name(value, label):
    if not isinstance(value, str) or not NAME_RE.match(value):
        raise MatrixError(f"{label}: must match [a-z0-9_-]+, got {value!r}")
    return value


def _layers(value, label):
    if not isinstance(value, list) or not value:
        raise MatrixError(f"{label}: must be a non-empty list of layer tokens")
    for token in value:
        if token not in FULL_LAYERS:
            raise MatrixError(f"{label}: unknown isolation layer token {token!r}")
    if len(set(value)) != len(value):
        raise MatrixError(f"{label}: duplicate layer tokens")
    return list(value)


def _weather(value, label):
    if not isinstance(value, dict) or set(value) - {"type", "intensity"}:
        raise MatrixError(f"{label}: weather takes only type and intensity")
    if value.get("type") not in WEATHER_TYPES:
        raise MatrixError(f"{label}: weather.type must be one of {WEATHER_TYPES}")
    _number(value.get("intensity", 0.0), f"{label}.intensity", 0.0, 1.0)


def _validate_scene(scene):
    sid = _name(scene.get("id"), "scene.id")
    label = f"scene {sid}"
    source = scene.get("pose_source")
    if source not in POSE_SOURCES:
        raise MatrixError(f"{label}: pose_source must be one of {POSE_SOURCES}")
    if not isinstance(scene.get("issue_refs"), list):
        raise MatrixError(f"{label}: issue_refs must be a list")
    body = scene.get("scene")
    if not isinstance(body, dict) or set(body) - set(SCENE_KEYS):
        raise MatrixError(f"{label}: scene keys must be a subset of {SCENE_KEYS}")
    camera = body.get("camera")
    if not isinstance(camera, dict):
        raise MatrixError(f"{label}: scene.camera is required")
    _number(camera.get("fov"), f"{label}.camera.fov", 0.0, 180.0, low_open=True, high_open=True)
    time_of_day = body.get("time_of_day")
    if source == "explicit":
        pos = camera.get("pos")
        if not isinstance(pos, list) or len(pos) != 3:
            raise MatrixError(f"{label}: explicit pose needs pos [x, y, z]")
        for axis, value in zip("xyz", pos):
            _number(value, f"{label}.camera.pos.{axis}")
        _number(camera.get("yaw"), f"{label}.camera.yaw")
        _number(camera.get("pitch"), f"{label}.camera.pitch", -90.0, 90.0)
        _number(time_of_day, f"{label}.time_of_day", 0.0, 1.0, high_open=True)
    else:
        if camera.get("pos") is not None or camera.get("yaw") is not None or camera.get("pitch") is not None:
            raise MatrixError(f"{label}: recorded-evidence pose must be null")
    if "weather" in body:
        _weather(body["weather"], f"{label}.weather")
    if "moon" in body:
        _number(body["moon"], f"{label}.moon")
    if "clouds" in body:
        clouds = body["clouds"]
        if not isinstance(clouds, dict) or set(clouds) - set(CLOUD_KEYS):
            raise MatrixError(f"{label}: clouds keys must be a subset of {CLOUD_KEYS}")
    if scene.get("base_layers") is not None:
        _layers(scene["base_layers"], f"{label}.base_layers")
    variants = scene.get("variants")
    if not isinstance(variants, list):
        raise MatrixError(f"{label}: variants must be a list")
    seen = set()
    for variant in variants:
        vid = _name(variant.get("id"), f"{label} variant id")
        if vid in seen:
            raise MatrixError(f"{label}: duplicate variant id {vid}")
        seen.add(vid)
        if set(variant) - set(VARIANT_KEYS):
            raise MatrixError(f"{label}.{vid}: keys must be a subset of {VARIANT_KEYS}")
        if "layers" in variant and "exclude_layers" in variant:
            raise MatrixError(f"{label}.{vid}: use layers or exclude_layers, not both")
        if "layers" in variant:
            _layers(variant["layers"], f"{label}.{vid}.layers")
        if "exclude_layers" in variant:
            _layers(variant["exclude_layers"], f"{label}.{vid}.exclude_layers")
        if "weather" in variant:
            _weather(variant["weather"], f"{label}.{vid}.weather")
    return sid


def validate_matrix(data):
    """Raise MatrixError unless data is a well-formed luminumbra.scene_matrix.v1 document."""
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise MatrixError(f"schema must be {SCHEMA}")
    world = data.get("world", {})
    if world.get("preset") != "default":
        raise MatrixError("world.preset must be 'default'")
    if world.get("world_seed_implied") != IMPLIED_SEED:
        raise MatrixError(f"world.world_seed_implied must be {IMPLIED_SEED}")
    if tuple(data.get("full_layers", ())) != FULL_LAYERS:
        raise MatrixError("full_layers must equal the IsolationConfig token list")
    fields = data.get("expected_report_fields")
    if not isinstance(fields, list) or not fields or not all(isinstance(f, str) for f in fields):
        raise MatrixError("expected_report_fields must be a non-empty list of strings")
    if not isinstance(data.get("settled_proof"), str) or not data["settled_proof"]:
        raise MatrixError("settled_proof text is required")
    scenes = data.get("scenes")
    if not isinstance(scenes, list) or not scenes:
        raise MatrixError("scenes must be a non-empty list")
    ids = [_validate_scene(scene) for scene in scenes]
    if len(set(ids)) != len(ids):
        raise MatrixError("scene ids must be unique")
    return data


def _is_resolved(scene):
    return scene["pose_source"] == "explicit"


def _variant_layers(base_layers, variant):
    if "layers" in variant:
        return list(variant["layers"])
    if "exclude_layers" in variant:
        return [layer for layer in FULL_LAYERS if layer not in variant["exclude_layers"]]
    return base_layers


def plan_entries(data):
    """Return (entries, unresolved_ids). Each entry is one scene or scene-variant."""
    entries = []
    unresolved = []
    for scene in data["scenes"]:
        if not _is_resolved(scene):
            unresolved.append(scene["id"])
            continue
        body = scene["scene"]
        base_weather = body.get("weather")
        base_layers = scene.get("base_layers")
        entries.append(
            {"name": scene["id"], "scene": scene, "weather": base_weather, "layers": base_layers}
        )
        for variant in scene.get("variants", []):
            weather = variant.get("weather", base_weather)
            layers = _variant_layers(base_layers, variant)
            entries.append(
                {
                    "name": f"{scene['id']}.{variant['id']}",
                    "scene": scene,
                    "weather": weather,
                    "layers": layers,
                }
            )
    names = [entry["name"] for entry in entries]
    if len(set(names)) != len(names):
        raise MatrixError("generated scene names collide")
    return entries, unresolved


def scene_config(entry, screenshot):
    """Build the --scene-config document; keys match main_client.cpp's parser."""
    body = entry["scene"]["scene"]
    camera = body["camera"]
    config = {
        "camera": {
            "pos": list(camera["pos"]),
            "yaw": camera["yaw"],
            "pitch": camera["pitch"],
            "fov": camera["fov"],
        },
        "time_of_day": body["time_of_day"],
    }
    if "moon" in body:
        config["moon"] = body["moon"]
    if entry["weather"] is not None:
        config["weather"] = dict(entry["weather"])
    if "clouds" in body:
        config["clouds"] = dict(body["clouds"])
    config["screenshot"] = screenshot
    return config


def _fmt(value):
    return f"{value:g}"


def _join(args):
    return " ".join(shlex.quote(str(arg)) for arg in args)


def command_blocks(entry, preset, scene_path, bench_path, build_dir):
    """Return the two NEEDS-NATIVE-RUN command blocks for one entry."""
    camera = entry["scene"]["scene"]["camera"]
    name = entry["name"]
    base = [
        f"{build_dir}/bin/{CLIENT_BINARY}",
        "--auto-create-world",
        "--auto-enter-world",
        "--world-preset",
        preset,
        "--no-audio",
        "--hidden-window",
        "--scene-config",
        scene_path,
    ]
    if entry["layers"] is not None:
        base += ["--isolation-layers", ",".join(entry["layers"])]
    benchmark = base + ["--render-benchmark", bench_path]
    validator = [
        "python3",
        VALIDATOR,
        bench_path,
        "--position",
        *[_fmt(v) for v in camera["pos"]],
        "--yaw",
        _fmt(camera["yaw"]),
        "--pitch",
        _fmt(camera["pitch"]),
        "--tod",
        _fmt(entry["scene"]["scene"]["time_of_day"]),
        "--fov",
        _fmt(camera["fov"]),
        "--require-geometry",
        "--require-settled",
    ]
    return [
        f"# NEEDS-NATIVE-RUN {name}: benchmark form (report-frame settled check only)",
        _join(benchmark),
        _join(validator),
        f"# NEEDS-NATIVE-RUN {name}: plain screenshot form",
        _join(base),
    ]


def emit(data, out_dir, build_dir, allow_unresolved):
    """Write configs and commands; return the process exit code."""
    entries, unresolved = plan_entries(data)
    if unresolved:
        print(
            "unresolved scenes (recorded-evidence pose, no --scene-config emitted): "
            + ", ".join(unresolved),
            file=sys.stderr,
        )
        if not allow_unresolved:
            print("refusing: pass --allow-unresolved to skip them", file=sys.stderr)
            return EXIT_UNRESOLVED
    out = Path(out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    preset = data["world"]["preset"]
    lines = []
    for entry in entries:
        name = entry["name"]
        scene_path = out / f"{name}.scene.json"
        screenshot = out / f"{name}.ppm"
        config = scene_config(entry, str(screenshot))
        scene_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
        bench_path = out / f"{name}.render_benchmark.json"
        lines.extend(command_blocks(entry, preset, str(scene_path), str(bench_path), build_dir))
        lines.append("")
    (out / "commands.txt").write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {len(entries)} scene configs and commands.txt to {out}")
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX)
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--allow-unresolved", action="store_true")
    parser.add_argument("--build-dir", default="<build>")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        data = validate_matrix(json.loads(args.matrix.read_text(encoding="utf-8")))
        return emit(data, args.out, args.build_dir, args.allow_unresolved)
    except (MatrixError, json.JSONDecodeError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_INVALID


if __name__ == "__main__":
    sys.exit(main())
