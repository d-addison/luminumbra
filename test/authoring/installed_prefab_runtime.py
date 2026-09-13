"""Exercise the installed C++ ECS consumer against actual service-compiled fixtures.

Optional CPU qualification, requiring an installed service, native compiler
toolchain and prefab inspector. No Blender, engine window or graphics calls.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("service", "toolchain", "consumer", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--windows-junction", action="store_true",
                        help="Require real NTFS junction creation and installed consumer refusal.")
    args = parser.parse_args()
    if args.windows_junction and os.name != "nt":
        parser.error("--windows-junction requires an actual Windows Python host")
    for name in ("service", "toolchain", "consumer", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    project = args.output / "project with spaces"
    project.mkdir()
    source = Path(__file__).resolve().parents[2]
    fixture_dir = source / "tools/blender/authoring/service/tests"
    sys.path.insert(0, str(fixture_dir))
    from prefab_fixture import fixture
    sys.path.insert(0, str(args.service))
    from luminumbra_author.prefab import encode_glb

    toolchain = json.loads(args.toolchain.read_text())
    inputs = {"consumer": sha(args.consumer), "service": sha(args.service),
              "toolchain_manifest": sha(args.toolchain), "fixture": sha(fixture_dir / "prefab_fixture.py")}
    for name, digest in toolchain["files"].items():
        if sha(args.toolchain.parent / name) != digest:
            raise AssertionError("Installed toolchain pin mismatch: " + name)
        inputs["toolchain:" + name] = digest
    report = {"schema": "luminumbra.prefab.installed-acceptance.v1", "inputs": inputs,
              "engine_rendered": False, "checks": [], "commands": [], "passed": False}

    def save():
        (args.output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")

    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        save()
        if not condition:
            raise AssertionError(name)

    def run(command, success=True, windows_command_line=None):
        if windows_command_line is not None and os.name != "nt":
            raise ValueError("Raw Windows command line requires Windows")
        result = subprocess.run(windows_command_line or [str(x) for x in command], cwd=args.output,
                                capture_output=True, text=True, timeout=120)
        index = len(report["commands"])
        log = args.output / f"command-{index}.log"
        log.write_text(result.stdout + result.stderr)
        report["commands"].append({"argv": [str(x) for x in command],
                                   "exit_code": result.returncode, "log_sha256": sha(log),
                                   "windows_command_line": windows_command_line})
        check(f"command {index} {'succeeds' if success else 'refuses'}",
              result.returncode == 0 if success else result.returncode != 0)
        return result

    document, binary = fixture()

    def build(revision):
        (project / "prefab.glb").write_bytes(encode_glb(document, binary))
        sidecar = {"schema": "luminumbra.authoring.asset.v1", "profile": "glb-static-prefab-v1",
                   "asset_id": "fixture.prefab", "revision": revision, "source": "prefab.glb",
                   "dependencies": [], "exporter": {"id": "fixture.prefab.v1", "sha256": inputs["fixture"]}}
        (project / "asset.json").write_text(json.dumps(sidecar))
        run([sys.executable, args.service, "--project", project, "--toolchain", args.toolchain,
             "build", "asset.json", "--revision", str(revision)])
        pointer = json.loads((project / ".luminumbra-author/current.json").read_text())
        directory = project / ".luminumbra-author/generations" / pointer["job_id"]
        return directory

    def inspect(directory, success=True, manifest_pin=None, project_path=None):
        return run([args.consumer, "--project", project_path or project, "--generation", directory.name,
                    "--manifest-sha256", manifest_pin or sha(directory / "manifest.json"),
                    "--instance", "placed.fixture", "--placement",
                    "[1,0,0,0,0,1,0,0,0,0,1,0,100,200,300,1]"], success)

    first = build(1)
    first_pin = sha(first / "manifest.json")
    initial = json.loads(inspect(first).stdout)
    (args.output / "first-instance.json").write_text(json.dumps(initial, indent=2) + "\n")
    nodes = {node["id"]: node for node in initial["nodes"]}
    check("actual registry preserves node IDs and hierarchy", set(nodes) ==
          {"fixture.root", "fixture.first", "fixture.second"} and
          nodes["fixture.first"]["parent"] == nodes["fixture.second"]["parent"] == "fixture.root")
    check("parent and caller transforms compose once", nodes["fixture.first"]["world_matrix"][12:15] ==
          [111, 202, 303] and nodes["fixture.second"]["world_matrix"][12:15] == [110, 202, 303])
    check("mirrored instance retains winding and normal transform", nodes["fixture.second"]["reverse_front_face"] and
          nodes["fixture.second"]["normal_matrix"] == [-1, 0, 0, 0, .5, 0, 0, 0, 1])
    check("instances share two compiled primitive bindings", nodes["fixture.first"]["mesh"] ==
          nodes["fixture.second"]["mesh"] == "fixture.mesh" and len(initial["meshes"]) == 2 and
          len(nodes["fixture.first"]["draws"]) == len(nodes["fixture.second"]["draws"]) == 2)
    descriptor = json.loads((first / "prefab.json").read_text())
    check("all exact authored material and texture bindings survive", initial["materials"] == descriptor["materials"])
    check("headless inspection refuses to claim renderer qualification", initial["renderer_qualified"] is False)
    if args.windows_junction:
        junction_project = args.output / "junction project"
        junction = junction_project / ".luminumbra-author/generations" / first.name
        junction.parent.mkdir(parents=True)
        # cmd's builtin mklink requires a command string. Reject all shell expansion
        # characters before quoting these owned paths; no user command is accepted.
        command_host = Path(os.environ["SystemRoot"]) / "System32/cmd.exe"
        for path in (junction, first, command_host):
            if any(char in str(path) for char in '\"&|<>^%!()\r\n'):
                raise ValueError("Junction qualification path contains a shell metacharacter")
        report["inputs"]["junction_command_host"] = sha(command_host)
        # Supply CMD's exact command line, avoiding Python's CRT quoting rules
        # (CMD does not interpret backslash-escaped quotes like a CRT program).
        body = f'mklink /J "{junction}" "{first}"'
        run([command_host, "/d", "/s", "/c", body],
            windows_command_line=f'"{command_host}" /d /s /c "{body}"')
        try:
            metadata = junction.lstat()
            check("actual NTFS directory junction exists", bool(metadata.st_file_attributes &
                  stat.FILE_ATTRIBUTE_REPARSE_POINT) and metadata.st_reparse_tag == stat.IO_REPARSE_TAG_MOUNT_POINT)
            refusal = inspect(first, success=False, manifest_pin=first_pin, project_path=junction_project)
            check("installed Windows consumer refuses actual generation junction",
                  not refusal.stdout and "symlink or reparse point" in refusal.stderr)
        finally:
            # Remove only the junction entry, retaining every original generation byte.
            junction.rmdir()
    document["nodes"][1]["translation"][0] = 5
    second = build(2)
    updated = json.loads(inspect(second).stdout)
    updated_nodes = {node["id"]: node for node in updated["nodes"]}
    check("new generation preserves identities with edited placement", set(nodes) == set(updated_nodes) and
          updated_nodes["fixture.first"]["world_matrix"][12:15] == [115, 202, 303])
    retained = json.loads(inspect(first, manifest_pin=first_pin).stdout)
    check("explicit original generation ignores changed current pointer", retained == initial)
    manifest = json.loads((second / "manifest.json").read_text())
    mesh_name = next(name for name in manifest["outputs"] if name.endswith(".lmesh"))
    with (second / mesh_name).open("r+b") as stream:
        stream.seek(28)
        stream.write(b"\xff\xff\xff\x7f")
    refusal = inspect(second, success=False)
    check("corrupt compiled generation refuses with no partial instance report",
          not refusal.stdout and "hash or byte count mismatch" in refusal.stderr)
    check("old generation still loads after another generation is corrupted",
          json.loads(inspect(first, manifest_pin=first_pin).stdout) == initial)
    check("consumer and service binaries remain unchanged",
          sha(args.consumer) == inputs["consumer"] and sha(args.service) == inputs["service"])
    check("compiler dependencies remain pinned", all(sha(args.toolchain.parent / name) == digest
          for name, digest in toolchain["files"].items()))
    report.update(passed=True, first_generation=first.name, first_manifest_sha256=first_pin)
    save()
    print(json.dumps({"passed": True, "checks": len(report["checks"]), "receipt": str(args.output / "receipt.json")}))


if __name__ == "__main__":
    main()
