# Literature Survey: Worldgen / Lockstep / SDF-RT (iteration 4 + SHIELD-RT spike)

Research artifact, 2026-06-11. Feeds iteration 4 Wave A (biomes/rivers/structures),
Wave C (input-lockstep transport + world-hash desync oracle), and the SHIELD-RT
raymarch spike, per `.forge/artifacts/engine-roadmap/long-range-roadmap.md`.
Engine systems referenced: `SHIELD_WorldSystem` (continentalness/erosion/peaks
noises at seed offsets +3/+4/+5, monotone piecewise-linear splines),
`TerrainPresetLoader`, `FarLodStore`/`FarLodSystem` (1536 m heightfield tiles),
`SimulationClock` (30 Hz), `NetworkStateHash` (FNV-1a-64 canonical state string,
the world_hash oracle), `WorldPersistenceRoundtrip`, `MarchingCubes`,
`sdf_generation.compute` + `test_sdf_gpu_cpu_parity`.

---

## Area 1 — Procedural world generation: biomes, rivers, structures

### Annotated sources

- **World generation — Minecraft Wiki** (community-maintained reverse-engineered spec, current)
  https://minecraft.wiki/w/World_generation
  The single most precise public description of 1.18+ worldgen: exact climate
  parameter ranges, the rivers-from-weirdness formula, aquifers, structure
  spacing/separation/salt. This is the reference spec for Wave A.
- **Reinventing Minecraft world generation — Henrik Kniberg** (Mojang gameplay dev, talk, 2022)
  https://www.youtube.com/watch?v=ob3VwY4JyzE
  First-party rationale for the 1.18 redesign: why terrain shaping and biome
  placement share the same climate noises, spline-driven offset/factor, and the
  designer-iteration workflow. Companion keynote: https://www.youtube.com/watch?v=OROPAKsqzRs
- **The World Generation of Minecraft — Alan Zucconi** (blog, 2022)
  https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/
  Readable walkthrough of multi-noise biome lookup (climate sampled per
  4-block cell, nearest-region match in 6-D parameter space) and cave noise.
- **Realtime Procedural Terrain Generation — Jacob Olsen** (paper, 2004)
  https://www.semanticscholar.org/paper/5961c577478f21707dad53905362e0ec4e6ec644
  The canonical game-practical erosion paper: thermal erosion (cheap, talus-angle
  relaxation) and a simplified hydraulic model, with cost/quality analysis.
- **Implementation of a Method for Hydraulic Erosion — Hans Theobald Beyer** (bachelor thesis, TU München, 2015); reference impl: https://github.com/henrikglass/erodr ; popularized by Sebastian Lague: https://github.com/SebLague/Hydraulic-Erosion
  The standard particle/droplet erosion algorithm used by most indie terrain
  tools — the practical mid-tier between Olsen's filters and full fluid sim.
- **Fast Hydraulic Erosion Simulation and Visualization on GPU — Mei, Decaudin, Hu** (paper, 2007)
  https://www.researchgate.net/publication/4295561_Fast_Hydraulic_Erosion_Simulation_and_Visualization_on_GPU
  Pipe-model shallow-water erosion on GPU; the basis of most "interactive
  erosion" tools (World Machine-class). Grid-parallel, fits compute shaders.
- **Terrain Erosion on the GPU — Axel Paris** (blog, 2019+)
  https://aparis69.github.io/public_html/posts/terrain_erosion.html
  Practitioner survey + implementations of thermal/hydraulic erosion in compute;
  honest notes on stability, time-step limits, and artifact control.
- **Terrain Generation Using Procedural Models Based on Hydrology — Génevaux, Galin, Guérin, Peytavie, Beneš** (SIGGRAPH 2013)
  https://www.researchgate.net/publication/248703095_Terrain_Generation_Using_Procedural_Models_Based_on_Hydrology
  Rivers-FIRST generation: grow a hierarchical drainage graph, classify river
  types, then synthesize terrain around the network with blend/carve operators.
  The strongest argument for carve-then-shape over erode-then-find-rivers.
- **Large Scale Terrain Generation from Tectonic Uplift and Fluvial Erosion — Cordonnier et al.** (Eurographics 2016)
  https://www.researchgate.net/publication/292192025
  Stream-power-equation erosion against uplift; builds an explicit drainage-basin
  graph with flow routing across basins. Globally consistent rivers at range.
