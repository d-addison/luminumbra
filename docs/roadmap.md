# Coordinated delivery roadmap

Updated October 15, 2026. This map distinguishes implemented release scope,
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
- **v0.3.0 defaults:** Every shipping v0.3.0 feature is enabled by default. Debug
  toggles and non-shipping Vulkan, Steam and GNS backends remain opt-in. Ordinary
  client launches enable audio playback; captures explicitly use `--no-audio`.
  Explicit false config values and saved user settings retain their opt-outs. The dedicated server autosaves every five minutes.
  Ladder is the default requested far-world mode. Missing immutable authority in normal
  presets still causes an explicit refusal and effective Legacy fallback (roughly 3 km
  range and a 3,200 m far plane); the default request does not establish 16 km native
  coverage. Saved-world far production authority remains fixture-only until FW S8
  (v0.3.1); the renderer reports an explicit unavailable/refusal diagnostic.
- **Session compatibility:** Effective SystemConfig identity is separate from the
  legacy config sub-hash and presentation settings. TCP admission, lockstep and
  replay bind simulation rules and resolved preset content, including biome/structure
  dependency bytes. Runtime/soak TCP Hello has a 10-second deadline (issue #315).
  Revision 7 enables preset features while revision 6 retains omission semantics;
  current-format saves lacking embedded packaged presets use frozen revision-6
  copies. Replication uses acknowledged deltas and radius-3 chunk AOI.
- **Distant simulation:** Persisted clock, active-region ledger, edited-content digest
  and scheduler-fault foundations are implemented. Indexed authority
  durability/rehydration, durable completed work and ground-object continuation, durable
  regional fields/weather/scent/energy, and regional foliage/water continuation with
  host persistence (issue #242) are planned for v0.3.1. Strict distant policy and distant creatures
  (S8), remaining S9 closure, the committed-generation edited-SDF producer and FW S8
  consumer, the render qualification tooling stack, acceptance-matrix completeness, the
  developer console, asymmetric day/night and the calendar-relative lunar cycle,
  GNS/Steam admission identity, and the MG2 E4 candidate directory are also planned for
  v0.3.1.
- **Qualification:** Complete native render qualification, the ladder horizon gate,
  authoritative ground/high/cave-air poses and fixed-view workloads, cave-walk and
  edited-world traversal producers, and outstanding acceptance capture/pinning are
  deferred to v0.3.1. Current-candidate Windows results are pending native verification; the owner
  runs the required release smoke on Windows.
  These obligations are not recorded as passed. The five-tier contract retains
  16,384 m outer coverage and qualification within 32 km of the origin. Quality
  (1.0) and Performance (0.67), at 3840×1600 and 3440×1440, retain the p99
  ≤16.67 ms floor and separately reported 8.33 ms target for deferred qualification.
- **Client hang:** the per-frame logging repair and watchdog instrumentation are
  implemented. The original hang mechanism remains unproven. Retain watchdog capture and
  local-disk logs for native runs; current-candidate packaged hang and teardown
  qualification remain pending.
- **Publication:** v0.3.0 is not yet published. The v0.3.0 tag gate requires all
  required CI checks green on the release commit with strict promotion, server-load
  qualification, the paired performance gates, independent release verification,
  signed source-only publication with independent release-download verification, and
  an owner native smoke on Windows: launch the client, create a world, edit, save,
  quit, reload and confirm the edit persists; run the server smoke. These checks remain
  required and are not reported as passed here; see the
  [acceptance record](v0.3-acceptance.md) for the full gate.
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
three timed-out processes emitted captures before termination. Separate historical
native runs passed the unchanged loading guard and original render cases on NVIDIA
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
| [v0.3.0 — Acceptance and release](https://github.com/d-addison/luminumbra/milestone/1) | Reconcile implemented world/render/persistence/authoring work, shipping defaults, session/content identity, TCP Hello admission/deadline, revision-6 preset fallback, replication defaults and five-minute server autosave; publish a signed source release with documented compatibility breaks and limitations after the scoped tag gate. | [Content/save/visual acceptance remains separate; source/runtime pack verification is historical evidence.](https://github.com/d-addison/luminumbra-game/milestone/1) |
| v0.3.1 — Deferred qualification and continuation | Indexed authority durability/rehydration, durable completed work and ground-object continuation, durable regional fields/weather/scent/energy, and regional foliage/water continuation with host persistence (issue #242) are planned for v0.3.1. Strict distant policy and distant creatures (S8), remaining S9 closure, the committed-generation edited-SDF producer and FW S8 consumer, the render qualification tooling stack, acceptance-matrix completeness, the developer console, asymmetric day/night and the calendar-relative lunar cycle, GNS/Steam admission identity, and the MG2 E4 candidate directory are also planned for v0.3.1. Render qualification; acceptance captures and pinning; ladder horizon gate; authoritative poses and fixed-view workloads; cave-walk and edited-world traversal producers; native Windows evidence not produced for v0.3.0. | Composed scenes, authored workloads and game integration evidence for the deferred scopes. |
| [Foundations — Engine/game separation and tooling](https://github.com/d-addison/luminumbra/milestone/2) | Installed SDK; lifecycle/schedule/state registration; optional field/behavior modules; script/component authoring; input, commands, inspection/profiling; dependency enforcement. | [Populated compatibility fixtures; mechanics/state/content extraction; pinned standalone launchers; script/data composition; screens/HUD/diagnostics/tests.](https://github.com/d-addison/luminumbra-game/milestone/2) |
| [v0.4.0 — Continuing-world multiplayer](https://github.com/d-addison/luminumbra/milestone/3) | Regional continuation performance/default enablement (issue #320); checkpoints/catch-up; first-time authoritative admission; returning-identity and two-established-peer lockstep reconnect; physical avatars; LAN/directory and live transport/fault/load qualification. | [Real host/join/reconnect flows; LAN/public/direct/favorites/recent browser; generated animated physical avatars; network AI clients; composed previews and process acceptance.](https://github.com/d-addison/luminumbra-game/milestone/3) |
| [v0.5.0 — Environmental audio](https://github.com/d-addison/luminumbra/milestone/4) | Diffuse RT60 DSP, weather-loop lifecycle, thunder propagation and device-independent decoding/graph acceptance. | [Soundscape/event integration, authored acoustic tuning and audible packaged acceptance.](https://github.com/d-addison/luminumbra-game/milestone/4) |
| [v0.6.0 — Dynamic motion history](https://github.com/d-addison/luminumbra/milestone/5) | Previous rendered transforms/bones, history validity, depth disocclusion and reset/resource/performance qualification. | [Animated avatar/wildlife fixtures and real gameplay/world-transition history acceptance.](https://github.com/d-addison/luminumbra-game/milestone/5) |
| [v0.7.0 — Rendering foundation — provisional](https://github.com/d-addison/luminumbra/milestone/6) | Production backend/capability settings, residual longer-view scope beyond the documented five-tier ladder; deferred v0.3.1 qualification remains separately tracked and native Windows/Linux/Proton/hardware qualification. | [Composed scene/UI parity, terrain/material/weather/water/long-view acceptance and scalable settings.](https://github.com/d-addison/luminumbra-game/milestone/6) |
| [v0.8.0 — Reconstruction and latency — provisional](https://github.com/d-addison/luminumbra/milestone/7) | Qualified reconstruction, dynamic resolution, latency and prerequisite-gated optional frame generation. | [Effective player settings and representative visual/input workloads.](https://github.com/d-addison/luminumbra-game/milestone/7) |
| [v0.9.0 — Lighting and effects — provisional](https://github.com/d-addison/luminumbra/milestone/8) | Improved raster lighting, optional hybrid RT/denoising and reusable field-effect interfaces. | [Game lighting integration and proposed aether visuals, provisionally placed pending architecture review.](https://github.com/d-addison/luminumbra-game/milestone/8) |

Full installed SDK delivery and game extraction in Foundations depend on
independently verified v0.3 publication **and** the agreed architecture and
implementation approval. Accepted ownership/authoring direction is recorded
in [Engine/game ownership](engine-game-boundary.md). Roadmap population does not
waive that implementation gate. The bounded optional authoring
packages above may be refreshed and qualified before release. v0.7–v0.9 remain provisional.

The foundation precedes substantial v0.4 work and does not renumber or reduce its
commitments. Keep authoritative first-time joins, continuing reconnect, strict
two-established-peer lockstep reconnect, discovery/directory/browser, physical
generated avatars, ordinary-protocol AI clients and runnable previews after
completed slices. Public discovery does not itself provide NAT traversal or
imply host migration, matchmaking or private Steam relay availability.

## Limits and acceptance still tracked

- Generation, meshing, queues, residency, upload, water, fog, shadow exclusion and
  horizon diagnostics are implemented. Ladder is the default requested far-world mode.
  Missing immutable authority in normal presets still causes an explicit refusal and
  effective Legacy fallback (roughly 3 km range and a 3,200 m far plane); the default
  request does not establish 16 km native coverage. Local full-SDF cave residency
  remains bounded (128 m horizontal/64 m vertical). Complete ladder coverage, cave/edit
  traversal, edited-bake runtime consumption and calibrated native budgets are planned
  for v0.3.1. The ladder horizon gate refuses before launch. See the [implementation and
  acceptance map](distant-world-implementation-status.md).
- The persisted clock, active-region ledger, edited-content digest and scheduler-fault
  foundations are implemented. The S3–S7 durability/continuation stack, strict distant
  policy and distant creatures (S8), and remaining S9 closure are planned for v0.3.1.
- The original client hang mechanism remains unproven. Logging repair and
  instrumentation do not qualify the current candidate; retain watchdog capture and
  local-disk logs during the pending Windows run.
- Source corrections and causal findings for water caustics/foam, soil environment
  lighting and foreground motes are recorded in the acceptance record. Final
  composed visual qualification remains deferred to v0.3.1.
- Ordinary client audio playback is enabled; captures use `--no-audio`.
  Audible quality and event coverage remain pending native verification.
  Dynamic-body acoustic material classification currently uses Stone.
- GNS compilation does not establish live-peer behavior. Steam SDK/live service,
  broad NVIDIA/AMD/Intel hardware qualification and separate native Linux/Windows/
  Proton qualification remain gaps. Resource estimates are not measured total VRAM.
- Server crash-report capture, repeat qualification and UCRT package-attribution
  tooling are implemented. They do not establish the original crash's cause or
  native dump qualification. The UCRT64 120-second guard remains unchanged.

Software releases remain source-only; art packs are separately versioned game
content. Preset revision 7 retains revision 6 omission compatibility. LMR1 v2,
FSD2 payload v3, canonical in-memory serialization and terrain-quality assertions
remain the format contracts. Lockstep Hello is version 4 and negotiates admission limits; LREC1 replay remains
version 3. Older layouts are refused.
Every shipping feature is enabled by default;
obsolete-world migration and legacy save loading are not supported.

## Work-package index

Each issue records owner, recovery state, remaining changes, dependencies,
acceptance and evidence required for closure. Capability and game-integration
issues link to one another; closing a counterpart does not automatically close
its integration. Visual reports retain reproduction context, expected/observed
behavior, revision/evidence identities, cause status, fix revision and packaged
verification.

### v0.3.0 — Acceptance and release

- [issue #58 — Finish verified game-content acquisition and Linux/Windows CI](https://github.com/d-addison/luminumbra/issues/58) — Existing pack verification is recorded; final composed content acceptance remains unqualified.
- [issue #59 — Complete authored tree materials, LODs and full-sphere impostors](https://github.com/d-addison/luminumbra/issues/59) — Existing pack verification is recorded; final composed content acceptance remains unqualified.
- [issue #60 — Bound saved-world catalog validation without blocking the UI](https://github.com/d-addison/luminumbra/issues/60) — Implemented — background validation and responsiveness regressions; final native-package evidence remains separate.
- [issue #61 — Verify packaged saved-world lifecycle and authority replacement](https://github.com/d-addison/luminumbra/issues/61) — Implemented — lifecycle and native refusal fixtures; native evidence not produced for v0.3.0 is deferred to v0.3.1.
- [issue #62 — Verify saved terrain edits across hierarchical culling boundaries](https://github.com/d-addison/luminumbra/issues/62) — Implemented corrections and regressions; complete packaged/native acceptance remains unqualified.
- [issue #63 — Verify no-UI saved-world loading and capture lifecycle](https://github.com/d-addison/luminumbra/issues/63) — Implemented corrections and regressions; complete packaged/native acceptance remains unqualified.
- [issue #65 — Investigate white ground patches in settled native scene](https://github.com/d-addison/luminumbra/issues/65) — Implemented correction; final composed scene approval remains unqualified.
- [issue #66 — Investigate purple shadows in settled native scene](https://github.com/d-addison/luminumbra/issues/66) — Implemented correction; final composed scene approval remains unqualified.
- [issue #67 — Investigate visible stars in settled native ground scene](https://github.com/d-addison/luminumbra/issues/67) — Cause classified as foreground motes; final composed scene approval remains unqualified.
- [issue #68 — Qualify serial loading, networking soak and performance gates](https://github.com/d-addison/luminumbra/issues/68) — Partial — historical soak/comparison evidence; candidate capture/pinning acceptance deferred to v0.3.1.
- [issue #69 — Resolve or qualify the intermittent server-load crash independently](https://github.com/d-addison/luminumbra/issues/69) — Implemented capture and qualification tooling; original cause and native evidence remain unqualified.
- [issue #70 — Independently verify, promote and publish signed source-only v0.3.0](https://github.com/d-addison/luminumbra/issues/70) — Not yet published — scoped v0.3.0 tag gate remains required.
- [issue #71 — Deliver a newly named verified private client/server preview](https://github.com/d-addison/luminumbra/issues/71) — Delivery remains an uncompleted, separate obligation.
- [issue #107 — Apply storm composition consistently to terrain and grass](https://github.com/d-addison/luminumbra/issues/107) — Implemented; final-candidate native acceptance remains separately recorded.
- [issue #108 — Align grass cloud-shadow projection with the terrain cloud field](https://github.com/d-addison/luminumbra/issues/108) — Implemented; final-candidate native acceptance remains separately recorded.
- [issue #109 — Classify the high-view live-to-distant terrain material seam](https://github.com/d-addison/luminumbra/issues/109) — Classified — aerial haze/detail continuity findings recorded; broader visual qualification deferred to v0.3.1.
- [issue #110 — Correct glass shared-depth attachment and resize lifetime](https://github.com/d-addison/luminumbra/issues/110) — Implemented; final-candidate native acceptance remains separately recorded.
- [issue #112 — Keep glass background sampling aligned at reduced render scales](https://github.com/d-addison/luminumbra/issues/112) — Implemented; final-candidate native acceptance remains separately recorded.
- [issue #113 — Keep the local walking capsule above terrain at world entry](https://github.com/d-addison/luminumbra/issues/113) — Implemented; final-candidate native acceptance remains separately recorded.
- [issue #115 — Resolve native MSVC terrain-height hash divergence without weakening pins](https://github.com/d-addison/luminumbra/issues/115) — Implemented; final-candidate native acceptance remains separately recorded.
- [issue #119 — Generate and verify a conformant SPDX source-release inventory](https://github.com/d-addison/luminumbra/issues/119) — Implemented; final-candidate native acceptance remains separately recorded.
- [issue #120 — Apply render benchmark camera and time before streaming and rendering](https://github.com/d-addison/luminumbra/issues/120) — Implemented; final-candidate native acceptance remains separately recorded.
- [issue #127 — Establish the cause of the native client hang and repair the per-frame audio log](https://github.com/d-addison/luminumbra/issues/127) — Logging repair and watchdog instrumentation implemented; original mechanism unproven and current-candidate native qualification pending.
- [issue #131 — Stream volumetric coarse SDF tiers to a 16 km view radius with distant cave interiors](https://github.com/d-addison/luminumbra/issues/131) — Five-tier infrastructure and default Ladder request implemented; normal presets still fall back to Legacy for missing immutable authority. Native Ladder qualification planned for v0.3.1.

- [issue #163 — Qualify ambient SIMD determinism without changing established goldens](https://github.com/d-addison/luminumbra/issues/163) — Implemented portability repair and receipt tooling; outstanding candidate capture/pinning deferred to v0.3.1.

### v0.3.1 — Planned continuation and qualification

- [issue #77 — Provide typed commands, console help/history and bounded logs](https://github.com/d-addison/luminumbra/issues/77) — Planned for v0.3.1.

Indexed authority durability/rehydration, durable completed work and ground-object
continuation, durable regional fields/weather/scent/energy, and regional foliage/water
continuation with host persistence are planned for v0.3.1. Strict distant policy and
distant creatures (S8), remaining S9 closure, the committed-generation edited-SDF
producer and FW S8 consumer, the render qualification tooling stack, acceptance-matrix
completeness, the developer console, asymmetric day/night and the calendar-relative
lunar cycle, GNS/Steam admission identity, and the MG2 E4 candidate directory are also
planned for v0.3.1.

Complete native render qualification, the ladder horizon gate, authoritative
ground/high/cave-air poses and fixed-view workloads, cave-walk and edited-world
traversal producers, and outstanding acceptance capture/pinning are deferred to v0.3.1.
Current-candidate Windows results are pending native verification; the owner runs the required
release smoke on Windows.

Performance work to ship regional continuation on by default is planned for v0.4.0 as
[issue #320](https://github.com/d-addison/luminumbra/issues/320).

- [issue #64 — Complete settled terrain, weather, cave and water visual acceptance](https://github.com/d-addison/luminumbra/issues/64) — Deferred to v0.3.1 — complete native render qualification.
- [issue #126 — Verify slice compliance with the accepted distant-world contract](https://github.com/d-addison/luminumbra/issues/126) — Partial — implementation integrated; full contract qualification deferred to v0.3.1.
- [issue #128 — Qualify native profiles, displays and traversals using landed render measurements](https://github.com/d-addison/luminumbra/issues/128) — Deferred to v0.3.1 — outstanding profile, display and traversal qualification.
- [issue #129 — Capture representative budgets using landed simulation telemetry](https://github.com/d-addison/luminumbra/issues/129) — Deferred to v0.3.1 — outstanding simulation-budget capture qualification.
- [issue #130 — Qualify integrated depth ordering and visual parity](https://github.com/d-addison/luminumbra/issues/130) — Deferred to v0.3.1 — outstanding depth and visual parity qualification.
- [issue #132 — Bake terrain edits into every far tier and persist the far authority overlay](https://github.com/d-addison/luminumbra/issues/132) — Overlay codec/writer and bake derivation implemented; committed-generation edited-SDF producer and FW S8 consumer planned for v0.3.1.
- [issue #133 — Measure, propose and enforce far-tier residency and streaming budgets](https://github.com/d-addison/luminumbra/issues/133) — Implemented accounting and bounded runtime infrastructure; calibrated native budgets deferred to v0.3.1.
- [issue #134 — Qualify persisted-clock continuation across simulation consumers](https://github.com/d-addison/luminumbra/issues/134) — Persisted clock implemented; durable continuation and complete consumer qualification planned for v0.3.1.
- [issue #135 — Track persistent active regions with ranked cadence, freeze and save-on-evict](https://github.com/d-addison/luminumbra/issues/135) — Ranked active-region ledger implemented; indexed authority durability/rehydration and durable completed work planned for v0.3.1.
- [issue #136 — Host wind, weather and aether ambience as stateful world-anchored pages](https://github.com/d-addison/luminumbra/issues/136) — Durable regional fields/weather/scent/energy and complete field qualification planned for v0.3.1.
- [issue #137 — Persist creatures and simulate distant wildlife coarsely with promotion](https://github.com/d-addison/luminumbra/issues/137) — Strict distant policy and distant creatures (S8), promotion and native qualification planned for v0.3.1.
- [issue #138 — Run plants and water in distant active regions under bounded cadence and budget](https://github.com/d-addison/luminumbra/issues/138) — Regional foliage/water continuation with host persistence planned for v0.3.1; performance/default enablement planned for v0.4.0 as issue #320.
- [issue #139 — Add a minimal persisted ground-object entity](https://github.com/d-addison/luminumbra/issues/139) — Durable completed work and ground-object continuation planned for v0.3.1.
- [issue #140 — Fold distant-simulation state into world hashes and expose a region diagnostic panel](https://github.com/d-addison/luminumbra/issues/140) — Clock, ledger, edited-content digest and scheduler-fault foundations implemented; remaining S8/S9 hash/diagnostic closure and qualification planned for v0.3.1.
- [issue #141 — Enforce engine budgets and qualify expanded v0.3 acceptance](https://github.com/d-addison/luminumbra/issues/141) — Expanded qualification deferred to v0.3.1; scoped v0.3.0 tag checks remain required.

### Foundations — Engine/game separation and tooling

- [issue #72 — Refresh architecture and SDK roadmap for the single implementation approval](https://github.com/d-addison/luminumbra/issues/72)
- [issue #73 — Introduce native module lifecycle and deterministic scheduling contracts](https://github.com/d-addison/luminumbra/issues/73)
- [issue #74 — Export an installed SDK and build an independent consumer](https://github.com/d-addison/luminumbra/issues/74)
- [issue #75 — Register configuration, content and authoritative state through engine interfaces](https://github.com/d-addison/luminumbra/issues/75)
- [issue #76 — Establish shared UI focus, capture, cursor and navigation services](https://github.com/d-addison/luminumbra/issues/76)
- [issue #78 — Register inspection panels, truthful profiling and local capture bundles](https://github.com/d-addison/luminumbra/issues/78)
- [issue #79 — Enforce game-free engine targets, headers, content and startup in CI](https://github.com/d-addison/luminumbra/issues/79)
- [issue #104 — Provide optional channel-neutral field simulation services](https://github.com/d-addison/luminumbra/issues/104)
- [issue #105 — Expose reusable resource, lifecycle and AI behavior building blocks](https://github.com/d-addison/luminumbra/issues/105)
- [issue #106 — Establish script and component authoring without engine recompilation](https://github.com/d-addison/luminumbra/issues/106)
- [issue #142 — Provide scheduled game-event mechanisms](https://github.com/d-addison/luminumbra/issues/142)
- [issue #143 — Preserve scene placement when compiling static glTF meshes](https://github.com/d-addison/luminumbra/issues/143)
- [issue #144 — Normalize skin imports and preserve animation interpolation](https://github.com/d-addison/luminumbra/issues/144)
- [issue #145 — Add the installed geometry build service with atomic generation publication](https://github.com/d-addison/luminumbra/issues/145)
- [issue #146 — Add Blender geometry authoring through installed tools](https://github.com/d-addison/luminumbra/issues/146)
- [issue #147 — Preserve inline texture alpha coverage at authored cutoffs](https://github.com/d-addison/luminumbra/issues/147)

- [issue #146 — Qualify remaining visual and native Linux acceptance for the merged Blender identity review/undo package](https://github.com/d-addison/luminumbra/issues/146)
- [Implemented optional prefab consumption, installed static preview and Blender viewport; native qualification remains separate](compiled-prefab-runtime.md)

### v0.4.0 — Continuing-world multiplayer

- [issue #320](https://github.com/d-addison/luminumbra/issues/320) — Planned regional continuation performance work and default enablement.
- [issue #80 — Capture and restore versioned complete runtime checkpoints](https://github.com/d-addison/luminumbra/issues/80)
- [issue #81 — Admit first-time players to continuing authoritative worlds](https://github.com/d-addison/luminumbra/issues/81)
- [issue #82 — Reconnect identities through ordered catch-up and replication](https://github.com/d-addison/luminumbra/issues/82)
- [issue #83 — Provide LAN discovery and a self-hostable public directory service](https://github.com/d-addison/luminumbra/issues/83)
- [issue #84 — Complete server-authoritative physical avatar input and prediction](https://github.com/d-addison/luminumbra/issues/84)
- [issue #85 — Qualify live GNS transport and bounded multiplayer fault/load behavior](https://github.com/d-addison/luminumbra/issues/85)
- [issue #114 — Qualify local movement and idle support on sloped terrain](https://github.com/d-addison/luminumbra/issues/114)

### v0.5.0 — Environmental audio

- [issue #86 — Implement diffuse RT60 reverb with allocation-free DSP](https://github.com/d-addison/luminumbra/issues/86)
- [issue #87 — Complete wind/rain audio loop lifecycle and smooth transitions](https://github.com/d-addison/luminumbra/issues/87)
- [issue #88 — Qualify bounded thunder reflections, absorption and propagation timing](https://github.com/d-addison/luminumbra/issues/88)
- [issue #89 — Verify audio decoding, routing and graph teardown without devices](https://github.com/d-addison/luminumbra/issues/89)

### v0.6.0 — Dynamic motion history

- [issue #90 — Track previous rendered transforms and bone palettes](https://github.com/d-addison/luminumbra/issues/90)
- [issue #91 — Add per-resource history validity and depth disocclusion](https://github.com/d-addison/luminumbra/issues/91)
- [issue #92 — Qualify temporal quality, bounded history and performance](https://github.com/d-addison/luminumbra/issues/92)

### v0.7.0 — Rendering foundation — provisional

- [issue #93 — Select and deliver a production rendering backend](https://github.com/d-addison/luminumbra/issues/93)
- [issue #94 — Expose truthful capability-based graphics settings](https://github.com/d-addison/luminumbra/issues/94)
- [issue #95 — Design and qualify longer views, depth precision and residency boundaries](https://github.com/d-addison/luminumbra/issues/95)
- [issue #96 — Qualify backend/platform parity and scalable hardware tiers](https://github.com/d-addison/luminumbra/issues/96)

### v0.8.0 — Reconstruction and latency — provisional

- [issue #97 — Qualify NVIDIA and cross-vendor reconstruction integrations](https://github.com/d-addison/luminumbra/issues/97)
- [issue #98 — Integrate dynamic resolution with valid reconstruction history](https://github.com/d-addison/luminumbra/issues/98)
- [issue #99 — Integrate and measure supported latency controls](https://github.com/d-addison/luminumbra/issues/99)
- [issue #100 — Qualify optional frame generation after base-frame and UI prerequisites](https://github.com/d-addison/luminumbra/issues/100)

### v0.9.0 — Lighting and effects — provisional

- [issue #101 — Improve scalable raster lighting and shadows](https://github.com/d-addison/luminumbra/issues/101)
- [issue #102 — Qualify optional hybrid ray tracing and denoising](https://github.com/d-addison/luminumbra/issues/102)
- [issue #103 — Provide bounded field-to-render effect extension interfaces](https://github.com/d-addison/luminumbra/issues/103)
