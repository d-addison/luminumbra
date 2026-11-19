# Terrain resolution options

Design and feasibility for issue #876. Both options below are proposals; this
document activates neither. Existing generation, saves, identities, fixtures and
settings remain unchanged.

The captured mountains case uses seed 424242, eye (8, 27.95498276, 8), and peak
target (-56, 45.23035812, 168). The target is 172.325 m away horizontally, but the
visible angular outline is foreground terrain in chunk (0, 1, 0), within about
five metres of the eye. Full authoritative SDF meshing at LOD 0, sample step 1,
reproduces corners at (6, 29.6131878, 12) and (6, 29.1669388, 11). A 65 by 65
height probe over x=[2,10], z=[8,16], spaced at 0.125 m, found rounded continuous
heights between lattice points. This supports a sampling/reconstruction cause;
it does not isolate an individual noise octave or prove a shading/LOD fault.

Source locations below refer to the implementation inspected for this design.
`WorldSystem.cpp` and `WorldSystem.h` abbreviate
`src/luminumbra_common/systems/SHIELD_WorldSystem.cpp` and its header; other
common paths are relative to `src/luminumbra_common/`. Line numbers are locators,
not contracts to pin in source tests.

## Option 2: a band-limited preset for new worlds

### Frequency and shaping controls

The base generator is Simplex followed by FractalFBm, configured by octave count,
lacunarity and gain at `WorldSystem.cpp:1277`. The scalar sample applies the base
frequency to warped world coordinates at `WorldSystem.cpp:1493` and
`WorldSystem.cpp:1513`. The integer-grid SIMD sibling is
`WorldSystem.cpp:2453`; coarse sampling is `WorldSystem.cpp:2392`; public scalar
height lookup is `WorldSystem.cpp:4849`. Any future algorithm change must keep
these paths, analytic materials, generation/promotion and far sampling coherent.

For octave i, starting at zero, nominal frequency is
`base_frequency * lacunarity^i` and relative amplitude is `persistence^i`.
FastNoise normalizes by the sum of amplitudes
(`vendor/fastnoise/include/FastNoise/Generators/Fractal.h:27`); its FBm loop
increments the seed per octave (`Fractal.inl:25` in that directory). These are
noise feature scales, not a strict Fourier cutoff: Simplex, domain warping and
piecewise shaping can retain or create higher frequencies.

All current preset files are under `worlds/atlas/presets/`. Mountains base
controls are at lines 8–12 and shaping is at lines 15–23. The corresponding
base-control ranges are amplified.json:9–13, temperate_forest.json:7–11 and
all other presets:8–12. Continental/erosion/peaks spline definitions are
amplified.json:22/23/24, archipelago.json:23/24/25, default.json:22/44/66,
temperate_forest.json:20/21/22 and caverns.json, cave_dense.json and
flat_lands.json:21/22/23. Inspect their neighboring shaping frequencies and
amplitudes when extending the mountains candidate to another envelope.

| Preset | Octaves | Base frequency | Gain / lacunarity | Highest two octaves: index, frequency in cycles/m |
|---|---:|---:|---|---|
| mountains | 6 | 0.008 | 0.5 / 2 | 4: 0.128; 5: 0.256 |
| default | 6 | 0.010 | 0.5 / 2 | 4: 0.160; 5: 0.320 |
| amplified | 6 | 0.0095 | 0.5 / 2 | 4: 0.152; 5: 0.304 |
| temperate_forest | 6 | 0.008 | 0.5 / 2 | 4: 0.128; 5: 0.256 |
| caverns, cave_dense | 6 | 0.009 | 0.5 / 2 | 4: 0.144; 5: 0.288 |
| flat_lands | 5 | 0.006 | 0.5 / 2 | 3: 0.048; 4: 0.096 |
| archipelago | 4 | 0.008 | 0.42 / 2.1 | 2: 0.03528; 3: 0.074088 |

Mountains octaves 0–5 have nominal wavelengths 125, 62.5, 31.25, 15.625,
7.8125 and 3.90625 m; weights are 1, 1/2, 1/4, 1/8, 1/16 and 1/32 before
normalization. Even the last nominal wavelength exceeds the 2 m Nyquist limit;
that alone cannot guarantee a smooth near-camera polygon silhouette.

