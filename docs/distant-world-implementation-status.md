# Distant-world implementation and acceptance gaps

Updated October 14, 2026 for the final v0.3.0 release scope. This document
separates implemented source from deferred native qualification; it changes no
format, scenario denominator, budget or acceptance threshold. Historical
execution receipts retain their original source identities.

The [accepted distant-world contract](distant-world.md) remains the scope
authority. Ladder is the default requested far-world mode. Missing immutable authority
in normal presets still causes an explicit refusal and effective Legacy fallback
(roughly 3 km range and a 3,200 m far plane); the default request does not establish 16
km native coverage. Generation, meshing, canonical build queues, residency, upload
accounting, renderer integration, far-water submission, fog, shadow exclusion and
horizon diagnostics are implemented. Overlay codecs/writers and bake derivation are
implemented; the committed-generation edited-SDF producer and FW S8 consumer are planned
for v0.3.1.

The persisted clock, active-region ledger, edited-content digest and scheduler-fault
foundations are implemented. Indexed authority durability/rehydration, durable completed
work and ground-object continuation, durable regional fields/weather/scent/energy, and
regional foliage/water continuation with host persistence are planned for v0.3.1. Strict
distant policy and distant creatures (S8), remaining S9 closure, the
committed-generation edited-SDF producer and FW S8 consumer, the render qualification
tooling stack, acceptance-matrix completeness, the developer console, asymmetric
day/night and the calendar-relative lunar cycle, GNS/Steam admission identity, and the
MG2 E4 candidate directory are also planned for v0.3.1.

