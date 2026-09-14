# Visual scenario catalog and acceptance

The [complete roster](visual-catalog-roster.md) restores all **70 original groups**:
R01–R23, U01–U10, D01–D22, B01–B08 and G01–G07. Twelve added groups cover the
expanded world and authoring commitments, with explicit mappings to original
parents. Added or split groups never replace original coverage. R01 contains all
96 summer/winter cells; 48 summer cells alone are incomplete.

There are **two deficient historical baselines, zero review-ready packets and
zero visual approvals**. The [September 8 images and receipts](visual-baselines.md)
retain their original executed source and artifact hashes. Their numeric passes,
later code fixes and this catalog restoration do not qualify improved visuals.
The other rows remain unfinished. The catalog describes intended fixtures and
current source support; it does not claim that every fixture or capture command
already exists.

The machine-readable [catalog](visual-catalog.json) contains every fixture,
required variant, source link, implementation dependency, command or explicit
capture gap, acceptance check, retained evidence and approval state. The
[original requirement snapshot](visual-catalog-original-contract.json) freezes
the restored identities, goals and minimum variants. Current rows may add proof
requirements but may not silently reduce the original contract.

## Reconciliation and execution

The initial source audit is pinned to
`945ecd295835a838eed0169ff709e7aabc134fa1`. Source reconciliation is a snapshot of
that tree, separate from the source identities of historical captures. Refresh
each row's reconciliation when implementation lands; keep old evidence intact.

Runtime selectors and all ten shipped UI documents are mapped automatically.
`forced_crash` is the explicit nonvisual selector exclusion. Lightning captures
are part of the weather selector. Waterfall has a diagnostic test and no separate
runtime selector. The UI screenshot producer retains only three default pages;
page-loading tests do not establish every required UI state. Geometry compilation
and the installed Blender service are distinct from installed target rendering.

Each family page supplies an existing command template where a retained image
producer was verified in source. `existing_subset` means that command covers only
part of the required packet and has not been rerun for this catalog. `missing`
means the complete producer or image retention still needs implementation. Empty
fields never stand for a successful capture. Build-defined test artifact roots
need isolated builds and fresh outputs; resolve actual executable names for the
chosen platform, including the Windows `.exe` suffix.

Work in capability order: terrain/caves/edits/streaming and foliage; materials,
lighting and atmospheric effects; water/weather/seasons; characters, interactions,
UI and Blender workflows. Preserve the existing regression checks. For a missing
feature, identify its concrete visible limitation, proposed addition and dependency
before implementation. Source presence and open issues alone do not decide whether
code or acceptance is complete.

## Review packets

Use original lossless screenshots and labelled still sheets in README/docs.
Motion claims require automated state or trajectory evidence with frame/tick
identities and retained ordered samples. This delivery does not require videos,
GIFs or interactive demos. Each packet contains both a deliberately composed
demonstration and its technical evidence.

For every original and added group, retain:

- A versioned reproducible fixture, required variants and exact capture command.
- Original PNG/P6 pixels, derived previews and the source-image mapping for sheets.
- Executed source, installed executable/library/tool/exporter and asset identities;
  source `.blend`, graph and registry identities where applicable.
- Frame/tick/camera/lighting/settings pins, hardware/backend/driver and actual
  Blender/exporter versions. Qualify Windows Blender 5.1.0/exporter 5.1.18 first,
  then Linux independently.
- Correctness and temporal checks, visible failures, performance scope and raw
  samples, current findings and known limitations.
- Aesthetic review of scale/grounding, material identity/detail, lighting/shadows/
  exposure, depth/environment integration, composition/color/atmosphere and
  readability/coherence. Each applicable dimension must meet the accepted quality
  bar; an inapplicable dimension needs a rationale. Numeric checks cannot approve
  aesthetics.
- An explicit user decision naming the scenario and immutable packet digest.

The accepted expanded-world workloads require **p99 ≤16.67 ms in both profiles**,
with 8.33 ms retained as the reported target. Freeze both workload/profile
identities before measurement. Report frame distributions and raw samples. The
foliage draw target remains **0.6 ms** and is separate from whole-frame performance.
Preserve existing scoped renderer targets and the
[performance comparison policy](performance.md). Historical means or a last
available GPU query do not meet distribution or source-frame attribution requirements.

Authoring targets are 30 FPS presentation, p95 camera/transform feedback below
100 ms, warm material refresh below two seconds and small-asset refresh below five
seconds, measured end to end through the installed host. Report misses explicitly.
Recheck available local hardware for every run; software rendering cannot qualify
native GPU performance. Mathematical, mock and CPU checks remain supporting evidence.

Before authoring acceptance, test cancellation, stale revisions, failed publication,
held generations, malformed IPC, stale frames, service crashes, extension unload,
undo and unknown required schemas. Prove geometry, component, graph and character
edits change behavior while installed engine executable/library hashes remain
unchanged. Keep GLB plus versioned sidecars and immutable generations; preserve
Tree Small 02's accepted packs and all 30,250 fitted leaf surfaces.

Use the first forest/canopy/LOD, shoreline/weather, menu/creation/saves and
Blender prop/plant/character panels to establish concrete quality references with
the user. These representative packets do not replace any remaining group.

## Approval and publication

Prepare and validate concrete packets before requesting visual approval. Each
group requires the user's explicit decision. A decision may name multiple IDs,
but shared batches, passing tests, silence or elapsed time never imply approval.
Changed screenshots, source identity, fixtures or required evidence invalidate
the decision for that packet; retain the previous packet as history.

README showcases require approved packets. Docs may show clearly labelled
pending, deficient and blocked records. This recovery adds no README showcase.
Keep original capture identities when editing documentation. Raw session material,
private inputs and builds stay outside the public source tree. Store compact
public receipts/originals in docs; retain allow-listed raw evidence in CI and
durable archives before CI expiry.

