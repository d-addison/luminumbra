# Iteration 6 Remaining Work

Scope: `include_waves=true`, `include_multiplayer=true`. Generated 2026-06-17 from `.forge/artifacts/engine-frontier/handoff.md`, `.forge/artifacts/engine-frontier/ultimate-plan.md`, `.forge/artifacts/engine-iteration-6/*`, `.forge/specs/iter6/*`, `plan.md`, `.forge/workflows/engine-frontier.yaml`, `tools/gates/validate-engine-frontier.ps1`, live source, and live build/CTest/gate runs.

## Hard Constraints

- Commit locally only; never push.
- Builds, `ctest`, and validators must prepend `C:\msys64\ucrt64\bin` to `PATH`.
- Research before spec; every implementation task is test-first and has a local-commit boundary.
- Determinism contract: sim/worldgen changes that move `world_hash` must be deliberate, isolated, documented, and re-blessed once per commit. Render-only and transport-only work must preserve the current canonical `world_hash=f17726d44054d133`.
- Ordered hash surfaces remain Aetheric first, erosion/worldgen second; do not mix them in one commit.
- Visual-debt BLOCKs are discharged only by passing objective gates, primarily `WorldVisualSweep`; do not reclassify, rename, or weaken thresholds to mark visual failures done.
- Steam transport cannot be fully validated on this single PC/single account. Steam over-the-wire validation is `spec+build, defer validation` until a second machine/account is available. Use TCP and optional standalone GNS UDP for local transport proof.

## Live Evidence Snapshot

- Debug build: PASS, `cmake --build --preset debug` reported no work to do with required PATH prefix.
- Full non-placeholder CTest: PASS. `ctest --preset debug --output-on-failure -E "_NOT_BUILT$"` completed in 381.13s with 305/305 executed tests passing; test #180 `JobSystemPoolTest.DispatchThroughputBenchmark` is disabled.
- SHIELD-RT GPU CTests: PASS inside the full CTest run: `ShieldRtTracerProfileGpu`, `ShieldRtFarFieldParityGpu`, `ShieldRtFarFieldGbufferGpu`, and `ShieldRtFarFieldMaxMipGpu`.
- `RenderHealth`: PASS; artifact reports `passed=true`, GL debug errors 0, shader health OK, and resource registry empty after shutdown.
- `FoliageInstancing`: PASS; 21055 instances, measured density 0.643 vs biome 0.300, windy sway 7.9319 m, 0.1078 ms.
- `WorldVisualSweep`: PASS; 48/48 cells produced and non-black, foliage/rain+lightning/cloud/water coverage present, objective critique reports 0/48 defect cells and no blocking flags.
- `HeadlessServerTick`: PASS; 90 ticks x 2 runs, 4498 chunks/run, `world_hash=f17726d44054d133 == world_hash_replay`, sub-hashes match.
- `ReplicationSmoke`: PASS; 4 avatars mirrored, seq=120, acked=120, max position error 0.0004355907440185547 m, controlled avatar moved +15.62 m, 3 GOAP NPCs approached water, arrow despawn signalled.
- `NetworkedReplication`: PASS; separate host/client TCP processes on port 27061, 60 ticks, joiner mirrored seq 60 / 4 entities and avatar id 1 at x=12.57 m.
- `NetworkStateHash` and `NetworkLoopbackAuthorityGate`: PASS.
- `FarLodHorizon`: FAILS on `mountains`; artifact reports `passed=false`, `regions_wanted=40`, `regions_resident=40`, `regions_missing=0`, `max_far_attributable_sliver_px=745` vs threshold 142, and `max_sky_sliver_px=763` vs threshold 569.
- `NetworkedSession`: FAILS validator because the artifact's agreed end hash is stale: `2fa007951a21e140` vs canonical `f17726d44054d133`. The artifact itself reports `passed=true` and equal peer hashes, which confirms this is a stale lockstep-era gate, not proof of the current replication/render path.
- `forge verify --new-only --validation-policy warn --testing-policy warn`: exit 0 / WARN with 9 contract warnings for invalid task type value `code` in `.forge/tasks/iter6-completion/dispatch.json`.
- Default debug build has `LUMINUMBRA_ENABLE_GNS=OFF` and `LUMINUMBRA_ENABLE_STEAM=OFF`; the Steamworks SDK files exist locally, but optional transports are not active in this build.

## WAVES

### Wave B Visual Debt

#### GPU Grass Residual - Partial

Current state: partial. GPU scatter exists through `res/shaders/grass_scatter.comp` and `FoliagePass` SSBO/compute plumbing, and `FoliageInstancing` plus `WorldVisualSweep` are green. The Wave B spec still names the remaining residual: curved/Bezier blade geometry, LOD bands, and far texture-grass coverage.

