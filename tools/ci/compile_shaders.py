#!/usr/bin/env python3
"""Compile the complete desktop OpenGL shader inventory without a GL context.

Shader.cpp expands quoted includes before ShaderSource.cpp injects defines.
The compute loaders in FoliagePass, FroxelPass and LuminanceMeterPass read raw
sources, without includes or defines. Initial load and reload use the same
permutations. RenderSmokeTest's native inventory is not recursive and recognizes
.compute rather than .comp; this inventory deliberately covers both suffixes.

glslang runs in desktop GLSL validation mode, using the source #version (450
core for the engine's OpenGL 4.5 context). Do not use -V or -G: SPIR-V generation
imposes different binding/location rules on otherwise valid OpenGL sources.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import subprocess
import sys


# Explicit inventory: additions, deletions and unknown extensions require review.
STAGE_FILES = {
    "vert": """basic.vert foliage.vert fullscreen_tri.vert g_buffer.vert glass_oit.vert
        instanced_mesh.vert lighting_pass.vert lightning_overlay.vert loading_hologram.vert
        loading_visual.vert magical_particles.vert plant_procgen.vert rml.vert shadow_map.vert
        shadow_tint.vert skinned_mesh.vert skybox.vert ssao.vert static_asset.vert
        tree_impostor.vert volumetric_lighting.vert water.vert""".split(),
    "frag": """basic.frag caustics_generator.frag cloud_composite.frag debug_view.frag
        enhanced_skybox.frag foliage.frag g_buffer.frag glass_oit.frag glass_oit_resolve.frag
        god_rays.frag lighting_pass.frag lightning_overlay.frag loading_hologram.frag
        loading_visual.frag magical_particles.frag plant_procgen.frag rml.frag scent_decal.frag
        shadow_map.frag shadow_tint.frag ssao.frag ssao_bilateral_upsample.frag ssao_blur.frag
        ssao_gtao.frag static_asset.frag taau_resolve.frag tree_impostor.frag
        volumetric_lighting.frag water.frag waterfall.frag weather_system.frag""".split(),
    "comp": """froxel_inject.comp froxel_integrate.comp grass_compact_prefix.comp
        grass_scatter.comp luminance_reduce.comp""".split(),
    "geom": ["magical_particles.geom"],
}
INCLUDES = frozenset({"config_constants.gen.glsl", "far_world_position.glsl"})
# These are Slang reflection pilots, not OpenGL inputs. Their HLSL register and
# entry-point conventions belong to the separate pilot reflection build/tests.
EXCLUDED = frozenset({"pilot/debug_view.frag.hlsl", "pilot/ssao.frag.hlsl"})
LADDER_FILES = frozenset({"g_buffer.vert", "g_buffer.frag", "instanced_mesh.vert", "skinned_mesh.vert"})
LADDER_DEFINES = ("LUMINUMBRA_FAR_WORLD_LADDER 1",)
DIAGNOSTIC_FILES = frozenset({
    "g_buffer.frag", "static_asset.frag", "tree_impostor.frag", "plant_procgen.frag",
    "scent_decal.frag", "foliage.frag", "water.frag", "glass_oit.frag", "waterfall.frag",
    "magical_particles.frag",
})
DIAGNOSTIC_DEFINES = ("LUMIN_DIAG_IDS 1",)
# Bounds cover the whole scan and expanded output, including repeated includes.
MAX_ENTRIES = 256
MAX_SOURCE_BYTES = 1024 * 1024
MAX_EXPANDED_BYTES = 8 * MAX_SOURCE_BYTES
MAX_INCLUDE_DEPTH = 32


class ShaderError(ValueError):
    """Inventory, preprocessing or compiler refusal."""


@dataclass(frozen=True)
class Source:
    text: str
    locations: tuple[tuple[str, int], ...]


@dataclass(frozen=True)
class Unit:
    name: str
    stage: str
    defines: tuple[str, ...]
    source: Source


def read_source(path: Path) -> str:
    # Open once, reject devices/FIFOs before reading, and bound the actual read.
    flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    flags |= getattr(os, "O_BINARY", 0)
    descriptor = os.open(path, flags)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_SOURCE_BYTES:
            raise ShaderError(f"{path}: not a regular shader within the source size bound")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            data = stream.read(MAX_SOURCE_BYTES + 1)
    finally:
        os.close(descriptor)
    if len(data) > MAX_SOURCE_BYTES:
        raise ShaderError(f"{path}: source size bound exceeded")
    return data.decode("utf-8")


def inventory(root: Path) -> dict[str, str]:
    """Snapshot the reviewed inventory; no compiler runs on partial coverage."""
    expected = {name for names in STAGE_FILES.values() for name in names} | INCLUDES | EXCLUDED
    found = {}
    pending = [root]
    entries = 0
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as children:
            scanned = []
            for child in children:
                entries += 1
                if entries > MAX_ENTRIES:
                    raise ShaderError("shader inventory entry bound exceeded")
                scanned.append(child)
            for child in sorted(scanned, key=lambda entry: entry.name):
                # lstat supplies Windows reparse metadata absent from DirEntry.stat.
                info = os.lstat(child.path)
                if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                    raise ShaderError(f"{child.path}: shader redirections are not allowed")
                if stat.S_ISDIR(info.st_mode):
                    pending.append(Path(child.path))
                else:
                    name = Path(child.path).relative_to(root).as_posix()
                    found[name] = read_source(Path(child.path))
    unknown = sorted(found.keys() - expected)
    missing = sorted(expected - found.keys())
    if unknown or missing:
        raise ShaderError(f"shader inventory mismatch: uncovered={unknown}, missing={missing}")
    return dict(sorted(found.items()))


def lines(text: str) -> list[str]:
    """Match std::getline's physical LF lines without universal-newline changes."""
    return re.findall(r"[^\n]*\n|[^\n]+$", text)


