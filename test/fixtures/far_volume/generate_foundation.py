#!/usr/bin/env python3
"""Export independent lattice receipts from the pinned, separately compiled foundation.

This uses local Git objects and dependency sources. No checkout, fetch, runtime,
Windows, or GPU action is performed. The build directory must be a fresh directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

FOUNDATION = "70b899e7b2ed60407a9d42da2089e99192d95b54"
RUNTIME_REFERENCE = "6a13b70e95f0c4a152f09e5df3422931c10dccfa"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="c++")
    args = parser.parse_args()
    args.build.mkdir(parents=True, exist_ok=False)
    original = args.build / "original"
    original.mkdir()
    files = {}

    def read(path):
        data = subprocess.check_output(["git", "-C", str(args.repo), "show", f"{FOUNDATION}:{path}"])
        files[path] = sha(data)
        destination = original / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        return data.decode()

    header = read("src/luminumbra_common/world/FarVolume.h")
    source = read("src/luminumbra_common/world/FarVolume.cpp")
    codec_header = read("src/luminumbra_common/world/FarLodStore.h")
    codec_source = read("src/luminumbra_common/world/FarLodStore.cpp")
    tier = read("src/luminumbra_common/world/FarTierTable.h")
    crc = read("src/luminumbra_common/core/Crc32.h")
    types = read("include/luminumbra/core/Types.h")
    # Omit only the live-world wrapper and its include. The original generator,
    # discovery, sparse validator and checksum functions remain byte-identical.
    cut = source.index("FarVolumeTile BuildPristineFarVolumeTile(")
    source = source[:cut] + "} // namespace Luminumbra::World\n"
    source = source.replace('#include "systems/SHIELD_WorldSystem.h"\n', "")
    start = codec_source.index("i16 QuantizeFarLodSdf(")
    end = codec_source.index("\n}", codec_source.index("float DequantizeFarLodSdf(", start)) + 2
    codec = codec_source[start:end]
    constants = []
    for name in ("kFarLodSdfQuantScale", "kFarLodSdfInvalid"):
        constants.append(next(line for line in codec_header.splitlines() if line.startswith("constexpr") and name in line))
    # Declaration-only stand-in for the unrelated store API; definitions below
    # are exact source extracts, including the original scalar aliases.
    shim = '#pragma once\n#include "luminumbra/core/Types.h"\nnamespace Luminumbra::World {\n'
    shim += "\n".join(constants) + "\ni16 QuantizeFarLodSdf(float);\nfloat DequantizeFarLodSdf(i16);\n}\n"
    support = {"FarVolume.h": header, "FarVolume.cpp": source, "FarTierTable.h": tier,
               "core/Crc32.h": crc, "luminumbra/core/Types.h": types, "FarLodStore.h": shim,
               "codec.cpp": '#include "FarLodStore.h"\n#include <algorithm>\n#include <cmath>\nnamespace Luminumbra::World {\n' + codec + "\n}\n"}
    for path, text in support.items():
        destination = args.build / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text)
    fixture = Path(__file__).resolve().parent
    executable = args.build / "foundation_reference"
    command = [args.compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I" + str(args.build), "-I" + str(fixture),
               "-I" + str(args.dependencies / "glm"),
               "-I" + str(args.dependencies / "entt/src"),
               "-I" + str(args.dependencies / "nlohmann_json/include"),
               str(fixture / "export_foundation.cpp"), str(args.build / "FarVolume.cpp"),
               str(args.build / "codec.cpp"), "-o", str(executable)]
    (args.build / "compile-command.json").write_text(json.dumps(command, indent=2) + "\n")
    subprocess.run(command, check=True)
    cases = json.loads(subprocess.check_output([str(executable)]))
    output = {"schema": 1, "foundation_commit": FOUNDATION, "runtime_reference_commit": RUNTIME_REFERENCE,
              "source_sha256": files, "executable_sha256": sha(executable.read_bytes()),
              "compiler": subprocess.check_output([args.compiler, "--version"], text=True).splitlines()[0],
              "fixture_sha256": sha((fixture / "FoundationFixture.h").read_bytes()),
              "exporter_sha256": sha((fixture / "export_foundation.cpp").read_bytes()),
              "scope": "Original foundation generation only; no canonical code, runtime queue, native or performance acceptance.",
              "cases": cases}
    args.output.write_text(json.dumps(output, indent=2) + "\n")


if __name__ == "__main__":
    main()
