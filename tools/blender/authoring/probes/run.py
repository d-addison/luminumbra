#!/usr/bin/env python3
"""Run CPU fixtures and mock contracts; native Blender execution is separate."""
from __future__ import annotations

import argparse
import ast
from datetime import datetime, timezone
import io
import json
from pathlib import Path
import platform
import subprocess
import sys
import unittest
import uuid

from common import ROOT, SOURCE, PIN, atomic_json, canonical, digest, file_digest
from contract_fixtures import write as write_contracts
from fixtures import asset, run as run_fixtures
from mock_service import MockService
from package_mock import build as build_package
import test_probes


def command(args, cwd=None):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=60, check=False)
    return {"argv": [str(x) for x in args], "cwd": str(cwd) if cwd else None,
            "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr}


def identity(blender_root=None):
    paths = ["tools/asset_processor.cpp", "tools/blender/validate_glb.py",
             "tools/blender/fixtures/make_fixtures.py", "tools/blender/geonodes_bake.py",
             "src/luminumbra_common/animation/SkinnedMeshFormat.h",
             "src/luminumbra_common/animation/AnimationRuntime.cpp"]
    result = {"engine_source": {"path": str(SOURCE),
              "head": command(["git", "rev-parse", "HEAD"], SOURCE),
              "status": command(["git", "status", "--short"], SOURCE)},
              "python": sys.version, "platform": platform.platform(),
              "source_hashes": {name: file_digest(SOURCE / name) for name in paths}}
    if blender_root:
        exporter = blender_root / "5.1/scripts/addons_core/io_scene_gltf2"
        files = [{"path": path.relative_to(exporter).as_posix(), "sha256": file_digest(path)}
                 for path in sorted(exporter.rglob("*.py"))]
        executable = blender_root / ("blender.exe" if (blender_root / "blender.exe").exists() else "blender")
        result["blender"] = {"executed": False, "path": str(executable),
                             "sha256": file_digest(executable)}
        result["exporter"] = {"files": files, "count": len(files), "tree_sha256": digest(canonical(files)),
            "algorithm": "SHA-256 of UTF-8 canonical JSON array of sorted {path,sha256} records; no trailing newline"}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("all", "prop", "plant", "character", "service", "viewport"), default="all")
    parser.add_argument("--require-baseline", action="store_true", help="Require a clean source checkout at the recorded baseline")
    parser.add_argument("--require-schemas", action="store_true", help="Fail if jsonschema is unavailable")
    parser.add_argument("--blender-root", type=Path, help="Optional Blender 5.1 installation to identify and validate packaging; never launched")
    args = parser.parse_args()
    if args.require_baseline:
        pin = command(["git", "rev-parse", "HEAD"], SOURCE)
        if pin["returncode"] or pin["stdout"].strip() != PIN:
            parser.error("LUMINUMBRA_AUTHOR_SOURCE must select the recorded baseline " + PIN)
        if command(["git", "status", "--porcelain"], SOURCE)["stdout"].strip():
            parser.error("Baseline checkout must be clean")
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    evidence = ROOT / "evidence" / run_id
    evidence.mkdir(parents=True)
    atomic_json(evidence / "identity.json", identity(args.blender_root))
    report = {"run_id": run_id, "command": [sys.executable, *sys.argv], "mode": "CPU_PROBES_AND_MOCK",
              "native_engine_executed": False, "blender_executed": False, "suites": {}}
    suites = ["prop", "plant", "character", "service", "viewport"] if args.suite == "all" else [args.suite]
    tests = unittest.TestSuite()
    loader = unittest.TestLoader()
    if any(suite in suites for suite in ("prop", "plant", "character")):
        atomic_json(evidence / "validator-probes.json", run_fixtures(evidence / "fixtures"))
        tests.addTests(loader.loadTestsFromTestCase(test_probes.ImporterEvidence))
    for suite in suites:
        if suite in ("prop", "plant", "character"):
            report["suites"][suite] = {"status": "CPU_SYNTHETIC_ONLY", "blender_export": "NOT_RUN",
                                      "compiled_runtime_fidelity": "NOT_RUN"}
        if suite == "viewport":
            report["suites"][suite] = {"status": "NOT_IMPLEMENTED", "reason": "An engine preview host is required",
                "checks": ["synchronized color/depth", "projection and color transfer", "overlays and picking",
                           "resize and render scaling", "multiple viewports", "stale frame refusal",
                           "worker crash/unload", "30 FPS presentation", "p95 edit-to-visible <100 ms"]}
    if "service" in suites:
        tests.addTests(loader.loadTestsFromTestCase(test_probes.Contracts))
        tests.addTests(loader.loadTestsFromTestCase(test_probes.Service))
    log = io.StringIO()
    result = unittest.TextTestRunner(stream=log, verbosity=2).run(tests)
    (evidence / "tests.log").write_text(log.getvalue(), encoding="utf-8")
    report["tests"] = {"run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
                       "skipped": len(result.skipped), "passed": result.wasSuccessful() and result.testsRun > 0}
    schemas, examples = write_contracts(evidence / "schemas")
    if "service" in suites:
        project = evidence / "mock-project"
        project.mkdir()
        atomic_json(project / "asset.json", asset())
        service = MockService(project, "fixture-only-not-a-deployed-token-00000000")
        try:
            job = service.submit("asset.json", 1)["job_id"]
            for _ in range(3):
                receipt = service.advance(job)
            atomic_json(evidence / "mock-receipt.json", receipt)
            examples["receipt"] = receipt
        finally:
            service.close()
        schema_validation = {"status": "NOT_RUN", "reason": "jsonschema package unavailable"}
        try:
            import jsonschema
            for name, value in examples.items():
                jsonschema.Draft202012Validator.check_schema(schemas[name])
                jsonschema.Draft202012Validator(schemas[name]).validate(value)
            schema_validation = {"status": "PASSED", "schemas": sorted(examples)}
        except ImportError:
            pass
        report["suites"]["service"] = {"status": "MOCK_TESTED" if result.wasSuccessful() else "FAILED",
                                        "schemas": schema_validation, "lua_execution": False, "engine_rendering": False}
        archive = build_package(evidence)
        report["package"] = {"path": str(archive), "sha256": file_digest(archive), "validation": "NOT_RUN"}
        if args.blender_root:
            cli = args.blender_root / "5.1/scripts/addons_core/bl_pkg/cli/blender_ext.py"
            validation = command([sys.executable, "-B", str(cli), "validate", str(archive)])
            validation.update(tool_sha256=file_digest(cli), scope="Packaging script only; no Blender process")
            atomic_json(evidence / "extension-validation.json", validation)
            report["package"]["validation_returncode"] = validation["returncode"]
            report["package"]["validation"] = "PASSED" if validation["returncode"] == 0 else "FAILED"
    elif args.require_schemas:
        parser.error("--require-schemas requires the service or all suite")
    probe_paths = [p for p in sorted((ROOT / "probes").rglob("*.py")) if "native-runs" not in p.parts]
    for path in probe_paths:
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    atomic_json(evidence / "probe-hashes.json", {str(p.relative_to(ROOT)): file_digest(p) for p in probe_paths})
    schema_ok = not args.require_schemas or schema_validation["status"] == "PASSED"
    report["passed"] = report["tests"]["passed"] and schema_ok and report.get("package", {}).get("validation_returncode", 0) == 0
    atomic_json(evidence / "report.json", report)
    atomic_json(ROOT / "evidence" / "latest.json", {"run_id": run_id, "report": str(evidence / "report.json")})
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
