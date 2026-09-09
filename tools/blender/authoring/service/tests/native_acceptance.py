"""Required-input acceptance for an installed service and real asset compiler.

This runner does not build native code. Missing inputs and failed builds fail the
run; unit-test compiler stand-ins are never used here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(archive, toolchain, output, fixtures, exporter_id, exporter_sha256):
    output.mkdir(parents=True, exist_ok=False)
    project = output / "project"
    project.mkdir()
    manifest = json.loads(toolchain.read_text())
    before = {name: sha(toolchain.parent / name) for name in manifest["files"]}
    command = [sys.executable, str(archive), "--project", str(project), "--toolchain", str(toolchain)]
    report = {"mode": "NATIVE", "service_sha256": sha(archive), "toolchain_before": before,
              "scope": "Installed compilation/publication; no rendered or composed-game acceptance",
              "cases": []}

    def invoke(arguments, expected_success=True):
        start = time.monotonic()
        result = subprocess.run(command + arguments, cwd=output, capture_output=True, text=True, timeout=180)
        value = json.loads(result.stdout)
        if (result.returncode == 0) != expected_success:
            raise RuntimeError(f"Unexpected exit {result.returncode}: {result.stdout}\n{result.stderr}")
        return value, time.monotonic() - start

    for index, fixture in enumerate(fixtures):
        shutil.copyfile(fixture, project / "source.glb")
        asset = {"schema": "luminumbra.authoring.asset.v1", "profile": "glb-geometry-v1",
                 "asset_id": "acceptance.geometry", "revision": index,
                 "source": "source.glb", "dependencies": [],
                 "exporter": {"id": exporter_id, "sha256": exporter_sha256}}
        (project / "asset.json").write_text(json.dumps(asset))
        first, elapsed = invoke(["build", "asset.json", "--revision", str(index)])
        second, _ = invoke(["build", "asset.json", "--revision", str(index)])
        one, two = first["result"], second["result"]
        assert one["status"] == two["status"] == "succeeded"
        assert one["engine_executed"] and two["engine_executed"]
        assert one["build_identity"] == two["build_identity"]
        assert one["outputs"] == two["outputs"]
        held, _ = invoke(["generation.inspect", "--job-id", one["job_id"]])
        assert held["result"]["outputs"] == one["outputs"]
        current = (project / ".luminumbra-author/current.json").read_bytes()
        # A changed required contract cannot replace the completed generation.
        invalid = {**asset, "required_schemas": ["luminumbra.unknown.required.v999"]}
        (project / "asset.json").write_text(json.dumps(invalid))
        refused, _ = invoke(["build", "asset.json", "--revision", str(index)], False)
        assert refused["result"]["status"] == "failed"
        assert not refused["result"]["engine_executed"]
        assert (project / ".luminumbra-author/current.json").read_bytes() == current
        report["cases"].append({"source": str(fixture), "source_sha256": sha(fixture),
                                "first": one, "repeat": two, "refusal": refused["result"],
                                "cli_to_published_seconds": elapsed, "retained_generation_verified": True})
        (output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    report["toolchain_after"] = {name: sha(toolchain.parent / name) for name in manifest["files"]}
    assert before == report["toolchain_after"]
    (output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": len(fixtures), "receipt": str(output / "receipt.json")}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--exporter-id", required=True)
    parser.add_argument("--exporter-sha256", required=True)
    parser.add_argument("fixtures", type=Path, nargs="+")
    args = parser.parse_args()
    run(args.service.resolve(strict=True), args.toolchain.resolve(strict=True), args.output.resolve(),
        [path.resolve(strict=True) for path in args.fixtures], args.exporter_id, args.exporter_sha256)
