# Coordinated delivery roadmap

Updated September 8, 2026. This is a delivery/status map with actionable issues in
both repositories. Engine, game and art-pack versions need not share tag numbers.
There are no assigned due dates or invented completion percentages.

## Current state

- **Released:** engine v0.2.1; separate game Tree Small 02 source/runtime packs
  `tree-small-02-source-v1.0.0` and `tree-small-02-runtime-v1.0.0`.
- **Implemented, awaiting final integration/acceptance:** candidate `2c32211`
  (repair branch, [#56](https://github.com/d-addison/luminumbra/pull/56)) contains devel `fdb16a8` plus asynchronous catalog
  validation, saved-world selection/loading, content acquisition, culling/no-UI and
  rendering repairs, the local-player feet-origin correction, explicit MSVC terrain
  arithmetic, validated source-release inventory, the benchmark camera/time
  ordering fix, the audio-off default and the refreshed public asset inventory.
  Game source and runtime pack tags target `9f3a66d` and `addab429`. None is a
  final verified engine v0.3 release.
- **Merged:** format retirement and optional Blender authoring fixtures/service mock
  are in `devel` (`fdb16a8`). Rendering/integration PR
  [#56](https://github.com/d-addison/luminumbra/pull/56) and promotion PR
  [#55](https://github.com/d-addison/luminumbra/pull/55) remain open at this update;
  #55 is blocked until the expanded scope below is accepted.
- **Verified in a bounded scope:** at `2c32211`, local Debug and ASan each pass all
  1,987 named tests with zero failures/errors/skips; format, public-tree and
  asset-inventory checks pass; the exact source-release rehearsal passes with a
  byte-reproducible SPDX inventory. Published source/runtime pack downloads match
  their pinned archives. Independent Astra/xhigh reviews accepted the bounded
  source corrections. Final native, packaged and integrated checks remain distinct.
- **Expanded before release (owner decision, 2026-09-08):** an unbounded world with a
  16 km view radius including distant cave interiors and edits, distant simulation
  over persistent active regions decided system by system, a measured 60 fps floor on
  two profiles and two displays, and causal closure of the native client hang. The
  accepted contract is [Distant world and distant simulation](distant-world.md);
  its work packages are the new v0.3.0 issues below and are gated by one bounded
  design review, not by the post-release Foundations approval.
- **Not released:** engine v0.3.0. Signed publication, independent release download
  verification and a newly named private preview remain outstanding. Preserve the
  delivered `5daeb03` preview unchanged.
- **Planned:** installed engine SDK, real game extraction and the shared
  UI/tooling/authoring foundation. Game code currently remains in the engine
  repository; the game repository initially supplies content and conversion tools.

An implementation is code present in a revision. Verification identifies the
tested revision, environment, scope and evidence. Merging integrates code into a
branch. Release means published artifacts with independently checked identities.
None of these states substitutes for the next.

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

Foundations is dependent on independently verified v0.3 publication **and** the
single implementation-roadmap approval after refreshed source/SDK architecture
review with actual Astra/xhigh. Accepted ownership/authoring direction is recorded
in [Engine/game ownership](engine-game-boundary.md). Roadmap population does not
waive that implementation gate. v0.7–v0.9 remain provisional.

The foundation precedes substantial v0.4 work and does not renumber or reduce its
commitments. Keep authoritative first-time joins, continuing reconnect, strict
two-established-peer lockstep reconnect, discovery/directory/browser, physical
generated avatars, ordinary-protocol AI clients and runnable previews after
completed slices. Public discovery does not itself provide NAT traversal or
imply host migration, matchmaking or private Steam relay availability.

## Limits and acceptance still tracked

- Today surface terrain is finite (approximately 3 km and a 3200 m camera far
  plane), local full-SDF cave residency is bounded (128 m horizontal/64 m vertical),
  far tiles omit caves and player edits, and far-tile persistence is never attached
  at runtime. The accepted v0.3 contract replaces this with a 16 km volumetric
  ladder over an unbounded world; until those slices land and are verified, the
  current limits stand.
- Simulation today ticks every creature and plant regardless of distance, restarts
  the simulation clock at zero on load, anchors weather/wind/aether on the spawn
  point, does not persist creatures and drops dirty chunks on eviction. The accepted
  contract replaces this with persistent active regions, a persisted clock and
  per-system distant policies; until those slices land, the current behaviour stands.
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
- [Review the distant-world and distant-simulation contract before implementation](https://github.com/d-addison/luminumbra/issues/126)
- [Establish the cause of the native client hang and repair the per-frame audio log](https://github.com/d-addison/luminumbra/issues/127)
- [Record per-frame render timing distributions, profiles, displays and traversal workloads](https://github.com/d-addison/luminumbra/issues/128)
- [Measure per-system simulation tick budgets on representative worlds](https://github.com/d-addison/luminumbra/issues/129)
- [Adopt reversed-Z float depth as the far-plane precision prerequisite](https://github.com/d-addison/luminumbra/issues/130)
- [Stream volumetric coarse SDF tiers to a 16 km view radius with distant cave interiors](https://github.com/d-addison/luminumbra/issues/131)
- [Bake terrain edits into every far tier and persist the far authority overlay](https://github.com/d-addison/luminumbra/issues/132)
- [Measure, propose and enforce far-tier residency and streaming budgets](https://github.com/d-addison/luminumbra/issues/133)
- [Persist the simulation clock and unify the calendar](https://github.com/d-addison/luminumbra/issues/134)
- [Track persistent active regions with ranked cadence, freeze and save-on-evict](https://github.com/d-addison/luminumbra/issues/135)
- [Host wind, weather and aether ambience as stateful world-anchored pages](https://github.com/d-addison/luminumbra/issues/136)
- [Persist creatures and simulate distant wildlife coarsely with promotion](https://github.com/d-addison/luminumbra/issues/137)
- [Run plants and water in distant active regions under bounded cadence and budget](https://github.com/d-addison/luminumbra/issues/138)
- [Add a minimal persisted ground-object entity](https://github.com/d-addison/luminumbra/issues/139)
- [Fold distant-simulation state into world hashes and expose a region diagnostic panel](https://github.com/d-addison/luminumbra/issues/140)
- [Activate the expanded v0.3 simulation through game data and record acceptance](https://github.com/d-addison/luminumbra/issues/141)

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