def raw_source(name: str, text: str) -> Source:
    return Source(text, tuple((name, number) for number, _ in enumerate(lines(text), 1)))


def expand_source(name: str, sources: dict[str, str], used: set[str],
                  stack: tuple[str, ...] = ()) -> Source:
    if name in stack:
        raise ShaderError(f"{name}: cyclic shader include")
    if len(stack) >= MAX_INCLUDE_DEPTH:
        raise ShaderError(f"{name}: shader include depth bound exceeded")
    if name not in sources:
        raise ShaderError(f"{name}: cannot open shader source")
    used.add(name)
    output = []
    locations = []
    size = 0
    for number, line in enumerate(lines(sources[name]), 1):
        has_newline = line.endswith("\n")
        directive = line[:-1] if has_newline else line
        if has_newline and directive.endswith("\r"):
            directive = directive[:-1]
        if '#include "' not in directive:
            piece = Source(line, ((name, number),))
        else:
            match = re.fullmatch(r'#include "([^"\n]*)"', directive)
            if not match:
                raise ShaderError(f"{name}:{number}: malformed shader include")
            operand = match[1]
            # Includes only consume the already captured inventory, never open a
            # path supplied by GLSL. Refuse absolute/drive/escaping spellings.
            target = posixpath.normpath(posixpath.join(posixpath.dirname(name), operand))
            if (not operand or PurePosixPath(operand).is_absolute() or "\\" in operand
                    or ":" in operand or target == ".." or target.startswith("../")):
                raise ShaderError(f"{name}:{number}: shader include escapes root")
            piece = expand_source(target, sources, used, (*stack, name))
            if has_newline and not piece.text.endswith("\n"):
                ending = "\r\n" if line.endswith("\r\n") else "\n"
                piece = Source(piece.text + ending, piece.locations or ((name, number),))
        size += len(piece.text.encode("utf-8"))
        if size > MAX_EXPANDED_BYTES:
            raise ShaderError(f"{name}: expanded shader size bound exceeded")
        output.append(piece.text)
        locations.extend(piece.locations)
    return Source("".join(output), tuple(locations))


def next_token(source: str, cursor: int) -> int:
    """ShaderSource.cpp's comment/string lexer, including whole trailing comments."""
    if source.startswith("/*", cursor):
        end = source.find("*/", cursor + 2)
        if end == -1:
            raise ShaderError(f"unterminated GLSL block comment at line {source.count(chr(10), 0, cursor) + 1}")
        return end + 2
    if source.startswith("//", cursor):
        end = source.find("\n", cursor + 2)
        return len(source) if end == -1 else end
    if source[cursor] == '"':
        cursor += 1
        while cursor < len(source):
            char = source[cursor]
            cursor += 1
            if char == '"':
                break
            if char == "\\" and cursor < len(source):
                cursor += 1
        return cursor
    return cursor + 1


