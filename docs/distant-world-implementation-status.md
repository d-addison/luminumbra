# Distant-world implementation and acceptance gaps

Updated October 14, 2026 for the final v0.3.0 release scope. This document
separates implemented source from deferred native qualification; it changes no
format, scenario denominator, budget or acceptance threshold. Historical
execution receipts retain their original source identities.

The [accepted distant-world contract](distant-world.md) remains the scope
authority. The five-tier volumetric ladder is the default far-world mode. The
legacy far-world renderer is retired behind compatibility settings; its roughly
3 km range and 3,200 m far plane describe the compatibility path, not the default
ladder. Generation, meshing, canonical build queues, residency, upload accounting,
renderer integration, far-water submission, fog, shadow exclusion and horizon
diagnostics are implemented. The overlay codec/writer and bake derivation are
implemented; the far-world edited-bake runtime consumer is deferred to v0.3.1.

The distant-simulation S3–S7 durability stack is implemented: indexed authority
durability; durable completed work and ground-object continuation; durable
regional fields with causal weather and scent; regional foliage and water
continuation with host persistence. S8/S9 and complete native continuation
qualification are deferred to v0.3.1.

## Dimensions and limits retained

The implemented [tier table](../src/luminumbra_common/world/FarTierTable.h)
defines the ladder consumed by the volumetric runtime.

| Tier | Sample spacing | Brick edge | Tile edge | Horizontal outer radius |
|---|---:|---:|---:|---:|
| V1 | 4 m | 16 m | 512 m | 1,024 m |
| V2 | 8 m | 32 m | 1,024 m | 2,048 m |
| V3 | 16 m | 64 m | 2,048 m | 4,096 m |
| V4 | 32 m | 128 m | 4,096 m | 8,192 m |
| V5 | 64 m | 256 m | 8,192 m | 16,384 m |

Bricks contain 5×5×5 samples. Tiles are aligned to the world origin; distance is
XZ camera-to-nearest-tile distance. Exact band radii belong to the finer tier.
The contract retains the current live disc and detail levels, procedural
streaming without a fixed world edge, and qualification within 32 km of the
origin. Floating-origin qualification beyond that extent remains deferred.
Render visibility, simulation activation and storage residency remain distinct.

Local full-SDF cave residency remains bounded at 128 m horizontally and 64 m
vertically. Volumetric far representation does not establish complete native
cave or edited-world acceptance. The ladder horizon gate in
[the frontier script](../tools/gates/validate-engine-frontier.ps1) refuses before
launch because its presets lack injected immutable authority. That gate remains
a documented v0.3.1 limitation; legacy evidence cannot qualify the ladder.

## Implementation map

DW01–DW09 are engineering work packages, not additional visual scenario IDs.
Their implementation subsets are recorded below. Complete native render and
budget qualification remains deferred to v0.3.1; none is inferred from source landing.