Performance work to ship regional continuation on by default is planned for v0.4.0 as
[issue #320](https://github.com/d-addison/luminumbra/issues/320).

## Dimensions and limits retained

The implemented `src/luminumbra_common/world/FarTierTable.h`
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
`tools/gates/validate-engine-frontier.ps1` refuses before
launch because its presets lack injected immutable authority. That gate remains
a documented v0.3.1 limitation; legacy evidence cannot qualify the ladder.

## Implementation map

DW01–DW09 are engineering work packages, not additional visual scenario IDs.
Their implementation subsets are recorded below. Complete native render and
budget qualification remains deferred to v0.3.1; none is inferred from source landing.

| Work package | Implemented v0.3.0 source | Remaining v0.3.1 obligation |
|---|---|---|
| **DW01: generation and tier identity** | Table-driven five-tier canonical volumetric generation and sparse brick identity in `src/luminumbra_common/world/FarVolumeTile.cpp`, with `test/fixtures/far_volume/foundation-reference.json`. | Complete native coverage and cold-rebuild packets across the contract variants. |
| **DW02: vertical requests and caves** | Volumetric world generation and request infrastructure in `src/luminumbra_common/world/FarVolumeWorld.cpp`. | Native deep/tall/cave stations, filtered cave comparison and span-extension qualification. |
| **DW03: volumetric meshing and bounds** | Canonical all-tier meshing and render meshes in `src/luminumbra_common/world/FarVolumeMesher.cpp` and `src/luminumbra_client/rendering/FarVolumeRenderMesh.cpp`. | Complete native wall/ceiling, border, water, deep-volume and edited-world qualification. |
| **DW04: scheduling and residency identity** | Canonical queue and bounded owner residency in `src/luminumbra_client/rendering/FarVolumeBuildQueue.cpp` and `src/luminumbra_client/rendering/FarVolumeResidency.cpp`. | Complete movement, world-swap, wanted-set and pressure qualification. |
| **DW05: arrival and live ownership** | Residency/upload/render integration, valid-empty handling and water handoff infrastructure, with `test/rendering/FarVolumeResidency_test.cpp`. | Composed native fallback, overlap, tier-crossing and live-edge packets. |
| **DW06: edit propagation and host baking** | Durable edit overlay and bake derivation in `src/luminumbra_common/world/FarAuthorityOverlay.cpp`. | Committed-generation edited-SDF producer and FW S8 consumer, save/evict/reload far views and edited-world traversal producer. |
| **DW07: durable overlay and tombstones** | Overlay codec/writer and `test/common/FarAuthorityOverlay_test.cpp`, with saved-world refusal coverage. | Complete native overlay/refusal/recovery qualification and runtime consumption. |
| **DW08: far rendering and independent coverage** | Default Ladder request, water/fog infrastructure, shadow exclusion and horizon diagnostics; `tools/gates/far_ladder_oracle.py` tooling. | Ladder horizon gate still refuses before launch; immutable fixture authority and complete native coverage remain required. |
| **DW09: measured residency/upload budgets** | Bounded runtime accounting, GPU/upload infrastructure and v3 measurement tools. | Calibrated native budgets, authoritative fixed views, cave-walk/edited-world producers and complete profile/display packets. |

Sub-spacing edits may appear or disappear according to sampling phase. Neither
durable overlays nor completed source fixtures waive this policy or the floor
for both Quality and Performance profiles. Publication uses the scoped tag gate
in the [acceptance record](v0.3-acceptance.md); other expanded acceptance is
deferred to v0.3.1 and is not recorded as passed.

## Ground-object notification persistence

The `luminumbra.ground_objects.v1` JSON envelope has an `objects` array and an
optional `notifications` object. Notifications contain `next_sequence` (the
unsigned event sequence cursor) and `events`, ordered by strictly increasing
sequence. Each event contains integer `tick`, signed 32-bit `lane`, unsigned
`sequence` below the cursor, and string `topic` and `payload`; supported topics
are `ground.spawned` and `ground.despawned`. Field order is canonical as listed,
with `schema`, `objects`, then `notifications` at the root and one trailing newline.
The whole file remains limited to 4 MiB, with nesting admitted by the shared
64-container JSON depth limit before parsing. Missing notifications restore an
empty queue and zero cursor, preserving older files. Empty delivery state keeps
the existing object-file bytes; empty objects with delivery state retain the file.

A missing ground-object file has two valid snapshot representations. A file
that has never been written is omitted from the index; protecting it during an
empty save does not add a member. Removing a previously indexed file retains
an explicit absent member with zero byte count and fingerprint, preserving its
member generation on repeated empty saves. Both forms load as empty objects
and delivery state, and both refuse an unexpected file. Notification-only
files remain present even when the object array is empty. These distinctions
preserve the existing fixtures and hashes for worlds without ground objects.

Saving never delivers simulation notifications. Reload restores ground events
with their original due ticks, lanes, sequences and payloads, so consumers run at
the usual end-of-tick delivery point and completed work feeds the following
schedule. Other publishers' pending events are cleared on load. A faulted save
omits notification state without invoking consumers, allowing recovery even
with future events whose delivery ticks cannot be reached. Flag-off saves do
not write this state.

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
| DW08 | `far_volume_horizon_smoke` v2 through `tools/gates/validate-engine-frontier.ps1` covers default, mountains, archipelago and cave-dense fixtures. At every station, complete independently expected per-tier coverage must settle within 90 s; below-horizon sky ≤2%, no tier seam band and no outer edge. Include valid empty tiles, cave openings, altitude fog and the 250 m cascade/V1 overlap. |
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

### Ground object authority and admission

Pending ground spawn/despawn notifications and their sequence cursor now join the
authoritative hash through the same capture used by saves and the heavy comparison
oracle. Empty notification state contributes no bytes, preserving existing empty
and disabled-world hash projections. Canonical delivery records retain their saved
sequence order and all event fields.

Spawn admission budgets queued notification bytes and the candidate's event within
the existing 4 MiB file cap. Loading applies the same combined growth budget before
replacing either output, and the save writer refuses the same over-budget state
before publishing. The envelope reserves maximum cursor width; object
reservations continue to cover settling, ageing and replacement by a despawn event.
Event byte accounting follows delivery, interrupted delivery, restore and clear.
Unknown-region work submitted by callbacks follows the scheduler's rejection path;
valid completed work still publishes at the boundary.

Regression coverage includes all notification fields, cursor-only state, canonical
ordering and save/load; byte and spawn-count admission boundaries; callback input
rejection and continued ticks; and comparison-oracle and event-byte accounting.
Clang 18 syntax checks pass for the changed translation units, and GCC builds the
standalone store/oracle suites and event-bus gate at two jobs. The 34 store/oracle
tests and the event-bus gate pass. The clone has no configured CMake build or built
engine libraries, so the headless session regressions, save-writer regression and
pinned engine hash tests remain unexecuted here. Fixture files and their pinned values are unchanged.

### Ground objects, water debts and regional energy integration

The ledger retains persistent ids, completed consumer work, sparse water debts and
energy ages together. Its optional version-2 TLVs encode in ascending order:
allocator 1, frozen projection 2, energy ages 4, water debts 5 and completed work 6.
The fixed record layout and original file cap are unchanged. Every admission path
counts all retained extensions before mutation, preserving boolean refusals for
energy/water setters and exceptions for allocator/completed-work capacity failures.
Idle retention protects both debt and completed-work owners; resume acknowledgement
and legacy frozen-projection validation remain intact. Empty ground registers add
no digest or persistence bytes.

Both CMake lists retain the independently added GroundObjectStore and WaterShare
sources/tests; neither side removes a listed file. The ledger tests retain both
sets of cases. The water malformed-envelope test now uses type 7 for its unknown
type refusal because type 6 carries completed work. Combined coverage checks all
five TLVs, descending water/completed types and atomic refusals when their combined
extensions exceed the original file cap.

Verification in this clone:

- No conflict markers under `src` or `test`; `git diff --check` passes.
- `clang-format-18` applied to the resolved C++ files; its dry-run check passes for
  all 11 C++ files changed by the merge.
- `clang++-18 -fsyntax-only` passes for 27 translation units covering direct header
  consumers and affected callers, including ActiveRegionLedger and its tests,
  GameSession, WorldSaveService, SHIELD_WorldSystem, EnergyFieldState, GroundObjectStore,
  WaterShare, ScalarFieldDiffusion, persistence tests and headless-session tests.
  Flags come from the existing compilation database with the available dependency
  include paths and source-root definition supplied for extra test consumers.
- Standalone builds at `-j2` pass 72 ledger, 6 water-share, 24 ground-store and
  59 energy/diffusion tests (161 total), using the production ledger, clock,
  persistence checksum, event bus and field implementations.
- Independently compiled devel and resolved ledgers produce identical encoded
  bytes, canonical bytes and schedule digests across 16 scenarios of 40 ticks,
  with/without energy ages and water debts, plus empty, anchor, eviction and
  restored-clock cases: 288,216 compared bytes match exactly.

There is no configured CMake build in this clone. Full engine-linked focused tests,
runtime headless/save integration and pinned engine hash tests remain unverified.
The standalone ledger executable omits only
`AllocatorOnlyLedgerIsSavedAndReloads`, which needs engine persistence linkage;
the repository test remains present and passes syntax checking.

### Save comparison and admission boundaries

The disabled ground-spawn save comparison now masks only the numeric
`creationTime` value in `world_info.json`. Session creation uses wall time without
a clock injection seam. All other metadata bytes, including formatting, and all
other files retain exact byte comparisons. The helper has one save-tree caller;
the other metadata comparisons preserve or reload an existing world's creation
time and remain strict. A regression uses different creation seconds while also
checking that changes to another field or formatting remain visible.

New ledger records now share the existing envelope budget with the allocator,
frozen projection, energy ages, water debts and completed work. Spawn admission
checks the proposed region after allocation and counts a refusal without adding
an object, region or event. The shared activation check also covers edits, pins
and scheduler visits; visits retain their capacity rejection accounting. Tests
exercise one below, at and one above the admissible record count, retained
extensions, writes to existing records and continued saveability.

The event bus now throws before publishing at an exhausted sequence cursor.
Ground spawns reject that cursor before allocating an id; despawn publication
uses the existing simulation fault latch. Loading still accepts the terminal
cursor and can save it again. Tests cover loaded `UINT64_MAX` and
`UINT64_MAX - 1`, the final publishable event, repeated refusal, pending byte
accounting, draining, save/reload and despawn faults. The sibling counter sweep
found the persistent allocator already stops at its limit, and restored ledger
and world-clock ticks remain bounded below their tick limit.

Verification in this clone:

- Eleven translation units pass `clang++-18 -fsyntax-only`, including the changed
  sources/tests and persistence, terrain-system and scripting callers.
- GCC standalone builds at `-j2` pass 73 ledger tests, 25 ground-store tests, the
  extracted metadata-masking regression and the event-bus gate. The standalone
  ledger target omits only `AllocatorOnlyLedgerIsSavedAndReloads`, which requires
  engine persistence linkage; the repository test is retained and syntax checked.
- Temporary builds with the record-budget guard or sequence guard removed fail
  the corresponding new regressions. The event-bus gate also fails without its
  sequence guard.
- Clang-tidy passes the cognitive-complexity check at 250 for all seven changed
  C++ translation units. Formatting and `git diff --check` pass.

There is no configured CMake build in this clone, only a compilation database.
The complete engine-linked headless/save tests, including the new integration
cases and original flaky test, and pinned engine hash tests remain unexecuted.
Acceptance contract documents and fixture bytes are unchanged.
