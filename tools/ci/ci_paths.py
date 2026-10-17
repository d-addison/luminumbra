#!/usr/bin/env python3
"""Conservative PR path gate and standalone coverage for Python tooling."""

from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys


# Only reviewed, test-backed files may skip native builds. New files require
# an explicit coverage decision; directory membership never grants an exemption.
PYTHON_TESTS = (
    "tools/ci/test_ci_paths.py",
    "tools/ci/test_cache_retention.py",
    "tools/ci/test_check_docs.py",
    "tools/ci/test_check_ctest_junit.py",
    "tools/ci/test_qualification_capture.py",
    "tools/ci/test_slowest_tests.py",
    "tools/ci/test_validate_performance_measurement.py",
    "tools/ci/test_validate_public_tree.py",
    "tools/ci/test_validate_visual_catalog.py",
    "tools/ci/test_tidy_cache.py",
    "tools/perf/test_capture_sim_budget.py",
    "tools/perf/test_check_gl_debug_log.py",
    "tools/perf/test_check_residency.py",
    "tools/perf/test_fixture_hash.py",
    "tools/perf/test_foliage_evidence.py",
    "tools/perf/test_perf.py",
    "tools/perf/test_render_budget.py",
    "tools/perf/test_render_contract.py",
    "tools/perf/test_render_runner_evidence.py",
    "tools/perf/test_sim_budget_gate.py",
    "tools/perf/test_summarize_process_receipts.py",
    "tools/perf/test_terrain_aovs.py",
    "tools/release/test_sbom.py",
    "tools/release/test_sbom_schema.py",
    "tools/release/test_verify_release.py",
    "tools/release/test_validate_release_gates.py",
    "tools/assets/test_acquire.py",
    "tools/audio/test_bank_validate.py",
    "tools/persistence/test_save_receipt.py",
    "tools/rendering/acceptance/test_scene_matrix.py",
)
PYTHON_MODULES = {
    "tools/ci/ci_paths.py", "tools/ci/check_docs.py",
    "tools/ci/check_ctest_junit.py", "tools/ci/slowest_tests.py",
    "tools/ci/validate_performance_measurement.py",
    "tools/ci/validate_public_tree.py", "tools/ci/validate_visual_catalog.py",
    "tools/perf/check_residency.py", "tools/perf/check_sim_budgets.py",
    "tools/perf/propose_sim_budgets.py", "tools/perf/sim_budget_compare.py",
    "tools/perf/sim_budget_io.py", "tools/perf/render_budget.py",
    "tools/perf/inspect_terrain_aovs.py", "tools/perf/validate_foliage_evidence.py",
    "tools/perf/validate_render_capture.py",
    "tools/release/generate_sbom.py", "tools/release/validate_sbom.py",
    "tools/release/verify_release.py", "tools/release/validate_release_gates.py",
    "tools/assets/acquire.py",
    "tools/audio/bank_validate.py", "tools/persistence/save_receipt.py",
    "tools/rendering/acceptance/scene_matrix.py",
}
# blender-authoring.yml covers these explicit files on Linux and Windows.
# The service implementation, package/launch scripts and prefab fixture stay on
# the build path: qualify_prefab_runtime.py ships/drives them with native tools.
AUTHORING_PYTHON_FILES = {
    "tools/blender/authoring/extension/recipes.py",
    "tools/blender/authoring/extension/viewport_math.py",
    "tools/blender/authoring/extension/viewport_adapter_state.py",
    "tools/blender/authoring/extension/viewport_generation.py",
    "tools/blender/authoring/extension_tests/test_recipes.py",
    "tools/blender/authoring/extension_tests/test_state.py",
    "tools/blender/authoring/extension_tests/test_viewport_math.py",
    "tools/blender/authoring/extension_tests/test_viewport_adapter_state.py",
    "tools/blender/authoring/extension_tests/test_viewport_generation.py",
    "tools/blender/authoring/extension_tests/test_evaluated_asset.py",
    "tools/blender/authoring/service/tests/test_meshgen_contracts.py",
    "tools/blender/authoring/service/tests/test_meshgen_config.py",
    "tools/blender/authoring/service/tests/test_meshgen_toolchain.py",
    "tools/blender/authoring/service/tests/test_prefab.py",
    "tools/blender/authoring/service/tests/test_service.py",
    "test/viewport/test_windows_process.py", "test/viewport/fixture_child.py",
    "test/viewport/test_client.py", "test/viewport/probe_windows_file_io.py",
    "test/viewport/test_shared_file_io.py", "test/viewport/test_transport.py",
    "test/viewport/test_plane_validation.py", "test/viewport/client_fixture.py",
    "test/viewport/probe_windows_client.py",
}
PYTHON_SELF_CHECKS = {
    "tools/visual_critique.py": ("tools/test_visual_critique.py",),
    "tools/test_visual_critique.py": ("tools/test_visual_critique.py",),
    "tools/timelapse.py": ("tools/timelapse.py", "--selftest"),
    "tools/check_sdf_contract_doc.py": ("tools/check_sdf_contract_doc.py",),
    "tools/check_runtime_data_manifest.py": ("tools/check_runtime_data_manifest.py",),
    "tools/gates/build-visual-sweep-montages.py": ("tools/gates/test_visual_sweep_montages.py",),
    "tools/gates/test_visual_sweep_montages.py": ("tools/gates/test_visual_sweep_montages.py",),
    "tools/blender/geonodes_bake_core.py": ("tools/blender/test_geonodes_bake.py",),
    "tools/blender/test_geonodes_bake.py": ("tools/blender/test_geonodes_bake.py",),
    "tools/blender/validate_glb.py": ("tools/blender/test_validate_glb.py",),
    "tools/blender/test_validate_glb.py": ("tools/blender/test_validate_glb.py",),
}
BUILD_PREFIXES = (
    "src/", "include/", "test/", "cmake/", "res/", "data/", "config/",
    "worlds/", "vendor/", "third_party/", "vcpkg/", "deps/", ".github/workflows/",
)
BUILD_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".cmake"}
DOC_SUFFIXES = {".md", ".rst", ".txt", ".json", ".yaml", ".yml", ".png", ".jpg", ".jpeg", ".svg", ".pdf", ".css", ".js", ".html"}


