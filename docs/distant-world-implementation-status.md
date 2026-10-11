# Distant-world implementation and acceptance gaps

This September 13, 2026 audit is pinned to recovered terrain source
[`32f4e6997779`](https://github.com/d-addison/luminumbra/commit/32f4e69977793267ef03ccbb9b73f50ebf17dc61),
which includes the prefab prerequisite from `devel` through `7b7510d157af`.
[The terrain recovery checkpoint](https://github.com/d-addison/luminumbra/pull/166)
awaits current-source native qualification. This document changes
no runtime, format, budget, scenario denominator or acceptance threshold.

The [accepted distant-world contract](distant-world.md) is the authority for
scope. Its earlier descriptions of current implementation are historical;
this audit records the code that now exists. The ladder declaration,
reversed-Z prerequisite and distribution-aware benchmark are implemented.
The five-tier volumetric runtime and its all-tier durable edit overlay are
unfinished. Closing an issue or passing a legacy horizon fixture cannot supply
those missing implementations.

## Dimensions and limits retained

The table in [FarTierTable.h](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_common/world/FarTierTable.h)
already declares these exact dimensions. Its only executable consumers in the
audited tree are the five [table tests](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/test/common/FarTierTable_test.cpp).

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

The running [FarLodSystem](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_client/rendering/FarLodSystem.h)
still uses F1/F2 at 4/8 m spacing, one 512 m region footprint for both tiers,
ranges of 768/3,000 m, fragment clips of 176/3,050 m and a 3,200 m camera far
plane. Its F2 authority brick is 3×3×3 samples over a 16 m chunk; it is not the
accepted V2 brick. Adding three enum values or increasing the far plane cannot
convert this representation into the accepted ladder.

## Implementation map

DW01–DW09 below are engineering work packages. They do not add visual scenario
IDs. All nine remain open; completed prerequisites are named within each row.

| Work package | Existing implementation and concrete gap | Proposed addition and dependencies |
|---|---|---|
| **DW01: generation and tier identity** | `FarLodTier`, `FarLodTile` and `BuildPristineFarLodTile` in [FarLodStore](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_common/world/FarLodStore.cpp) generate two heightfield tiers using `GetTerrainHeightAtCoarse`. The volumetric tier table is not consumed. | Introduce table-driven tier/tile identity and sparse zero-crossing 5³ brick generation for all five tile sizes. Pristine tiles remain regenerable memory caches. Preserve existing format readers until their replacement is declared and qualified. Depends on the table already present and DW02's sampling policy. |
| **DW02: vertical requests and caves** | [BuildFarLodWorkerTile](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_client/rendering/FarLodSystem.cpp) reconstructs authority columns and a transient halo using full SDF generation. An unedited distant tile remains a heightfield. There is no tile span record or camera/sightline span-extension queue. | Start each pristine tile at its surface band plus 256 m below its lowest surface. Extend upward/downward for anchors, camera, sightline requests and durable chunks, with bounded requests and stored span bounds. Reuse the live cave composition in [SHIELD_WorldSystem](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_common/systems/SHIELD_WorldSystem.cpp) unchanged at V1/V2. Measure band-limited and filtered V3–V5 variants on the cave-dense fixture before choosing; retain the recorded band-limited fallback and full-radius openings/arches/overhangs. Depends on DW01 and the representation review. |
| **DW03: volumetric meshing and bounds** | [GenerateFarLodRegionMesh](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_common/world/MarchingCubes.cpp) combines a heightfield with authoritative F1/F2 SDF columns and transient support. It validates only F1/F2. The height cache is limited to −2,048 through 2,047.9375 m, and render bounds come from those heights; arbitrary deep/tall volumes are not represented by those bounds. | Mesh sparse bricks at every accepted spacing, sharing deterministic border samples across tile boundaries. Derive render bounds from the actual requested volume/mesh, including water and edits above pristine heights. Preserve deterministic materials and normal/winding behavior. Depends on DW01–DW02. |
| **DW04: scheduling and residency identity** | `FarLodSystem::update` chooses one wanted tier per region, orders work by nearest distance, and keys pending/resident maps by region alone. Epoch and authority checks reject stale worker results. | Key all lifecycle states by tier/tile identity; compute wanted sets per tier, request required parent coverage and vertical spans, and retain stale-result/world-swap protections. Adopt normalized-distance eviction for the ladder while protecting the horizon layer. Depends on DW01–DW03; measured caps follow DW09. |
| **DW05: arrival and live ownership** | An existing region stays until its replacement uploads, but multiple nested tiers cannot coexist at that key. `integrate_completed_builds` treats every empty mesh as failure. The [coverage repair](terrain-coverage-diagnostics.md) submits the camera region while retaining a fixed 176 m exclusion, even if its live mesh is missing. | Distinguish valid empty tiles from failed/missing work. Keep coarser coverage resident and drawn beneath finer tiles with one-brick overlap; transfer ownership only after upload. Derive live-edge ownership from actually meshed/uploaded live geometry, including floors and ceilings. Prove the accepted finer-wins depth bias without introducing stitching topology. Depends on DW03–DW04; eviction must preserve fallback. |
| **DW06: edit propagation and host baking** | Captured live authoritative lattices are reduced at 4/8 m; region revisions invalidate affected neighbors. The worker can rebuild from durable lod-0 records when passed a save path. `FarLodSystem::set_save_dir` has no runtime caller. [WorldSaveService::save_dirty_chunks](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_common/persistence/WorldSaveService.cpp) writes chunks and clears dirty flags without all-tier bake obligations. | Bake every affected tier/tile on the host after durable chunk writes and before far-authority notification. Derive pending bakes from committed chunk generation versus overlay generation on save/load; a failed bake remains due without invalidating the committed chunk save. Generalize authority keys and wire world-entry save ownership. Depends on DW01–DW04 and the accepted snapshot generation/commit service. |
| **DW07: durable overlay and tombstones** | [FarLodStore](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_common/world/FarLodStore.h) stores FSD2 authority alongside chunk records in LMR1. It supports immutable snapshots, CRCs, stale-parameter handling, home-only persistence and interruption-safe region replacement, but has no FSV1 overlay generation or present/air/solid state. | Declare the accepted `<save>/far/v<tier>.<tx>.<tz>.lmr` FSV1 family before writing it: version/length/allocation/checksum/refusal rules, edit-baked bricks, homogeneous-air and homogeneous-solid tombstones, and source generation. Keep FSD2 v3 decodable. Rebuild missing/stale derived data from durable edited chunks; refuse corrupt/future data. Publish whole generations and preserve the prior complete overlay on interruption. Depends on DW06 and the snapshot integrity/refusal contract. |
| **DW08: far rendering and independent coverage** | [Camera.h](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_client/rendering/Camera.h), the [render pipeline](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_client/rendering/RenderPipeline.cpp) and G-buffer use reversed-Z/32-bit float depth, with the far plane still 3,200 m. Far water sheets exist. The [legacy horizon scenario](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/src/luminumbra_client/core/scenarios/FarLodHorizonSmoke.cpp) checks its runtime's own missing count; it does not independently derive coverage. | Adopt the 17,408 m far plane after the ladder works; enforce outer-clip composited transmittance ≤5%, including altitude fog. Keep shadow cascades ending at 250 m and add a reliable far-surface discriminator so far geometry does not receive cascaded shadows. Deliver the accepted independent per-tier horizon v2 oracle and station captures. Depends on DW03–DW05 plus the existing depth prerequisite. |
| **DW09: measured residency/upload budgets** | Runtime caps are 384 MiB of estimated tile plus GPU mesh bytes, eight dispatches and six integrations/uploads per frame. Snapshot, in-flight worker, completed CPU mesh and transient upload storage have no combined measured ladder budget. The legacy horizon fixture separately uses 128 MiB; neither number is an approved five-tier budget. | Measure per-tier bytes, first fill, build-cost histograms, queue storage, uploads, draws and frame hitches on default, mountains, cave-dense and edited worlds. Present measured per-tier limits/caps for design review, then enforce and report them. Preserve horizon fallback under pressure. Depends on DW01–DW08 and the existing v3 measurement path; no new numeric memory/upload limits are invented here. |

The work above follows [volumetric runtime #131](https://github.com/d-addison/luminumbra/issues/131),
[authority #132](https://github.com/d-addison/luminumbra/issues/132) and
[budgets #133](https://github.com/d-addison/luminumbra/issues/133).
Issue #132's older pit/ridge examples do not override the accepted sampling-phase
policy: sub-spacing edits may appear or disappear according to phase. Issue
#133's Quality-only wording does not waive the floor for Performance.

## Acceptance cases to implement

The following cases specify the missing evidence. Existing test names are linked;
new suite/scenario names retain those already recorded in the contract and are
not claimed to be available commands today.

| Package | Required executable and native acceptance |
|---|---|
| DW01 | `FarVolumeTile` fixtures independently enumerate all five dimensions, positive/negative tile origins, exact band boundaries, shared borders, deterministic cold rebuilds and valid empty volumes. Retain the five existing `FarTierTable` cases; do not use the producer's wanted set as the oracle. |
| DW02 | Generate deep interiors below the initial span and tall requests above it; verify bounded extension and deterministic revisits. Compare V1/V2 density to the live router. Record V3–V5 aliasing/cost for both cave variants and the reviewed choice. Native stations cover long interiors, distant mouths beyond the noise-cave cutoff, arches and overhangs. |
| DW03 | All-tier mesher cases cover wall/ceiling winding, analytic crossings, water, negative coordinates and bit-identical shared borders. Frustum tests include deep cave geometry and construction above pristine maxima. Rebuild independently with different work completion orders. |
| DW04 | Wanted-set tests independently calculate every tier at each station and along a 20 km drift within the qualified extent. Delay, reorder and invalidate jobs during movement/world swaps; verify stale results cannot replace current authority. Record per-tier requested/building/ready/empty/failed states. |
| DW05 | Delay finer builds and uploads, evict under pressure, and exercise valid empty parents/children. Inspect synchronized color/depth/coverage for uninterrupted coarse fallback, actual live ownership and one-brick overlaps on ground, walls, ceilings and water. Repeat at all tier radii and the live edge. |
| DW06 | `edit_bake_all_tiers_test` uses independent sampled expectations across edit sizes and phases, including region/tile edges, removals/fills and construction above pristine maxima. Save, evict, reload and view without a live authoritative chunk. The edited-world traversal includes edit, successful snapshot commit and all-tier bake within its measured window. |
| DW07 | `far_volume_store_test` and `world_open_refusal_test` distinguish absent override from homogeneous air/solid, preserve disk bytes on refusal, and verify no far files for an unedited world. `snapshot_commit_test` interrupts every publication rename and the bake; retry must converge byte-identically, retain the prior complete generation, and reconstruct stale/missing overlays from durable chunks. |
| DW08 | `far_volume_horizon_smoke` v2 through [the frontier gate](https://github.com/d-addison/luminumbra/blob/32f4e69977793267ef03ccbb9b73f50ebf17dc61/tools/gates/validate-engine-frontier.ps1) covers default, mountains, archipelago and cave-dense fixtures. At every station, complete independently expected per-tier coverage must settle within 90 s; below-horizon sky ≤2%, no tier seam band and no outer edge. Include valid empty tiles, cave openings, altitude fog and the 250 m cascade/V1 overlap. |
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
or any visual approval. Fresh native qualification and all DW01–DW09 closure
packets remain outstanding.
