# Coordinated delivery roadmap

Updated October 14, 2026. This map distinguishes implemented release scope,
deferred acceptance and publication. Engine, game and art-pack versions need not
share tag numbers. There are no assigned due dates or invented completion percentages.

## Current state

- **Implemented:** recovery integration, ambient SIMD and wind-input repairs,
  source-release validation, active-region scheduling foundations, volumetric
  generation and meshing, canonical build queues, residency and upload accounting,
  renderer integration, far-water submission, shadow exclusion, horizon diagnostics,
  saved-world lifecycle and native refusal fixtures, and installed Blender viewport
  tooling are integrated. Historical verification records retain their original
  revisions and scope.
- **v0.3.0 defaults:** Shipping features are enabled by default; debug toggles, Vulkan, Steam and GNS remain disabled. Audio playback keeps its separate disabled-by-default release policy. The volumetric far-world ladder is the default mode; the legacy far-world renderer is retired behind compatibility settings.
- **Distant simulation:** The distant-simulation S3–S7 durability stack is implemented: indexed authority durability, durable completed work and ground-object continuation, durable regional fields with causal weather and scent, and regional foliage and water continuation with host persistence.
- **Qualification:** Complete render qualification, the far-world edited-bake runtime consumer, distant-simulation S8/S9, the ladder horizon gate, authoritative ground/high/cave-air poses and fixed-view workloads, cave-walk and edited-world traversal producers, outstanding acceptance capture/pinning, and native Windows evidence not produced for v0.3.0 are deferred to v0.3.1.
  These obligations are not recorded as passed. The five-tier contract retains
  16,384 m outer coverage and qualification within 32 km of the origin. Quality
  (1.0) and Performance (0.67), at 3840×1600 and 3440×1440, retain the p99
  ≤16.67 ms floor and separately reported 8.33 ms target for deferred qualification.
- **Client hang:** the acceptance record documents owner-accepted closure after
  unsuccessful reproduction following the logging repair. Retain watchdog capture
  and local-disk logs for native runs; a recurrence requires investigation.
  Dedicated-server crash qualification remains separate.
- **Publication:** v0.3.0 is not yet published. The v0.3.0 tag gate requires all required CI checks green on the release commit, a signed source tag, independent release-download verification, and an owner native smoke on Windows: launch the client, create a world, edit, save, quit, reload and confirm the edit persists; run the server smoke. These checks remain required and are not reported as passed here.
  Draft CI and batched devel CI policies are implemented; required checks follow
  the final applicable policy rather than a fixed context count. Release notes
  belong in the GitHub release body.
- **Authoring:** compiled-prefab consumption, installed static preview, persistent
  viewport hosting, the Blender RenderEngine adapter and bounded recipe tooling
  are implemented. Complete native editor acceptance, Windows runtime closure,
  performance targets and visual approval remain unqualified where no packet exists.
  Full engine/game extraction remains Foundations work.

### Historical recovery verification

The September 13 UCRT job passed 2,245 of 2,250 cases at its recorded revision;
the surface-loading guard and three render-process timeouts did not pass. All
three timed-out processes emitted captures before termination. Local native
`e1080d7f` passed the unchanged loading guard and original render cases on NVIDIA
and CI-pinned Mesa 26.1.7 llvmpipe. Different hardware and toolchains keep these
non-reproductions separate from CI and original-hang causal closure. Later
Debug/ASan and native records supersede the failed `ff0362b` baseline in their
recorded scopes. None of these historical records qualifies a new candidate.
The Blender identity review/undo package retains its 22 recorded native checks;
visual approval and Linux native editor qualification remain outstanding.

An implementation is code present in a revision. Verification identifies the
tested revision, environment, scope and evidence. Merging integrates code into a
branch. Publication delivers artifacts with independently checked identities.

The engine owns budget enforcement and qualification mechanisms; the game owns
authored rates, activation data and populator policy. Future content/data
distribution and optional non-Steam loading remain game obligations. All v0.4
commitments remain assigned.

## Milestones and ownership