COVERED_PYTHON_FILES = (
    set(PYTHON_TESTS) | PYTHON_MODULES
    | AUTHORING_PYTHON_FILES | PYTHON_SELF_CHECKS.keys()
)

# CI infrastructure and executable test/validation entrypoints cannot exempt
# themselves from native qualification. Keep this ahead of the Python allowlist.
CI_PREFIXES = ("tools/ci/", ".github/workflows/", "scripts/")
CI_ENTRYPOINTS = (
    set(PYTHON_TESTS)
    | {command[0] for command in PYTHON_SELF_CHECKS.values()}
    | {
        "tools/assets/acquire.py", "tools/blender/authoring/probes/run.py",
        "tools/check_runtime_data_manifest.py", "tools/check_sdf_contract_doc.py",
        "tools/config_codegen.py", "tools/perf/capture_sim_budget.py",
        "tools/perf/check_sim_budgets.py", "tools/perf/fixture_hash.py",
        "tools/perf/perf.py", "tools/perf/propose_sim_budgets.py",
        "tools/release/generate_sbom.py", "tools/release/validate_package.py",
        "tools/release/validate_sbom.py", "tools/release/validate_release_gates.py",
    }
    | {path for path in AUTHORING_PYTHON_FILES
       if "/tests/" in path or "/extension_tests/" in path or "/test_" in path}
)


def covered_python(path: str) -> bool:
    return path in COVERED_PYTHON_FILES


def needs_cpp(path: str) -> bool:
    item = PurePosixPath(path)
    if path.startswith(CI_PREFIXES) or path in CI_ENTRYPOINTS:
        return True
    if covered_python(path):
        return False
    if path.startswith(BUILD_PREFIXES) or item.suffix.lower() in BUILD_SUFFIXES:
        return True
    if item.name in {"CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "vcpkg-configuration.json"}:
        return True
    if path.startswith("docs/") and item.suffix.lower() in DOC_SUFFIXES:
        return False
    if item.suffix.lower() in {".md", ".rst"} and (len(item.parts) == 1 or path.startswith(".github/")):
        return False
    if path in {"Doxyfile", "LICENSE", ".gitignore", ".editorconfig"}:
        return False
    return True


def needs_python(path: str) -> bool:
    # Standalone Python suites validate workflow and CI configuration files,
    # so CI infrastructure changes run the Python plan as well as native lanes.
    return path.endswith(".py") or path.startswith(CI_PREFIXES)


def changed_paths(root: Path, base: str, head: str) -> list[str]:
    merge_base = subprocess.check_output(
        ["git", "merge-base", base, head], cwd=root, text=True
    ).strip()
    # Disable rename detection so both the deleted source and added destination
    # are classified. NUL separation also preserves unusual filenames.
    result = subprocess.check_output(
        ["git", "diff", "--name-only", "--no-renames", "-z", merge_base, head, "--"], cwd=root
    )
    return [os.fsdecode(path) for path in result.split(b"\0") if path]


def classify(root: Path, event: str, base: str, head: str,
             repository: str, head_repository: str) -> tuple[bool, bool]:
    if event != "pull_request":
        return True, True
    try:
        if not repository or not head_repository or repository != head_repository:
            raise ValueError("fork PR or missing repository identity")
        shallow = subprocess.check_output(
            ["git", "rev-parse", "--is-shallow-repository"], cwd=root, text=True
        ).strip()
        if shallow != "false":
            raise ValueError("shallow repository or unknown history depth")
        if not base or not head:
            raise ValueError("missing PR base/head SHA")
        paths = changed_paths(root, base, head)
        if not paths:
            raise ValueError("empty PR diff")
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"Cannot classify PR diff; running all lanes: {error}", file=sys.stderr)
        return True, True
    return any(needs_cpp(path) for path in paths), any(needs_python(path) for path in paths)


def python_test_commands(root: Path) -> list[tuple[str, ...]]:
    commands = [(test,) for test in PYTHON_TESTS]
    commands.extend(sorted(set(PYTHON_SELF_CHECKS.values())))
    return commands


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("classify", "python-tests"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--event", default=os.environ.get("CI_EVENT", ""))
    parser.add_argument("--base", default=os.environ.get("CI_BASE_SHA", ""))
    parser.add_argument("--head", default=os.environ.get("CI_HEAD_SHA", ""))
    parser.add_argument("--repository", default=os.environ.get("CI_REPOSITORY", ""))
    parser.add_argument("--head-repository", default=os.environ.get("CI_HEAD_REPOSITORY", ""))
    parser.add_argument("--output", type=Path, default=os.environ.get("GITHUB_OUTPUT"))
    args = parser.parse_args()
    root = args.root.resolve()
    if args.mode == "python-tests":
        for command in python_test_commands(root):
            print("Running:", " ".join(command), flush=True)
            subprocess.run([sys.executable, "-B", *command], cwd=root, check=True)
        return 0
    cpp, python = classify(root, args.event, args.base, args.head,
                           args.repository, args.head_repository)
    output = f"needs_cpp={str(cpp).lower()}\nneeds_python={str(python).lower()}\n"
    print(output, end="")
    if args.output:
        with args.output.open("a", encoding="utf-8") as stream:
            stream.write(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
