#!/usr/bin/env python3
"""Check the preview source receipt against actual first-party compiler dependencies.

Run after building the module and capture executable with Ninja. System and
vendor headers belong to the separately qualified compiler/dependency profile.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess


TARGETS = {"luminumbra_render_static", "luminumbra_prefab_runtime", "luminumbra_preview_capture"}
OBJECT = re.compile(r"CMakeFiles/([^/]+)\.dir/.*\.(?:o|obj)$")


def validate(source, manifest, dependencies, expected_objects):
    source = source.resolve()
    inputs = {}
    for line in manifest.read_text().splitlines():
        name, digest = line.rsplit(":", 1)
        path = source / name
        if name in inputs or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Malformed or duplicate source identity member: " + name)
        if not path.resolve().is_relative_to(source):
            raise ValueError("Source identity member escapes the source root: " + name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError("Source identity member changed since configure: " + name)
        inputs[name] = digest
    found, compiled, objects = set(), set(), set()
    active = False
    for line in dependencies.splitlines():
        if not line.startswith(" "):
            match = re.search(r"CMakeFiles/([^/]+)\.dir/.*: #deps \d+", line.replace("\\", "/"))
            active = bool(match and match[1] in TARGETS)
            if active:
                found.add(match[1])
                objects.add(line.split(": #deps", 1)[0])
            continue
        if not active:
            continue
        path = Path(line.strip()).resolve()
        if path.is_relative_to(source):
            name = path.relative_to(source).as_posix()
            if name.startswith(("src/", "include/", "tools/")):
                compiled.add(name)
    if found != TARGETS:
        raise ValueError("Missing compiled preview targets: " + ", ".join(sorted(TARGETS - found)))
    if objects != set(expected_objects):
        raise ValueError("Missing compiler dependencies for preview objects: " +
                         ", ".join(sorted(set(expected_objects) - objects)))
    missing = compiled - inputs.keys()
    if missing:
        raise ValueError("Compiled first-party files absent from source identity: " + ", ".join(sorted(missing)))
    return len(compiled)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "build", "manifest"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--ninja", default="ninja")
    args = parser.parse_args()
    def ninja(*arguments):
        return subprocess.run([args.ninja, "-C", str(args.build), "-t", *arguments],
                              check=True, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=60).stdout

    objects = []
    for line in ninja("targets", "all").splitlines():
        path = line.rsplit(": ", 1)[0]
        match = OBJECT.search(path.replace("\\", "/"))
        if match and match[1] in TARGETS:
            objects.append(path)
    if not objects:
        raise ValueError("No preview compiler objects in the build graph")
    # Inspect only the qualified targets, even in a complete game build.
    count = validate(args.source, args.manifest, ninja("deps", *objects), objects)
    print(f"Source identity: {count} compiled first-party dependencies covered; all manifest hashes match")


if __name__ == "__main__":
    main()
