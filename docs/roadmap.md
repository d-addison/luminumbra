# Coordinated delivery roadmap

Updated September 7, 2026. This is a delivery/status map with actionable issues in
both repositories. Engine, game and art-pack versions need not share tag numbers.
There are no assigned due dates or invented completion percentages.

## Current state

- **Released:** engine v0.2.1; separate game Tree Small 02 source/runtime packs
  `tree-small-02-source-v1.0.0` and `tree-small-02-runtime-v1.0.0`.
- **Implemented, awaiting final integration/acceptance:** candidate `4ca9215`
  includes asynchronous catalog validation, saved-world selection/loading, content acquisition,
  culling/no-UI and rendering repairs, local-player feet-origin correction, explicit
  MSVC terrain arithmetic and validated source-release inventory. Game source and
  runtime pack tags target `9f3a66d` and `addab429`; their independent verification is
  documented in game `ecdcc9c`. None is a final verified engine v0.3 release.
- **Merged:** format retirement and optional Blender authoring fixtures/service mock
  are in `devel` (`fdb16a8`). The mock is planning evidence, not a production engine
  build/preview service. Rendering/integration PR
  [#56](https://github.com/d-addison/luminumbra/pull/56) and promotion PR
  [#55](https://github.com/d-addison/luminumbra/pull/55) remain open at this update.
- **Verified in a bounded scope:** published source/runtime pack downloads match
  their pinned archives, all 14/22 regular members and companion receipts. Native
  saved-world restart/switching passes on the private `14480a8` package. The MSVC
  correction passes all 33 terrain/player cases with all six grids matching CI.
  Independent Astra/xhigh reviews accepted the bounded source corrections. Final
  integrated checks, packaged visuals/audio and release verification remain distinct.
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
| [v0.3.0 — Acceptance and release](https://github.com/d-addison/luminumbra/milestone/1) | Final saved-world/render/content correctness, native/CI acceptance, signed source release, independent verification and new private preview. | [Source/runtime art-pack verification, final canopy conversion/provenance and packaged content/save/visual/audible acceptance.](https://github.com/d-addison/luminumbra-game/milestone/1) |
| [Foundations — Engine/game separation and tooling](https://github.com/d-addison/luminumbra/milestone/2) | Installed SDK; lifecycle/schedule/state registration; optional field/behavior modules; script/component authoring; input, commands, inspection/profiling; dependency enforcement. | [Populated compatibility fixtures; mechanics/state/content extraction; pinned standalone launchers; script/data composition; screens/HUD/diagnostics/tests.](https://github.com/d-addison/luminumbra-game/milestone/2) |
| [v0.4.0 — Continuing-world multiplayer](https://github.com/d-addison/luminumbra/milestone/3) | Checkpoints/catch-up; first-time authoritative admission; returning-identity and two-established-peer lockstep reconnect; physical avatars; LAN/directory and live transport/fault/load qualification. | [Real host/join/reconnect flows; LAN/public/direct/favorites/recent browser; generated animated physical avatars; network AI clients; composed previews and process acceptance.](https://github.com/d-addison/luminumbra-game/milestone/3) |
| [v0.5.0 — Environmental audio](https://github.com/d-addison/luminumbra/milestone/4) | Diffuse RT60 DSP, weather-loop lifecycle, thunder propagation and device-independent decoding/graph acceptance. | [Soundscape/event integration, authored acoustic tuning and audible packaged acceptance.](https://github.com/d-addison/luminumbra-game/milestone/4) |
| [v0.6.0 — Dynamic motion history](https://github.com/d-addison/luminumbra/milestone/5) | Previous rendered transforms/bones, history validity, depth disocclusion and reset/resource/performance qualification. | [Animated avatar/wildlife fixtures and real gameplay/world-transition history acceptance.](https://github.com/d-addison/luminumbra-game/milestone/5) |
| [v0.7.0 — Rendering foundation — provisional](https://github.com/d-addison/luminumbra/milestone/6) | Production backend/capability settings, longer views/depth/coordinates/residency and native Windows/Linux/Proton/hardware qualification. | [Composed scene/UI parity, terrain/material/weather/water/long-view acceptance and scalable settings.](https://github.com/d-addison/luminumbra-game/milestone/6) |
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

- Surface terrain is finite (approximately 3 km and a 3200 m camera far plane).
  Local full-SDF cave residency is bounded (up to 128 m horizontal/64 m vertical).
  Distant terrain omits caves and player edits. Complete-chunk culling bounds and
  no-UI loading were repaired with causal regressions; holes inside intended
  residency remain correctness failures. Longer views and distant-interior/edit
  scope need explicit architecture allocation and acceptance.
- Settled Default/424242/FOV 110° investigations established separate causes:
  clipped caustics/broad foam caused white shallows; excessive ambient soil
  reflection caused purple shadows; apparent daytime stars were ambient particles.
  Water/lighting fixes and particle classification retain separate evidence. Broad
  high-view pastel lift was isolated primarily to aerial haze. Final packaged
  composition remains required; tree-only tests do not establish scene acceptance.
- Audio decoding/device initialization does not prove audible output or complete
  event coverage. Listening/loopback qualification remains required; dynamic-body
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

- [Preserve glass refraction at reduced render scales](https://github.com/d-addison/luminumbra/issues/112)
- [Keep the local walking capsule above terrain at world entry](https://github.com/d-addison/luminumbra/issues/113)
- [Resolve native MSVC terrain-height divergence without weakening pins](https://github.com/d-addison/luminumbra/issues/115)
- [Generate and verify a conformant SPDX source-release inventory](https://github.com/d-addison/luminumbra/issues/119)

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

The parallel Blender session has draft implementations assigned to Foundations.
They remain subject to the release and roadmap approval gates:

- [Preserve static glTF scene placement](https://github.com/d-addison/luminumbra/pull/116)
- [Normalize skins and preserve animation interpolation](https://github.com/d-addison/luminumbra/pull/117)
- [Provide an installed geometry build service with atomic publication](https://github.com/d-addison/luminumbra/pull/118)

### v0.4.0 — Continuing-world multiplayer

- [Capture and restore versioned complete runtime checkpoints](https://github.com/d-addison/luminumbra/issues/80)
- [Admit first-time players to continuing authoritative worlds](https://github.com/d-addison/luminumbra/issues/81)
- [Reconnect identities through ordered catch-up and replication](https://github.com/d-addison/luminumbra/issues/82)
- [Provide LAN discovery and a self-hostable public directory service](https://github.com/d-addison/luminumbra/issues/83)
- [Complete server-authoritative physical avatar input and prediction](https://github.com/d-addison/luminumbra/issues/84)
- [Qualify live GNS transport and bounded multiplayer fault/load behavior](https://github.com/d-addison/luminumbra/issues/85)

- [Qualify local movement and idle slope support](https://github.com/d-addison/luminumbra/issues/114)

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


Current evidence and open gates: [v0.3 acceptance record](v0.3-acceptance.md).

Additional v0.3 visual findings are tracked separately: [storm grass #107](https://github.com/d-addison/luminumbra/issues/107), [cloud-shadow agreement #108](https://github.com/d-addison/luminumbra/issues/108), [high-view material/haze classification #109](https://github.com/d-addison/luminumbra/issues/109), and [glass depth/lifetime #110](https://github.com/d-addison/luminumbra/issues/110). Follow-up fixes and diagnostic classification do not by themselves complete packaged release acceptance.