## File-only validation

```sh
python3 tools/ci/test_validate_visual_catalog.py
python3 tools/ci/validate_visual_catalog.py
python3 tools/ci/render_visual_catalog.py --check
```

Update the catalog JSON, then run `python3 tools/ci/render_visual_catalog.py` to
refresh its generated roster and family pages. CI checks those pages against the
catalog. The original snapshot is deliberately immutable: a changed acceptance
contract requires explicit review of both the snapshot and its validator identity.

Consistency success checks coverage and evidence bookkeeping only. It neither
runs native tools nor verifies that a recorded human decision is authentic.
The native evidence validators and human reviewer still assess the actual
correctness, aesthetics, measurement provenance and target applicability.

```sh
python3 tools/ci/validate_visual_catalog.py --require-complete
```

Completion mode fails until **every original and added group** has an explicitly
approved packet. It currently fails for all 82 groups. Do not use the ordinary
consistency pass as a completion claim.

## Packet record interface

`review_packet` is initially null. A prepared packet is a repository-relative JSON
file referenced as `{"path": "docs/assets/…/packet.json", "sha256": "…"}`. Paths
may not escape the repository or use links/private build directories. Hash every
original and supporting file. Packet fields are:

| Field | Required content |
|---|---|
| `schema`, `scenario_id` | `luminumbra.visual_review_packet.v1` and exactly one catalog ID |
| `requirement_sha256` | Digest of current fixture file contents, variants, acceptance checks, scoped performance requirements and shared policy; print with `--requirement-digest R12` |
| `fixture_files` | Exact map from each declared fixture source path to its current file SHA-256. `identities.fixture_sha256` hashes this map as sorted compact JSON |
| `source_commit` | Actual executed source commit, separate from documentation revision |
| `capture_status`, `correctness` | `native_qualified`, `passed` before review-ready state |
| `reproduction`, `camera_lighting` | Exact commands, inputs, environment and actual frame/tick/pins |
| `identities` | `engine_binary_sha256`, `fixture_sha256`, `asset_manifest_sha256`, `tool_manifest_sha256`; include installed library hashes and real tool/exporter identities in the manifests |
| `hardware` | `os`, `cpu`, `gpu`, `driver`, `backend`, `hardware_rendering: true`; include framebuffer/profile/measurement context |
| `performance` | Accepted metric scopes require `status: measured`, exact scope, complete frozen profiles and `target_status: met`. Other scopes may report `not_established`, or `not_applicable` with a reviewed rationale |
| `findings`, `aesthetic_review` | Concrete findings, limitations and per-dimension review; never infer a user decision |
| `artifacts` | Unique IDs, roles, repository paths and SHA-256 digests. Roles include `original`, `functional`, `temporal`, `raw_performance`, `performance_profile`, `capture_manifest` and `preview` |
| `variants` | Exactly every required variant ID, `status: passed` and references to its lossless `originals`; previews cannot substitute |
| `capture_manifest` | Artifact ID of a `luminumbra.visual_capture_manifest.v1` record joining each original to its actual scenario variant, run and frame |

The capture manifest repeats the packet's `scenario_id`, `source_commit`,
`engine_binary_sha256` and `fixture_sha256`. Its `captures` list contains
`variant_id`, `run_id`, nonnegative integer `frame_id`, `original` artifact ID
and `original_sha256`. It must join every variant original exactly. A source
frame or original file cannot satisfy different variants, including different
sweep cells. Independent captures may have identical pixel hashes; deterministic
rebuild comparisons rely on that possibility.

Every row declares `performance_requirements`. R24 owns the accepted expanded
world workload: two distinct frozen configurations, p99 frame time at most
16.67 ms and the separately reported 8.33 ms target. R14 owns the separate
0.6 ms foliage draw scope. B09 owns installed authoring presentation and refresh
targets. Other rows remain `report_only`; this does not waive any requirement of
their parent or dependent acceptance scope or invent a new target for them.

For a required scope, `performance.profiles` has exactly the required number of
records, each with a unique `id`, a `manifest` artifact ID and complete `metrics`.
The manifest has schema `luminumbra.visual_performance_profile.v1`, matching
`profile_id` and `scope`, a nonempty `configuration` object specifying the actual
workload/settings, and a `statistics` map from metric ID to its aggregation.
Review and freeze this profile before capture. World frame time uses p99;
camera/transform feedback uses p95. Where the accepted target did not specify
an aggregation, the profile must declare mean, median, p95, p99, min or max;
human review must establish that it represents the requested workload.

Each metric record has `id`, computed `value` and a `raw_samples` artifact ID.
World frame time also records `reported_target: 8.33`. The raw JSON record uses
schema `luminumbra.visual_performance_samples.v1`, repeats the packet's four
capture identities, and records `profile_id`, `profile_sha256`, `metric_id` and
nonempty `samples`. Each sample contains a finite nonnegative `value` and actual
`run_id`/`frame_id`; duplicate sample identities are rejected. The validator
recomputes the declared statistic (nearest-rank percentiles), checks it against
the reported value and enforces the metric's strict or inclusive bound.
Missing profiles, N/A and `not_established` cannot qualify these accepted scopes.
Retain the full distributions and workload details alongside these compact joins.

For `approved` or `changes_requested`, the row's `approval` records `actor: user`,
the reviewed `packet_sha256` and a concrete `decision_reference`. The validator
requires this evidence and rejects stale digests, missing variants and motion
packets without temporal artifacts. Human review remains responsible for validating
the referenced decision and the actual contents of every supporting receipt.
Failed or incomplete packets may be retained with `changes_requested`; those
states remain unfinished and cannot be promoted to review-ready or approved
without complete, qualified evidence.