Gap: implement the residual grass quality layer without disturbing the current visual gates: near curved blades, mid/far LOD behavior, and far texture-grass transition if needed. Any far-field texture grass must not worsen the `FarLodHorizon` sliver issue.

Acceptance signal: `FoliageInstancing` proves curved/LOD grass coverage, `WorldVisualSweep` stays green with zero objective/fidelity flags, `RenderHealth` stays green, release perf is re-blessed if cost changes, and `FarLodHorizon` passes if far-visible grass or terrain composition changes.

#### Volumetric Clouds Tier-2 - Done, Not Remaining

Current state: done. The tier-2 cloud raymarch path is present in `res/shaders/enhanced_skybox.frag`, and the live `WorldVisualSweep` found cloud coverage in expected storm/up cells with zero defects.

Gap: none for current iteration-6 remaining work.

Acceptance signal: already satisfied by `WorldVisualSweep` and `RenderHealth`. Re-run those gates, plus perf if cloud cost changes, for future cloud edits.

#### Aurora Curtains - Done, Not Remaining

Current state: done. Aurora curtain logic and deep-night/weather gating are present in `enhanced_skybox.frag` and `SkyboxPass`, and the live `WorldVisualSweep` has zero objective defects.

Gap: none for current iteration-6 remaining work.

Acceptance signal: already satisfied by `WorldVisualSweep` and `RenderHealth`. Future aurora edits must preserve day/dusk absence, night presence, storm occlusion, and zero objective flags.

#### Ocean/Water Waves - Done, Not Remaining

Current state: done. The water vertex shader implements render-only Gerstner/trochoid waves, and the live `WorldVisualSweep` detected water in expected water-aimed cells with zero defects.

Gap: none for current iteration-6 remaining work.

Acceptance signal: already satisfied by `WorldVisualSweep` and `RenderHealth`. Future water/far-water edits must also keep `FarLodHorizon` green.

### Wave C

#### Erosion Gaps + Far-LOD Meshing - Partial

Current state: partial. Hydraulic/erosion, terrain realism, FarLod unit coverage, and SHIELD-RT offscreen GPU tests are green in full CTest. `HeadlessServerTick` remains canonical at `f17726d44054d133`. The live blocker is still `FarLodHorizon` on `mountains`, and the SHIELD-RT live far-field path remains parked/off by default.

Gap: fix the `mountains` horizon/sliver failure without hiding the objective gate failure. If SHIELD-RT far-field resumes, complete the parked plan: shared cache-backed `FarLodHeightProvider`, incremental/clipmap or band updates, all-or-nothing parallel build, live flag-on visual/perf validation, and shading parity. If the Wave C bar still requires true mixed-resolution transition meshing beyond the current far-LOD region mesher, that remains open.

Acceptance signal: `validate-engine-frontier.ps1 -Mode FarLodHorizon` passes all presets, `ctest --preset debug --output-on-failure -R "FarLod|Hydraulic"` remains green, and `HeadlessServerTick` stays at the blessed hash unless a deliberate worldgen bump is documented and re-blessed. SHIELD-RT resume additionally requires `ctest -R "ShieldRt|FarField|MaxMip"` plus a flag-on `FarLodHorizon`/visual/perf artifact before enable-by-default.

#### Multi-Anchor Streaming + Server Scale - Partial

Current state: partial. `SHIELD_WorldSystem` supports multiple anchors; `ServerWorldRunner` feeds avatar positions as streaming anchors; `MultiAnchorStreaming`, `PlayerAvatar`, `ReplicationScale`, `ReplicationLifecycle`, protocol/reliability tests, `ReplicationSmoke`, and one-host/one-client TCP `NetworkedReplication` are green. Runtime transport host loops still use one connected transport and add exactly one replication client; TCP listens with backlog 1 and accepts one client.

Gap: build a persistent N-client server loop over TCP and optional GNS UDP, with stable client IDs, per-client inbound pump, per-client AOI snapshots, disconnect handling, late join, leave/despawn propagation, and server-scale telemetry for chunk residency/eviction, bandwidth, and tick cost under multi-anchor pressure.

Acceptance signal: a `NetworkedReplication` successor runs N live clients against one server over TCP; an optional GNS-enabled lane proves the same over UDP; `ReplicationScale`, `ReplicationLifecycle`, `PlayerAvatar`, `MultiAnchorStreaming`, `NetworkStateHash`, and `NetworkLoopbackAuthorityGate` remain green; a server-scale artifact records N anchors, AOI snapshots, churn, bandwidth, tick cost, and residency behavior.

### Wave D Closeout

#### Closeout Gate Sweep - Partial

