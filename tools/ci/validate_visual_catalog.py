#!/usr/bin/env python3
"""Check visual coverage, immutable evidence, and explicit approval bookkeeping.

This is a file-only check. It never captures pixels or approves a visual result.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
import statistics
import sys


ROOT = Path(__file__).resolve().parents[2]
CATALOG = "docs/visual-catalog.json"
ORIGINAL_CONTRACT = "docs/visual-catalog-original-contract.json"
ORIGINAL_CONTRACT_SHA256 = "2e71754d85d75c19d7301ce92d0341a176f6ee25b37c4cd79c6419b1ed4b3af6"
FAMILIES = {"R": (23, "runtime"), "U": (10, "ui"),
            "D": (22, "diagnostic"), "B": (8, "blender"),
            "G": (7, "game/campaign")}
ORIGINAL_IDS = {f"{prefix}{number:02}" for prefix, (count, _) in FAMILIES.items()
                for number in range(1, count + 1)}
REQUIRED_ADDITIONS = {"R24", "R25", "G08", "G09", "G10"} | {f"B{i:02}" for i in range(9, 16)}
SWEEP = ["/".join(cell) for cell in itertools.product(
    ("summer", "winter"), ("dawn", "noon", "dusk", "night"),
    ("yaw000", "yaw120", "yaw240", "down35", "up25", "water"),
    ("clear", "storm"))]
HISTORICAL_SOURCE = "17c70af79c96d600bde11b0fb13c59b7af15a105"
BASELINES = {
    "G04": ("forest-reference", "c8ea3be89ed6d9a5b33c5501c0c925f31b0a1398667788d02a8ee124fa95f4a7",
            "b6fdf25209b5abb1ff024a134bdafc68a8d3be4543a3fa3b524b594f46c89dd7"),
    "R14": ("foliage-functional", "6d864644ab5f40b4fdedd4125163bcdc16049af49dbddf9b42dc98bc1b226e6f",
            "4c4df138b8cdb3eb668e2aeead13beda3463656cfaf2cbbd23d177f613cae7ac"),
}
EVIDENCE_DIR = "docs/assets/visual-baselines/20260908/"
PERFORMANCE_REQUIREMENTS = {
    "R24": {"scope": "accepted_expanded_world", "profile_count": 2, "metrics": [
        {"id": "frame_ms", "statistic": "p99", "operator": "<=", "target": 16.67,
         "reported_target": 8.33}]},
    "R14": {"scope": "foliage_draw", "profile_count": 1, "metrics": [
        {"id": "foliage_draw_ms", "statistic": "profile", "operator": "<=", "target": 0.6}]},
    "B09": {"scope": "installed_authoring", "profile_count": 1, "metrics": [
        {"id": "presentation_fps", "statistic": "profile", "operator": ">=", "target": 30},
        {"id": "camera_transform_ms", "statistic": "p95", "operator": "<", "target": 100},
        {"id": "warm_material_refresh_seconds", "statistic": "profile", "operator": "<", "target": 2},
        {"id": "small_asset_refresh_seconds", "statistic": "profile", "operator": "<", "target": 5}]},
}
REPORT_ONLY = {"scope": "report_only", "profile_count": 0, "metrics": []}
STATISTICS = {"mean", "median", "p95", "p99", "min", "max"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def nonempty(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def strings(value: object) -> bool:
    return isinstance(value, list) and bool(value) and all(nonempty(x) for x in value)


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha(value: object, length: int = 64) -> bool:
    return isinstance(value, str) and re.fullmatch(rf"[0-9a-f]{{{length}}}", value) is not None


def local_file(root: Path, relative: str) -> Path:
    require(nonempty(relative), "artifact/source path must be nonempty")
    path = Path(relative)
    require(bool(path.parts) and not path.is_absolute() and ".." not in path.parts and "\\" not in relative,
            f"path must be repository relative: {relative}")
    require(path.parts[0] not in {"build", ".git", ".codex", ".agents"},
            f"private/build path is not publishable evidence: {relative}")
    resolved_root = root.resolve()
    candidate = resolved_root
    for part in path.parts:
        candidate = candidate / part
        require(not candidate.is_symlink(), f"linked evidence/source is not allowed: {relative}")
    require(candidate.is_file(), f"missing file: {relative}")
    return candidate


def read_json(path: Path) -> dict:
    def unique(pairs: list[tuple[str, object]]) -> dict:
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)
    require(isinstance(value, dict), f"expected JSON object: {path.name}")
    return value


def fixture_manifest(root: Path, row: dict) -> dict:
    return {path: digest(local_file(root, path).read_bytes())
            for path in sorted(row["fixture"]["source_paths"])}


def manifest_digest(value: dict) -> str:
    return digest(json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8"))


def requirement_digest(row: dict, policy: dict, root: Path = ROOT) -> str:
    fields = ("id", "name", "visual_goal", "fixture", "required_variants",
              "temporal_evidence_required", "dependencies", "acceptance_checks", "performance_requirements")
    value = {key: row[key] for key in fields}
    value["fixture_files"] = fixture_manifest(root, row)
    value["policy"] = policy
    return digest(json.dumps(value, sort_keys=True, separators=(",", ":"),
                             ensure_ascii=False).encode("utf-8"))


def finite(value: object) -> bool:
    return type(value) in {int, float} and math.isfinite(value) and value >= 0


def statistic(samples: list[float], name: str) -> float:
    if name in {"p95", "p99"}:
        return sorted(samples)[math.ceil(len(samples) * int(name[1:]) / 100) - 1]
    return {"mean": statistics.mean, "median": statistics.median,
            "min": min, "max": max}[name](samples)


def artifact_json(root: Path, by_id: dict, artifact_id: str, role: str) -> dict:
    require(nonempty(artifact_id) and by_id.get(artifact_id, {}).get("role") == role,
            f"missing {role} artifact reference")
    return read_json(local_file(root, by_id[artifact_id]["path"]))


def identity_join(document: dict, packet: dict) -> None:
    require(document.get("scenario_id") == packet["scenario_id"]
            and document.get("source_commit") == packet["source_commit"]
            and document.get("engine_binary_sha256") == packet["identities"]["engine_binary_sha256"]
            and document.get("fixture_sha256") == packet["identities"]["fixture_sha256"],
            "evidence scenario/source/engine/fixture identity mismatch")


def validate_captures(root: Path, row: dict, packet: dict, by_id: dict) -> None:
    manifest = artifact_json(root, by_id, packet.get("capture_manifest"), "capture_manifest")
    require(manifest.get("schema") == "luminumbra.visual_capture_manifest.v1", "unknown capture manifest schema")
    identity_join(manifest, packet)
    captures = manifest.get("captures")
    require(isinstance(captures, list) and all(isinstance(c, dict) for c in captures), "invalid capture records")
    frames, originals, seen = {}, {}, set()
    variants = {v["id"]: v for v in packet["variants"]}
    for capture in captures:
        variant = capture.get("variant_id")
        require(nonempty(variant) and variant in variants, "unknown capture variant")
        require(nonempty(capture.get("run_id")) and type(capture.get("frame_id")) is int
                and capture["frame_id"] >= 0, "capture needs actual run/frame identity")
        original = capture.get("original")
        require(nonempty(original) and by_id.get(original, {}).get("role") == "original",
                "capture original is absent or is a preview")
        require(capture.get("original_sha256") == by_id[original]["sha256"], "capture pixel identity mismatch")
        frame = (capture["run_id"], capture["frame_id"])
        path = local_file(root, by_id[original]["path"]).resolve()
        require(frame not in frames or frames[frame] == variant, "source frame reused across distinct variants")
        require(path not in originals or originals[path] == variant, "original file reused across distinct variants")
        require((variant, original) not in seen, "duplicate capture original")
        frames[frame], originals[path] = variant, variant
        seen.add((variant, original))
    expected = {(v["id"], original) for v in packet["variants"] for original in v["originals"]}
    require(seen == expected, "capture manifest must join every variant original exactly")


def validate_metrics(root: Path, row: dict, packet: dict, by_id: dict, require_qualified: bool) -> None:
    requirements = row["performance_requirements"]
    if not requirements["metrics"]:
        return
    performance = packet["performance"]
    if not require_qualified and performance["status"] != "measured":
        return
    require(performance["status"] == "measured", "required performance cannot be N/A or unmeasured")
    require(performance.get("scope") == requirements["scope"], "required performance scope mismatch")
    profiles = performance.get("profiles")
    require(isinstance(profiles, list) and len(profiles) == requirements["profile_count"],
            "required performance profiles are incomplete")
    ids, configurations = set(), set()
    for profile in profiles:
        require(isinstance(profile, dict) and nonempty(profile.get("id")), "invalid performance profile")
        require(profile["id"] not in ids, "duplicate performance profile")
        ids.add(profile["id"])
        frozen = artifact_json(root, by_id, profile.get("manifest"), "performance_profile")
        require(frozen.get("schema") == "luminumbra.visual_performance_profile.v1"
                and frozen.get("profile_id") == profile["id"]
                and frozen.get("scope") == requirements["scope"], "performance profile identity mismatch")
        require(isinstance(frozen.get("configuration"), dict) and bool(frozen["configuration"]),
                "profile needs frozen workload/configuration")
        configuration = manifest_digest(frozen["configuration"])
        require(configuration not in configurations, "profiles must identify distinct workload configurations")
        configurations.add(configuration)
        metrics = profile.get("metrics")
        require(isinstance(metrics, list) and all(isinstance(m, dict) for m in metrics), "invalid metric records")
        require(len(metrics) == len(requirements["metrics"])
                and {m.get("id") for m in metrics} == {m["id"] for m in requirements["metrics"]},
                "required performance metrics are incomplete")
        for requirement in requirements["metrics"]:
            metric = next(m for m in metrics if m["id"] == requirement["id"])
            aggregation = frozen.get("statistics", {}).get(metric["id"])
            require(aggregation in STATISTICS and (requirement["statistic"] == "profile"
                    or aggregation == requirement["statistic"]), "required metric statistic changed or missing")
            raw = artifact_json(root, by_id, metric.get("raw_samples"), "raw_performance")
            require(raw.get("schema") == "luminumbra.visual_performance_samples.v1"
                    and raw.get("profile_id") == profile["id"] and raw.get("metric_id") == metric["id"]
                    and raw.get("profile_sha256") == by_id[profile["manifest"]]["sha256"],
                    "raw sample profile/metric identity mismatch")
            identity_join(raw, packet)
            samples = raw.get("samples")
            require(isinstance(samples, list) and bool(samples), "metric needs nonempty raw samples")
            sample_ids = set()
            for sample in samples:
                require(isinstance(sample, dict) and finite(sample.get("value")), "metric samples must be finite")
                require(nonempty(sample.get("run_id")) and type(sample.get("frame_id")) is int
                        and sample["frame_id"] >= 0, "sample needs actual run/frame identity")
                key = (sample["run_id"], sample["frame_id"])
                require(key not in sample_ids, "duplicate raw sample identity")
                sample_ids.add(key)
            actual = statistic([s["value"] for s in samples], aggregation)
            require(finite(metric.get("value")) and math.isclose(metric["value"], actual, rel_tol=1e-9, abs_tol=1e-9),
                    "reported metric differs from raw samples")
            if "reported_target" in requirement:
                require(metric.get("reported_target") == requirement["reported_target"], "reported target was omitted")
            if require_qualified:
                target = requirement["target"]
                require({"<=": actual <= target, "<": actual < target, ">=": actual >= target}[requirement["operator"]],
                        f"required metric target missed: {metric['id']}")
    if require_qualified:
        require(performance.get("target_status") == "met", "required established targets must be met")


def validate_packet(root: Path, row: dict, policy: dict, require_qualified: bool = True) -> None:
    ref = row["review_packet"]
    require(isinstance(ref, dict) and sha(ref.get("sha256")), "packet needs path and SHA-256")
    path = local_file(root, ref["path"])
    require(digest(path.read_bytes()) == ref["sha256"], "packet digest changed")
    packet = read_json(path)
    require(packet.get("schema") == "luminumbra.visual_review_packet.v1", "unknown packet schema")
    require(packet.get("scenario_id") == row["id"], "packet belongs to a different scenario")
    require(packet.get("requirement_sha256") == requirement_digest(row, policy, root),
            "packet does not cover current fixture, variants, acceptance checks and policy")
    files = fixture_manifest(root, row)
    require(packet.get("fixture_files") == files, "packet fixture content manifest is stale")
    require(sha(packet.get("source_commit"), 40), "packet needs actual executed source commit")
    require(packet.get("correctness") in {"passed", "failed", "incomplete"}, "unknown correctness state")
    require(packet.get("capture_status") in {"native_qualified", "incomplete", "unavailable"},
            "unknown capture status")
    if require_qualified:
        require(row["fixture"]["status"] == "source_fixture" and bool(files),
                "review ready packet needs implemented fixture source")
        require(packet["correctness"] == "passed", "failed correctness cannot be review ready")
        require(packet["capture_status"] == "native_qualified", "actual native capture is required")
    for field in ("reproduction", "hardware", "camera_lighting", "identities", "performance",
                  "findings", "aesthetic_review"):
        require(isinstance(packet.get(field), dict) and bool(packet[field]), f"missing packet {field}")
    identities = packet["identities"]
    require(identities.get("fixture_sha256") == manifest_digest(files), "fixture digest does not bind source files")
    for field in ("engine_binary_sha256", "fixture_sha256", "asset_manifest_sha256",
                  "tool_manifest_sha256"):
        require(sha(identities.get(field)), f"missing actual {field}")
    if require_qualified:
        require(packet["hardware"].get("hardware_rendering") is True,
                "software rendering cannot qualify native GPU acceptance")
    for field in ("os", "cpu", "gpu", "driver", "backend"):
        require(nonempty(packet["hardware"].get(field)), f"missing hardware {field}")
    performance = packet["performance"]
    require(performance.get("status") in {"measured", "not_applicable", "not_measured"},
            "unknown performance status")
    if require_qualified:
        require(performance["status"] != "not_measured", "unmeasured performance cannot silently qualify")
    if performance["status"] == "not_applicable":
        require(nonempty(performance.get("rationale")), "performance N/A requires rationale")
    elif performance["status"] == "measured":
        require(performance.get("target_status") in {"met", "missed", "not_comparable", "not_established"},
                "unknown target status")
        if require_qualified:
            require(performance["target_status"] in {"met", "not_established"},
                    "missed or incomparable required target cannot qualify")
        require(nonempty(performance.get("scope")), "performance needs workload/target scope")
    artifacts = packet.get("artifacts")
    require(isinstance(artifacts, list) and bool(artifacts), "packet needs original artifacts")
    by_id = {}
    for artifact in artifacts:
        require(isinstance(artifact, dict) and nonempty(artifact.get("id")), "invalid packet artifact")
        require(artifact["id"] not in by_id, "duplicate artifact ID")
        require(sha(artifact.get("sha256")), "artifact needs SHA-256")
        artifact_path = local_file(root, artifact["path"])
        require(digest(artifact_path.read_bytes()) == artifact["sha256"], "artifact digest changed")
        if artifact.get("role") == "original":
            require(artifact_path.suffix.lower() in {".png", ".ppm"}, "original must be lossless PNG/PPM")
        by_id[artifact["id"]] = artifact
    variants = packet.get("variants")
    require(isinstance(variants, list) and all(isinstance(v, dict) for v in variants),
            "packet requires variant records")
    names = [v.get("id") for v in variants]
    require(len(names) == len(set(names)) and set(names) == set(row["required_variants"]),
            "packet variants must cover the complete scenario exactly")
    for variant in variants:
        require(variant.get("status") in {"passed", "failed", "missing"}, "unknown variant status")
        if require_qualified:
            require(variant["status"] == "passed", "missing/failed variant cannot qualify")
        originals = variant.get("originals")
        require(strings(originals) or (variant["status"] == "missing" and originals == []),
                "every produced variant needs original pixels")
        require(all(by_id.get(x, {}).get("role") == "original" for x in originals),
                "variant original reference is absent or is a preview")
    roles = {a.get("role") for a in artifacts}
    require("functional" in roles, "packet needs functional evidence")
    if row["temporal_evidence_required"] and require_qualified:
        require("temporal" in roles, "motion claims need automated temporal evidence")
    if performance["status"] == "measured":
        require("raw_performance" in roles, "measured performance needs raw samples")
    validate_captures(root, row, packet, by_id)
    validate_metrics(root, row, packet, by_id, require_qualified)


def validate(catalog: dict, root: Path) -> list[str]:
    errors = []
    try:
        require(catalog.get("schema") == "luminumbra.visual_catalog.v1", "unknown catalog schema")
        require(catalog.get("original_group_count") == 70, "original denominator must remain 70")
        require(catalog.get("original_family_counts") == {family: count for count, family in FAMILIES.values()},
                "original family denominators changed")
        require(sha(catalog.get("source_audit_commit"), 40), "catalog needs audited source commit")
        rows = catalog["rows"]
        require(isinstance(rows, list) and all(isinstance(r, dict) for r in rows), "rows must be objects")
        ids = [r.get("id") for r in rows]
        require(all(nonempty(i) for i in ids) and len(ids) == len(set(ids)), "missing or duplicate scenario IDs")
        require({r["id"] for r in rows if r.get("origin") == "original"} == ORIGINAL_IDS,
                "original R01-R23/U01-U10/D01-D22/B01-B08/G01-G07 roster changed")
        require(REQUIRED_ADDITIONS <= {r["id"] for r in rows if r.get("origin") == "added"},
                "required added world/authoring/game scenario disappeared")
        original_file = local_file(root, ORIGINAL_CONTRACT)
        require(digest(original_file.read_bytes()) == ORIGINAL_CONTRACT_SHA256,
                "immutable original requirement snapshot changed")
        originals = {r["id"]: r for r in read_json(original_file)["rows"]}
        require(isinstance(catalog.get("policy"), dict), "missing catalog policy")
        frame_targets = catalog["policy"]["frame_targets"]
        require(frame_targets["p99_max_ms"] == 16.67 and frame_targets["reported_target_ms"] == 8.33
                and frame_targets["foliage_draw_target_ms"] == 0.6, "accepted target values changed")
        source = local_file(root, "src/luminumbra_client/core/RuntimeScenarioConfig.h").read_text()
        selectors = set(re.findall(r'return scenario == "([^"]+)"', source)) - {"forced_crash"}
        require({r.get("runtime_selector") for r in rows if r.get("runtime_selector")} == selectors,
                "runtime selector discovery does not reconcile to catalog")
        ui_paths = {p.relative_to(root).as_posix() for p in (root / "data/ui").glob("*.rml")}
        mapped_ui = {p for r in rows if r.get("family") == "ui" for p in r["fixture"]["source_paths"]
                     if p.startswith("data/ui/") and p.endswith(".rml")}
        require(ui_paths == mapped_ui, "shipped UI documents do not reconcile to catalog")
        receipt = read_json(local_file(root, EVIDENCE_DIR + "receipts.json"))
        entries = {e["scenario_id"]: e for e in receipt["entries"]}
        require(len(receipt["entries"]) == 2 and set(entries) == set(BASELINES), "historical baseline roster changed")
        historical = catalog["historical_baselines"]
        require(historical["source_commit"] == HISTORICAL_SOURCE and historical["approved_groups"] == 0
                and set(historical["scenario_ids"]) == set(BASELINES)
                and historical["receipt"] == EVIDENCE_DIR + "receipts.json",
                "historical catalog baseline identity or approval changed")
        for scenario_id, (name, image_hash, analysis_hash) in BASELINES.items():
            entry = entries[scenario_id]
            require(entry["source_commit"] == HISTORICAL_SOURCE and entry["visual_approval"] == "not_approved",
                    "historical baseline identity or approval changed")
            require(entry["image"] == name + ".png" and entry["image_sha256"] == image_hash
                    and entry["analysis"] == name + ".json" and entry["analysis_sha256"] == analysis_hash,
                    "historical receipt artifact identity changed")
            for suffix, expected in ((".png", image_hash), (".json", analysis_hash)):
                require(digest(local_file(root, EVIDENCE_DIR + name + suffix).read_bytes()) == expected,
                        f"historical {scenario_id} artifact identity changed")
    except (ValueError, KeyError, TypeError, OSError) as error:
        return [str(error)]
    for row in rows:
        try:
            scenario_id = row["id"]
            require(re.fullmatch(r"[RUDBG][0-9]{2,}", scenario_id) is not None, "invalid scenario ID")
            require(row["family"] == FAMILIES[scenario_id[0]][1], "ID family mismatch")
            if row["origin"] == "original":
                require(row["parents"] == [], "original rows cannot be folded into other groups")
                original = originals[scenario_id]
                require(row["name"] == original["name"] and row["visual_goal"] == original["visual_goal"],
                        "preserve original scenario identity and visual goal")
                for field in ("required_variants", "acceptance_checks"):
                    require(set(original[field]) <= set(row[field]), f"original {field} were reduced")
                require(not original["temporal_evidence_required"] or row["temporal_evidence_required"] is True,
                        "original temporal requirement was removed")
            else:
                require(row["origin"] == "added" and strings(row["parents"]), "added rows need original parents")
                require(int(scenario_id[1:]) > FAMILIES[scenario_id[0]][0], "added ID must follow original family IDs")
                require(set(row["parents"]) <= ORIGINAL_IDS, "added row parent must be an original group")
            for field in ("name", "visual_goal", "next_action"):
                require(nonempty(row[field]), f"missing {field}")
            for field in ("required_variants", "acceptance_checks", "dependencies"):
                require(strings(row[field]) and len(row[field]) == len(set(row[field])), f"invalid {field}")
            require(set(row["dependencies"]) <= set(catalog["dependencies"]), "unknown implementation dependency")
            require(row.get("performance_requirements") == PERFORMANCE_REQUIREMENTS.get(scenario_id, REPORT_ONLY),
                    "accepted scoped performance requirements changed")
            if scenario_id == "R01":
                require(set(row["required_variants"]) == set(SWEEP), "R01 must retain all 96 summer/winter cells")
            require(type(row["temporal_evidence_required"]) is bool, "temporal requirement must be boolean")
            fixture = row["fixture"]
            require(fixture["status"] in {"source_fixture", "missing"} and nonempty(fixture["description"]),
                    "fixture needs implementation status and description")
            require(isinstance(fixture["source_paths"], list), "source paths must be a list")
            for path in fixture["source_paths"]:
                local_file(root, path)
            require(fixture["status"] != "source_fixture" or bool(fixture["source_paths"]),
                    "source fixture needs current source links")
            require(sha(row["reconciliation"]["source_commit"], 40) and nonempty(row["reconciliation"]["finding"]),
                    "missing source reconciliation")
            capture = row["capture"]
            require(capture["state"] in {"missing", "existing_subset", "complete"} and nonempty(capture["scope"]),
                    "capture needs explicit implementation status and scope")
            require(capture["command"] is None if capture["state"] == "missing" else strings(capture["command"]),
                    "capture command must agree with implementation status")
            require(isinstance(capture["environment"], dict), "capture environment must be explicit")
            require(row["execution_state"] in {"not_captured", "deficient_baseline", "blocked", "captured",
                    "ready_for_review", "approved", "changes_requested"}, "unknown execution state")
            if row["execution_state"] == "blocked":
                require(nonempty(row.get("blocker")), "blocked scenario needs a concrete blocker")
            evidence = row["evidence"]
            require(isinstance(evidence, list), "evidence must be a list")
            for item in evidence:
                require(item["kind"] in {"deficient_baseline", "capture"}, "unknown evidence kind")
                require(sha(item["source_commit"], 40), "evidence needs original source identity")
                local_file(root, item["receipt"])
                require(isinstance(item["artifacts"], list) and bool(item["artifacts"]), "missing evidence artifacts")
                for artifact in item["artifacts"]:
                    require(sha(artifact["sha256"]) and digest(local_file(root, artifact["path"]).read_bytes())
                            == artifact["sha256"], "evidence artifact hash mismatch")
            if row["execution_state"] in {"captured", "deficient_baseline"}:
                require(bool(evidence), "captured state needs retained evidence")
            if scenario_id in BASELINES:
                require(any(e["kind"] == "deficient_baseline" and e["source_commit"] == HISTORICAL_SOURCE
                            and e["receipt"] == EVIDENCE_DIR + "receipts.json" for e in evidence),
                        "preserve original deficient baseline evidence independently of new results")
            approval = row["approval"]
            require(approval["state"] in {"pending", "approved", "changes_requested"}, "unknown approval state")
            reviewed = row["execution_state"] in {"ready_for_review", "approved", "changes_requested"}
            if reviewed or row["review_packet"] is not None:
                validate_packet(root, row, catalog["policy"],
                                require_qualified=row["execution_state"] in {"ready_for_review", "approved"})
            if approval["state"] == "pending":
                require(approval["packet_sha256"] is None and approval["decision_reference"] is None,
                        "pending approval must not contain an invented decision")
                require(row["execution_state"] not in {"approved", "changes_requested"}, "execution/approval mismatch")
            else:
                require(row["execution_state"] == approval["state"], "execution/approval mismatch")
                require(approval["packet_sha256"] == row["review_packet"]["sha256"], "decision refers to stale packet")
                require(nonempty(approval["decision_reference"]) and approval.get("actor") == "user",
                        "explicit user decision reference is required")
        except (ValueError, KeyError, TypeError, OSError) as error:
            errors.append(f"{row['id']}: {error}")
    return errors


def summary(catalog: dict) -> str:
    rows = catalog["rows"]
    counts = {state: sum(row["execution_state"] == state for row in rows)
              for state in ("deficient_baseline", "ready_for_review", "approved")}
    return (f"70 original groups; {len(rows) - 70} added groups; "
            f"{counts['deficient_baseline']} deficient baselines; "
            f"{counts['ready_for_review']} ready for review; {counts['approved']} approved")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=ROOT / CATALOG)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--require-complete", action="store_true",
                        help="Fail until every original and added group has explicit approval")
    parser.add_argument("--requirement-digest", metavar="SCENARIO_ID",
                        help="Print the current requirement digest when preparing a packet")
    args = parser.parse_args()
    try:
        catalog = read_json(args.catalog)
        errors = validate(catalog, args.root.resolve())
        if args.require_complete:
            unfinished = [row["id"] for row in catalog["rows"] if row["execution_state"] != "approved"]
            if unfinished:
                errors.append("unfinished groups: " + ", ".join(unfinished))
        if errors:
            print("\n".join(errors), file=sys.stderr)
            return 1
        if args.requirement_digest:
            row = next((r for r in catalog["rows"] if r["id"] == args.requirement_digest), None)
            require(row is not None, "unknown scenario ID")
            print(requirement_digest(row, catalog["policy"], args.root.resolve()))
        else:
            print("Visual catalog consistency: PASS; " + summary(catalog))
        return 0
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
