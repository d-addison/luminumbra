#!/usr/bin/env python3
"""Cross-platform performance sampling, comparison, and bisect verdicts."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Sequence


SCHEMA = "luminumbra.perf_run.v2"
COMPARISON_SCHEMA = "luminumbra.perf_comparison.v1"
REBASELINE_SCHEMA = "luminumbra.performance_rebaseline.v1"
REBASELINE_PATH = Path(__file__).with_name("baselines") / "rebaseline.json"
UNEVALUATED = 125
MIN_GATING_SAMPLES = 20
MIN_EFFECT_PERCENT = 5.0
P_VALUE_MAX = 0.05
LAYERS = (
    "algorithms",
    "simulation",
    "world-streaming",
    "networking",
    "rendering-cpu",
    "rendering-gpu",
    "end-to-end",
    "tooling",
)
RENDERERS = ("none", "software-opengl", "opengl", "vulkan", "d3d12", "metal")
# Opt-in metrics sampled FROM the workload's evidence artifact (one scalar per
# iteration, summarized exactly like wall_time). Each entry names the JSON path
# inside the evidence document. Declared per-run via --evidence-metric; they fold
# into metric_schema (and therefore the comparability key), so base and candidate
# must request the identical set.
EVIDENCE_METRICS: dict[str, dict[str, Any]] = {
    "water_phase_p95_ms": {
        "unit": "ms",
        "direction": "lower",
        "path": ("water_phase_ms", "total", "p95"),
    },
}
BUILD_CONFIGURATION_FIELDS = {
    "build_type",
    "effective_flags_sha256",
    "asan",
    "coverage",
    "diligent",
    "frame_pointers",
    "tracy",
    "warnings_as_errors",
}


def percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile requires samples")
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    weight = position - lower
    return float(ordered[lower] * (1.0 - weight) + ordered[upper] * weight)


def summarize(
    samples: Sequence[float], unit: str, direction: str = "lower"
) -> dict[str, Any]:
    if not samples or any(not math.isfinite(value) or value < 0 for value in samples):
        raise ValueError("samples must be non-empty, finite, and non-negative")
    if direction not in {"lower", "higher"}:
        raise ValueError("direction must be lower or higher")
    median = statistics.median(samples)
    deviations = [abs(value - median) for value in samples]
    return {
        "unit": unit,
        "direction": direction,
        "samples": list(samples),
        "sample_count": len(samples),
        "p50": median,
        "p95": percentile(samples, 0.95),
        "p99": percentile(samples, 0.99),
        "max": max(samples),
        "mad": statistics.median(deviations),
    }


def cpu_description() -> str:
    if sys.platform.startswith("linux"):
        try:
            for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
                if line.lower().startswith("model name"):
                    return line.split(":", 1)[1].strip()
        except OSError:
            pass
    if sys.platform == "darwin":
        try:
            return subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"], text=True
            ).strip()
        except (OSError, subprocess.SubprocessError):
            pass
    return os.environ.get("PROCESSOR_IDENTIFIER") or platform.processor() or "unknown"


def git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def executable_path(command: str) -> Path | None:
    candidate = Path(command)
    if candidate.is_file():
        return candidate.resolve()
    found = shutil.which(command)
    return Path(found).resolve() if found else None


def file_hash(path: Path | None) -> str:
    if path is None:
        return "unknown"
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError:
        return "unknown"
    return digest.hexdigest()


def stable_hash(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_build_manifest(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    compiler = document.get("compiler") if isinstance(document, dict) else None
    configuration = document.get("configuration") if isinstance(document, dict) else None
    if document.get("schema") != "luminumbra.performance_build.v1" or not isinstance(
        compiler, dict
    ):
        raise ValueError("build manifest schema is invalid")
    fields = {name: compiler.get(name) for name in ("id", "version", "target")}
    if any(
        not isinstance(value, str) or not value or value == "unknown"
        for value in fields.values()
    ):
        raise ValueError("build manifest compiler identity is incomplete")
    if any("/" in value or "\\" in value for value in fields.values()):
        raise ValueError("build manifest compiler identity contains a path")
    if (
        not isinstance(configuration, dict)
        or set(configuration) != BUILD_CONFIGURATION_FIELDS
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(configuration.get("effective_flags_sha256", ""))
        )
        or any(
            not isinstance(value, str) or "/" in value or "\\" in value
            for value in configuration.values()
        )
    ):
        raise ValueError("build manifest configuration is incomplete")
    return {"compiler": fields, "configuration": configuration}


def parse_parameters(values: Sequence[str]) -> dict[str, Any]:
    parameters: dict[str, Any] = {}
    for value in values:
        name, separator, raw = value.partition("=")
        if not separator or not re.fullmatch(r"[a-z][a-z0-9_]*", name):
            raise ValueError(f"invalid parameter {value!r}")
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = raw
        if isinstance(parsed, float) and not math.isfinite(parsed):
            raise ValueError(f"parameter {name} is not finite")
        if not isinstance(parsed, (str, int, float, bool)):
            raise ValueError(f"parameter {name} must be a scalar")
        if isinstance(parsed, str) and (
            len(parsed) > 64 or not re.fullmatch(r"[A-Za-z0-9_.-]+", parsed)
        ):
            raise ValueError(f"parameter {name} contains unsafe text")
        parameters[name] = parsed
    return dict(sorted(parameters.items()))


def normalized_metadata(value: str, name: str) -> str:
    normalized = " ".join(value.strip().split())
    if (
        not normalized
        or len(normalized) > 128
        or "/" in normalized
        or "\\" in normalized
        or not re.fullmatch(r"[A-Za-z0-9 ._()+,:-]+", normalized)
    ):
        raise ValueError(f"{name} identity is unsafe")
    return normalized


def canonical_identity(document: dict[str, Any]) -> dict[str, Any]:
    workload = document["workload"]
    build = document["build"]
    return {
        "workload_id": workload["id"],
        "workload_version": workload["version"],
        "layer": workload["layer"],
        "parameters": workload["parameters"],
        "evidence_contract": workload["evidence_contract"],
        "fixture_hash": workload["fixture_hash"],
        "preset": build["preset"],
        "compiler": build["compiler"],
        "configuration": build["configuration"],
        "renderer": build["renderer"],
        "platform": document["platform"],
        "sample_policy": document["sample_policy"],
        "metric_schema": document["metric_schema"],
    }


def validate_server_smoke_evidence(path: Path, parameters: dict[str, Any]) -> None:
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"server smoke evidence is missing or invalid: {exc}") from exc
    expected = {
        "ticks_requested": parameters.get("ticks"),
        "surface_radius": parameters.get("surface_radius"),
        "collision_radius": parameters.get("collision_radius"),
        "seed": str(parameters.get("seed")),
        "preset": parameters.get("world_preset"),
    }
    if evidence.get("schema") != "luminumbra.server_tick.v1":
        raise ValueError("server smoke evidence schema is invalid")
    if not evidence.get("passed") or not evidence.get("deterministic"):
        raise ValueError("server smoke did not prove deterministic completed work")
    if any(evidence.get(name) != value for name, value in expected.items()):
        raise ValueError("server smoke evidence parameters do not match the workload")
    runs = evidence.get("runs")
    ticks = parameters.get("ticks")
    if not isinstance(runs, list) or len(runs) != 2 or any(
        not isinstance(run, dict)
        or not run.get("ok")
        or run.get("ticks_executed") != ticks
        or run.get("chunks_streamed", 0) <= 0
        for run in runs
    ):
        raise ValueError("server smoke evidence has incomplete engine work")
    world_hash = evidence.get("world_hash")
    if not world_hash or world_hash != evidence.get("world_hash_replay"):
        raise ValueError("server smoke evidence has no stable state hash")


def read_evidence_metric(path: Path, name: str) -> float:
    spec = EVIDENCE_METRICS[name]
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"evidence for metric {name} is missing or invalid: {exc}") from exc
    value: Any = evidence
    for key in spec["path"]:
        if not isinstance(value, dict) or key not in value:
            raise ValueError(f"evidence metric {name} is missing from the artifact")
        value = value[key]
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(float(value))
        or float(value) < 0
    ):
        raise ValueError(f"evidence metric {name} is not a finite non-negative number")
    return float(value)


def validate_workload_evidence(
    contract: str, path: Path | None, parameters: dict[str, Any]
) -> None:
    if contract == "server-smoke" and path is not None:
        validate_server_smoke_evidence(path, parameters)
        return
    raise ValueError(f"unsupported or missing workload evidence contract: {contract}")


def run_command(args: argparse.Namespace) -> int:
    command = list(args.command)
    if command and command[0] == "--":
        command.pop(0)
    if not command:
        raise ValueError("run requires a command after --")

    parameters = parse_parameters(args.parameter)
    build_manifest = load_build_manifest(args.build_manifest)
    evidence_metrics = sorted(dict.fromkeys(args.evidence_metric or []))
    if evidence_metrics and args.evidence is None:
        raise ValueError("evidence metrics require an evidence path")
    if args.mode == "gating":
        if args.samples < MIN_GATING_SAMPLES:
            raise ValueError(f"gating runs require at least {MIN_GATING_SAMPLES} samples")
        if not args.evidence_contract or args.evidence is None or not args.fixture_hash:
            raise ValueError(
                "gating runs require evidence contract, evidence path, and fixture hash"
            )

    binary = executable_path(command[0])
    binary_sha256 = file_hash(binary)
    gpu = normalized_metadata(
        args.gpu or os.environ.get("LUMINUMBRA_PERF_GPU", "unknown"), "GPU"
    )
    driver = normalized_metadata(
        args.driver or os.environ.get("LUMINUMBRA_PERF_DRIVER", "unknown"), "driver"
    )
    platform_data = {
        "os": platform.system().lower(),
        "os_release": platform.release(),
        "architecture": platform.machine().lower(),
        "cpu": cpu_description(),
        "gpu": "not-required" if args.layer != "rendering-gpu" else gpu,
        "driver": "not-required" if args.layer != "rendering-gpu" else driver,
    }
    result: dict[str, Any] = {
        "schema": SCHEMA,
        "status": "failed",
        "enforcement": args.mode,
        "reasons": [],
        "workload": {
            "id": args.workload,
            "version": args.workload_version,
            "layer": args.layer,
            "parameters": parameters,
            "evidence_contract": args.evidence_contract or "process-exit",
            "fixture_hash": args.fixture_hash or "not-required",
        },
        "build": {
            "commit": args.commit or git_commit(),
            "preset": args.preset,
            "compiler": build_manifest["compiler"],
            "configuration": build_manifest["configuration"],
            "renderer": args.renderer,
            "binary": binary.name if binary else Path(command[0]).name,
            "binary_sha256": binary_sha256,
        },
        "platform": platform_data,
        "sample_policy": {"warmup": args.warmup, "samples": args.samples},
        "metric_schema": {
            "wall_time": {"unit": "ms", "direction": "lower"},
            **{
                name: {
                    "unit": EVIDENCE_METRICS[name]["unit"],
                    "direction": EVIDENCE_METRICS[name]["direction"],
                }
                for name in evidence_metrics
            },
        },
        "comparability_key": "pending",
        "metrics": {},
    }
    result["comparability_key"] = stable_hash(canonical_identity(result))

    if args.layer == "rendering-gpu" and (gpu == "unknown" or driver == "unknown"):
        result["status"] = "unevaluated"
        result["reasons"] = ["GPU and driver identity are required for GPU measurements"]
        write_json(args.output, result)
        return UNEVALUATED

    samples: list[float] = []
    evidence_samples: dict[str, list[float]] = {name: [] for name in evidence_metrics}
    for iteration in range(args.warmup + args.samples):
        if args.evidence is not None:
            args.evidence.unlink(missing_ok=True)
        started = time.perf_counter_ns()
        try:
            completed = subprocess.run(
                command,
                timeout=args.timeout,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            result["reasons"] = [f"command could not complete: {exc}"]
            write_json(args.output, result)
            return 1
        elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000.0
        if completed.returncode != 0:
            result["reasons"] = [
                f"command returned {completed.returncode} on iteration {iteration + 1}"
            ]
            write_json(args.output, result)
            return 1
        if args.evidence_contract:
            try:
                validate_workload_evidence(args.evidence_contract, args.evidence, parameters)
            except ValueError as exc:
                result["reasons"] = [str(exc)]
                write_json(args.output, result)
                return 1
        if iteration >= args.warmup:
            samples.append(elapsed_ms)
            for name in evidence_metrics:
                try:
                    evidence_samples[name].append(read_evidence_metric(args.evidence, name))
                except ValueError as exc:
                    result["reasons"] = [str(exc)]
                    write_json(args.output, result)
                    return 1

    result["status"] = "evaluated"
    result["metrics"] = {"wall_time": summarize(samples, "ms", "lower")}
    for name in evidence_metrics:
        result["metrics"][name] = summarize(
            evidence_samples[name],
            EVIDENCE_METRICS[name]["unit"],
            EVIDENCE_METRICS[name]["direction"],
        )
    write_json(args.output, result)
    print(f"evaluated {args.workload}: p50={result['metrics']['wall_time']['p50']:.3f} ms")
    return 0


def validate_run(document: Any) -> list[str]:
    errors: list[str] = []
    if not isinstance(document, dict) or document.get("schema") != SCHEMA:
        return [f"schema must be {SCHEMA}"]
    if document.get("status") not in {"evaluated", "unevaluated", "failed"}:
        errors.append("status is invalid")
    if not isinstance(document.get("comparability_key"), str):
        errors.append("comparability_key is missing")
    metrics = document.get("metrics")
    if document.get("status") == "evaluated" and (
        not isinstance(metrics, dict) or not metrics
    ):
        errors.append("evaluated result has no metrics")
    workload = document.get("workload")
    build = document.get("build")
    platform_data = document.get("platform")
    if (
        not isinstance(workload, dict)
        or not re.fullmatch(r"[a-z][a-z0-9-]{0,63}", str(workload.get("id", "")))
        or not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", str(workload.get("version", "")))
        or workload.get("layer") not in LAYERS
        or not isinstance(workload.get("parameters"), dict)
        or not workload.get("evidence_contract")
        or not workload.get("fixture_hash")
    ):
        errors.append("workload identity is missing")
    compiler = build.get("compiler") if isinstance(build, dict) else None
    configuration = build.get("configuration") if isinstance(build, dict) else None
    if (
        not isinstance(build, dict)
        or not re.fullmatch(r"[0-9a-f]{40,64}", str(build.get("commit", "")))
        or not re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", str(build.get("preset", "")))
        or build.get("renderer") not in RENDERERS
        or not re.fullmatch(r"[A-Za-z0-9_.+-]{1,128}", str(build.get("binary", "")))
        or not re.fullmatch(r"[0-9a-f]{64}", str(build.get("binary_sha256", "")))
        or not isinstance(compiler, dict)
        or any(
            not compiler.get(field) or compiler.get(field) == "unknown"
            for field in ("id", "version", "target")
        )
        or not isinstance(configuration, dict)
        or set(configuration) != BUILD_CONFIGURATION_FIELDS
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(configuration.get("effective_flags_sha256", ""))
        )
        or any(
            not isinstance(value, str) or "/" in value or "\\" in value
            for value in configuration.values()
        )
        or any(
            "/" in str(compiler.get(field, "")) or "\\" in str(compiler.get(field, ""))
            for field in ("id", "version", "target")
        )
    ):
        errors.append("build identity is missing")
    if (
        not isinstance(platform_data, dict)
        or not platform_data.get("os")
        or not platform_data.get("os_release")
        or not platform_data.get("architecture")
        or platform_data.get("cpu") in {None, "", "unknown"}
    ):
        errors.append("platform identity is missing")
    if isinstance(workload, dict) and isinstance(workload.get("parameters"), dict):
        try:
            reparsed_parameters = parse_parameters(
                [
                    f"{name}={json.dumps(value, separators=(',', ':'))}"
                    for name, value in workload["parameters"].items()
                ]
            )
        except ValueError:
            errors.append("workload parameters are unsafe")
        else:
            if reparsed_parameters != workload["parameters"]:
                errors.append("workload parameters are not canonical")
    sample_policy = document.get("sample_policy")
    if (
        not isinstance(sample_policy, dict)
        or not isinstance(sample_policy.get("warmup"), int)
        or sample_policy.get("warmup", -1) < 0
        or not isinstance(sample_policy.get("samples"), int)
        or sample_policy.get("samples", 0) < 1
    ):
        errors.append("sample policy is invalid")
    if document.get("enforcement") not in {"observation", "gating"}:
        errors.append("enforcement mode is invalid")
    if document.get("enforcement") == "gating" and (
        not isinstance(workload, dict)
        or workload.get("evidence_contract") != "server-smoke"
        or not re.fullmatch(r"[0-9a-f]{40,64}", str(workload.get("fixture_hash", "")))
    ):
        errors.append("gating workload evidence identity is invalid")
    metric_schema = document.get("metric_schema")
    if not isinstance(metric_schema, dict) or set(metric_schema) != set(metrics or {}):
        errors.append("metric schema does not match metrics")
    if (
        document.get("status") == "evaluated"
        and isinstance(workload, dict)
        and workload.get("layer") == "rendering-gpu"
        and (
            not isinstance(platform_data, dict)
            or platform_data.get("gpu") in {None, "unknown", "not-required"}
            or platform_data.get("driver") in {None, "unknown", "not-required"}
        )
    ):
        errors.append("evaluated GPU result has no GPU or driver identity")
    if isinstance(metrics, dict):
        for name, metric in metrics.items():
            if not isinstance(metric, dict):
                errors.append(f"metric {name} is not an object")
                continue
            if not isinstance(metric.get("unit"), str) or not metric["unit"]:
                errors.append(f"metric {name} has no unit")
                continue
            if metric.get("direction") not in {"lower", "higher"}:
                errors.append(f"metric {name} has invalid direction")
                continue
            if isinstance(metric_schema, dict) and metric_schema.get(name) != {
                "unit": metric.get("unit"),
                "direction": metric.get("direction"),
            }:
                errors.append(f"metric {name} does not match its schema")
            try:
                expected = summarize(
                    metric.get("samples", []),
                    metric.get("unit", ""),
                    metric.get("direction", ""),
                )
            except (TypeError, ValueError):
                errors.append(f"metric {name} has invalid samples")
                continue
            for field in ("sample_count", "p50", "p95", "p99", "max", "mad"):
                if not math.isclose(float(metric.get(field, math.nan)), float(expected[field])):
                    errors.append(f"metric {name} has inconsistent {field}")
    if not errors:
        try:
            expected_key = stable_hash(canonical_identity(document))
        except (KeyError, TypeError):
            errors.append("comparability identity is incomplete")
        else:
            if document.get("comparability_key") != expected_key:
                errors.append("comparability_key does not match the result identity")
    return errors


def load_run(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    errors = validate_run(document)
    if errors:
        raise ValueError("; ".join(errors))
    return document


def validate_rebaseline(document: Any) -> dict[str, str]:
    """Validate the tracked, single-use comparability-key transition."""
    required = {
        "schema",
        "retired_comparability_key",
        "replacement_comparability_key",
        "reason",
    }
    if not isinstance(document, dict) or set(document) != required:
        raise ValueError("re-baseline declaration fields are invalid")
    if document.get("schema") != REBASELINE_SCHEMA:
        raise ValueError(f"re-baseline schema must be {REBASELINE_SCHEMA}")
    retired = document.get("retired_comparability_key")
    replacement = document.get("replacement_comparability_key")
    if not isinstance(retired, str) or not re.fullmatch(r"[0-9a-f]{64}", retired):
        raise ValueError("retired comparability key must be a SHA-256 digest")
    if not isinstance(replacement, str) or not re.fullmatch(
        r"[0-9a-f]{64}", replacement
    ):
        raise ValueError("replacement comparability key must be a SHA-256 digest")
    if retired == replacement:
        raise ValueError("retired and replacement comparability keys must differ")
    reason = document.get("reason")
    if not isinstance(reason, str) or not reason.strip() or len(reason.strip()) > 1000:
        raise ValueError("re-baseline reason must be non-empty prose of at most 1000 characters")
    return {
        "schema": REBASELINE_SCHEMA,
        "retired_comparability_key": retired,
        "replacement_comparability_key": replacement,
        "reason": " ".join(reason.split()),
    }


def load_rebaseline(path: Path | None = None) -> dict[str, str] | None:
    """Load the tracked declaration; absence means the strict default applies."""
    path = REBASELINE_PATH if path is None else path
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"re-baseline declaration is invalid: {exc}") from exc
    return validate_rebaseline(document)


def mann_whitney_p(left: Sequence[float], right: Sequence[float]) -> float:
    values = list(left) + list(right)
    ordered_indices = sorted(range(len(values)), key=values.__getitem__)
    ranks = [0.0] * len(values)
    tie_counts: list[int] = []
    index = 0
    while index < len(ordered_indices):
        end = index + 1
        while (
            end < len(ordered_indices)
            and values[ordered_indices[end]] == values[ordered_indices[index]]
        ):
            end += 1
        average_rank = (index + 1 + end) / 2.0
        for ordered_index in ordered_indices[index:end]:
            ranks[ordered_index] = average_rank
        tie_counts.append(end - index)
        index = end
    n_left = len(left)
    n_right = len(right)
    rank_sum_left = sum(ranks[:n_left])
    u_left = rank_sum_left - n_left * (n_left + 1) / 2.0
    mean = n_left * n_right / 2.0
    total = n_left + n_right
    if total <= 20:
        observed_distance = abs(u_left - mean)
        extreme = 0
        combinations = 0
        rank_offset = n_left * (n_left + 1) / 2.0
        for selection in itertools.combinations(range(total), n_left):
            permuted_u = sum(ranks[position] for position in selection) - rank_offset
            combinations += 1
            if abs(permuted_u - mean) + 1e-12 >= observed_distance:
                extreme += 1
        return extreme / combinations
    tie_term = sum(count**3 - count for count in tie_counts)
    variance = n_left * n_right / 12.0 * (
        total + 1 - tie_term / (total * (total - 1)) if total > 1 else 0
    )
    if variance <= 0:
        return 1.0
    z = max(0.0, abs(u_left - mean) - 0.5) / math.sqrt(variance)
    return math.erfc(z / math.sqrt(2.0))


def paired_sign_p(base: Sequence[float], candidate: Sequence[float]) -> float:
    """Two-sided exact sign-test p value for ordered base/candidate observations."""
    if len(base) != len(candidate) or not base:
        raise ValueError("paired samples must be non-empty and equal in length")
    deltas = [candidate_value - base_value
              for base_value, candidate_value in zip(base, candidate)]
    positive = sum(delta > 0 for delta in deltas)
    negative = sum(delta < 0 for delta in deltas)
    count = positive + negative
    if count == 0:
        return 1.0
    tail = min(positive, negative)
    probability = 2.0 * sum(math.comb(count, value) for value in range(tail + 1)) / (2**count)
    return min(1.0, probability)


def combine_command(args: argparse.Namespace) -> int:
    try:
        runs = [load_run(path) for path in args.runs]
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"performance fragments are invalid: {exc}", file=sys.stderr)
        return UNEVALUATED
    first = runs[0]
    if any(
        run["status"] != "evaluated"
        or run["comparability_key"] != first["comparability_key"]
        or run["build"] != first["build"]
        or set(run["metrics"]) != set(first["metrics"])
        for run in runs
    ):
        print("performance fragments are not equivalent", file=sys.stderr)
        return UNEVALUATED

    combined = json.loads(json.dumps(first))
    combined["enforcement"] = "gating"
    combined["sample_policy"] = {
        "warmup": sum(run["sample_policy"]["warmup"] for run in runs),
        "samples": sum(run["sample_policy"]["samples"] for run in runs),
    }
    if combined["sample_policy"]["samples"] < MIN_GATING_SAMPLES:
        print("performance fragments are underpowered", file=sys.stderr)
        return UNEVALUATED
    for name, metric in combined["metrics"].items():
        samples = [
            sample
            for run in runs
            for sample in run["metrics"][name]["samples"]
        ]
        combined["metrics"][name] = summarize(
            samples, metric["unit"], metric["direction"]
        )
    combined["comparability_key"] = stable_hash(canonical_identity(combined))
    errors = validate_run(combined)
    if errors:
        print(f"combined performance run is invalid: {'; '.join(errors)}", file=sys.stderr)
        return UNEVALUATED
    write_json(args.output, combined)
    print(f"combined {len(runs)} fragments into {args.output}")
    return 0


def compare_runs(
    base: dict[str, Any],
    candidate: dict[str, Any],
    limit: float,
    min_samples: int = MIN_GATING_SAMPLES,
    rebaseline: dict[str, Any] | None = None,
) -> dict[str, Any]:
    declaration = validate_rebaseline(rebaseline) if rebaseline is not None else None
    result: dict[str, Any] = {
        "schema": COMPARISON_SCHEMA,
        "status": "unevaluated",
        "verdict": "unevaluated",
        "policy": {
            "version": "relative-paired-v2",
            "requested_limit_percent": limit,
            "effect_floor_percent": MIN_EFFECT_PERCENT,
            "limit_percent": max(limit, MIN_EFFECT_PERCENT),
            "minimum_samples": min_samples,
            "p_value_max": P_VALUE_MAX,
            "pooled_mad_multiplier": 3.0,
            "test": "paired two-sided sign test",
        },
        "base": {
            "commit": base.get("build", {}).get("commit"),
            "comparability_key": base.get("comparability_key"),
        },
        "candidate": {
            "commit": candidate.get("build", {}).get("commit"),
            "comparability_key": candidate.get("comparability_key"),
        },
        "reasons": [],
        "metrics": {},
    }
    if declaration is not None:
        result["rebaseline"] = {
            "applied": False,
            "retired_comparability_key": declaration["retired_comparability_key"],
            "replacement_comparability_key": declaration[
                "replacement_comparability_key"
            ],
            "reason": declaration["reason"],
            "warning": "declaration does not match this base/candidate transition",
        }
    if not math.isfinite(limit) or limit <= 0 or min_samples < 2:
        result["reasons"] = ["comparison policy is invalid"]
        return result
    if base["status"] != "evaluated" or candidate["status"] != "evaluated":
        result["reasons"] = ["base and candidate must both be evaluated"]
        return result
    if base.get("enforcement") != "gating" or candidate.get("enforcement") != "gating":
        result["reasons"] = ["only gating runs can produce a comparison verdict"]
        return result
    if base["comparability_key"] != candidate["comparability_key"]:
        if declaration is not None and (
            base["comparability_key"]
            == declaration["retired_comparability_key"]
            and candidate["comparability_key"]
            == declaration["replacement_comparability_key"]
        ):
            result["status"] = "evaluated"
            result["verdict"] = "rebaseline"
            result["reasons"] = [
                f"comparability mismatch explicitly re-baselined: {declaration['reason']}"
            ]
            result["rebaseline"]["applied"] = True
            result["rebaseline"].pop("warning")
            return result
        result["reasons"] = ["comparability keys differ"]
        return result

    if set(base["metrics"]) != set(candidate["metrics"]) or not base["metrics"]:
        result["reasons"] = ["metric sets differ or are empty"]
        return result
    shared = sorted(base["metrics"])
    verdict = "stable"
    for name in shared:
        base_metric = base["metrics"][name]
        candidate_metric = candidate["metrics"][name]
        if (
            base_metric["unit"] != candidate_metric["unit"]
            or base_metric["direction"] != candidate_metric["direction"]
        ):
            result["reasons"] = [f"metric {name} schema differs"]
            return result
        if len(base_metric["samples"]) != len(candidate_metric["samples"]):
            result["reasons"] = [f"metric {name} does not contain paired samples"]
            return result
        if len(base_metric["samples"]) < min_samples:
            result["status"] = "evaluated"
            result["verdict"] = "warning"
            result["reasons"] = [
                f"metric {name} is underpowered: {len(base_metric['samples'])} paired "
                f"observations, {min_samples} required"
            ]
            return result
        base_median = float(base_metric["p50"])
        candidate_median = float(candidate_metric["p50"])
        if base_median <= 0:
            result["reasons"] = [f"metric {name} base median is not positive"]
            return result
        delta = candidate_median - base_median
        delta_percent = delta / base_median * 100.0
        p_value = paired_sign_p(base_metric["samples"], candidate_metric["samples"])
        pooled_mad = math.hypot(float(base_metric["mad"]), float(candidate_metric["mad"]))
        significant = p_value <= P_VALUE_MAX and abs(delta) > 3.0 * pooled_mad
        metric_verdict = "stable"
        direction = base_metric["direction"]
        effective_limit = max(limit, MIN_EFFECT_PERCENT)
        regression = (
            delta_percent >= effective_limit
            if direction == "lower"
            else delta_percent <= -effective_limit
        )
        improvement = (
            delta_percent <= -effective_limit
            if direction == "lower"
            else delta_percent >= effective_limit
        )
        if significant and regression:
            metric_verdict = "regression"
            verdict = "regression"
        elif significant and improvement:
            metric_verdict = "improvement"
            if verdict == "stable":
                verdict = "improvement"
        result["metrics"][name] = {
            "unit": base_metric["unit"],
            "direction": direction,
            "base_sample_count": len(base_metric["samples"]),
            "candidate_sample_count": len(candidate_metric["samples"]),
            "base_p50": base_median,
            "candidate_p50": candidate_median,
            "delta_percent": delta_percent,
            "p_value": p_value,
            "pooled_mad": pooled_mad,
            "verdict": metric_verdict,
        }
    result["status"] = "evaluated"
    result["verdict"] = verdict
    return result


def compare_command(args: argparse.Namespace) -> int:
    try:
        comparison = compare_runs(
            load_run(args.base),
            load_run(args.candidate),
            args.limit,
            args.min_samples,
            load_rebaseline(),
        )
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"performance comparison is invalid: {exc}", file=sys.stderr)
        return UNEVALUATED
    comparison["policy"]["enforcement"] = (
        "observation" if args.report_only else "gating"
    )
    if args.output:
        write_json(args.output, comparison)
    print(json.dumps(comparison, indent=2, sort_keys=True))
    if comparison["status"] != "evaluated":
        return UNEVALUATED
    return 1 if comparison["verdict"] == "regression" and not args.report_only else 0


def validate_command(args: argparse.Namespace) -> int:
    try:
        load_run(args.run)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"performance run is invalid: {exc}", file=sys.stderr)
        return 1
    print(f"valid performance run: {args.run}")
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        epilog=(
            "Fixture re-baselines require a reviewed tools/perf/baselines/rebaseline.json "
            "with schema luminumbra.performance_rebaseline.v1, the exact retired and "
            "replacement comparability keys, and a prose reason. Treat it like a fixture "
            "change: reviewers must verify both identities and the justification, then "
            "remove the declaration after the stored base advances. A stale declaration "
            "only emits evidence metadata and never waives another transition."
        )
    )
    commands = root.add_subparsers(dest="action", required=True)

    run = commands.add_parser("run", help="sample a command and write a performance run")
    run.add_argument("--workload", required=True)
    run.add_argument("--workload-version", default="1")
    run.add_argument("--layer", required=True, choices=LAYERS)
    run.add_argument("--preset", default="perf")
    run.add_argument("--build-manifest", type=Path, required=True)
    run.add_argument("--commit")
    run.add_argument("--renderer", choices=RENDERERS, default="none")
    run.add_argument("--gpu")
    run.add_argument("--driver")
    run.add_argument("--mode", choices=("observation", "gating"), default="observation")
    run.add_argument("--parameter", action="append", default=[])
    run.add_argument("--fixture-hash")
    run.add_argument("--evidence-contract", choices=("server-smoke",))
    run.add_argument("--evidence", type=Path)
    run.add_argument(
        "--evidence-metric",
        action="append",
        default=[],
        choices=sorted(EVIDENCE_METRICS),
        help="also sample this scalar from the evidence artifact each iteration",
    )
    run.add_argument("--warmup", type=int, default=2)
    run.add_argument("--samples", type=int, default=10)
    run.add_argument("--timeout", type=float, default=300.0)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("command", nargs=argparse.REMAINDER)
    run.set_defaults(handler=run_command)

    for name in ("compare", "bisect-eval"):
        compare = commands.add_parser(name, help="compare base and candidate runs")
        compare.add_argument("--base", type=Path, required=True)
        compare.add_argument("--candidate", type=Path, required=True)
        compare.add_argument("--limit", type=float, default=MIN_EFFECT_PERCENT)
        compare.add_argument("--min-samples", type=int, default=MIN_GATING_SAMPLES)
        compare.add_argument("--report-only", action="store_true")
        compare.add_argument("--output", type=Path)
        compare.set_defaults(handler=compare_command)

    combine = commands.add_parser("combine", help="combine equivalent sampled fragments")
    combine.add_argument("--runs", type=Path, nargs="+", required=True)
    combine.add_argument("--output", type=Path, required=True)
    combine.set_defaults(handler=combine_command)

    validate = commands.add_parser("validate", help="validate a run document")
    validate.add_argument("run", type=Path)
    validate.set_defaults(handler=validate_command)
    return root


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if getattr(args, "warmup", 0) < 0 or getattr(args, "samples", 1) < 1:
        raise ValueError("warmup must be non-negative and samples must be positive")
    if hasattr(args, "timeout") and (not math.isfinite(args.timeout) or args.timeout <= 0):
        raise ValueError("timeout must be finite and positive")
    if hasattr(args, "limit") and (not math.isfinite(args.limit) or args.limit <= 0):
        raise ValueError("limit must be finite and positive")
    if hasattr(args, "min_samples") and args.min_samples < 2:
        raise ValueError("minimum samples must be at least two")
    return args.handler(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