- **Procedural river drainage basins — Amit Patel, Red Blob Games** (interactive article, 2017)
  https://www.redblobgames.com/x/1723-procedural-river-growing/
  Minimal grow-rivers-from-mouths-upstream algorithm with live demos; the
  cheapest credible drainage-graph method, good spike fodder.
- **Jigsaw structure — Minecraft Wiki** https://minecraft.wiki/w/Jigsaw_structure
  Template pools + typed connectors, greedy expansion with depth limit; what
  actually ships villages/bastions/ancient cities at Minecraft scale.
- **WaveFunctionCollapse — Maxim Gumin** (repo, 2016) https://github.com/mxgmn/WaveFunctionCollapse ; explainer: **Wave Function Collapse Explained — Boris the Brave** https://www.boristhebrave.com/2020/04/13/wave-function-collapse-explained/
  The constraint-solver alternative for structure/settlement layout; explains
  contradiction handling and why WFC is best on small, dense tile domains.

### Key takeaways (Wave A actions)

1. **Biome placement must REUSE the existing shaping noises, not add a parallel
   stack.** Minecraft's core 1.18 insight (Kniberg) is that biomes and terrain
   agree *because they read the same climate values*. `SHIELD_WorldSystem`
   already computes continentalness/erosion/peaks-valleys per column (+3/+4/+5).
   Wave A should add only temperature (+8) and humidity (+9), then select biome
   = nearest matching region in (temp, humid, cont, erosion, weirdness[, depth])
   parameter space — Minecraft uses explicit interval boxes per biome with
   closest-box fallback. O(1), chunk-local, order-independent: safe for
   streaming and for world_hash.
2. **Biome regions as data, not code.** Minecraft's parameter ranges are pure
   data (5 temperature bands, 5 humidity bands, 7 erosion levels, etc. — exact
   ranges on the wiki page). Put biome interval boxes in a JSON next to
   `data/common/materials.json` and load via `TerrainPresetLoader`-style preset
   machinery. This keeps biome CONTENT game-side per the engine/game split-lint.
3. **Rivers: use the PV-band trick, not simulation, for iteration 4.** Minecraft
   derives `PV = 1 - |3*|weirdness| - 2|` and rivers generate where the PV level
   is "Valleys" (PV in [-1.0, -0.85]) at low continentalness / high erosion —
   i.e., rivers are *where the ridged noise crosses its valley band and the
   height spline pulls terrain to sea level there*. We already have a
   peaks_spline on a ridged input; seed +10 becomes a river-dedicated ridged
   noise whose near-zero band (a) carves the height spline down and (b) assigns
   the river biome. Fully chunk-local — works identically in near chunks and
   `FarLodStore` tiles, which matters because both must agree at the seam.
4. **Drainage-graph rivers are a world-precompute, not a streaming feature.**
   Génevaux 2013 and Cordonnier 2016 produce rivers that actually join and flow
   downhill, but both require a global pass (drainage graph over the whole
   domain). That cannot run per-chunk on demand. The roadmap's committed
   "deterministic erosion detail pass (thermal+hydraulic) feeding river valleys"
   (iteration 6) is the right home: run a seed-deterministic, low-res
   drainage/erosion pass once at world creation over the far heightfield domain,
   persist alongside `FarLodStore` tiles, and have near-field generation sample
   it. Red Blob's grow-upstream method is the cheapest credible version to spike.
5. **Erosion algorithm choice by domain:** Olsen-style *thermal* erosion is a
   neighborhood filter — usable per far-tile with an overlap apron, cheap and
   deterministic. *Droplet* erosion (Beyer/Lague) is NOT chunk-local (particles
   wander arbitrarily far), so it is only safe on a bounded precomputed domain
   (the world-creation pass in takeaway 4), never in streaming generation.
   Pipe-model GPU erosion (Mei 2007) is an interactive-editing tool; on our
   stack GPU generation would have to pass a CPU/GPU parity gate like
   `test_sdf_gpu_cpu_parity` before its output may touch world_hash — default to
   CPU for any erosion that feeds authoritative terrain.
6. **Structure placement: adopt Minecraft's spacing/separation/salt grid.**
   Partition the world into spacing×spacing chunk cells; within each cell, pick
   a position from hash(world_seed, cell, structure_salt), with `separation`
   enforcing minimum distance and a frequency roll (villages: spacing 34,
   separation 8, 100%; outposts: 32/8/20%). O(1) deterministic lookup gives a
   free "locate nearest structure" query. Register each structure salt in the
   seed registry exactly like noise offsets — same append-only discipline.
