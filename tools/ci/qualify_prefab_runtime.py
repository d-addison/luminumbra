#!/usr/bin/env python3
"""Qualify the installed, headless prefab consumer using an existing Linux build.

The full CTest run supplies the C++ results; this gate does not run them twice.
The installed acceptance uses the real service zipapp, asset compiler and ECS CLI.
System C/C++ libraries belong to the host profile, not this toolchain manifest.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET


# Required Linux cases, including the actual POSIX symlink refusal. Additional
# cases in either suite must also pass; deleting an obligation fails this gate.
REQUIRED_CASES = {"PrefabDigest.MatchesIndependentSha256Vectors"} | {
    "PrefabRuntimeTest." + name for name in (
        "LoadsOwnedSharedGeometryAndExactMaterialBindings",
        "InstantiatesRealHierarchyShearNormalsMirroringAndWorldBounds",
        "EqualCountReplacementRefreshesTransformsAndStableIdentities",
        "FailedReloadAndInvalidPlacementRetainPreviousInstance",
        "RegistryConstructionFailureRollsBackOnlyCandidateEntities",
        "DestructionRemovesOnlyOwnedEntities",
        "RefusesUnpinnedOrArbitraryGenerationPaths",
        "RefusesMissingAndUnlistedGenerationMembers",
        "RefusesTraversalAndLinksInReferencedMembers",
        "RefusesSymlinkedMembersAndGenerationAncestors",
        "RefusesUnknownSchemaComponentsAndMaterialFields",
        "RefusesMissingMeshMaterialAndTextureReferences",
        "RefusesCyclesMissingParentsAndDuplicateIds",
        "RefusesSingularLocalShearAndIncorrectWinding",
        "RefusesBadIndexAndNonfiniteMeshEvenWithMatchingHashes",
        "RefusesIncompleteTextureMipsAndManifestMetricDrift",
        "RefusesInvalidSamplerAndMixedUvTransforms",
        "RefusesDuplicateJsonKeysEvenWithMatchingPin",
        "Full4096NodeChainIsIterativeAndBounded",
    )
}
REQUIRED_INSTALLED_CHECKS = {
    "command 0 succeeds", "command 1 succeeds", "command 2 succeeds",
    "command 3 succeeds", "command 4 succeeds", "command 5 refuses", "command 6 succeeds",
    "actual registry preserves node IDs and hierarchy",
    "parent and caller transforms compose once",
    "mirrored instance retains winding and normal transform",
    "instances share two compiled primitive bindings",
    "all exact authored material and texture bindings survive",
    "headless inspection refuses to claim renderer qualification",
    "new generation preserves identities with edited placement",
    "explicit original generation ignores changed current pointer",
    "corrupt compiled generation refuses with no partial instance report",
    "old generation still loads after another generation is corrupted",
    "consumer and service binaries remain unchanged",
    "compiler dependencies remain pinned",
}


def validate_ctest(root):
    cases = [case for case in root.iter("testcase")
             if case.get("name", "").startswith(("PrefabDigest.", "PrefabRuntimeTest."))]
    names = Counter(case.get("name") for case in cases)
    missing = REQUIRED_CASES - names.keys()
    if missing:
        raise ValueError("Missing prefab CTest cases: " + ", ".join(sorted(missing)))
    if any(count != 1 for count in names.values()):
        raise ValueError("Duplicate prefab CTest cases")
    for case in cases:
        if (case.get("status") != "run" or case.get("result") == "suppressed"
                or any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))):
            raise ValueError("Unevaluated or failing prefab CTest case: " + case.get("name"))
    return sorted(names)


def validate_acceptance(receipt):
    checks = receipt.get("checks", [])
    names = Counter(check.get("name") for check in checks)
    if (receipt.get("passed") is not True or not REQUIRED_INSTALLED_CHECKS <= names.keys()
            or any(count != 1 for count in names.values())
            or any(check.get("passed") is not True for check in checks)):
        raise ValueError("Installed prefab acceptance is incomplete or has duplicate checks")
    return len(checks)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "ctest-report", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("This CI host profile requires Linux; Windows has separate qualification.")
    build, output = args.build.resolve(), args.output.resolve()
    cases = validate_ctest(ET.parse(args.ctest_report).getroot())
    source = Path(__file__).resolve().parents[2]
    output.mkdir(parents=True, exist_ok=False)
    install = output / "installed tools"
    subprocess.run(["cmake", "--install", str(build), "--prefix", str(install),
                    "--component", "PrefabRuntime"], check=True)
    consumer = install / "bin/luminumbra_prefab_inspect"
    if sha(consumer) != sha(build / "bin/luminumbra_prefab_inspect"):
        raise ValueError("Installed consumer differs from this build")
    processor = install / "asset_processor"
    shutil.copy2(build / "bin/asset_processor", processor)
    toolchain = install / "toolchain.json"
    toolchain.write_text(json.dumps({"schema": "luminumbra.authoring.toolchain.v1",
                                    "id": "ci.linux.prefab", "processor": processor.name,
                                    "files": {processor.name: sha(processor)}}, indent=2) + "\n")
    service = install / "luminumbra-author"
    subprocess.run([sys.executable, str(source / "tools/blender/authoring/service/package.py"),
                    str(service)], check=True)
    receipt = {"schema": "luminumbra.prefab.ci.v1", "passed": False,
               "source_commit": subprocess.check_output(
                   ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
               "source_dirty": bool(subprocess.check_output(
                   ["git", "-C", str(source), "status", "--porcelain"], text=True).strip()),
               "gate_sha256": sha(__file__),
               "acceptance_script_sha256": sha(source / "test/authoring/installed_prefab_runtime.py"),
               "platform": sys.platform, "python": sys.version,
               "ctest_report_sha256": sha(args.ctest_report), "cpp_cases": cases,
               "consumer_sha256": sha(consumer), "processor_sha256": sha(processor),
               "service_sha256": sha(service), "engine_rendered": False}
    receipt_path = output / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
    subprocess.run([sys.executable, str(source / "test/authoring/installed_prefab_runtime.py"),
                    "--service", str(service), "--toolchain", str(toolchain),
                    "--consumer", str(consumer), "--output", str(output / "acceptance")], check=True)
    acceptance_path = output / "acceptance/receipt.json"
    acceptance = json.loads(acceptance_path.read_text())
    receipt.update(passed=True, installed_checks=validate_acceptance(acceptance),
                   acceptance_receipt_sha256=sha(acceptance_path))
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"passed": True, "cpp_cases": len(cases),
                      "installed_checks": receipt["installed_checks"], "receipt": str(receipt_path)}))


if __name__ == "__main__":
    main()