| Control and current mountains value | Source | Effect |
|---|---|---|
| base amplitude 70 m; height offset 10 m | `worlds/atlas/presets/mountains.json:9` | Amplitude scales detail; offset translates it. |
| continentalness frequency 0.0008; continental spline `[[-1,-6],[-0.2,4],[0.3,8],[0.65,27.5],[1,50]]` | `worlds/atlas/presets/mountains.json:15`, `:21` | Regional elevation; changing segment slopes/knees changes curvature. |
| erosion frequency 0.0015; erosion spline `[[-1,0.5],[-0.6,0.36],[-0.35,0.25],[0,0.18],[0.4,0.13],[1,0.1]]` | `worlds/atlas/presets/mountains.json:16`, `:22` | Multiplies the base detail; low erosion preserves more detail. |
| peaks frequency 0.0025, amplitude 52 m; peaks spline `[[-1,0],[0.35,0.08],[0.72,0.5],[1,1]]` | `worlds/atlas/presets/mountains.json:17`, `:23` | Ridge term, additionally multiplied by `1 - clamp((erosion+1)/2)`. |
| domain warp amplitude 20 m, frequency 0.003 | `worlds/atlas/presets/mountains.json:19` | Displaces base and peaks sampling; can compress local feature spacing. |
| spline interpolation and combination | `WorldSystem.cpp:1453`, `WorldSystem.cpp:1534` | Piecewise linear interpolation has derivative jumps at knots; erosion modulates detail and temperature modulates ridges. |
| defaults and JSON transfer | `WorldSystem.h:53`, `WorldSystem.h:111`; `world/TerrainPresetLoader.cpp:279`, `:655`, `:678` | Missing controls resolve to compiled defaults; the loader copies authored splines to TerrainGenParams. |

Island smoothstep, cliffs, river/lake carving, biome relief, 8 m hydro relief,
caves, surface breaks and structures remain independent contributors
(`WorldSystem.cpp:1570`, `:1580`, `:1602`, `:2080`, `:2130`, `:5830`;
`worlds/atlas/presets/mountains.json:25`, `:38`, `:44`). A reduced base spectrum
cannot promise to remove corners caused by those features. Do not silently
smooth authored cliffs, edits or cave openings.

### Concrete first candidate and versioning decision

Introduce a distinct opt-in `mountains_band_limited` new-world preset, leaving
`mountains` and all saved presets intact. First candidate: `octaves=4` (zero
weights for octaves 4 and 5), retain frequency/gain/lacunarity, and use
`base_amplitude=66.6666667`. The amplitude compensates for the new FBm
normalization, preserving the first four octave amplitudes approximately at
binary32 precision. This uses existing parameters and needs no generator fork.

Proposed shaping comparison, to qualify rather than assume visually superior:

- Continental spline: `[[-1,-6],[-0.2,4],[0.3,8],[0.65,25],[1,44]]`.
- Erosion spline: `[[-1,0.44],[-0.6,0.34],[-0.35,0.26],[0,0.20],[0.4,0.15],[1,0.12]]`.
- Peaks spline: `[[-1,0],[0.35,0.10],[0.72,0.48],[1,0.85]]`.

Keep control-channel frequencies, warp and all other features fixed for the
first comparison. Run the octave-only candidate before the spline candidate
to separate causes. The proposed knots reduce the upper peaks segment slope
from 1.786 to 1.321 and reduce extreme regional elevation; they still have
piecewise linear knees. Judge mountain character, traversable slopes, coastline,
spawn safety and hydrology together. Do not apply a uniform two-octave deletion
to archipelago or flat_lands: their starting envelopes differ substantially.

Use the complete resolved preset as the versioned content, not its display name.
Creation resolves `TerrainPresetUse::NewWorld` before persisting `preset.json`
(`world/GameSession.cpp:1715`, `:1733`, `:1800`); loading uses saved preset bytes
(`world/GameSession.cpp:2003`). Existing `surface_sampling_revision=2` means
the current sampling algorithm, not octave tuning. Keep it at 2 for this
data-only candidate. Omitted revisions on historical loads retain historical
semantics (`world/TerrainPresetLoader.cpp:436`); never re-run the new-world
selection on load or substitute the latest catalog entry for saved content.
Saved dependencies must remain available with identical content; otherwise
refuse compatibility rather than generate different unexplored chunks.
That last condition is a preservation requirement, not a claim that ordinary
save loading already pins every dependency: current load reconstructs its
dependency snapshot from runtime data (`world/GameSession.cpp:2003`), while
the saved `presetIdentity` covers preset JSON only. Keep runtime dependency
assets and historical generator defaults unchanged in the data-only slices.
Supporting dependency upgrades would need separately versioned persisted
dependency identity or immutable dependency assets and matching admission;
the peer/replay content hash alone cannot guarantee old-save regeneration.

**No kAuthorityRulesRevision bump is required for this existing-parameter,
additional-preset approach.** Current revision is 7
(`core/SystemConfig.h:119`). The rules and canonical encoding are unchanged;
the new resolved content already distinguishes sessions. Changing the default
catalog selection requires explicit new-world UI/CLI behavior and evidence,
but does not rewrite saved worlds. If subsequent work introduces per-octave
weights, a new interpolation algorithm or new omitted-field defaults, version
the preset schema and generator selector and increment the then-current
authority revision on activation. Do not add an ignored field under schema 8:
older readers could accept it while generating different terrain. Validate
unknown selectors before any directory creation or save publication.

### Identity and fixture consequences