7. **Jigsaw over WFC for iteration-4 surface structures.** Jigsaw is greedy
   template assembly: template pools, typed connectors, random pool draws seeded
   by (chunk, world seed), depth-limited expansion. It is deterministic by
   construction, fails gracefully (just stops expanding), and is fully
   data-driven (game content). WFC adds contradiction/restart handling and
   global constraint cost; the literature consensus (Boris the Brave, shipped
   uses like Bad North) is WFC shines on small dense tile domains — revisit for
   iteration-6 underground/cavern content, not for Wave A.
8. **Far-tile material variety should blend by climate distance, not biome id.**
   Minecraft samples climate at 4-block granularity and fuzzes lookups; hard
   per-tile biome ids in `FarLodStore` would produce visible far-field seams.
   Store the dominant biome id per far-tile texel but derive material blend
   weights from distance in climate space, so adjacent biomes shade into each
   other the same way near-field chunks will.
9. **Aquifer pattern (file for iteration 6):** Minecraft replaces a global sea
   level with per-cell "local fluid level" noise (64×40×64 cells; empty /
   flooded / local-level states). Directly relevant to `WaterSystem` when
   underground biome volumes land — local water tables make caves photogenic
   without global flow simulation.

---

## Area 2 — Deterministic lockstep networking

### Annotated sources

- **1500 Archers on a 28.8: Network Programming in Age of Empires and Beyond — Bettner & Terrano** (Game Developer Conference paper, 2001)
  https://www.gamedeveloper.com/programming/1500-archers-on-a-28-8-network-programming-in-age-of-empires-and-beyond (PDF: https://zoo.cs.yale.edu/classes/cs538/readings/papers/terrano_1500arch.pdf)
  The canonical lockstep paper: communication turns, input scheduled 2 turns
  ahead, adaptive turn length from measured latency, speed control to the
  slowest machine, and their catalog of desync sources.
- **Factorio Friday Facts #52 — Ups and Downs** (dev blog, 2014) https://factorio.com/blog/post/fff-52
  Ground truth on float determinism in a shipped lockstep game: feared float
  divergence never materialized; their first real desync was an *ambiguous
  std::sort comparator* behaving differently across STL implementations. They
  wrote their own trig functions rather than trust libm.
- **Factorio Friday Facts #47 — CRC fun** (dev blog, 2014) https://www.factorio.com/blog/post/fff-47
  Debug mode CRCs the whole map every tick and saves human-readable tagged
  state, then diffs the first divergent tick. The desync-localization playbook.
- **Factorio Friday Facts #340 — Deep desyncs** (dev blog, 2020) https://www.factorio.com/blog/post/fff-340
  A mature desync postmortem: hidden state not covered by the CRC, and how
  their tooling found it.
- **Desynchronization — Factorio Wiki** https://wiki.factorio.com/Desynchronization
  Documents "heavy mode": save → load → re-simulate on multiple instances with
  intermediate saves, catching both desyncs and save/load state divergence.
- **Floating-Point Determinism — Bruce Dawson** (blog, 2013) https://randomascii.wordpress.com/2013/07/16/floating-point-determinism/
  and **Intermediate Floating-Point Precision** (2012) https://randomascii.wordpress.com/2012/03/21/intermediate-floating-point-precision/
  The authoritative breakdown of what actually diverges: x87 80-bit
  intermediates, FMA contraction, compiler reassociation, denormal/rounding
  modes, transcendental library differences — and what is actually IEEE-stable
  (+, -, *, /, sqrt, comparisons under fixed modes).
- **Floating Point Determinism — Glenn Fiedler** (gafferongames, 2010) https://gafferongames.com/post/floating_point_determinism/
  Survey of real-world experience reports; the practical conclusion: same
  binary + same architecture is achievable, cross-platform requires discipline
  or fixed point.
- **Deterministic cross-platform floating point arithmetics — Christian Seiler** http://christian-seiler.de/projekte/fpmath/
  Concrete compiler/FPU control recipes (SSE2, precision modes) per platform.
