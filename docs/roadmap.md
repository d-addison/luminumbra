# Coordinated delivery roadmap

Updated September 13, 2026. This is a delivery/status map with actionable issues in
both repositories. Engine, game and art-pack versions need not share tag numbers.
There are no assigned due dates or invented completion percentages.

## Current state

- **Recovery baseline:** engine `945ecd295835a838eed0169ff709e7aabc134fa1`
  and game `552a26684215542b327a842104864dda14e31652`. Subsequent verified upstream
  revisions are engine `devel` at `e60c6554c323f9fd8a8098bbac81e5ea1793cd7e`
  and game `main` at `f266fe4e121b6a29361c70116ad82e89560b21a3` (September 13).
- **Merged implementation:** repair PR [#56](https://github.com/d-addison/luminumbra/pull/56),
  accepted distance contract, hang instrumentation, reversed-Z depth, persisted clock,
  five-tier declaration, render measurements and simulation telemetry. The original
  Blender stack was superseded by merged [#155](https://github.com/d-addison/luminumbra/pull/155).
  The optional headless prefab runtime [#165](https://github.com/d-addison/luminumbra/pull/165)
  has also merged after current-head checks and independent native evidence review.
  Blender review/undo [#162](https://github.com/d-addison/luminumbra/pull/162)
  and the restored [visual catalog](visual-catalog.md)
  [#170](https://github.com/d-addison/luminumbra/pull/170) have merged.
  These implementations still have the acceptance obligations listed below.
- **Recovery in progress:** the SBOM repair [#169](https://github.com/d-addison/luminumbra/pull/169)
  passes the actual MSYS Git/UCRT Python path-disagreement cases. Ambient SIMD
  [#172](https://github.com/d-addison/luminumbra/pull/172), tracked by
  [#163](https://github.com/d-addison/luminumbra/issues/163), passes actual Linux
  and native UCRT Debug/Release controls at `e1080d7f`. Actual selected-ISA
  evidence and failing original-dispatch controls preserve each configuration's
  established goldens. Active-region
  [#161](https://github.com/d-addison/luminumbra/pull/161) has been refreshed against
  the recovery baseline and its scheduler, persistence, recovery and replay checks
  pass. The composed candidate [#175](https://github.com/d-addison/luminumbra/pull/175)
  awaits current-head integration checks and landing. The separate
  wind input-padding defect is tracked by [#173](https://github.com/d-addison/luminumbra/issues/173).
  The September 13 promotion UCRT job passed 2,245 of 2,250 cases at its recorded merge revision;
  its surface-loading guard and three render-process timeouts remain unresolved.
  All three timed-out render processes emitted their captures before termination;
  shutdown diagnosis must establish the cause. Local native `e1080d7f` passes
  the unchanged loading guard and original render cases on NVIDIA and verified
  CI-pinned Mesa 26.1.7 llvmpipe. Different hardware and toolchain versions keep
  these non-reproductions separate from CI and original-hang causal closure.
  The candidate retains failed-process captures and adds shutdown-stage evidence;
  its fresh native build and qualification remain in progress.
  Historical results do not qualify a new candidate; discover its tests again.
- **Verification:** later Debug/ASan and native records supersede the failed
  `ff0362b` baseline. Expanded integrated, native and packaged acceptance is
  outstanding; implementation, test evidence and merged state are distinct.
- **Accepted pre-release scope:** five volumetric tiers through 16,384 m, qualified
  within 32 km of the origin; per-system simulation policies over persistent active
  regions; Quality (1.0) and Performance (0.67) at 3840×1600 and 3440×1440.
  Both profiles require p99 frame time ≤16.67 ms; 120 fps remains the reported
  target. The accepted [distance contract](distant-world.md) governs every slice;
  its completed design interview is not a new implementation prerequisite.
- **Remaining runtime implementation:** the tier table alone does not render 16 km;
  the runtime still uses legacy far ranges and a 3,200 m far plane. Telemetry and
  a region ledger alone do not schedule distant simulation consumers.
  The pristine brick generator/mesher [#176](https://github.com/d-addison/luminumbra/pull/176)
  has separate Debug/Release and boundary/cave controls; its CI correction and
  landing are pending. A fixture-only asynchronous consumer is undergoing review
  and verification. Runtime paging, graphical integration, final cave-filter
  qualification and durable dirty residency remain subsequent work.
- **Hang closure:** Windows reported the original `0xCFFFFFFF` application hang
  and the operator closed it. The underlying mechanism remains unproven.
  Instrumentation and successful non-reproductions do not close
  [#127](https://github.com/d-addison/luminumbra/issues/127); the server-load
  incident [#69](https://github.com/d-addison/luminumbra/issues/69) is separate.
- **Publication:** v0.3.0 remains unpublished and promotion
  [#55](https://github.com/d-addison/luminumbra/pull/55) is blocked. Strict promotion
  requires all 15 contexts after expanded acceptance, followed by signed source-only
  publication, independent artifact verification and a newly named private preview.
  Audio stays off; the delivered `5daeb03` preview stays unchanged.
- **Authoring ownership:** generic packages remain in the engine monorepo.
  Merged [#162](https://github.com/d-addison/luminumbra/pull/162) supplies the Blender
  identity review/undo package. Its refreshed package matches the retained Windows
  archive byte-for-byte and has 22 recorded native checks. Visual approval and
  Linux native editor qualification remain outstanding.
  Merged [#165](https://github.com/d-addison/luminumbra/pull/165)
  supplies optional compiled-prefab consumption. Headless consumption is not graphical qualification.
  Full installed SDK delivery and game extraction retain the post-release
  Foundations approval boundary.

An implementation is code present in a revision. Verification identifies the
tested revision, environment, scope and evidence. Merging integrates code into a
branch. Publication delivers artifacts with independently checked identities.

Engine [#141](https://github.com/d-addison/luminumbra/issues/141) owns budget enforcement and qualification mechanisms; game [#31](https://github.com/d-addison/luminumbra-game/issues/31) owns authored rates, activation data and populator policy. Game [#34](https://github.com/d-addison/luminumbra-game/issues/34) retains future content/data distribution and optional non-Steam loading. All v0.4 commitments remain assigned.

## Milestones and ownership

| Coordinated milestone | This repository | Counterpart repository |
|---|---|---|
| [v0.3.0 — Acceptance and release](https://github.com/d-addison/luminumbra/milestone/1) | Final saved-world/render/content correctness; the expanded pre-release scope (16 km unbounded world with distant cave interiors and edits, persistent-active-region distant simulation, measured 60 fps floor on two profiles, client-hang closure); native/CI acceptance, signed source release, independent verification and new private preview. | [Source/runtime art-pack verification, final canopy conversion/provenance, distant-world scenes, game distant-simulation data and packaged content/save/visual acceptance with audio off.](https://github.com/d-addison/luminumbra-game/milestone/1) |
| [Foundations — Engine/game separation and tooling](https://github.com/d-addison/luminumbra/milestone/2) | Installed SDK; lifecycle/schedule/state registration; optional field/behavior modules; script/component authoring; input, commands, inspection/profiling; dependency enforcement. | [Populated compatibility fixtures; mechanics/state/content extraction; pinned standalone launchers; script/data composition; screens/HUD/diagnostics/tests.](https://github.com/d-addison/luminumbra-game/milestone/2) |
| [v0.4.0 — Continuing-world multiplayer](https://github.com/d-addison/luminumbra/milestone/3) | Checkpoints/catch-up; first-time authoritative admission; returning-identity and two-established-peer lockstep reconnect; physical avatars; LAN/directory and live transport/fault/load qualification. | [Real host/join/reconnect flows; LAN/public/direct/favorites/recent browser; generated animated physical avatars; network AI clients; composed previews and process acceptance.](https://github.com/d-addison/luminumbra-game/milestone/3) |
| [v0.5.0 — Environmental audio](https://github.com/d-addison/luminumbra/milestone/4) | Diffuse RT60 DSP, weather-loop lifecycle, thunder propagation and device-independent decoding/graph acceptance. | [Soundscape/event integration, authored acoustic tuning and audible packaged acceptance.](https://github.com/d-addison/luminumbra-game/milestone/4) |
| [v0.6.0 — Dynamic motion history](https://github.com/d-addison/luminumbra/milestone/5) | Previous rendered transforms/bones, history validity, depth disocclusion and reset/resource/performance qualification. | [Animated avatar/wildlife fixtures and real gameplay/world-transition history acceptance.](https://github.com/d-addison/luminumbra-game/milestone/5) |
| [v0.7.0 — Rendering foundation — provisional](https://github.com/d-addison/luminumbra/milestone/6) | Production backend/capability settings, residual longer-view scope beyond the 16 km v0.3 contract and native Windows/Linux/Proton/hardware qualification. | [Composed scene/UI parity, terrain/material/weather/water/long-view acceptance and scalable settings.](https://github.com/d-addison/luminumbra-game/milestone/6) |
| [v0.8.0 — Reconstruction and latency — provisional](https://github.com/d-addison/luminumbra/milestone/7) | Qualified reconstruction, dynamic resolution, latency and prerequisite-gated optional frame generation. | [Effective player settings and representative visual/input workloads.](https://github.com/d-addison/luminumbra-game/milestone/7) |
| [v0.9.0 — Lighting and effects — provisional](https://github.com/d-addison/luminumbra/milestone/8) | Improved raster lighting, optional hybrid RT/denoising and reusable field-effect interfaces. | [Game lighting integration and proposed aether visuals, provisionally placed pending architecture review.](https://github.com/d-addison/luminumbra-game/milestone/8) |

Full installed SDK delivery and game extraction in Foundations depend on
independently verified v0.3 publication **and** the single implementation-roadmap approval after refreshed source/SDK architecture
review with actual Astra/xhigh. Accepted ownership/authoring direction is recorded
in [Engine/game ownership](engine-game-boundary.md). Roadmap population does not
waive that implementation gate. The already-authorized bounded optional authoring
packages above may be refreshed and qualified before release. v0.7–v0.9 remain provisional.

The foundation precedes substantial v0.4 work and does not renumber or reduce its
commitments. Keep authoritative first-time joins, continuing reconnect, strict
two-established-peer lockstep reconnect, discovery/directory/browser, physical
generated avatars, ordinary-protocol AI clients and runnable previews after
completed slices. Public discovery does not itself provide NAT traversal or
imply host migration, matchmaking or private Steam relay availability.

## Limits and acceptance still tracked

- Current far rendering reaches approximately 3 km with a 3200 m camera far
  plane; world generation itself has no fixed edge. Pristine far tiles are
  heightfields; bounded authoritative SDF overlays can carry resident edits, but
  the far store is not attached to saves at runtime. The accepted v0.3 contract replaces this with a 16 km volumetric
  ladder over an unbounded world; until those slices land and are verified, the
  current limits stand.
- With `sim.active_regions` disabled, the simulation clock retains its legacy
  load-time reset; the landed enabled clock resumes the persisted absolute tick.
  Creature/plant distance scheduling, durable field pages and wildlife, and dirty
  eviction parking still require their consumer/residency slices. Wind/weather/aether
  remain anchored grids; a clock or ledger alone does not implement these policies.
- The native client exit 0xCFFFFFFF (PID 66604, package 2871c72) was a Windows
  Application Hang closed by the operator, not a crash; the hang mechanism is not yet
  established and its closure is a release gate.
- White ground patches, purple shadows and visible stars in the settled
  Default/424242/FOV 110° native capture have separate investigation issues.
  Their causes remain unconfirmed. Tree-only tests do not close whole-scene
  appearance reports. Culling and no-UI fixes retain their separate causal evidence.
- Audio is disabled for v0.3 and its private preview by owner direction; audible
  quality and event coverage are future v0.5 work in a separate workstream. Dynamic-body
  acoustic material classification currently uses Stone.
- GNS compilation does not establish live-peer behavior. Steam private SDK/live
  service and broad NVIDIA/AMD/Intel hardware qualification remain explicit gaps.
  Native Windows, native Linux and Proton require separate results. Resource
  estimates must not be presented as measured total VRAM.
- The intermittent server-load crash retains unresolved cause status. The proven
  Jolt exception-configuration cause of the MSVC catalog failure does not prove a
  shared server-crash cause. The UCRT64 120-second guard stays unchanged.

Software releases remain source-only. Art packs are separately versioned game
content. Private recordings, Steam payloads and private previews remain outside
public packs. Preset revision 6, LMR1 v2, FSD2 payload v3, canonical in-memory
serialization, terrain-quality assertions and default-off policies are preserved;
obsolete-world migration and legacy loading are not planned.

## Work-package index

Each issue records owner, recovery state, remaining changes, dependencies,
acceptance and evidence required for closure. Capability and game-integration
issues link to one another; closing a counterpart does not automatically close
its integration. Visual reports retain reproduction context, expected/observed
behavior, revision/evidence identities, cause status, fix revision and packaged
verification.

### v0.3.0 — Acceptance and release

- [Finish verified game-content acquisition and Linux/Windows CI](https://github.com/d-addison/luminumbra/issues/58)
- [Complete authored tree materials, LODs and full-sphere impostors](https://github.com/d-addison/luminumbra/issues/59)
- [Bound saved-world catalog validation without blocking the UI](https://github.com/d-addison/luminumbra/issues/60)
- [Verify packaged saved-world lifecycle and authority replacement](https://github.com/d-addison/luminumbra/issues/61)
- [Verify saved terrain edits across hierarchical culling boundaries](https://github.com/d-addison/luminumbra/issues/62)
- [Verify no-UI saved-world loading and capture lifecycle](https://github.com/d-addison/luminumbra/issues/63)
- [Complete settled terrain, weather, cave and water visual acceptance](https://github.com/d-addison/luminumbra/issues/64)
- [Investigate white ground patches in settled native scene](https://github.com/d-addison/luminumbra/issues/65)
- [Investigate purple shadows in settled native scene](https://github.com/d-addison/luminumbra/issues/66)
- [Investigate visible stars in settled native ground scene](https://github.com/d-addison/luminumbra/issues/67)
- [Qualify serial loading, networking soak and performance gates](https://github.com/d-addison/luminumbra/issues/68)
- [Resolve or qualify the intermittent server-load crash independently](https://github.com/d-addison/luminumbra/issues/69)
- [Independently verify, promote and publish signed source-only v0.3.0](https://github.com/d-addison/luminumbra/issues/70)
- [Deliver a newly named verified private client/server preview](https://github.com/d-addison/luminumbra/issues/71)
- [Apply storm composition consistently to terrain and grass](https://github.com/d-addison/luminumbra/issues/107)
- [Align grass cloud-shadow projection with the terrain cloud field](https://github.com/d-addison/luminumbra/issues/108)
- [Classify the high-view live-to-distant terrain material seam](https://github.com/d-addison/luminumbra/issues/109)
- [Correct glass shared-depth attachment and resize lifetime](https://github.com/d-addison/luminumbra/issues/110)
- [Keep glass background sampling aligned at reduced render scales](https://github.com/d-addison/luminumbra/issues/112)
- [Keep the local walking capsule above terrain at world entry](https://github.com/d-addison/luminumbra/issues/113)
- [Resolve native MSVC terrain-height hash divergence without weakening pins](https://github.com/d-addison/luminumbra/issues/115)
- [Generate and verify a conformant SPDX source-release inventory](https://github.com/d-addison/luminumbra/issues/119)
- [Apply render benchmark camera and time before streaming and rendering](https://github.com/d-addison/luminumbra/issues/120)
- [Verify slice compliance with the accepted distant-world contract](https://github.com/d-addison/luminumbra/issues/126)
- [Establish the cause of the native client hang and repair the per-frame audio log](https://github.com/d-addison/luminumbra/issues/127)
- [Qualify native profiles, displays and traversals using landed render measurements](https://github.com/d-addison/luminumbra/issues/128)
- [Capture representative budgets using landed simulation telemetry](https://github.com/d-addison/luminumbra/issues/129)
- [Qualify integrated depth ordering and visual parity](https://github.com/d-addison/luminumbra/issues/130)
- [Stream volumetric coarse SDF tiers to a 16 km view radius with distant cave interiors](https://github.com/d-addison/luminumbra/issues/131)
- [Bake terrain edits into every far tier and persist the far authority overlay](https://github.com/d-addison/luminumbra/issues/132)
- [Measure, propose and enforce far-tier residency and streaming budgets](https://github.com/d-addison/luminumbra/issues/133)
- [Qualify persisted-clock continuation across simulation consumers](https://github.com/d-addison/luminumbra/issues/134)
- [Track persistent active regions with ranked cadence, freeze and save-on-evict](https://github.com/d-addison/luminumbra/issues/135)
- [Host wind, weather and aether ambience as stateful world-anchored pages](https://github.com/d-addison/luminumbra/issues/136)
- [Persist creatures and simulate distant wildlife coarsely with promotion](https://github.com/d-addison/luminumbra/issues/137)
- [Run plants and water in distant active regions under bounded cadence and budget](https://github.com/d-addison/luminumbra/issues/138)
- [Add a minimal persisted ground-object entity](https://github.com/d-addison/luminumbra/issues/139)
- [Fold distant-simulation state into world hashes and expose a region diagnostic panel](https://github.com/d-addison/luminumbra/issues/140)
- [Enforce engine budgets and qualify expanded v0.3 acceptance](https://github.com/d-addison/luminumbra/issues/141)

- [Qualify ambient SIMD determinism without changing established goldens](https://github.com/d-addison/luminumbra/issues/163)

### Foundations — Engine/game separation and tooling

- [Refresh architecture and SDK roadmap for the single implementation approval](https://github.com/d-addison/luminumbra/issues/72)
- [Introduce native module lifecycle and deterministic scheduling contracts](https://github.com/d-addison/luminumbra/issues/73)
- [Export an installed SDK and build an independent consumer](https://github.com/d-addison/luminumbra/issues/74)
- [Register configuration, content and authoritative state through engine interfaces](https://github.com/d-addison/luminumbra/issues/75)
- [Establish shared UI focus, capture, cursor and navigation services](https://github.com/d-addison/luminumbra/issues/76)
- [Provide typed commands, console help/history and bounded logs](https://github.com/d-addison/luminumbra/issues/77)
- [Register inspection panels, truthful profiling and local capture bundles](https://github.com/d-addison/luminumbra/issues/78)
- [Enforce game-free engine targets, headers, content and startup in CI](https://github.com/d-addison/luminumbra/issues/79)
- [Provide optional channel-neutral field simulation services](https://github.com/d-addison/luminumbra/issues/104)
- [Expose reusable resource, lifecycle and AI behavior building blocks](https://github.com/d-addison/luminumbra/issues/105)
- [Establish script and component authoring without engine recompilation](https://github.com/d-addison/luminumbra/issues/106)
- [Provide scheduled game-event mechanisms](https://github.com/d-addison/luminumbra/issues/142)
- [Preserve scene placement when compiling static glTF meshes](https://github.com/d-addison/luminumbra/issues/143)
- [Normalize skin imports and preserve animation interpolation](https://github.com/d-addison/luminumbra/issues/144)
- [Add the installed geometry build service with atomic generation publication](https://github.com/d-addison/luminumbra/issues/145)
- [Add Blender geometry authoring through installed tools](https://github.com/d-addison/luminumbra/issues/146)
- [Preserve inline texture alpha coverage at authored cutoffs](https://github.com/d-addison/luminumbra/issues/147)

- [Qualify remaining visual and native Linux acceptance for the merged Blender identity review/undo package (#146)](https://github.com/d-addison/luminumbra/issues/146)
- [Merged optional prefab runtime consumption package; graphical consumption remains separate (#145)](https://github.com/d-addison/luminumbra/pull/165)

### v0.4.0 — Continuing-world multiplayer

- [Capture and restore versioned complete runtime checkpoints](https://github.com/d-addison/luminumbra/issues/80)
- [Admit first-time players to continuing authoritative worlds](https://github.com/d-addison/luminumbra/issues/81)
- [Reconnect identities through ordered catch-up and replication](https://github.com/d-addison/luminumbra/issues/82)
- [Provide LAN discovery and a self-hostable public directory service](https://github.com/d-addison/luminumbra/issues/83)
- [Complete server-authoritative physical avatar input and prediction](https://github.com/d-addison/luminumbra/issues/84)
- [Qualify live GNS transport and bounded multiplayer fault/load behavior](https://github.com/d-addison/luminumbra/issues/85)
- [Qualify local movement and idle support on sloped terrain](https://github.com/d-addison/luminumbra/issues/114)

### v0.5.0 — Environmental audio

- [Implement diffuse RT60 reverb with allocation-free DSP](https://github.com/d-addison/luminumbra/issues/86)
- [Complete wind/rain audio loop lifecycle and smooth transitions](https://github.com/d-addison/luminumbra/issues/87)
- [Qualify bounded thunder reflections, absorption and propagation timing](https://github.com/d-addison/luminumbra/issues/88)
- [Verify audio decoding, routing and graph teardown without devices](https://github.com/d-addison/luminumbra/issues/89)

### v0.6.0 — Dynamic motion history

- [Track previous rendered transforms and bone palettes](https://github.com/d-addison/luminumbra/issues/90)
- [Add per-resource history validity and depth disocclusion](https://github.com/d-addison/luminumbra/issues/91)
- [Qualify temporal quality, bounded history and performance](https://github.com/d-addison/luminumbra/issues/92)

### v0.7.0 — Rendering foundation — provisional

- [Select and deliver a production rendering backend](https://github.com/d-addison/luminumbra/issues/93)
- [Expose truthful capability-based graphics settings](https://github.com/d-addison/luminumbra/issues/94)
- [Design and qualify longer views, depth precision and residency boundaries](https://github.com/d-addison/luminumbra/issues/95)
- [Qualify backend/platform parity and scalable hardware tiers](https://github.com/d-addison/luminumbra/issues/96)

### v0.8.0 — Reconstruction and latency — provisional

- [Qualify NVIDIA and cross-vendor reconstruction integrations](https://github.com/d-addison/luminumbra/issues/97)
- [Integrate dynamic resolution with valid reconstruction history](https://github.com/d-addison/luminumbra/issues/98)
- [Integrate and measure supported latency controls](https://github.com/d-addison/luminumbra/issues/99)
- [Qualify optional frame generation after base-frame and UI prerequisites](https://github.com/d-addison/luminumbra/issues/100)

### v0.9.0 — Lighting and effects — provisional

- [Improve scalable raster lighting and shadows](https://github.com/d-addison/luminumbra/issues/101)
- [Qualify optional hybrid ray tracing and denoising](https://github.com/d-addison/luminumbra/issues/102)
- [Provide bounded field-to-render effect extension interfaces](https://github.com/d-addison/luminumbra/issues/103)