| Coordinated milestone | This repository | Counterpart repository |
|---|---|---|
| [v0.3.0 — Acceptance and release](https://github.com/d-addison/luminumbra/milestone/1) | Reconcile implemented world/render/persistence/authoring work, shipping defaults and the durability stack; publish a signed source release with documented compatibility breaks and limitations after the scoped tag gate. | [Content/save/visual acceptance remains separate; source/runtime pack verification is historical evidence.](https://github.com/d-addison/luminumbra-game/milestone/1) |
| v0.3.1 — Deferred qualification and continuation | Render qualification; acceptance captures and pinning; distant-simulation S8/S9; far-world edited-bake runtime consumption; ladder horizon gate; authoritative poses and fixed-view workloads; cave-walk and edited-world traversal producers; native Windows evidence not produced for v0.3.0. | Composed scenes, authored workloads and game integration evidence for the deferred scopes. |
| [Foundations — Engine/game separation and tooling](https://github.com/d-addison/luminumbra/milestone/2) | Installed SDK; lifecycle/schedule/state registration; optional field/behavior modules; script/component authoring; input, commands, inspection/profiling; dependency enforcement. | [Populated compatibility fixtures; mechanics/state/content extraction; pinned standalone launchers; script/data composition; screens/HUD/diagnostics/tests.](https://github.com/d-addison/luminumbra-game/milestone/2) |
| [v0.4.0 — Continuing-world multiplayer](https://github.com/d-addison/luminumbra/milestone/3) | Checkpoints/catch-up; first-time authoritative admission; returning-identity and two-established-peer lockstep reconnect; physical avatars; LAN/directory and live transport/fault/load qualification. | [Real host/join/reconnect flows; LAN/public/direct/favorites/recent browser; generated animated physical avatars; network AI clients; composed previews and process acceptance.](https://github.com/d-addison/luminumbra-game/milestone/3) |
| [v0.5.0 — Environmental audio](https://github.com/d-addison/luminumbra/milestone/4) | Diffuse RT60 DSP, weather-loop lifecycle, thunder propagation and device-independent decoding/graph acceptance. | [Soundscape/event integration, authored acoustic tuning and audible packaged acceptance.](https://github.com/d-addison/luminumbra-game/milestone/4) |
| [v0.6.0 — Dynamic motion history](https://github.com/d-addison/luminumbra/milestone/5) | Previous rendered transforms/bones, history validity, depth disocclusion and reset/resource/performance qualification. | [Animated avatar/wildlife fixtures and real gameplay/world-transition history acceptance.](https://github.com/d-addison/luminumbra-game/milestone/5) |
| [v0.7.0 — Rendering foundation — provisional](https://github.com/d-addison/luminumbra/milestone/6) | Production backend/capability settings, residual longer-view scope beyond the documented five-tier ladder; deferred v0.3.1 qualification remains separately tracked and native Windows/Linux/Proton/hardware qualification. | [Composed scene/UI parity, terrain/material/weather/water/long-view acceptance and scalable settings.](https://github.com/d-addison/luminumbra-game/milestone/6) |
| [v0.8.0 — Reconstruction and latency — provisional](https://github.com/d-addison/luminumbra/milestone/7) | Qualified reconstruction, dynamic resolution, latency and prerequisite-gated optional frame generation. | [Effective player settings and representative visual/input workloads.](https://github.com/d-addison/luminumbra-game/milestone/7) |
| [v0.9.0 — Lighting and effects — provisional](https://github.com/d-addison/luminumbra/milestone/8) | Improved raster lighting, optional hybrid RT/denoising and reusable field-effect interfaces. | [Game lighting integration and proposed aether visuals, provisionally placed pending architecture review.](https://github.com/d-addison/luminumbra-game/milestone/8) |

Full installed SDK delivery and game extraction in Foundations depend on
independently verified v0.3 publication **and** the agreed architecture and implementation approval. Accepted ownership/authoring direction is recorded
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

- The five-tier volumetric renderer and its queue, residency, upload, fog, shadow
  and water infrastructure are implemented and selected by default. The legacy
  renderer is retired behind compatibility settings. Local full-SDF cave residency
  remains bounded (128 m horizontal/64 m vertical). Complete native ladder coverage,
  cave/edit traversal and calibrated budgets are deferred to v0.3.1. Durable overlay
  codecs, writers and bake derivation do not establish edited-bake runtime consumption.
  The ladder horizon gate refuses before launch today; its qualification is a
  documented v0.3.1 limitation. See the
  [implementation and acceptance map](distant-world-implementation-status.md).
- The persisted clock, active-region ledger, edited-content digest, scheduler fault
  handling and S3–S7 durability stack are implemented. S8/S9 and complete native
  continuation qualification are deferred to v0.3.1.
- The client hang has owner-accepted closure after unsuccessful reproduction
  following the logging repair, without a proven original mechanism. Retain watchdog
  capture and local-disk logs; a recurrence requires investigation.
- Source corrections and causal findings for water caustics/foam, soil environment
  lighting and foreground motes are recorded in the acceptance record. Final
  composed visual qualification remains deferred to v0.3.1.
- Audio playback is disabled by default under its separate release policy.
  Audible quality and event coverage remain future environmental-audio work.
  Dynamic-body acoustic material classification currently uses Stone.
- GNS compilation does not establish live-peer behavior. Steam SDK/live service,
  broad NVIDIA/AMD/Intel hardware qualification and separate native Linux/Windows/
  Proton qualification remain gaps. Resource estimates are not measured total VRAM.
- Server crash-report capture, repeat qualification and UCRT package-attribution
  tooling are implemented. They do not establish the original crash's cause or
  native dump qualification. The UCRT64 120-second guard remains unchanged.

Software releases remain source-only; art packs are separately versioned game
content. Preset revision 6, LMR1 v2, FSD2 payload v3, canonical in-memory
serialization and terrain-quality assertions remain the format contracts.
Shipping features are enabled by default with the exclusions documented above;
obsolete-world migration and legacy save loading are not supported.

## Work-package index

Each issue records owner, recovery state, remaining changes, dependencies,
acceptance and evidence required for closure. Capability and game-integration
issues link to one another; closing a counterpart does not automatically close
its integration. Visual reports retain reproduction context, expected/observed
behavior, revision/evidence identities, cause status, fix revision and packaged
verification.

### v0.3.0 — Acceptance and release

- [Finish verified game-content acquisition and Linux/Windows CI](https://github.com/d-addison/luminumbra/issues/58) — Existing pack verification is recorded; final composed content acceptance remains unqualified.
- [Complete authored tree materials, LODs and full-sphere impostors](https://github.com/d-addison/luminumbra/issues/59) — Existing pack verification is recorded; final composed content acceptance remains unqualified.
- [Bound saved-world catalog validation without blocking the UI](https://github.com/d-addison/luminumbra/issues/60) — Implemented — background validation and responsiveness regressions; final native-package evidence remains separate.
- [Verify packaged saved-world lifecycle and authority replacement](https://github.com/d-addison/luminumbra/issues/61) — Implemented — lifecycle and native refusal fixtures; native evidence not produced for v0.3.0 is deferred to v0.3.1.
- [Verify saved terrain edits across hierarchical culling boundaries](https://github.com/d-addison/luminumbra/issues/62) — Implemented corrections and regressions; complete packaged/native acceptance remains unqualified.
- [Verify no-UI saved-world loading and capture lifecycle](https://github.com/d-addison/luminumbra/issues/63) — Implemented corrections and regressions; complete packaged/native acceptance remains unqualified.
- [Complete settled terrain, weather, cave and water visual acceptance](https://github.com/d-addison/luminumbra/issues/64) — Deferred to v0.3.1 — complete native render qualification.
- [Investigate white ground patches in settled native scene](https://github.com/d-addison/luminumbra/issues/65) — Implemented correction; final composed scene approval remains unqualified.
- [Investigate purple shadows in settled native scene](https://github.com/d-addison/luminumbra/issues/66) — Implemented correction; final composed scene approval remains unqualified.
- [Investigate visible stars in settled native ground scene](https://github.com/d-addison/luminumbra/issues/67) — Cause classified as foreground motes; final composed scene approval remains unqualified.
- [Qualify serial loading, networking soak and performance gates](https://github.com/d-addison/luminumbra/issues/68) — Partial — historical soak/comparison evidence; candidate capture/pinning acceptance deferred to v0.3.1.
- [Resolve or qualify the intermittent server-load crash independently](https://github.com/d-addison/luminumbra/issues/69) — Implemented capture and qualification tooling; original cause and native evidence remain unqualified.
- [Independently verify, promote and publish signed source-only v0.3.0](https://github.com/d-addison/luminumbra/issues/70) — Not yet published — scoped v0.3.0 tag gate remains required.
- [Deliver a newly named verified private client/server preview](https://github.com/d-addison/luminumbra/issues/71) — Delivery remains an uncompleted, separate obligation.
- [Apply storm composition consistently to terrain and grass](https://github.com/d-addison/luminumbra/issues/107) — Implemented; final-candidate native acceptance remains separately recorded.
- [Align grass cloud-shadow projection with the terrain cloud field](https://github.com/d-addison/luminumbra/issues/108) — Implemented; final-candidate native acceptance remains separately recorded.
- [Classify the high-view live-to-distant terrain material seam](https://github.com/d-addison/luminumbra/issues/109) — Classified — aerial haze/detail continuity findings recorded; broader visual qualification deferred to v0.3.1.
- [Correct glass shared-depth attachment and resize lifetime](https://github.com/d-addison/luminumbra/issues/110) — Implemented; final-candidate native acceptance remains separately recorded.
- [Keep glass background sampling aligned at reduced render scales](https://github.com/d-addison/luminumbra/issues/112) — Implemented; final-candidate native acceptance remains separately recorded.
- [Keep the local walking capsule above terrain at world entry](https://github.com/d-addison/luminumbra/issues/113) — Implemented; final-candidate native acceptance remains separately recorded.
- [Resolve native MSVC terrain-height hash divergence without weakening pins](https://github.com/d-addison/luminumbra/issues/115) — Implemented; final-candidate native acceptance remains separately recorded.
- [Generate and verify a conformant SPDX source-release inventory](https://github.com/d-addison/luminumbra/issues/119) — Implemented; final-candidate native acceptance remains separately recorded.
- [Apply render benchmark camera and time before streaming and rendering](https://github.com/d-addison/luminumbra/issues/120) — Implemented; final-candidate native acceptance remains separately recorded.
- [Verify slice compliance with the accepted distant-world contract](https://github.com/d-addison/luminumbra/issues/126) — Partial — implementation integrated; full contract qualification deferred to v0.3.1.
- [Establish the cause of the native client hang and repair the per-frame audio log](https://github.com/d-addison/luminumbra/issues/127) — Owner-accepted closure after unsuccessful reproduction following the logging repair; original mechanism unproven.
- [Qualify native profiles, displays and traversals using landed render measurements](https://github.com/d-addison/luminumbra/issues/128) — Deferred to v0.3.1 — outstanding profile, display and traversal qualification.
- [Capture representative budgets using landed simulation telemetry](https://github.com/d-addison/luminumbra/issues/129) — Deferred to v0.3.1 — outstanding simulation-budget capture qualification.
- [Qualify integrated depth ordering and visual parity](https://github.com/d-addison/luminumbra/issues/130) — Deferred to v0.3.1 — outstanding depth and visual parity qualification.
- [Stream volumetric coarse SDF tiers to a 16 km view radius with distant cave interiors](https://github.com/d-addison/luminumbra/issues/131) — Implemented infrastructure, default ladder and legacy retirement behind compatibility settings; native qualification deferred to v0.3.1.
- [Bake terrain edits into every far tier and persist the far authority overlay](https://github.com/d-addison/luminumbra/issues/132) — Implemented overlay codec/writer and bake derivation; edited-bake runtime consumer deferred to v0.3.1.
- [Measure, propose and enforce far-tier residency and streaming budgets](https://github.com/d-addison/luminumbra/issues/133) — Implemented accounting and bounded runtime infrastructure; calibrated native budgets deferred to v0.3.1.
- [Qualify persisted-clock continuation across simulation consumers](https://github.com/d-addison/luminumbra/issues/134) — Implemented persisted clock and S3–S7 continuation durability; complete native consumer qualification deferred to v0.3.1.
- [Track persistent active regions with ranked cadence, freeze and save-on-evict](https://github.com/d-addison/luminumbra/issues/135) — Implemented ranked active-region ledger, indexed authority durability and durable completed work; complete native qualification deferred to v0.3.1.
- [Host wind, weather and aether ambience as stateful world-anchored pages](https://github.com/d-addison/luminumbra/issues/136) — Implemented durable regional fields with causal weather and scent; complete native field qualification deferred to v0.3.1.
- [Persist creatures and simulate distant wildlife coarsely with promotion](https://github.com/d-addison/luminumbra/issues/137) — Partial — durability foundations implemented; distant wildlife promotion and remaining S8/S9 scope deferred to v0.3.1.
- [Run plants and water in distant active regions under bounded cadence and budget](https://github.com/d-addison/luminumbra/issues/138) — Implemented regional foliage and water continuation with host persistence; complete native qualification deferred to v0.3.1.
- [Add a minimal persisted ground-object entity](https://github.com/d-addison/luminumbra/issues/139) — Implemented durable ground-object continuation; complete native qualification deferred to v0.3.1.
- [Fold distant-simulation state into world hashes and expose a region diagnostic panel](https://github.com/d-addison/luminumbra/issues/140) — Partial — clock, ledger, edited-content digest and fault handling implemented; S8/S9 hash/diagnostic closure and outstanding qualification deferred to v0.3.1.
- [Enforce engine budgets and qualify expanded v0.3 acceptance](https://github.com/d-addison/luminumbra/issues/141) — Expanded qualification deferred to v0.3.1; scoped v0.3.0 tag checks remain required.

- [Qualify ambient SIMD determinism without changing established goldens](https://github.com/d-addison/luminumbra/issues/163) — Implemented portability repair and receipt tooling; outstanding candidate capture/pinning deferred to v0.3.1.

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

- [Qualify remaining visual and native Linux acceptance for the merged Blender identity review/undo package](https://github.com/d-addison/luminumbra/issues/146)
- [Implemented optional prefab consumption, installed static preview and Blender viewport; native qualification remains separate](compiled-prefab-runtime.md)

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