| Work package | Implemented v0.3.0 source | Remaining v0.3.1 obligation |
|---|---|---|
| **DW01: generation and tier identity** | Table-driven five-tier canonical volumetric generation and sparse brick identity in [FarVolumeTile](../src/luminumbra_common/world/FarVolumeTile.cpp), with [foundation fixtures](../test/fixtures/far_volume/foundation-reference.json). | Complete native coverage and cold-rebuild packets across the contract variants. |
| **DW02: vertical requests and caves** | Volumetric world generation and request infrastructure in [FarVolumeWorld](../src/luminumbra_common/world/FarVolumeWorld.cpp). | Native deep/tall/cave stations, filtered cave comparison and span-extension qualification. |
| **DW03: volumetric meshing and bounds** | Canonical all-tier meshing and render meshes in [FarVolumeMesher](../src/luminumbra_common/world/FarVolumeMesher.cpp) and [FarVolumeRenderMesh](../src/luminumbra_client/rendering/FarVolumeRenderMesh.cpp). | Complete native wall/ceiling, border, water, deep-volume and edited-world qualification. |
| **DW04: scheduling and residency identity** | Canonical queue and bounded owner residency in [FarVolumeBuildQueue](../src/luminumbra_client/rendering/FarVolumeBuildQueue.cpp) and [FarVolumeResidency](../src/luminumbra_client/rendering/FarVolumeResidency.cpp). | Complete movement, world-swap, wanted-set and pressure qualification. |
| **DW05: arrival and live ownership** | Residency/upload/render integration, valid-empty handling and water handoff infrastructure, with [residency fixtures](../test/rendering/FarVolumeResidency_test.cpp). | Composed native fallback, overlap, tier-crossing and live-edge packets. |
| **DW06: edit propagation and host baking** | Durable edit overlay and bake derivation in [FarAuthorityOverlay](../src/luminumbra_common/world/FarAuthorityOverlay.cpp). | Edited-bake runtime consumer, save/evict/reload far views and edited-world traversal producer. |
| **DW07: durable overlay and tombstones** | Overlay codec/writer and [authority fixtures](../test/common/FarAuthorityOverlay_test.cpp), with saved-world refusal coverage. | Complete native overlay/refusal/recovery qualification and runtime consumption. |
| **DW08: far rendering and independent coverage** | Default ladder, water, fog, shadow exclusion and horizon diagnostics; [independent oracle](../tools/gates/far_ladder_oracle.py) tooling. | Ladder horizon gate still refuses before launch; immutable fixture authority and complete native coverage remain required. |
| **DW09: measured residency/upload budgets** | Bounded runtime accounting, GPU/upload infrastructure and v3 measurement tools. | Calibrated native budgets, authoritative fixed views, cave-walk/edited-world producers and complete profile/display packets. |

Sub-spacing edits may appear or disappear according to sampling phase. Neither
durable overlays nor completed source fixtures waive this policy or the floor
for both Quality and Performance profiles. Publication uses the scoped tag gate
in the [acceptance record](v0.3-acceptance.md); other expanded acceptance is
deferred to v0.3.1 and is not recorded as passed.

## Qualification deferred to v0.3.1

The following cases preserve the outstanding qualification obligations. Source
fixtures and supporting tests exist for subsets; the table is an acceptance
contract, not a claim that every capture producer is available or that any
current native packet has passed. Complete render qualification is deferred to
v0.3.1, including the authoritative ground/high/cave-air poses, fixed-view
workloads, cave-walk and edited-world traversal producers, and outstanding
acceptance capture/pinning.

| Package | Required executable and native acceptance |
|---|---|
| DW01 | `FarVolumeTile` fixtures independently enumerate all five dimensions, positive/negative tile origins, exact band boundaries, shared borders, deterministic cold rebuilds and valid empty volumes. Retain the five existing `FarTierTable` cases; do not use the producer's wanted set as the oracle. |
| DW02 | Generate deep interiors below the initial span and tall requests above it; verify bounded extension and deterministic revisits. Compare V1/V2 density to the live router. Record V3–V5 aliasing/cost for both cave variants and the reviewed choice. Native stations cover long interiors, distant mouths beyond the noise-cave cutoff, arches and overhangs. |
| DW03 | All-tier mesher cases cover wall/ceiling winding, analytic crossings, water, negative coordinates and bit-identical shared borders. Frustum tests include deep cave geometry and construction above pristine maxima. Rebuild independently with different work completion orders. |
| DW04 | Wanted-set tests independently calculate every tier at each station and along a 20 km drift within the qualified extent. Delay, reorder and invalidate jobs during movement/world swaps; verify stale results cannot replace current authority. Record per-tier requested/building/ready/empty/failed states. |
| DW05 | Delay finer builds and uploads, evict under pressure, and exercise valid empty parents/children. Inspect synchronized color/depth/coverage for uninterrupted coarse fallback, actual live ownership and one-brick overlaps on ground, walls, ceilings and water. Repeat at all tier radii and the live edge. |
| DW06 | `edit_bake_all_tiers_test` uses independent sampled expectations across edit sizes and phases, including region/tile edges, removals/fills and construction above pristine maxima. Save, evict, reload and view without a live authoritative chunk. The edited-world traversal includes edit, successful snapshot commit and all-tier bake within its measured window. |
| DW07 | `far_volume_store_test` and `world_open_refusal_test` distinguish absent override from homogeneous air/solid, preserve disk bytes on refusal, and verify no far files for an unedited world. `snapshot_commit_test` interrupts every publication rename and the bake; retry must converge byte-identically, retain the prior complete generation, and reconstruct stale/missing overlays from durable chunks. |
| DW08 | `far_volume_horizon_smoke` v2 through [the frontier gate](../tools/gates/validate-engine-frontier.ps1) covers default, mountains, archipelago and cave-dense fixtures. At every station, complete independently expected per-tier coverage must settle within 90 s; below-horizon sky ≤2%, no tier seam band and no outer edge. Include valid empty tiles, cave openings, altitude fog and the 250 m cascade/V1 overlap. |
| DW09 | Six fixed views and surface-flight, cave-walk and edited-world traversals for each profile/display, with cold-cache identity and streaming transients retained. Publish measured budget proposals before enforcing caps; then rerun and prove eviction protects the horizon. Record raw frame samples and actual source/tool/asset/GPU identities. |

