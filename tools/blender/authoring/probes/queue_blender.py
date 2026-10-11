#!/usr/bin/env python3
"""Plan or explicitly run coordinated Blender exports. Never launches by default."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

from compare_exports import compare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender", type=Path, required=True)
    parser.add_argument("--suite", choices=("prop", "plant", "character"), required=True)
    parser.add_argument("--execute-coordinated", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    run = here / "native-runs" / (args.suite + "-" + uuid.uuid4().hex)
    env = {name: str(run / "user" / name.lower()) for name in (
        "BLENDER_USER_RESOURCES", "BLENDER_USER_CONFIG", "BLENDER_USER_SCRIPTS",
        "BLENDER_USER_DATAFILES", "BLENDER_USER_EXTENSIONS")}
    env.update(OMP_NUM_THREADS="2", OPENBLAS_NUM_THREADS="2")
    env.update(TEMP=str(run / "temp"), TMP=str(run / "temp"))
    command = [str(args.blender), "--background", "--factory-startup", "--disable-autoexec",
               "--threads", "2", "--python-exit-code", "23", "--python",
               str(here / "blender_fixture.py"), "--", "--suite", args.suite,
               "--output", str(run / "outputs")]
    record = {"status": "QUEUED", "command": command, "environment_overrides": env,
              "native_gpu_coordination_required": True, "timeout_seconds": 180}
    if args.execute_coordinated:
        if args.blender.suffix.lower() == ".exe" and os.name != "nt":
            parser.error("Use native Windows Python for Windows Blender so environment isolation and child ownership are reliable.")
        for name, path in env.items():
            if name.startswith("BLENDER_") or name in ("TEMP", "TMP"):
                Path(path).mkdir(parents=True, exist_ok=True)
        record["blender_sha256"] = hashlib.sha256(args.blender.read_bytes()).hexdigest()
        record["script_sha256"] = hashlib.sha256((here / "blender_fixture.py").read_bytes()).hexdigest()
        with (run / "blender.log").open("wb") as log:
            try:
                completed = subprocess.run(command, env={**os.environ, **env}, cwd=run,
                                           stdout=log, stderr=subprocess.STDOUT, timeout=180, check=False)
                record.update(status="EXPORT_COMPLETED" if completed.returncode == 0 else "FAILED",
                              returncode=completed.returncode)
                if completed.returncode == 0:
                    comparison = compare(run / "outputs" / "first.glb", run / "outputs" / "repeat.glb")
                    record["comparison"] = comparison
                    if not comparison["canonical_document_and_buffer_equal"]:
                        record.update(status="EXPORT_DIFFERENCES_REQUIRE_REVIEW", returncode=2)
            except subprocess.TimeoutExpired:
                record.update(status="TIMEOUT", returncode=124)
            except (OSError, ValueError, KeyError) as error:
                record.update(status="INVALID_EXPORT", returncode=2, error=str(error))
        (run / "command-receipt.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(json.dumps(record, indent=2))
    return record.get("returncode", 0)


if __name__ == "__main__":
    sys.exit(main())
