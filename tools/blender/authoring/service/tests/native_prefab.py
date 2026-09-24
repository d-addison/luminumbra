"""Qualify an installed prefab compiler with independent decoded-output checks."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

from prefab_fixture import fixture


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("service", "toolchain", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    project = args.output / "project"
    project.mkdir()
    sys.path.insert(0, str(args.service))
    from luminumbra_author.prefab import encode_glb
    toolchain = json.loads(args.toolchain.read_text())
    before = {name: sha(args.toolchain.parent / name) for name in toolchain["files"]}
    assert before == toolchain["files"]
    report = {"mode": "NATIVE_PREFAB", "engine_rendered": False, "checks": [],
              "service_sha256": sha(args.service), "toolchain": before}
    def check(name, condition, **details):
        report["checks"].append({"name": name, "passed": bool(condition), **details})
        (args.output / "receipt.json").write_text(json.dumps(report, indent=2))
        if not condition:
            raise AssertionError(name + ": " + str(details))
    def publish_source(document, binary, revision):
        (project / "prefab.glb").write_bytes(encode_glb(document, binary))
        asset = {"schema": "luminumbra.authoring.asset.v1", "profile": "glb-static-prefab-v1",
                 "asset_id": "fixture.prefab", "revision": revision, "source": "prefab.glb", "dependencies": [],
                 "exporter": {"id": "fixture.prefab.v1", "sha256": sha(Path(__file__).with_name("prefab_fixture.py"))}}
        (project / "asset.json").write_text(json.dumps(asset))
    commands = []
    def build(revision, success=True):
        command = [sys.executable, str(args.service), "--project", str(project), "--toolchain", str(args.toolchain),
                   "build", "asset.json", "--revision", str(revision)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (args.output / f"build-{len(commands)}.log").write_text(result.stdout + result.stderr)
        commands.append({"argv": command, "returncode": result.returncode})
        check("native build success" if success else "unsupported build refused", (result.returncode == 0) == success,
              output=result.stdout[-3000:] if result.returncode == 0 else result.stderr[-3000:])
        if not success:
            return None
        pointer = json.loads((project / ".luminumbra-author/current.json").read_text())
        directory = project / ".luminumbra-author/generations" / pointer["job_id"]
        return directory, json.loads((directory / "manifest.json").read_text())

    document, binary = fixture()
    publish_source(document, binary, 1)
    first, manifest = build(1)
    descriptor = json.loads((first / "prefab.json").read_text())
    nodes = {n["id"]: n for n in descriptor["nodes"]}
    check("shared geometry retains two instances and parent placement", len(nodes) == 3
          and nodes["fixture.first"]["mesh"] == nodes["fixture.second"]["mesh"]
          and nodes["fixture.first"]["parent"] == "fixture.root"
          and nodes["fixture.root"]["local_matrix"][12:15] == [10,2,3]
          and nodes["fixture.second"]["reverse_front_face"])
    draws = descriptor["meshes"]["fixture.mesh"]
    check("two material slots remain separate draws", len(draws) == 2
          and {d["material"] for d in draws} == {"fixture.paint", "fixture.leaf"})
    expected = {(0,0,0):(.2,.3), (2,0,0):(.8,.3), (0,1,0):(.2,.9)}
    for draw in draws:
        data = (first / draw["file"]).read_bytes()
        magic, vertices, indices = struct.unpack_from("<4sII", data)
        check("native part is one local triangle", magic == b"LMSH" and vertices == indices == 3)
        rows = [struct.unpack_from("<8f", data, 28 + i*32) for i in range(vertices)]
        check("positions and selected raw UVs survive compilation", all(tuple(row[:3]) in expected
              and all(abs(a-b) < 1e-6 for a,b in zip(row[6:8], expected[tuple(row[:3])])) for row in rows))
    leaf = descriptor["materials"]["fixture.leaf"]
    color_file = leaf["textures"]["baseColorTexture"]["file"]
    data = (first / color_file).read_bytes()
    magic, version, levels, width, height, channels = struct.unpack_from("<4sHHIIB",data)
    check("native cutout texture has complete RGBA mips", (magic,version,levels,width,height,channels)==(b"LTEX",1,4,8,8,4))
    at, coverage = 17, []
    for level in range(levels):
        pixels = data[at:at+width*height*4]
        covered = sum(value >= .625*255 for value in pixels[3::4])
        coverage.append({"width":width,"height":height,"covered":covered,"texels":width*height})
        if width > 1:
            check("cutout coverage survives authored factor and cutoff", covered == width*height//2, mip=level)
        at += width*height*4
        width, height = max(1,width//2),max(1,height//2)
    check("native mip dimensions match exact payload bytes", at == len(data))
    check("material factors and sampler policy retained", leaf["alpha_mode"] == "MASK"
          and leaf["base_color"][3] == .8 and leaf["alpha_cutoff"] == .5
          and leaf["textures"]["baseColorTexture"]["sampler"]["wrapS"] == 33071
          and leaf["textures"]["baseColorTexture"]["coordinates"]["offset"] == [.125,.25]
          and all(not draw["uv_transform_baked"] for draw in draws))
    second, repeated = build(1)
    check("repeat compilation produces identical part bytes", manifest["outputs"] == repeated["outputs"]
          and manifest["build_identity"] == repeated["build_identity"])
    # New geometry, placement and a material value, using the same installation.
    document["nodes"][1]["translation"][0] = 5
    document["materials"][0]["pbrMetallicRoughness"]["roughnessFactor"] = .9
    changed_binary = bytearray(binary)
    struct.pack_into("<f",changed_binary,12,3)
    publish_source(document, bytes(changed_binary), 2)
    third, changed = build(2)
    changed_descriptor = json.loads((third / "prefab.json").read_text())
    check("new geometry and material values publish without rebuilding tools",
          changed["outputs"][draws[0]["file"]]["sha256"] != manifest["outputs"][draws[0]["file"]]["sha256"]
          and changed_descriptor["materials"]["fixture.paint"]["roughness"] == .9)
    check("prior generation stays byte-identical", all(sha(first/name)==entry["sha256"] for name,entry in manifest["outputs"].items()))
    pointer = (project / ".luminumbra-author/current.json").read_bytes()
    document["materials"][1]["normalTexture"]["extensions"]["KHR_texture_transform"]["offset"] = [.7,0]
    publish_source(document,bytes(changed_binary),3)
    build(3,success=False)
    check("unsupported map coordinates retain last valid generation", pointer == (project / ".luminumbra-author/current.json").read_bytes())
    after = {name: sha(args.toolchain.parent / name) for name in toolchain["files"]}
    check("installed executables and libraries unchanged", before == after)
    report.update(passed=True, outputs=manifest["outputs"], coverage=coverage, commands=commands)
    (args.output / "receipt.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"passed":True,"checks":len(report["checks"]),"outputs":manifest["outputs"]},indent=2))


if __name__ == "__main__":
    main()