These packets support the original R04/R05 transition, R18 horizon, D20 mixed-LOD
and G06 cave campaign groups. Each still requires its own composed result and
visual approval; technical pass counts do not approve a scenario.

## Performance and evidence boundary

The existing [v3 benchmark](render-benchmark.md) records frame distributions,
originating-frame GPU joins, real adapter strings and explicit workload/profile
identities. The default remains v2. The surface-flight fixture exists; the cave
walk and edited-world save/bake qualification are still missing. Measurement
plumbing does not establish a 16 km frame-time result.

| Profile | Scale | Required floor, both output sizes | Reported target |
|---|---:|---|---|
| Quality | 1.0 | p99 ≤16.67 ms | p50 against 8.33 ms |
| Performance | 0.67 | p99 ≤16.67 ms | p50 and p99 against 8.33 ms |

Qualify 3840×1600 and 3440×1440 on the recorded primary GPU after rechecking the
machine. Fixed views retain 400 warmup/240 measured frames; traversals retain
the accepted 60-second scripted workloads. Keep raw samples, p50/p95/p99/max/MAD,
CPU/present/whole-frame GPU metrics and unavailable timers. Undeclared overrides,
incomplete timers, mismatched workload/adapter identity or failure to settle
invalidate a capture. The separate foliage 0.6 ms draw target remains separate.

The [September 8 terrain guard comparison](terrain-coverage-diagnostics.md)
retains its original `773f60d4b877` source, forward-depth images, diagnostic means
and zero approvals. It provides no five-tier or current-source performance
qualification.

### Historical correctness evidence

The September 13 source audit was pinned to
`32f4e69977793267ef03ccbb9b73f50ebf17dc61`, including the prefab prerequisite
through `7b7510d157af`. References to the audited source in this historical
subsection mean that revision, not the final release candidate.

The September 13 native correctness attempt used recovered source
`7244cf2a46bff63229890cc1eb743b4c64cd83b2`: 24 of 25 selected terrain cases
passed; `FarLodWorker.CameraRegionOuterMeshRetainsCavityAndEditedSurface` failed,
and the later 23 foliage cases were not run. The original XML SHA-256 is
`e684b67ceb721d8788a439ced77488bba621dd1e7ee20522b8340c8664dead86`;
raw receipts are retained outside Git. That fixture's thin roof had no solid
sample at F1's 4 m spacing. The corrected fixture at the audited source verifies
both retained air and roof samples before the unchanged mesh assertions and
passes on Linux. Preservation of unsampled thin roofs is not established.

Five additional software-rendered foliage correctness cases pass at the audited
source. Those results and the earlier native attempt do not qualify the new
source on native hardware, the five-tier runtime, the world performance matrix,
or any visual approval. Complete native qualification and the outstanding DW01–DW09 closure packets
are deferred to v0.3.1. Implemented source does not approve a visual scenario.