Current state: partial. Build, full CTest, `RenderHealth`, `WorldVisualSweep`, `HeadlessServerTick`, `ReplicationSmoke`, `NetworkedReplication`, `NetworkStateHash`, `NetworkLoopbackAuthorityGate`, and SHIELD-RT GPU CTests are green. Closeout is not done because `FarLodHorizon` is red on `mountains`, `NetworkedSession` is stale/red against the canonical hash, and Forge verification still reports 9 WARN findings.

Gap: repair the `FarLodHorizon` failure; repair, replace, or formally retire `NetworkedSession` through project process; resolve or document the Forge `code` task-type warnings; run the final closeout gate set including endurance/storm/release perf as required by `plan.md`.

Acceptance signal: all named engine-frontier gates are green or explicitly deferred by spec-approved hardware/account constraint; `NetworkedSession` has a green approved successor or documented retirement; `forge verify --new-only --validation-policy warn --testing-policy warn` has no new blockers and no unexplained warnings; final handoff records gate artifacts, world_hash chain, deferred Steam validation, and remaining lanes precisely.

## MULTIPLAYER

### Steam P2P + Lobby + SDR - Partial, Spec+Build, Defer Validation

Current state: partial. Steamworks SDK files are present, and source/CMake provide `LUMINUMBRA_ENABLE_STEAM`, `SteamNetworkingTransport`, `SteamLink`, and `--net-host/--net-join --steam` direct-IP paths behind the optional flag. The default debug build has Steam disabled. No Steam lobby/P2P/SDR orchestration is implemented.

Gap: implement lobby create/list/join/invite/leave, identity exchange, Steam P2P socket setup, SDR/auth ticket path for the shipping shape, admission validation, and reuse of the future N-client transport manager. Steam over-the-wire validation is blocked on this single PC/single account.

Acceptance signal: optional Steam build compiles and links with `-DLUMINUMBRA_ENABLE_STEAM=ON`; local startup proves Steam API init where available; two-machine/two-account artifact shows lobby/P2P/SDR connection and replicated snapshots/usercmds/acks over Steam. Until that hardware/account exists, over-the-wire Steam validation is `spec+build, defer validation`.

### Multi-Client Accept Over TCP/UDP - Partial

Current state: partial. Common replication supports multiple clients and scoped AOI in unit tests, and one-host/one-client TCP `NetworkedReplication` passes. Standalone GNS UDP has a manual one-host/one-join status artifact, but GNS is disabled in the default build and there is no N-client UDP gate.

Gap: replace the one-peer runtime accept/listen shape with N-client TCP and optional GNS UDP server loops. The host must accept/reaccept clients while ticking, assign stable IDs, pump inbound frames per client, broadcast scoped snapshots per client, handle disconnects, and bound bandwidth.

Acceptance signal: N-client TCP integration gate passes on one server with multiple live joiners; optional GNS-enabled artifact/gate passes the same over UDP; `ReplicationScale`, `ReplicationLifecycle`, protocol/reliability tests, and `NetworkedReplication` remain green.

### Runtime Join/Leave Wired Into Server Mode - Partial

Current state: partial. Unit-level lifecycle support exists: prune, removed IDs, join/leave survivor continuity, and connected-client retention are covered by `ReplicationLifecycle` tests. The live server modes are still fixed one-peer runs, not persistent runtime join/leave servers.

Gap: wire late join baseline delivery, leave/despawn propagation, duplicate/idempotent leave handling, survivor continuity, and server-stays-alive behavior into a real server loop that keeps ticking across connect/disconnect churn.

Acceptance signal: runtime integration gate where client A joins, client B joins late, one leaves, survivor clients continue receiving snapshots, removed IDs are observed, and the server remains alive. TCP first; optional GNS UDP when enabled.

### Client-Side Render Integration Of Remote Players - Partial

Current state: partial. `SnapshotInterpolator`, `LocalPlayerPredictor`, and an in-process `ReplicatedAvatarDemo` can drive skinned showcase avatars from loopback replication. `ReplicationSmoke` proves authoritative snapshots can mirror avatars. There is still no live game-client network render path that joins a real server transport and renders remote players from those snapshots.

Gap: wire the client app to `ReplicationClient` over live transport, instantiate/update/despawn remote render entities from snapshot state, interpolate remote transforms, predict/reconcile the local player, handle stale snapshots/removals, and cover replicated NPC/projectile visuals as required. The old `NetworkedSession` gate is stale and cannot prove this path.

Acceptance signal: approved client render integration gate or `NetworkedSession` successor launches a server and rendered client, verifies visible/moving remote avatars from live snapshots, verifies despawn/removal, and keeps `NetworkedReplication`, `ReplicationSmoke`, skinned/avatar render tests, `NetworkStateHash`, and `NetworkLoopbackAuthorityGate` green.