| Layer | Existing path and consequence of the new preset |
|---|---|
| Resolved content identity | `world/TerrainPresetLoader.cpp:55`, `:459`, `:843`: FNV-1a64 over compact ASCII JSON containing the canonical effective preset and a sorted dependency manifest. Entries bind logical relative paths and hashes of exact consumed file bytes. Parameters and splines therefore affect `preset_content_hash`; a preset name alone is insufficient. |
| Effective rules identity | `core/SystemConfig.cpp:718`, `:778`; `world/GameSession.cpp:624`: simulation tuning/calendar/authority revision enter SystemConfigIdentity, separately from world content. The new data-only preset changes the content hash, not this rules hash. A rules revision bump changes the rules identity even for disabled gates. |
| Save integrity | `world/GameSession.cpp:3773`, `:3886`: `presetFormat` and `presetIdentity` bind saved canonical preset JSON via StableChecksum. This is distinct from dependency-inclusive `preset_content_hash`. Region payloads/manifests bind their own bytes; a world with new generated/edited state gets new payload checksums. |
| State and subhashes | `persistence/WorldPersistenceRoundtrip.cpp:1898`, `:1991`, `:2001`: sorted authoritative state projects SDF, heightmap and material fields; render mesh bytes are excluded. Generated residency is not universally terrain authority: tests must distinguish a content-identity change from an edited-region digest change. A changed preset need not change every empty-world subhash, but generated samples, collision and subsequent simulation can differ. |
| Peer admission | `net/LockstepSession.h:105`, `net/LockstepSession.cpp:1221`; `net/ReplicationAdmission.cpp:136`: nonempty matching content and rules identities are required before admission. Exercise loopback, TCP/POSIX, TCP/Windows, GNS and Steam through the shared admission paths. |
| Replay | `replay/ReplayStream.h:76`, `:100`: headers carry both identities; compatibility precedes stepping, then start/checkpoint hashes verify state. The preset-name hash is not a content hash. Old name-only recordings cannot be made compatible by repinning checkpoints. |

For the proposed additional preset, add new expected canonical-preset/content
hashes and terrain sample/visual fixtures; **no existing numerical pin should
be replaced**. In particular preserve `worlds/atlas/presets-rev6/sha256.json`,
`test/fixtures/saves/revision6-hills/`, existing preset hashes in
`test/common/TerrainPresetLoader_test.cpp`, and the explicit synthetic
DefaultShapingHeightFixtures at
`test/shield/test_worldgen_layer_snapshots.cpp:1365`. Those synthetic mountains
parameters (120 m amplitude, gain 0.65, lacunarity 2.2) are not the shipping
mountains preset (70 m, 0.5, 2). The current archipelago pin at line 1473 also
stays unchanged. Extend the catalog-set assertion in
`tools/test_frozen_terrain_presets.py:26` deliberately: new shipping entries
must not require fabricated historical revision-6 counterparts.

New-preset fixtures should cover scalar/batch/coarse/material parity
(`test/shield/height_sampling_equivalence_test.cpp`), loader refusal and
dependency changes (`test/common/TerrainPresetLoader_test.cpp`), saved-preset
roundtrip, peer mismatch and replay mismatch (`test/common/ReplayStream_test.cpp`).
New-world player-view and terrain-profile captures need new baseline images and
sample/checkpoint hashes; old captures retain their original preset and pins.
If the catalog default is later switched, requalify only fixtures which resolve
that default instead of explicit old content, including server/replay boot and
save manifests. Do not invent replacement hashes in this design-only change.

### Expected visual effect and the dense-probe gate

For gain 0.5/lacunarity 2, the removed octaves are 4.762% of the normalized
amplitude sum, 33.333% of the unnormalized slope-weight sum `sum(a*f)`, and
76.190% of the curvature-weight sum `sum(a*f*f)`. With the proposed amplitude
compensation these latter two are also the relative reductions in the base
component's derivative-envelope proxies, ignoring warp derivatives. They are
analytical estimates, **not measured facet reductions**. Without compensation,
FBm renormalization boosts retained weights by 5%, yielding slope/curvature
proxy reductions of 30%/75%. Erosion, warp, other features and camera projection
prevent translating either estimate into a percentage of visible corners.

The earlier 0.125 m probe establishes only the old terrain's rounded shape.
No new candidate height sampling or native executable was run for this document;
there is no measured captured-pose facet reduction to report. Reproducing
FastNoise/SIMD/hydro behavior in Python would not establish native parity.
A new common-only executable or modification of the earlier fixed-parameter
probe would require native compilation, outside this task's no-build rule.
The repository timing probe is not a substitute: its CLI exposes JSON output,
chunk count and smoke mode (`test/diagnostics/worldgen_probe.cpp:172`), and its
workload fixes the default preset (`:406`) rather than emitting this candidate's
dense height grid.

Before selection, run a common-only diagnostic against matching source/library
versions: load the saved resolved mountains preset and seed, verify the two
corner heights above at float precision, then sample each candidate on the same
65 by 65 grid at 0.125 m. For every dense sample compare height against the
piecewise planar reconstruction from its 1 m cell using a fixed diagonal; report
RMS and maximum vertical residual and adjacent-facet normal-angle distribution.
Also project the actual step-1 Marching Cubes mesh from the fixed captured eye
and target and report silhouette slope jumps and silhouette displacement in
pixels. The fixed-diagonal height proxy is not an exact Marching Cubes result.
Publish old/new values and `100*(old-new)/old` for each metric, with zero
denominators reported explicitly. Use the same world-space patch even if new
terrain makes the old eye underground; evaluate a separately labelled
ground-adjusted eye for playability. Include octave-only, spline-only and
combined variants, seams and edited/cave refusal cases. Native visual and
collision qualification remains pending.

