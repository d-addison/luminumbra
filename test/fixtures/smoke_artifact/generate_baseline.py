#!/usr/bin/env python3
"""Explicitly regenerate fixed smoke bytes from pinned devel (Linux/Ninja build)."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

REVISION = "f2895388cd14fda8591ed571f87c0ffe026db15d"
SOURCE = "src/luminumbra_server/modes/Smoke.cpp"


def main() -> None:
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    original = subprocess.check_output(["git", "show", f"{REVISION}:{SOURCE}"], cwd=root)
    source = original.decode()
    # Substitute only the simulation-input producer. Keep the complete devel
    # verdict, JSON construction, optional blocks and file writer verbatim.
    start = source.index("SmokeRunResult RunSmokeOnce(")
    end = source.index("nlohmann::json SmokeRunJson", start)
    source = source[:start] + '''SmokeRunResult RunSmokeOnce(const ServerCliOptions&, const char* label) {
    ServerCliOptions fixed_options;
    SmokeRunResult first, replay;
    FixedSmokeInputs(fixture_scenario, fixed_options, first, replay);
    return std::string(label) == "run-1" ? first : replay;
}

''' + source[end:]
    source = ('#include "test/fixtures/smoke_artifact/FixedInputs.h"\n'
              'int fixture_scenario = 0;\n' + source)
    source += '''
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    for (fixture_scenario = 0; fixture_scenario != 3; ++fixture_scenario) {
        ServerCliOptions options;
        SmokeRunResult first, replay;
        FixedSmokeInputs(fixture_scenario, options, first, replay);
        options.artifact_path = (std::filesystem::path(argv[1]) /
            ("devel-" + std::to_string(fixture_scenario) + ".json")).string();
        if (RunSmoke(options) != (fixture_scenario == 2 ? 1 : 0)) return 3;
    }
}
'''
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = next(c for c in commands if c["file"].endswith("/modes/Smoke.cpp"))
    link_line = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", "luminumbra_server_app"], text=True
    ).splitlines()[-1]
    with tempfile.TemporaryDirectory(prefix="smoke-baseline-") as temp:
        temp = Path(temp)
        cpp, obj, binary = temp / "baseline.cpp", temp / "baseline.o", temp / "baseline"
        cpp.write_text(source)
        compile_cmd = shlex.split(entry["command"])
        compile_cmd[compile_cmd.index("-o") + 1] = str(obj)
        compile_cmd[compile_cmd.index("-c") + 1] = str(cpp)
        compile_cmd += [f"-I{root}", f"-I{root / Path(SOURCE).parent}"]
        subprocess.run(compile_cmd, cwd=build, check=True)
        link_cmd = shlex.split(link_line)[2:-2]  # Ninja's ': && <link> && :'.
        link_cmd = [arg for arg in link_cmd if not arg.endswith(".o")]
        link_cmd[link_cmd.index("-o") + 1] = str(binary)
        link_cmd.insert(link_cmd.index("-o"), str(obj))
        subprocess.run(link_cmd, cwd=build, check=True)
        subprocess.run([str(binary), str(output)], cwd=root, check=True)
    provenance = {
        "revision": REVISION,
        "source": SOURCE,
        "source_sha256": hashlib.sha256(original).hexdigest(),
        "fixed_inputs_sha256": hashlib.sha256(Path(__file__).with_name("FixedInputs.h").read_bytes()).hexdigest(),
        "baselines": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted(output.glob("devel-*.json"))},
        "method": "Pinned devel Smoke.cpp with only RunSmokeOnce replaced by FixedSmokeInputs; complete production serializer and text-mode writer unchanged; Linux LF output.",
    }
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")


if __name__ == "__main__":
    main()