- **Determinism in League of Legends — Riot Games engineering** (blog series, 2017)
  https://technology.riotgames.com/news/determinism-league-legends-introduction ;
  https://www.riotgames.com/en/news/determinism-league-legends-implementation
  Retrofit of determinism into a live game for replay/disaster-recovery
  (Chronobreak): ~a year of multi-engineer effort, unified clock, recorded-input
  replay, server state restore. Proof of replay value and retrofit cost.
- **Netcode Architectures Part 2: Rollback — SnapNet** (engineering blog, 2021) https://www.snapnet.dev/blog/netcode-architectures-part-2-rollback/
  and **The Netcode of GGPO-style games — Infil** https://ki.infil.net/w02-netcode.html
  Clear-headed rollback-vs-delay analysis: rollback buys reaction-time latency
  at the cost of re-simulation budget, state snapshot/restore, and visual
  inconsistency. Frames the decision for non-competitive co-op.

### Key takeaways (Wave C actions)

1. **Delay-based lockstep, no rollback.** Rollback exists to hide latency in
   reaction-critical competitive play; its price is N-tick re-simulation +
   full state snapshot/restore every frame. Our world tick includes chunk
   streaming, water, and physics — rollback re-simulation would be brutal, and
   a zen co-op photography game does not need it (SnapNet/Infil analysis).
   Adopt the 1500-Archers model on the 30 Hz `SimulationClock`: inputs sent for
   tick T+k, k adaptive from measured RTT (their "communication turn" was
   200 ms; we can quantize k in ticks and start at ~3-4 ticks = 100-133 ms).
2. **Hide the delay render-side.** Camera look must remain client-local
   (`PlayerController` orientation is render state, not sim state); only
   quantized movement/interaction intents enter the lockstep input stream.
   With look latency at zero, 100 ms+ of movement input delay is essentially
   invisible in a non-twitch game. This is the single biggest perceived-quality
   lever and costs nothing.
3. **Speed control:** AoE's adaptive turn scales to the slowest machine; the
   headless server (`ServerWorldRunner`) is the natural pacing authority — it
   should lengthen the input horizon (k) rather than stall the tick when a
   client lags, and surface a UI signal. Decide stall policy before the LAN
   milestone, not after.
4. **The desync oracle needs sub-hashes, not one hash.** Factorio's CRC-per-tick
   plus human-readable tagged saves works because divergence is *localized*.
   `NetworkStateHash`'s canonical string already has named fields
   (position_x_mm, entities, world_hash) — extend it to ordered per-system
   sections (physics, water, streaming, entity snapshot, RNG cursor) and report
   the FIRST divergent field + tick, not just hash mismatch. This turns a
   desync from a repro hunt into a bug report.
5. **Add a "heavy mode" gate variant.** Factorio's heavy mode (save → load →
   resimulate → compare) catches two bug classes our per-tick hash cannot:
   state excluded from the hash, and save/load round-trip divergence. We
   already own `WorldPersistenceRoundtrip` — wire a validator mode that runs
   N ticks, round-trips, runs N more on both instances, and compares
   world_hash streams. Cheap to build, disproportionately effective.
6. **Float determinism is a non-problem with discipline — same binary, same
   arch.** Factorio shipped float-based lockstep; Dawson confirms IEEE basic
   ops (+ - * / sqrt, comparisons) are bit-identical under fixed
   rounding/denormal settings on SSE2. The actual hazards, in observed order:
   (a) ambiguous sort comparators / unordered container iteration order
   (Factorio's first real desync), (b) libm transcendentals (different results
   per implementation — Factorio wrote their own sin/cos), (c) FMA contraction
   (compilers fuse a*b+c differently), (d) /fp:fast or -ffast-math
   reassociation. Mitigations to land BEFORE Wave C code: pin
   `/fp:precise` + contraction off (`/fp:contract-` on MSVC ≥17.0, `-ffp-contract=off`)
   for `luminumbra_common` + server targets in CMake; add deterministic
   sin/cos/atan2/pow wrappers and lint-ban `std::` transcendentals and
   `std::unordered_*` iteration in sim code paths; document the flag set as a
   versioned determinism contract in `EngineContracts`.
7. **Cross-compiler builds (MSVC client vs future Linux/Clang server) are the
   real risk.** Seiler/Dawson: identical *instruction sequences* are
   deterministic, but different compilers emit different sequences. Either
   (a) commit to identical-flag Clang on both platforms, or (b) keep extending
   the fixed-point pattern already visible in `NetworkStateHash`
   (position_*_mm integers) for everything hashed. Decide which before the
   "one remote client" milestone; (b) is the robust default for hashed state.