### Delivery slices (each at most ten files)

Paths below are proposed implementation scope, including test registration and
inventory changes; no slice activates an unqualified default. New paths are
proposals. Split further if additional registration files are needed.

| Slice | Files (maximum count) | Exit evidence |
|---|---|---|
| Opt-in content and loader compatibility | `worlds/atlas/presets/mountains_band_limited.json`; `tools/test_frozen_terrain_presets.py`; `config/public-asset-inventory.json`; `test/common/TerrainPresetLoader_test.cpp`; `test/shield/test_worldgen_layer_snapshots.cpp`; `docs/development/terrain-resolution-options.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | Old content/sample pins unchanged, new content hash and heights pinned; public asset inventory includes new preset and package staging is checked. |
| Dense comparison diagnostic | `test/diagnostics/terrain_resolution_probe.cpp`; `CMakeLists.txt`; `test/shield/height_sampling_equivalence_test.cpp`; `docs/development/terrain-resolution-options.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (6) | Bounded 4,225 samples per variant; captured-pose residuals, mesh silhouette and seam parity recorded. |
| Persistence, admission and replay qualification | `test/common/TerrainPresetLoader_test.cpp`; `test/common/ReplayStream_test.cpp`; `test/common/LockstepSession_test.cpp`; `test/common/ReplicationAdmission_test.cpp`; `docs/development/terrain-resolution-options.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (7) | Saved old world generates identically; content mismatch refuses before join/playback. Confirm existing test filenames when implementing. |
| Expose opt-in preset and qualify visuals | `data/ui/world_creation.rml`; `config/public-asset-inventory.json`; `src/luminumbra_client/ui/Rml_UIManager.cpp`; `src/luminumbra_client/ui/components/game/WorldList.cpp`; `src/luminumbra_server/ServerWorldRunner.cpp`; `src/luminumbra_client/core/scenarios/PlayerViewSmoke.cpp`; `test/ui/worldgen_preview_test.cpp`; `docs/development/terrain-resolution-options.md`; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | Selector at world_creation.rml:75 and its reader at Rml_UIManager.cpp:686 expose the preset; WorldList.cpp:515 supplies display names. Old selection preserved; visuals/perf reviewed before any default switch. |

## Option 3: a 0.5 m authoritative lattice

### Representation and resource arithmetic

`include/luminumbra/core/Types.h:46` defines 16 by 16 by 16 chunk dimensions and
CHUNK_VOLUME=4096. There is no independent authoritative cell-width constant:
many consumers treat an integer lattice coordinate as metres. Keep **16 m
physical chunk edges**, introduce explicit lattice cells/spacing, and use 32
cells per axis. Merely changing CHUNK_SIZE to 32 doubles physical world units in
existing coordinate paths. Keeping 16 cells while shrinking chunks to 8 m would
instead multiply chunk count by about eight for the same volume, halve the
metre reach of existing chunk radii, and change chunk IDs/region membership.
Do not combine that alternative with this cost model.

Raw binary32 arrays, excluding vector capacity, object/allocator overhead,
water, worker copies, collision and meshes:

| Quantity per 16 m chunk | 1 m | 0.5 m | Ratio |
|---|---:|---:|---:|
| Cells | 4,096 | 32,768 | 8 |
| Padded SDF samples | 17³ = 4,913 | 33³ = 35,937 | 7.314 |
| SDF bytes | 19,652 | 143,748 | 7.314 |
| Heightmap bytes | 1,156 | 4,356 | 3.768 |
| Optional u8 structure material bytes | 4,913 | 35,937 | 7.314 |
| SDF + heightmap + allocated material | 25,721 | 184,041 | 7.155 |
| 8,192 such full chunks | 200.945 MiB | 1,437.820 MiB | 7.155 |

Arrays and lazy materials are declared at `world/Chunk.h:406`; generation sizes
them at `WorldSystem.cpp:5907`. Coarse chunks may carry only heightmaps, so
8,192 full chunks is a conservative resident-lattice scenario, not current
measured memory. Generation/cave/SDF cell work tends toward 8x; base height
evaluation tends toward 3.77x for a padded surface. Hashing/copying raw SDF
approaches 7.31x; JSON serialization and compression have separate overhead
and need measurement rather than a raw-byte prediction.

Streaming currently caps 8,192 active chunks and activates every four frames
(`WorldSystem.cpp:42`); work budget is 320 equivalent 4,096-cell chunks, with
at most 16 collision builds per frame. For 32³ cells, the same cell budget is
40 chunks, not 320. Leaving CHUNK_VOLUME at its physical-volume value would
fail to reduce work. Batch FIFO depths and eight-tick activation latency
(`WorldSystem.cpp:78`) are deterministic; slower workers may stall at the due
tick. Byte and cell admission must bound actual work before dispatch without
changing publication order according to wall-clock timing.

The 256 KiB meshing input cap at `WorldSystem.h:721` is particularly relevant.
Its conservative formula (`WorldSystem.cpp:67`) includes material and a maximum
32² water grid (20,480 bytes): 46,201 bytes today versus 204,521 bytes at 0.5 m,
or five versus one complete worst-case snapshot per cap. This does not include
output meshes, job scratch or pending/live duplication. The new snapshot fits,
but substantially reduces throughput under the same cap. Do not raise the cap
as a substitute for an admitted-work design.

Keep each budget's ownership distinct:

| Limit/source | Consequence |
|---|---|
| Parked chunks: 256 MiB, `WorldSystem.h:1267` | At most 1,458 raw lattice+material bundles at 0.5 m versus 10,436 at 1 m, before all other charged data; actual admissible counts are lower. |
| Example 96 MiB lattice allowance | Fits 700 SDF-only half-metre chunks versus 5,122 today; including height/material, 546 versus 3,913. This is capacity arithmetic, not a new or existing terrain-SDF budget. |
| Existing 96 MiB trace/undo caps, `replay/RegionScheduleTrace.cpp:17`, `persistence/WorldSaveService.cpp:7083` | Bound diagnostic replay companion bytes, not terrain arrays. They do not grow automatically with terrain resolution. The terrain texture comment at `src/luminumbra_client/rendering/RenderPipeline.cpp:4904` also refers to a different residency owner. |
| Legacy far residency: 384 MiB, `src/luminumbra_client/rendering/FarLodSystem.h:64` | Holds tile payload plus GPU meshes. Preserve metre-scale far strides; denser edited-SDF captures can exhaust input budgets sooner. |
| Far-volume charged allocations, `world/FarVolumeMemory.h:57`, `:263` | Account capacities and simultaneous old/new buffers; failed admission leaves outputs unchanged. Retain hard limits and refuse/defer work in smaller pages. |
| Persistence, `persistence/SavedInputLimits.h:59` | 64 MiB decoded record, 512 MiB region writer work, 1 GiB reader work, bounded compressed aggregate. JSON floats/material arrays count as encoded bytes; admit before serialization/decompression. A 184,041-byte raw bundle is not its JSON record size. |

Normal meshing of a smooth single surface grows roughly with area: a flat
16 m square is about 512 triangles at 1 m versus 2,048 at 0.5 m. For the captured
49 surface chunks that illustrative count is 25,088 versus 100,352, not a
measured capture triangle count. Standard Marching Cubes can emit up to five
triangles per cube: 20,480 versus 163,840 per full chunk before transition/skirt
geometry. At 28 bytes per VoxelVertex plus u32 indices (`world/Chunk.h:58`),
unshared three-vertex triangles cost 96 bytes each: 1,966,080 versus 15,728,640
bytes at those bounds. Confirm sizeof/alignment on each supported build and
charge capacities, pending/live buffers, gradients, edge caches and GPU pools.
Existing mesh pooling can reduce duplication; it cannot make the 8x pathological
case a 4x guarantee. Finer sampling still produces planar facets; for a smooth
height function the interpolation-error scale is O(h²), suggesting about 75%
less error at half spacing, without guaranteeing the captured silhouette.

### Consumer and sibling sweep

Search scope: `rg -n 'CHUNK_SIZE_[XYZ]|CHUNK_VOLUME' src include test` plus
`cell_size|sample_step|sdf_data|heightmap_data|floor|surface_sampling_revision`.
No single VOXEL_SIZE/CELL_SIZE constant controls this terrain lattice. The table
covers all production files returned by the dimension/volume search and the
additional indirect contracts. These are audit obligations, not changes made
by this document. Shared code applies on Windows and POSIX; backend-specific
entry points must be qualified together.

| Consumers (file:line or file groups) | Required semantics at 0.5 m |
|---|---|
| `include/luminumbra/core/Types.h:46`; `world/Chunk.cpp:101`, `world/Chunk.h:406` | Separate metres, cell counts and sample counts. Keep 16 m chunk IDs/extents and signed floor division. Snapshot shape, lazy material and authority provenance must use the versioned lattice. |
| `WorldSystem.cpp:2392`, `:2453`, `:5907`, `:6186`, `:6713`; `world/WorldStreamingState.*` | Convert world positions to lattice indices explicitly, including negative coordinates and shared borders; scalar/SIMD/coarse/promotion paths agree. Bound dense counts, seed caches, wanted sets and meshing copies. |
| `world/MarchingCubes.cpp:203`, `:406`, `:1643` | Scale lattice positions and normals to metres; update density/material indexing, full-SDF admission, step semantics, heightfield fallback, transition faces, skirts and neighbor halos together. LOD steps 1/2/4 become 0.5/1/2 m if left as cell strides; decide explicitly how existing metre footprints are retained outside the near ring. |
| Collision: `systems/PhysicsSystem.cpp:367`, `:407`; `WorldSystem.cpp:4813` | Current terrain collider is a Jolt heightfield, not the render triangle mesh. Give its 33 by 33 samples 0.5 m horizontal spacing; preserve vertical density units and 16 m origins. Requalify slope, grounding, contacts and edited terrain; half-metre SDF alone does not add volumetric cave collision. |
| Pathing: `ai/AStarGrid.h:3`; `ai/InstinctLocomotionSystem.cpp:134`; `world/GameSession.cpp:4882` | A* is a standalone caller-supplied grid, not a wired 1 m terrain navmesh. Locomotion spatial cells and the 1 m scent field are independent grids. Keep their world-unit behavior; audit terrain-height/collision callers rather than doubling every AI grid. |
| Water: `systems/WaterSystem.cpp`, `systems/WaterSystem.h:107`, `systems/RegionalWater.cpp`; `core/WaterSeedInputs.h` | Keep 16 m footprint and independent 4/8/16/32 water resolutions. High already has 1 m cells and Ultra 0.5 m. Update terrain-bed sampling, edit displacement and neighbor mapping, not water cell count by implication. Preserve 4,096-cell scheduling budget and fixed-point volume conservation. |
| Edits: `WorldSystem.cpp:5278`, `:5330`, `:5830`; `world/GameSession.cpp` | Brush radius/distance remain metres; iterate lattice indices at 0.5 m, including duplicated border samples and negative positions. Rework structure voxel stamping/materials, loaded-authority preservation, water coupling and bounded synchronous remesh preflight. A same-radius brush visits about 8x cells. |
| Saves/hashes: `persistence/WorldSaveEncoding.h:281`; `persistence/WorldSaveService.cpp:3294`, `:8567`; `persistence/WorldPersistenceRoundtrip.cpp:879`, `:1991`; `persistence/SyntheticSaveFixture.cpp` | Update exact array-size admission, serialization/readers, byte budgets and deterministic sample order together. Preserve provenance and hash all authoritative resolution selectors. Synthetic save and corrupt-shape fixtures must exercise both supported layouts. |
| Replay: `replay/ReplayStream.h:76`, `:100`; `src/luminumbra_server/modes/Replay.cpp`; `core/SystemConfig.cpp:718` | Reject incompatible grid identity before ticks; revised header layout needs a new LREC version if fields are added. Existing headers can bind a selector through resolved preset content, but rules must also change. Keep historical playback pinned to historical rules. |
| Networking: `net/ReplicationEndpoint.h:267`; `net/LockstepSession.cpp:1221`; `net/ReplicationAdmission.cpp:136` | AOI chunk edge stays 16,000 mm. Update shared identity/refusal paths for lockstep and replication, keeping mm positions unchanged. Audit `net/NetSocketsTransport.*` (Windows/POSIX), `net/GnsTransport.*`, `net/SteamNetworkingTransport.*`, loopback, and server modes `Gns.cpp`, `NetRuntime.cpp`, `NetSoak.cpp`, `NetTcp.cpp`, `Replicate.cpp`, `Steam.cpp`; do not put grid policy inside transports. |
| Far authority: `world/FarLodStore.cpp`, `world/FarLodStore.h`; `world/FarWorldAuthorityProducer.cpp`; `world/FarAuthorityOverlay.*`, `world/FarEditedBake.*`; `world/FarVolumeWorld.cpp` | Update immutable edited-SDF snapshot shape/index interpolation, halo copies, capture freshness, producer identity and cache keys. Preserve far region/metre stride and brick layout unless separately versioned. Rebuild derived tiles; do not discard edit authority. |
| Client far/ownership: `src/luminumbra_client/rendering/FarLodSystem.cpp`, `FarWorldOwnership.cpp` | Qualify Legacy and Ladder, edited and pristine, positive/negative seams, live/far overlap/exclusion and LOD transition steps in metres. Finer local sampling does not require making far 16 km coverage uniformly 0.5 m. |
| Client render consumers, under `src/luminumbra_client/rendering/` | `FoliageGroundMesh.h`, `RenderPipeline.cpp`, `WorldLoadingVisualizer.cpp`, `passes/FoliagePass.cpp`, `passes/HierarchicalCuller.cpp`, `passes/LightingPass.cpp`: retain 16 m bounds, anchors and world-space sampling; change only lattice-specific assumptions. |
| Boot/preview/capture consumers | `src/luminumbra_common/world/GameSession.cpp`; `src/luminumbra_server/ServerWorldRunner.cpp`; `src/luminumbra_client/main_client.cpp`, `world/WorldgenPreview.cpp`, `core/ScenarioRunnerDrive.cpp`, `core/scenarios/PersistenceRoundtrip.cpp`, `TimeOfDaySeasonSweep.cpp`, `WaterVisual.cpp`, `WorldVisualSweep.cpp`: select the same versioned preset/grid, preserve physical camera distances, and expose accurate dimensions in diagnostics. |

### Identity, save format and existing-world policy

Activation **requires incrementing kAuthorityRulesRevision** from the
then-current value (7 today); sampling changes collision, edits, water-bed inputs
and hashes. Also bind an explicit supported grid selector to resolved preset
content, with missing historical selector meaning 1 m. Introduce a new preset
schema for that selector so schema-8 readers refuse it. Keep selector-to-spacing
mapping canonical (for example an integer resolution revision), rather than
platform-dependent floating-point text. Content identity, session rules, hello,
replay compatibility and saved metadata must describe the same choice.

Current LMR1 container v2, indexed metadata v3
(`persistence/WorldSaveService.h:102`), chunk array admission and world manifest
do not distinguish these lattices. Allocate a new supported container/payload
and manifest identity for half-metre saves; account for the existing indexed
metadata version instead of blindly reusing 3. Update writers, readers, version
dispatch, exact sizes, checksums and backward/future refusal tests together.
Never write 33³ samples into an old-format record. The saved preset checksum
alone does not make an old reader understand new array sizes. FSD2 far tiles and
FSV1 edited bricks need either versioned identity binding and explicit cache
invalidation or a payload version bump if their encoded interpretation changes;
reusing a stale tile with the same seed is unsafe.

Recommended migration is **parallel world versions, no in-place conversion**.
Retain a 1 m runtime path for current-format existing worlds and their unexplored
chunks; new 0.5 m worlds opt in. New executables must preserve the old rules
identity through an explicit supported historical encoder/runtime pairing,
not merely load old bytes and run revision-new simulation. If maintaining that
runtime is unaffordable, ship half-metre worlds in an explicit incompatible
release and refuse old worlds without modifying them. Neither choice may imply
that arbitrary old replays work under new rules.

A later optional offline copy conversion must be a separately qualified tool:
leave the original untouched; validate all metadata, dependencies and bounded
region sizes first; regenerate pristine areas from the pinned old generator;
resample edited SDF/materials, preserve water/ECS/clock and rebuild collision/far
caches. Trilinear upsampling preserves old lattice values but creates no missing
detail and changes reconstruction/contact behavior. Regenerating edited areas
from continuous noise can erase edits. Converted worlds need new identity and
new checkpoint baselines and cannot claim bit-identical continuation. Stage all
payloads before publishing metadata/manifest last, with interruption/refusal
tests. Do not mix 1 m and 0.5 m chunks in one world under an implicit selector.

### Performance gate and decision

The release target is **120 fps, p99 frame time ≤8.33 ms on High and Ultra** on
the reference PC (`docs/roadmap.md:257`). The current distant-world capture
contract's 16.67 ms floor and report-only 8.33 ms target
(`docs/distant-world.md:595`) are weaker evidence; passing them cannot qualify
this option for the release bar.

No native performance result is supplied here. Do not assume eight cores hide
8x generation: due-tick stalls, main-thread snapshot admission, collision
publication, upload bandwidth and increased triangle work still compete with
the frame budget. If a measured affected stage costs T ms today, volumetric work
may add about 7T and surface work about 3T under these scaling assumptions;
those estimates are not additive if stages overlap. Require paired old/new
High and Ultra captures at identical physical radii, seeds, poses and camera
paths, covering cold load, steady traversal, fast travel, caves, edits, water,
multi-anchor streaming and both far renderers. Report CPU/GPU p50/p95/p99,
streaming latency/due-tick stalls, admitted/refused bytes, total peak residency,
triangle/upload/collision counts and deterministic run/replay hashes. Include
Windows/MSVC, UCRT64 and Linux parity; do not relax caps or gate thresholds.

**Go for a bounded, opt-in feasibility experiment; no-go for a shipping default
or in-place migration now.** Progress only if the exact lattice and snapshot
budgets fit, all old-world paths remain stable or explicitly refuse, and paired
native qualification meets the 8.33 ms p99 bar on both presets. The expected
cost is disproportionate for a near-camera presentation defect alone.

### Delivery slices (each at most ten files)

Preparatory helpers remain dormant until the activation slice. Every slice
includes its registrations within the stated count; expand into more slices
instead of exceeding ten files. Test files named below are proposed if absent.

| Slice | Files (maximum count) | Exit evidence |
|---|---|---|
| Grid descriptor and arithmetic, dormant | `include/luminumbra/core/TerrainGrid.h`; `test/common/TerrainGrid_test.cpp`; `CMakeLists.txt`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (6) | Checked products/conversions, negative borders, extremes and unsupported selectors; old runtime unchanged. |
| Versioned preset admission, dormant | `world/TerrainPresetLoader.h`, `.cpp`; `WorldSystem.h`; `test/common/TerrainPresetLoader_test.cpp`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (7) | Historical omission preserved; unknown/nonfinite/cross-field inputs refuse before writes; descriptor not yet selected in shipping sessions. |
| Versioned chunk encoding, dormant | `world/Chunk.h`, `.cpp`; `persistence/WorldSaveEncoding.h`; `persistence/WorldPersistenceRoundtrip.cpp`; `test/persistence/terrain_grid_encoding_test.cpp`; `CMakeLists.txt`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (9) | Dual-layout array/order/hash tests, malformed/partial inputs, no changed old bytes. |
| Container and manifest admission, dormant | `persistence/WorldSaveService.h`, `.cpp`; `persistence/SyntheticSaveFixture.cpp`; `test/persistence/terrain_grid_save_test.cpp`; `CMakeLists.txt`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | Bidirectional refusal, bounded records/regions, interruption recovery and unchanged source world. |
| Generation/edits behind descriptor | `WorldSystem.h`, `.cpp`; `test/shield/height_sampling_equivalence_test.cpp`; `test/shield/terrain_grid_edits_test.cpp`; `CMakeLists.txt`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | Scalar/SIMD/coarse/promotion/material parity; old samples identical; edit borders and allocation refusal transactional. |
| Meshing and collision | `world/MarchingCubes.h`, `.cpp`; `systems/PhysicsSystem.cpp`; `test/shield/terrain_grid_mesh_test.cpp`; `test/sim/terrain_grid_collision_test.cpp`; `CMakeLists.txt`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (9) | Metre bounds, seams, malformed SDF, terrain contacts and cave limitations established. |
| Water and seed mapping | `systems/WaterSystem.cpp`; `systems/RegionalWater.cpp`; `core/WaterSeedInputs.h`; `test/common/terrain_grid_water_test.cpp`; `CMakeLists.txt`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | All four independent water resolutions, volume conservation, edits and refusal retain budgets. |
| Far snapshot/store and producer | `world/FarLodStore.h`, `.cpp`; `world/FarWorldAuthorityProducer.cpp`; `world/FarEditedBake.cpp`; `test/common/terrain_grid_far_test.cpp`; `CMakeLists.txt`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (9) | Edited/pristine snapshots and stale-cache refusal, charged capacities and unchanged metre coverage. |
| Client far transitions | `src/luminumbra_client/rendering/FarLodSystem.cpp`; `src/luminumbra_client/rendering/FarWorldOwnership.cpp`; `test/rendering/FarWorldWaterGl_test.cpp`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (6) | Legacy/Ladder boundaries, both signs, edits/caves/water and exact ownership tested. |
| Remaining client physical-unit audit | Six rendering consumers listed above; `test/rendering/terrain_culling_test.cpp`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | Culling/lighting/foliage bounds stay 16 m; grid-specific inputs explicit. |
| Identity and network refusal, dormant | `core/SystemConfig.h`, `.cpp`; `net/LockstepSession.h`; `net/ReplicationAdmission.cpp`; `test/tools/test_system_config_identity.py`; `test/common/terrain_grid_admission_test.cpp`; `CMakeLists.txt`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (10) | Explicit historical/current encoding; mismatch before simulation across all shared transport admissions. Revision bump deferred to activation. |
| Replay compatibility | `replay/ReplayStream.h`, `.cpp`; `src/luminumbra_server/modes/Replay.cpp`; `test/common/ReplayStream_test.cpp`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (7) | Old identity/runtime pairing, future layout refusal and new checkpoint replay verified. |
| Boot and preview preparation | `world/GameSession.h`, `.cpp`; `src/luminumbra_server/ServerWorldRunner.cpp`; `src/luminumbra_client/main_client.cpp`; `src/luminumbra_client/world/WorldgenPreview.cpp`; `test/ui/worldgen_preview_test.cpp`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (9) | Both layouts prepare without changing shipping selection, physical spawn/camera and rollback preserved. |
| Capture preparation | `src/luminumbra_client/core/ScenarioRunnerDrive.cpp`; four scenario files listed above; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (8) | Workloads select explicit grid; same physical coverage and honest measured counters. |
| Opt-in activation and authority revision | `core/SystemConfig.h`; `world/GameSession.cpp`; `worlds/atlas/presets/mountains_half_metre.json`; `config/public-asset-inventory.json`; `test/tools/test_system_config_identity.py`; `test/common/terrain_grid_admission_test.cpp`; this document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (9) | Then-current authority revision increment and new content hash atomic with opt-in reachability; public asset inventory/staging checked; shipping default still 1 m. |
| Qualification, then default decision | This document; `tools/ci/acceptance_matrix.json`; `docs/acceptance-matrix.md` (3, before any separately inventoried assets) | Paired High/Ultra p99 ≤8.33 ms, memory/cap/refusal evidence, old-world continuation and new-world replay across supported toolchains. |

Do not add shipping half-metre saves in an intermediate slice. Descriptor-based
1 m behavior must stay identical throughout preparation. If a sibling requires
changes outside a slice's file budget, split dormant preparation further and
keep activation closed until all consumers and refusal paths are qualified.

## Combined recommendation

Pursue option 1 (render-only refinement) first for existing and new worlds,
with bounded cached work, seams, edited/cave fallback and render/collision
disagreement measured. It directly addresses visible near-eye facets without
altering authoritative terrain. In parallel qualify option 2 as opt-in
new-world content; start with the exact mountains capture, then inspect terrain
character and representative seeds. Enable both only after their combined
captures establish that smoothing and reconstruction complement each other.

Keep option 3 as a separate authority/format experiment. It can combine with
the new preset and presentation refinement, but each must be independently
selectable in diagnostics to explain gains and cost. Consider shipping 0.5 m
authority only for an additional gameplay need, such as edit/contact fidelity,
after the performance and compatibility gates above. No existing world should
acquire a new generator or lattice merely by loading a newer executable.
