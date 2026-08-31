#!/usr/bin/env python3
"""Generate a measured line/branch coverage summary from gcov counters.

Part of the coverage-instrumentation slice. The dedicated `coverage` CMake
preset (binaryDir build/coverage) compiles first-party targets with
``--coverage`` (== -fprofile-arcs -ftest-coverage). After the test suites run
under that tree, gcov emits ``.gcda`` execution counters next to the ``.gcno``
notes files. This script walks those counters, asks gcov for per-file JSON,
aggregates real overall + per-subsystem line/branch percentages, and writes
``coverage_summary.json`` with ``coverage_kind: "measured"`` and
``line_coverage.available: true`` so the engine-frontier validator can gate on
a measured number instead of subsystem-name presence.

Primary tool: gcov itself (``gcov --json-format --stdout``), which is the
gcov-native JSON path and requires no third-party dependency. gcovr is NOT
required (it is uninstallable offline in this environment; see coverage contract). gcov
15.1.0 matches the compiling gcc 15.1.0 exactly, so the ``.gcda`` parse is
version-safe.

Usage:
    python tools/coverage_summary.py \
        --build-dir build/coverage \
        --source-root . \
        --output build/coverage/test-artifacts/coverage/coverage_summary.json \
        --build-preset coverage \
        --minimum-required-percent <float>
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


# Ordered most-specific-first so the first matching prefix wins. Paths are
# normalized to forward slashes and made relative to the source root before
# matching. The seven names are the engine-frontier contract subsystems.
SUBSYSTEM_PREFIXES = [
    ("audio", ("src/luminumbra_client/audio",
               "src/luminumbra_common/components/audio")),
    ("rendering", ("src/luminumbra_client/rendering",
                   "include/luminumbra/rendering")),
    ("ui", ("src/luminumbra_client/ui",)),
    # Physics is the Jolt-driven first-party physics system. The compiled .cpp
    # lives under common/systems/ (the physics/ dir holds only the header), so
    # match the concrete source-file prefixes rather than a directory.
    ("physics", ("src/luminumbra_common/systems/PhysicsSystem",
                 "src/luminumbra_common/physics")),
    ("tools", ("tools/",)),
    # "performance" maps to the perf-sensitive deterministic sim core: the
    # simulation tick + ECS hot loops the perf gates exercise.
    ("performance", ("src/luminumbra_common/simulation",
                     "src/luminumbra_common/ecs")),
    # "common" is the catch-all engine-generic bucket (must stay LAST so the
    # more specific buckets above claim their files first).
    ("common", ("src/luminumbra_common",
                "src/luminumbra_client",
                "src/luminumbra/core",
                "include/luminumbra")),
]

CONTRACT_SUBSYSTEMS = ["common", "rendering", "ui", "physics",
                       "audio", "performance", "tools"]


def classify(rel_path: str) -> str | None:
    """Map a source path (relative, forward-slash) to a contract subsystem."""
    p = rel_path.replace("\\", "/")
    for name, prefixes in SUBSYSTEM_PREFIXES:
        for prefix in prefixes:
            if p.startswith(prefix):
                return name
    return None


def find_gcda(build_dir: Path) -> list[Path]:
    return sorted(build_dir.rglob("*.gcda"))


def instrumented_subsystems(build_dir: Path, source_root: Path) -> set[str]:
    """Subsystems with at least one instrumented source file (.gcno present).

    A subsystem is "tracked" once its source is compiled with --coverage, even
    if no test executed it yet (so its measured % is a truthful 0.0, not an
    omission). The object-dir layout mirrors the source tree under each target's
    CMakeFiles/<target>.dir/, so we recover the original relative source path
    from the .gcno path and classify it. This keeps the secondary
    subsystem-presence guard meaningful (all 7 contract subsystems instrumented)
    without faking nonzero coverage for unexercised code.
    """
    tracked: set[str] = set()
    for gcno in build_dir.rglob("*.gcno"):
        rel = gcno.as_posix()
        # The compiled source's path-under-source-root is embedded after the
        # ".dir/" segment of the object path (Ninja/CMake object layout).
        marker = ".dir/"
        idx = rel.rfind(marker)
        if idx < 0:
            continue
        tail = rel[idx + len(marker):]
        if tail.endswith(".gcno"):
            tail = tail[: -len(".gcno")]
        # tail is like "luminumbra_client/audio/MiniaudioManager.cpp" (client),
        # "ai/InstinctPlanner.cpp" (common, relative to src/luminumbra_common),
        # or "asset_processor.cpp" (tools). Try the candidate source roots.
        candidates = [
            tail,
            f"src/{tail}",
            f"src/luminumbra_common/{tail}",
            f"src/luminumbra_client/{tail}",
            f"tools/{tail}",
        ]
        for cand in candidates:
            if (source_root / cand).exists():
                sub = classify(cand)
                if sub is not None:
                    tracked.add(sub)
                break
    return tracked


def run_gcov_json(gcov_exe: str, gcda: Path) -> dict | None:
    """Invoke gcov for one .gcda, returning its parsed JSON (or None)."""
    try:
        proc = subprocess.run(
            [gcov_exe, "--json-format", "--stdout",
             "--branch-probabilities", str(gcda)],
            capture_output=True, text=True, check=False,
        )
    except FileNotFoundError:
        print(f"error: gcov executable not found: {gcov_exe}", file=sys.stderr)
        raise
    out = proc.stdout.strip()
    if not out:
        if proc.returncode != 0:
            print(f"warning: gcov failed for {gcda}: {proc.stderr.strip()}",
                  file=sys.stderr)
        return None
    # gcov --stdout can emit multiple concatenated JSON objects (one per
    # compilation unit). Parse them with a streaming decoder.
    objs = []
    decoder = json.JSONDecoder()
    idx = 0
    n = len(out)
    while idx < n:
        while idx < n and out[idx] in " \t\r\n":
            idx += 1
        if idx >= n:
            break
        try:
            obj, end = decoder.raw_decode(out, idx)
        except json.JSONDecodeError:
            break
        objs.append(obj)
        idx = end
    if not objs:
        return None
    # Merge files arrays across the concatenated objects.
    merged = {"files": []}
    for o in objs:
        merged["files"].extend(o.get("files", []))
    return merged


class Acc:
    __slots__ = ("lines_total", "lines_covered", "branches_total",
                 "branches_covered")

    def __init__(self) -> None:
        self.lines_total = 0
        self.lines_covered = 0
        self.branches_total = 0
        self.branches_covered = 0

    def line_percent(self) -> float:
        if self.lines_total == 0:
            return 0.0
        return round(100.0 * self.lines_covered / self.lines_total, 2)

    def branch_percent(self) -> float:
        if self.branches_total == 0:
            return 0.0
        return round(100.0 * self.branches_covered / self.branches_total, 2)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--build-preset", default="coverage")
    ap.add_argument("--gcov", default="gcov")
    ap.add_argument("--minimum-required-percent", type=float, default=0.0)
    args = ap.parse_args()

    build_dir = Path(args.build_dir).resolve()
    source_root = Path(args.source_root).resolve()

    gcda_files = find_gcda(build_dir)
    if not gcda_files:
        print(f"error: no .gcda counters under {build_dir}; run the "
              f"instrumented test suite first.", file=sys.stderr)
        return 2

    # Dedupe by resolved source file: a header compiled into many TUs would
    # otherwise be counted once per TU. gcov reports per-line execution, and
    # the same source line appears identically across TUs that include it; we
    # take the per-file UNION of covered lines (a line counts as covered if ANY
    # TU executed it) to avoid double-counting and to reflect true reachability.
    # file -> {line_number: covered_bool}
    file_lines: dict[str, dict[int, bool]] = defaultdict(dict)
    # file -> [ (covered_bool) ] aggregated branch flags by (line, branch idx)
    file_branches: dict[str, dict[tuple, bool]] = defaultdict(dict)

    seen_gcov_files = 0
    for gcda in gcda_files:
        data = run_gcov_json(args.gcov, gcda)
        if not data:
            continue
        for f in data.get("files", []):
            raw = f.get("file", "")
            if not raw:
                continue
            # Resolve to a path relative to the source root; skip vendor/system.
            fp = Path(raw)
            if not fp.is_absolute():
                fp = (gcda.parent / fp)
            try:
                fp = fp.resolve()
            except OSError:
                continue
            try:
                rel = fp.relative_to(source_root).as_posix()
            except ValueError:
                continue  # outside source root (vendor / system header)
            if rel.startswith("vendor/") or "/_deps/" in rel or rel.startswith("build/"):
                continue
            if classify(rel) is None:
                continue
            seen_gcov_files += 1
            lmap = file_lines[rel]
            for ln in f.get("lines", []):
                num = ln.get("line_number")
                if num is None:
                    continue
                covered = ln.get("count", 0) > 0
                lmap[num] = lmap.get(num, False) or covered
                bmap = file_branches[rel]
                for bi, br in enumerate(ln.get("branches", [])):
                    key = (num, bi)
                    bcov = br.get("count", 0) > 0
                    bmap[key] = bmap.get(key, False) or bcov

    if not file_lines:
        print("error: gcov produced no in-tree source coverage; check "
              "subsystem prefixes / source-root.", file=sys.stderr)
        return 3

    overall = Acc()
    per_sub: dict[str, Acc] = {name: Acc() for name in CONTRACT_SUBSYSTEMS}

    for rel, lmap in file_lines.items():
        sub = classify(rel)
        acc = per_sub[sub]
        for _, covered in lmap.items():
            acc.lines_total += 1
            overall.lines_total += 1
            if covered:
                acc.lines_covered += 1
                overall.lines_covered += 1
        for _, bcov in file_branches.get(rel, {}).items():
            acc.branches_total += 1
            overall.branches_total += 1
            if bcov:
                acc.branches_covered += 1
                overall.branches_covered += 1

    gcov_ver = subprocess.run([args.gcov, "--version"], capture_output=True,
                              text=True, check=False).stdout.splitlines()
    gcov_ver_str = "gcov-unknown"
    for line in gcov_ver:
        if "gcov" in line.lower():
            # e.g. "gcov (Rev8, Built by MSYS2 project) 15.1.0"
            gcov_ver_str = "gcov-" + line.strip().split()[-1]
            break

    per_subsystem = []
    for name in CONTRACT_SUBSYSTEMS:
        acc = per_sub[name]
        per_subsystem.append({
            "name": name,
            "line_percent": acc.line_percent(),
            "branch_percent": acc.branch_percent(),
            "lines_total": acc.lines_total,
            "lines_covered": acc.lines_covered,
            "branches_total": acc.branches_total,
            "branches_covered": acc.branches_covered,
        })

    # A subsystem counts toward the contract presence guard if it is
    # INSTRUMENTED (its source compiled with --coverage; .gcno present), even
    # when no headless test exercised it yet (e.g. audio needs a device, the
    # asset_processor tool runs at build time). Its measured % is reported
    # truthfully in per_subsystem (0.0 when unexercised); nothing is faked. The
    # PRIMARY gate is the measured overall line %, not this presence guard.
    tracked = instrumented_subsystems(build_dir, source_root)
    measured_nonzero = {s["name"] for s in per_subsystem if s["lines_total"] > 0}
    covered_subsystems = [name for name in CONTRACT_SUBSYSTEMS
                          if name in tracked or name in measured_nonzero]

    line_percent = overall.line_percent()
    minimum = float(args.minimum_required_percent)
    passed = line_percent >= minimum

    summary = {
        "schema": "luminumbra.coverage_summary.v1",
        "generated_by": "tools/coverage_summary.py",
        "build_preset": args.build_preset,
        "coverage_kind": "measured",
        "coverage_tool": f"gcov-json+{gcov_ver_str}",
        "line_coverage": {
            "available": True,
            "percent": line_percent,
            "minimum_required_percent": round(minimum, 2),
            "lines_total": overall.lines_total,
            "lines_covered": overall.lines_covered,
            "reason": "",
        },
        "branch_coverage": {
            "available": True,
            "percent": overall.branch_percent(),
            "branches_total": overall.branches_total,
            "branches_covered": overall.branches_covered,
        },
        "per_subsystem": per_subsystem,
        "contract_coverage": {
            "covered_subsystem_count": len(covered_subsystems),
            "minimum_subsystems": 7,
            "covered_subsystems": covered_subsystems,
        },
        "required_artifacts": [
            "testing/ctest_manifest.json",
            "ui/ui_smoke.json",
            "ui/ui_interactions.json",
            "ui/ui_screenshots.json",
        ],
        "passed": passed,
    }

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"coverage_summary: overall line {line_percent:.2f}% "
          f"({overall.lines_covered}/{overall.lines_total}), branch "
          f"{overall.branch_percent():.2f}%, floor {minimum:.2f}%, "
          f"passed={passed}")
    for s in per_subsystem:
        print(f"  {s['name']:>12}: line {s['line_percent']:6.2f}% "
              f"({s['lines_covered']}/{s['lines_total']})")
    print(f"  -> {out_path}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