8. **Replay is nearly free once lockstep exists — build it inside Wave C.**
   Riot spent ~an engineer-year retrofitting determinism for replay; for us
   replay = world seed + versioned input stream + periodic world_hash
   checkpoints. The checkpoints make replay self-verifying: divergence during
   playback is detected at the first mismatching checkpoint (this is exactly
   Factorio's desync-repro tool). Stamp recordings with `EngineVersion` and
   refuse playback across versions — Factorio replays break silently across
   versions; refuse loudly instead.
9. **RNG discipline:** one seeded sim RNG stream, advanced only by sim code;
   render/UI/audio get separate non-hashed streams. The 1500-Archers desync
   catalog (out-of-sync RNG, uninitialized state, checksum too narrow) should
   become a checklist item in the Wave C spec review.

---

## Area 3 — SDF rendering / sphere tracing (SHIELD-RT spike)

### Annotated sources

- **GPU-Based Clay Simulation and Ray-Tracing Tech in Claybook — Sebastian Aaltonen** (GDC 2018)
  Slides: https://media.gdcvault.com/gdc2018/presentations/Aaltonen_Sebastian_GPU_Based_Clay.pdf ;
  video: https://www.youtube.com/watch?v=Xpf7Ua3UqOA
  A shipped console game rendering its whole deformable world by ray-tracing a
  GPU-resident SDF volume with a mip pyramid: coarse-mip-first traversal, cone
  footprint mip selection, SDF soft shadows/AO, 60 fps on base consoles. The
  existence proof + optimization playbook for SHIELD-RT.
- **Sphere Tracing: A Geometric Method for the Antialiased Ray Tracing of Implicit Surfaces — John C. Hart** (The Visual Computer, 1996)
  https://link.springer.com/article/10.1007/s003710050084
  The foundational paper: Lipschitz-bounded marching, convergence guarantees,
  cone tracing for antialiasing. Defines the correctness conditions every
  acceleration must preserve.
- **Enhanced Sphere Tracing — Keinert, Schäfer, Korndörfer, Niessner, Stamminger** (Smart Tools & Apps for Graphics, 2014)
  https://www.lgdv.tf.fau.de/publications/enhanced-sphere-tracing/
  Over-relaxation (step scale ω ∈ [1,2) with safe fallback when consecutive
  spheres disjoin), plus robust termination and self-intersection-free surface
  offset. The standard cheap 25-40% step-count win.
- **Accelerating Sphere Tracing — Bálint & Valasek** (Eurographics short paper, 2018)
  https://people.inf.elte.hu/csabix/publications/articles/eurographics-2018-shortpaper.pdf
  Improves over-relaxation by fitting a local linear approximation of the SDF
  to choose per-step optimal ω.
- **Segment Tracing Using Local Lipschitz Bounds — Galin, Guérin, Paris, Peytavie** (Computer Graphics Forum / Eurographics, 2020; PDF via authors' pages)
  Direction-local Lipschitz bounds give far larger steps for procedural SDFs;
  relevant only if we trace analytic/procedural fields rather than sampled grids.
- **Mesh Distance Fields / Lumen Technical Details — Epic Games** (UE5 documentation)
  https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine ;
  https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
  Production hybrid architecture at scale: per-mesh SDFs near, a merged Global
  Distance Field in camera-centered clipmaps far ("smaller and denser closer to
  the camera"), incremental clipmap updates only where the scene changed, and
  Lumen's policy of tracing detail SDFs for the first ~2 m and the global SDF
  beyond. The blueprint for our near/far split.
- **Learning from Failure (Dreams) — Alex Evans, Media Molecule** (SIGGRAPH 2015, Advances in Real-Time Rendering)
  https://advances.realtimerendering.com/s2015/ ; https://www.mediamolecule.com/blog/article/siggraph_2015
  Four years and three abandoned renderers for an SDF-native game; they
  ultimately splatted point clouds generated FROM the SDFs rather than
  raymarching them. The strongest published caution against assuming
  raymarching is the endgame — and the best catalog of alternatives.
- **Ray Tracing of Signed Distance Function Grids — Söderlund, Evans, Akenine-Möller** (JCGT, 2022)
  https://jcgt.org/published/0011/03/06/
  Rigorous treatment of tracing *sampled* (grid) SDFs, including sparse brick
  sets vs sparse voxel sets and correct intersection with trilinear-interpolated
  fields — directly applicable to bricks generated from our chunk SDFs.
- **Raymarching Distance Fields / Terrain Raymarching — Inigo Quilez** (articles, 2008+)
  https://iquilezles.org/articles/raymarchingdf/ ; https://iquilezles.org/articles/terrainmarching/
  Fundamentals and the heightfield-specific march (which is not sphere tracing
  and is much cheaper for terrain): fixed/adaptive stepping against a
  heightfield with LOD by distance.
- **SDF resource collection — Cédric Guillemet** (curated repo) https://github.com/CedricGuillemet/SDF
  Maintained index of SDF papers/talks/implementations; the follow-the-links
  hub for anything not covered above.

### Key takeaways (SHIELD-RT spike actions)

1. **The hybrid split is a solved pattern — copy Lumen's policy shape.** Raster
   the meshed marching-cubes near field (F0/F1) exactly as today; raymarch a
   coarse merged SDF beyond. UE blends per-ray: detail representation for the
   first N meters, global field after. For us the transition band sits near the
   far-tile horizon; blend with a depth-aware dither/dissolve over a distance
   band rather than a hard plane, and gate it with the planned near-to-far seam
   gate (iteration 6 list already names this).
2. **Storage: camera-centered clipmaps of sparse bricks.** Every production
   system converges here (UE Global DF clipmaps; Claybook mip volume; JCGT
   sparse brick sets): concentric clipmap levels, denser near the camera, each
   level an indirection table into a pool of ~8³ voxel bricks, empty-air
   regions unallocated. Our chunk SDFs (from `sdf_generation.compute` /
   `MarchingCubes` source data) downsample into bricks; `FarLodStore`
   heightfield tiles can synthesize coarse far bricks analytically (a
   heightfield gives a cheap conservative distance bound: vertical distance is
   an upper bound, scaled by max slope for a Lipschitz-correct bound) without
   ever storing volumes for the sky.
3. **Mips/coarse levels MUST be conservative lower bounds.** Sphere tracing is
   only correct if every sampled distance never exceeds the true distance.
   Averaging mips (standard texture mip generation) breaks this and produces
   silent surface overshoot that presents as seam/popping artifacts. Coarse
   levels need min-style filtering with a sample-spacing correction (JCGT 2022
   covers correct grid-SDF interpretation; Claybook's pyramid is built for
   conservative traversal). Make "conservative coarse levels" an explicit spike
   success criterion so overshoot is not misdiagnosed as a blending bug.
4. **Acceleration adoption order:** (a) coarse-mip-first traversal with a fixed
   per-pixel step budget (Claybook's biggest win); (b) Keinert over-relaxation
   ω≈1.2-1.6 with disjoint-sphere fallback — nearly free; (c) a quarter-res
   tile prepass that cone-traces a safe start distance per 8×8 pixel tile
   (Claybook-style), letting full-res rays skip empty space; (d) Bálint/segment
   tracing only if profiling still demands it. Do NOT start with (c)/(d); (a)+(b)
   alone may hit the far-field budget.
5. **Spike must benchmark heightfield marching as the baseline, not just sphere
   tracing.** Beyond F2 the world data IS a heightfield (`FarLodStore`); iq-style
   terrain raymarching with distance-scaled steps (or maximum-mip quadtree
   traversal) is substantially cheaper than generic SDF sphere tracing and
   trivially LODs. Generic SDF tracing only pays for itself where the far field
   needs true 3D content (overhangs, large structures, cave mouths). The spike
   should produce numbers for BOTH paths at the 1536 m horizon — this directly
   informs whether iteration 6 "F1/F2 tiles retire" or instead *become the
   raymarch source representation*.
6. **Normals and self-intersection:** use 4-tap tetrahedron central differences
   for hit normals (not 6-tap), switch to screen-space derivative normals past a
   distance threshold; apply Keinert's offset-based self-intersection avoidance
   for secondary rays (shadows). At far-field distances, normal quality
   requirements are low — budget accordingly.
7. **Keep the spike render-only and GPU-resident from day one.** The roadmap
   already notes GPU SDF enablement and SHIELD-RT converge; Claybook's entire
   architecture presumes a GPU-resident SDF + compute traversal. Prototyping a
   CPU raymarcher would measure the wrong thing. Because it is render-only,
   nothing touches world_hash or the determinism contracts — the
   `test_sdf_gpu_cpu_parity` gate continues to police only the *authoritative*
   SDF data, not the render bricks.
8. **Dreams is the kill-criteria benchmark.** Media Molecule burned three
   renderers before abandoning direct raymarching for splatting. Our scope is
   far narrower (far-field-only, terrain-dominated, no per-frame deformation),
   which is exactly why it can work where Dreams' general case did not — but
   write the spike's evidence thresholds (ms budget at horizon, seam quality,
   memory for clipmap pool) BEFORE building, per the "evidence-only" mandate.
9. **Free dividends once the global SDF exists (file for iteration 6):** SDF
   soft shadows and AO via cone traces (Claybook, UE DFAO) — relevant to
   `lighting_pass.frag`/`ssao.frag` and to Aetheric crystal-light mood shots;
   and volumetric cloud marching can share the same clipmap traversal code
   (already a committed iteration-6 expansion: "volumetric clouds as a second
   SHIELD-RT raymarch consumer").

---

## Decisions this research should change

1. **Rivers (Wave A): commit to the PV-band/noise method now; move any
   drainage-simulation ambition explicitly to the iteration-6 erosion pass.**
   The literature is unambiguous that globally-consistent drainage requires a
   global pass incompatible with chunk-local streaming generation. Seed +10
   stays a ridged river noise; carve via existing spline machinery in
   `SHIELD_WorldSystem`. Do not let the Wave A spec drift toward "rivers that
   flow downhill" — that is a different (precompute) feature.
2. **Biomes (Wave A): the spec should mandate reuse of the existing +3/+4/+5
   noises as biome-selection dimensions.** Only temperature/humidity are new.
   A biome system with its own independent noise stack would diverge from
   terrain (deserts on mountaintops) and double the climate sampling cost.
3. **Session replay: upgrade from "consider inside the iter-4 transport wave"
   (roadmap line) to a committed Wave C deliverable.** Riot's retrofit cost
   (~1 engineer-year) vs near-zero marginal cost during lockstep construction,
   plus replay being THE desync-repro tool (Factorio), makes deferral strictly
   more expensive. Minimum scope: record seed + input stream + periodic
   world_hash checkpoints + EngineVersion stamp; playback with checkpoint
   verification.
4. **Add a determinism build/lint contract BEFORE Wave C implementation:** pin
   float flags (`/fp:precise`, contraction off) for common+server targets,
   deterministic transcendental wrappers, lint bans on libm trig/pow and
   unordered-container iteration in sim code, sort-comparator strictness review.
   Factorio's first production desync was a comparator, not a float — our
   current plan focuses on the hash oracle (detection) but has no prevention
   item. Cheap now, expensive after Wave C code exists.
5. **Extend the world_hash oracle design to per-system sub-hashes + a heavy-mode
   (save/load/resimulate) validator variant.** A single FNV-1a over a canonical
   string detects desyncs but cannot localize them; Factorio's experience says
   localization is where all the time goes. `WorldPersistenceRoundtrip` already
   exists — wiring it into the oracle is small.
6. **SHIELD-RT spike scope change: benchmark heightfield ray-marching of
   FarLodStore tiles AGAINST generic SDF sphere tracing.** The current spike
   framing assumes "SDF raymarch"; the literature suggests terrain-only far
   fields are cheaper as marched heightfields, which would flip the iteration-6
   assumption that tiles "retire" — they may instead become the far renderer's
   source data, with sparse SDF bricks reserved for 3D features only.
7. **Spike success criteria must include conservative-coarse-level correctness**
   (min-filtered mips / Lipschitz-valid heightfield bounds). Overshoot artifacts
   from averaged mips look like blending bugs and would corrupt the spike's
   evidence if not controlled for.
8. **Structures (Wave A): jigsaw-style template pools + spacing/separation/salt
   placement; explicitly defer WFC.** Salts join the seed registry under the
   same append-only discipline as noise offsets.
9. **Rollback is rejected, permanently, for this title** — record it as a
   decision with rationale (re-simulation cost of world tick + non-competitive
   design) so it does not get relitigated each networking discussion; pair it
   with the mitigations that make delay-based feel good: render-side camera
   look, adaptive input horizon, server-paced speed control.