def skip_trivia(source: str, cursor: int) -> int:
    while cursor < len(source):
        if source[cursor] in " \t\n\r\v\f":
            cursor += 1
        elif source.startswith(("//", "/*"), cursor):
            cursor = next_token(source, cursor)
        else:
            break
    return cursor


def inject_defines(source: str, defines: tuple[str, ...]) -> str:
    if not defines:
        return source
    cursor = 0
    while cursor < len(source):
        cursor = next_token(source, cursor)
    token = skip_trivia(source, 0)
    insertion = 0
    newline = source.find("\n")
    needs_newline = False
    if source[token:token + 1] == "#":
        name = skip_trivia(source, token + 1)
        after = name + len("version")
        if (source.count("\n", token, name) == 0 and source[name:after] == "version"
                and (after == len(source) or source[after] not in
                     "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")):
            version_newline = source.find("\n", name)
            if version_newline != -1:
                newline = version_newline
            insertion = after
            while insertion < len(source) and source[insertion] != "\n":
                insertion = next_token(source, insertion)
            if insertion < len(source):
                insertion += 1
            else:
                needs_newline = True
    ending = "\r\n" if newline > 0 and source[newline - 1] == "\r" else "\n"
    original_line = source.count("\n", 0, insertion) + 1 + int(needs_newline)
    injected = ending if needs_newline else ""
    injected += "".join(f"#define {define}{ending}" for define in defines)
    injected += f"#line {original_line}{ending}"
    return source[:insertion] + injected + source[insertion:]


def compile_units(sources: dict[str, str]) -> list[Unit]:
    used = set()
    units = []
    for stage, names in sorted(STAGE_FILES.items()):
        for name in sorted(names):
            if stage == "comp":
                source = raw_source(name, sources[name])
                used.add(name)
            else:
                source = expand_source(name, sources, used)
            permutations = ((), LADDER_DEFINES) if name in LADDER_FILES else ((),)
            if name in DIAGNOSTIC_FILES:
                permutations += tuple((*DIAGNOSTIC_DEFINES, *defines) for defines in permutations)
            for defines in permutations:
                units.append(Unit(name, stage, defines,
                                  Source(inject_defines(source.text, defines), source.locations)))
    uncovered = sorted(sources.keys() - used - EXCLUDED)
    if uncovered:
        raise ShaderError(f"shader sources have no compile consumer: {uncovered}")
    return units


def compile_unit(unit: Unit, validator: str) -> str:
    """Return mapped diagnostics; a nonzero validator status always fails."""
    result = subprocess.run([validator, "-d", "--stdin", "-S", unit.stage],
                            input=unit.source.text, text=True, encoding="utf-8",
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=30, check=False)

    def location(match: re.Match[str]) -> str:
        number = int(match[2])
        if 1 <= number <= len(unit.source.locations):
            path, line = unit.source.locations[number - 1]
            return f"{path}:{line}: {match[1].lower()}:"
        return f"{unit.name}:{number}: {match[1].lower()}:"

    diagnostic = re.sub(r"(?m)^(ERROR|WARNING): 0:(\d+):", location, result.stdout)
    if result.returncode:
        label = ", ".join(unit.defines) or "default"
        raise ShaderError(f"{unit.name} [{label}]: glslangValidator failed ({result.returncode})\n{diagnostic}")
    return diagnostic


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2] / "res/shaders")
    parser.add_argument("--validator", default="glslangValidator")
    args = parser.parse_args()
    try:
        sources = inventory(args.root)
        units = compile_units(sources)
        failures = []
        for unit in units:
            try:
                diagnostic = compile_unit(unit, args.validator)
                if diagnostic:
                    print(diagnostic, end="" if diagnostic.endswith("\n") else "\n")
            except ShaderError as error:
                failures.append(str(error))
        if failures:
            raise ShaderError("\n".join(failures))
        print(f"Compiled {len(units)} shader permutations; covered {len(sources) - len(EXCLUDED)} GLSL files; "
              f"excluded {len(EXCLUDED)} Slang HLSL pilots.")
        return 0
    except (OSError, UnicodeError, ShaderError, subprocess.TimeoutExpired) as error:
        print(f"shader compile failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
