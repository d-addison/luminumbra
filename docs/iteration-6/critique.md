# Iteration 6 Spec Critique

Scope: every spec under `.forge/specs/iter6/`, reviewed from devil's-advocate,
red-team, and pre-mortem angles.

## multiplayer-client-render-remote-avatars.md

- [devil's-advocate] The spec hides too much coupling inside `NetworkClientSession`: transport pumping, `ReplicationClient`, interpolation, remote render-entity lifetime, local-player prediction, input submission, and animation state all land behind one umbrella. That makes it easy for a task to "wire remote avatars" while leaving local prediction/reconcile, usercmd generation, despawn, and render ownership only partially integrated.

- [devil's-advocate] The local-vs-remote split depends on identity being perfectly stable across `client_id`, `avatar_id`, and replicated `entity_id`, but the acceptance criteria only assert that the local player id is not duplicated. They do not force a negative fixture where `JoinAccept.avatar_id`, snapshot `entity_id`, and self prediction disagree, which is exactly where self-ghosts ship.

- [red-team] The render gate can pass while still using non-authoritative transforms if the old `--replicated` showcase/debug path remains reachable. The spec says "no direct-transform shortcut," but the guard needs a structural assertion, for example no networked runtime render entity may update from anything except accepted `ReplicationClient` snapshots. Otherwise pixels prove an avatar, not replication.

- [red-team] Animation clocks are explicitly render-side and hash-neutral, but they can still break reproducibility of visual evidence. If clocks advance from wall-clock frame delta instead of tick-derived sampled snapshot deltas, capture timing changes can flip the "animation_clock_not_advancing" result without any protocol change.

- [red-team] The malformed-snapshot lane is harness-oriented and does not explicitly cover real transport framing, out-of-order removed ids, stale baselines, or AOI leave/re-enter for the same entity id. A ghost avatar after network jitter is more likely than a clean malformed record in a test harness.

- [pre-mortem] Regression: remote avatars duplicate or remain as ghosts after leave/rejoin. Most likely cause: removed ids clear the render handle but not the interpolator history, or a later id reuse consumes stale samples. Guard: a TCP and GNS join/leave/rejoin lane that asserts remote table absence, zero skinned draws for the removed id, cleared interpolation buffers, and pixel-level absence before re-entry.

## multiplayer-multi-client-accept-over-tcp-udp.md

- [devil's-advocate] "Stable client ids independent of socket accept order" conflicts with the proposed monotonically assigned server allocator. If two clients complete the join handshake in different poll/callback orders across TCP and GNS, the ids will still reflect external timing unless the scripted gate pins admission order or the protocol carries deterministic join slots.

- [devil's-advocate] The join handshake is named but not fully specified. `JoinRequest(protocol, build_id, requested_name)` lacks canonical binary layout, endianness, frame length caps, versioning, replay/duplicate behavior, and queue overflow policy. Those omissions matter because the red-team lane relies on rejecting hostile joins before `ReplicationServer::AddClient`.

- [red-team] Socket callback order is treated as non-hash input, but callbacks enqueue connection and frame intents that can decide which usercmds are ready at a tick boundary. If the tick consumes "currently queued" data without a deterministic cutoff and per-client ordering, the same scripted clients can produce different authoritative input sets and drift `world_hash`.

- [red-team] A three-client accept gate can pass with an implementation that is still effectively broadcast or O(N^2). The spec mentions the 20-client scale proof, but the accept task could ship a one-shared-transport or broadcast fanout that looks correct at N=3 and only fails under the later AOI scale lane.

- [red-team] Polling/accept "must not busy-spin" is too weak for the 8 ms p95 server tick. A slow client with a full inbound queue can still consume the bounded drain budget every tick and starve survivors unless the artifact records per-client drain caps and disconnect/drop decisions under flood.

- [pre-mortem] Regression: when client B disconnects, A and C stop receiving monotonic snapshots or share ack state. Most likely cause: the connection manager closes or mutates a shared transport/poll group rather than owning independent endpoints. Guard: a three-client TCP and GNS lane that kills B mid-run and asserts distinct endpoint ids, independent snapshot seq/ack counters, survivor bytes, and no host loop reset.

## multiplayer-runtime-join-leave-server-mode.md

- [devil's-advocate] This spec depends on the multi-client accept manager but does not make sequencing explicit. Runtime join/leave cannot be made truthful until the transport layer has stable per-client endpoints, per-client queues, and admission rejection. Otherwise lifecycle work will bake around a one-peer host loop and be reworked later.

- [devil's-advocate] The tick-boundary state machine is under-specified around `JoinAccept`. It says allocate/activate the avatar, add the endpoint, and send `JoinAccept`, but the exact order relative to baseline snapshot creation is load-bearing. If `JoinAccept` goes out before the authoritative avatar and anchor exist, the client can render/predict a player the server has not actually admitted.

- [devil's-advocate] Duplicate leave idempotence is scoped to "one PendingLeave per client id," but the inputs are lobby leave, transport close, timeout, and explicit disconnect. The spec does not require a lifecycle generation/version, so a late timeout from an old connection can race a new join that reused a table slot or endpoint handle.

- [red-team] Join and leave are external-session events, yet they mutate canonical avatars and anchors. If hash-bearing lanes run with live external clients and use callback arrival time as the spawn/despawn tick, the default `world_hash` can move without a deliberate bump. The gate needs to prove scripted lifecycle events are consumed from a deterministic tick schedule.

- [red-team] Streaming anchor cleanup is the failure that ships: endpoint state can be gone while the anchor or AOI subscription remains keyed by avatar id. That keeps chunks resident, skews AOI bandwidth, and can hide chunk-budget regressions until long endurance runs.

- [pre-mortem] Regression: churn leaks anchors and memory under budget pressure. Most likely cause: disconnect cleanup prunes `ReplicationServer` client state but misses the streaming anchor table or AOI subscription set. Guard: a churn artifact that records active clients, avatar ids, anchor ids, AOI subscription counts, resident chunks by eviction cause, queue lengths, and memory watermark before and after each leave/rejoin.

## multiplayer-steam-p2p-lobby-and-sdr.md

- [devil's-advocate] This is too large for one spec: lobby create/list/join/invite/leave, P2P transport, SDR behavior, identity/auth, N-client integration, two-machine validation, and future dedicated SDR. The testable implementation slice is not obvious, so a dispatch can close "Steam build compiles" while the actual lobby-to-replication path remains manually deferred.

- [devil's-advocate] The identity story conflicts with the id story. The design says bind runtime client id to SteamID, then says ids are assigned by server admission order. Those can both be true only if the identity binding is metadata and the deterministic sim key is the assigned id. The spec should name that explicitly and test duplicate SteamID, reconnect, and lobby spectator/non-admitted member cases.

- [devil's-advocate] "MVP listen-host path can authenticate with Steam identity plus app ownership through Steam's normal P2P/certificate path and optional session tickets" is an unsupported assumption. If tickets are optional, the security claim should be narrowed to identity-bearing P2P transport, not authenticated game admission.

- [red-team] Lobby metadata is correctly called discovery-only, but the acceptance criteria still emphasize create/list/join success. The failure that ships is stale lobby metadata or a malicious duplicate identity reaching `ReplicationServer::AddClient` because the P2P connection succeeds before the reliable join handshake rejects it.

- [red-team] Steam callbacks can arrive in orders that TCP/GNS tests will not reproduce. If callback handlers accept, close, or mutate lifecycle tables directly instead of enqueueing intents, disconnect and lobby-leave races can double-despawn an avatar or remove a survivor's anchor.

- [red-team] The local GNS substitute proves API shape, not Steam account, relay, invite, lobby, or duplicate-identity behavior. Closeout must not let `NetworkedReplication --udp` stand in for the Steam two-machine lane except as an explicit deferred-validation artifact.

- [pre-mortem] Regression: a Steam lobby user with stale build metadata or duplicate SteamID gets an avatar, then leaves and produces a ghost/despawn race. Most likely cause: lobby admission and P2P connected state are treated as game admission before the reliable join handshake and identity checks complete. Guard: a two-machine red-team lane with stale build, over-capacity, duplicate SteamID, and simultaneous `LobbyChatUpdate_t` plus connection-close events, asserting no `AddClient` for rejected peers and exactly one `PendingLeave` for admitted peers.

## wave-b-aurora-curtains.md

- [devil's-advocate] This is a closure/regression spec that leans on "already present" shader state. It does not require a source-level or shader-inventory assertion that the day/dusk/storm gates still use the same constants in all sky consumers, so a future refactor can bypass the intended envelope while the existing capture cells still pass.

- [devil's-advocate] Reflection coherence is conditional: "where applicable." If water or reflection paths ever sample aurora color, the spec should require a fixture that compares main-sky and reflection gating. Otherwise an independent water-only tint can ship without violating a hard acceptance line.

- [red-team] Aurora is render-only, but render-time phase/noise can still break deterministic visual artifacts. If phase comes from wall-clock or frame count, deep-night coverage and capture classifiers can flicker across machines without changing `world_hash`.

- [red-team] The visual-debt gate can miss faint dusk or storm leakage because aurora absence is likely measured in low-luma regions. A threshold that passes dark captures can hide a green tint in reflections or clouds, especially after brightness-normalized metrics are adjusted for other Wave B work.

- [red-team] The perf budget measures deep-night cells, but a shader branch can still run or allocate resources at day/dusk/storm. The budget should include an "aurora disabled by gating" path to prove the cheap path is actually cheap outside valid night conditions.

- [pre-mortem] Regression: aurora appears faintly at dusk or in storm water reflections while `WorldVisualSweep` remains green. Most likely cause: water/reflection code samples a different sky tint or skips the aurora time/weather predicate. Guard: a dusk/storm reflection fixture that compares aurora fraction in main-sky and reflected-water ROIs and fails on any water-only aurora tint.

## wave-b-gpu-grass-residual.md

- [devil's-advocate] The task is still broad: curved blades, LOD tiering, deterministic thinning, far texture grass, lighting/shadow integration, metric fixture work, CPU shadow/readback, and perf re-bless. That is several risk surfaces under one "residual" item, with no sequencing that forces the gate instrumentation to land before the visual closure claim.

- [devil's-advocate] "Curved/Bezier geometry coverage" is not defined. A renderer can emit a token Bezier field or a tiny curved subset while most near grass remains flat/billboarded. The gate needs a measurable near-band blade-shape distribution, not just a boolean.

- [devil's-advocate] The spec allows either a CPU shadow placement function or deterministic readback. That flexibility is risky because GPU scatter/append order is the exact nondeterminism hazard. If no CPU shadow exists, the readback format must sort stable blade keys and exclude append order from the hash.

- [red-team] Deterministic thinning can be defeated by GPU append/compaction order, atomic counters, or unordered tile iteration. The placement hash can match counts while individual visible blades flicker at LOD boundaries or differ across drivers.

- [red-team] Far-field texture grass couples directly to horizon classifiers and terrain color bands. A far material tweak can close foliage coverage while masking a `FarLodHorizon` sliver or reclassifying sky/grass pixels as acceptable terrain.

- [red-team] The 0.35 ms median / 0.55 ms p95 budget is easy to under-measure if the camera fixture keeps only a bounded visible subset. Dense windy cells, hotspot screenshots, and LOD-boundary camera movement are the likely perf cliffs.

- [pre-mortem] Regression: grass flickers or disappears while crossing LOD bands, and the placement hash differs on another GPU. Most likely cause: visibility is tied to GPU append order or per-dispatch compaction instead of deterministic slot-id thinning. Guard: CPU shadow/readback of sorted per-tile blade keys, LOD counts, and coverage probes across two runs and a camera crossing the near/mid/live-ring boundaries.

## wave-b-ocean-water-waves.md

- [devil's-advocate] This is a closure spec, but it explicitly allows wave phase to derive from render time. That is acceptable for `world_hash`, but it weakens repeatable visual evidence unless capture runs pin render time or derive phase from tick/time-of-day fixtures.

- [devil's-advocate] "Net cost over baseline" is ambiguous. If baseline means water disabled, current water path without waves, or previous blessed build, teams can report different deltas. The perf artifact should name the baseline executable/config and record total frame time in addition to net water pass cost.

- [red-team] Render-only waves can create a visual/sim mismatch: avatars, collision, buoyancy, shoreline wetness, or erosion can continue using flat water while the rendered surface moves. The spec forbids authoritative feedback, but the visual gate should still check that obvious gameplay contacts do not look broken.

- [red-team] Far-water continuity is classifier-sensitive. A water color or band tweak can reduce below-horizon sliver metrics by changing attribution rather than fixing the geometry/composition issue, which ships a hidden horizon defect into Wave C/D.

- [red-team] Degrading reflection update frequency can preserve perf while introducing stale sky/cloud/aurora reflections. `RenderHealth` will not catch temporal incoherence unless the visual gate includes changing weather/time fixtures.

- [pre-mortem] Regression: archipelago far water develops horizon slivers after a classifier/color tweak, but the water spec passes. Most likely cause: thresholds or classifier bands were adjusted without fixture-backed before/after evidence. Guard: `FarLodHorizon` before/after artifact with fixed water classifier fixtures, sliver metrics, and explicit proof that no water flag was cleared by threshold or label change alone.

## wave-b-volumetric-clouds-tier-2.md

- [devil's-advocate] The spec calls the current `enhanced_skybox.frag` path "tier-2 volumetric," but the future-change rule says half-res/froxel/temporal substrate work is not required. That can overfit the closure to a shader-only approximation while the cited research and visual debt are about actual volumetric depth and structure.

- [devil's-advocate] "Cloud coverage" and "storm depth" are not concrete enough. A milky sheet or noisy flat sky can satisfy coverage while failing the visual reason this item exists. The gate needs depth/structure metrics, not just presence.

- [red-team] Temporal history, noise phase, and GPU floating point are render-only, but they can make `WorldVisualSweep` flaky. If history reset, jitter, or phase is frame-count dependent, two captures at the same weather/tick can produce different cloud coverage and shadow positions.

- [red-team] Cloud shadows are a hidden coupling path. If cloud coverage writes into a lighting state that later feeds sim, terrain generation, or persistence, a render-only cloud edit can accidentally become hash-affecting. The spec says not to write back, but the gate only checks the final `world_hash`; it should also assert no sim-side cloud-shadow state changed.

- [red-team] The cloud p95 budget is per-feature, but the failure that ships is aggregate contention: clouds, grass, water, aurora, far-LOD, and remote avatars all pass individual budgets while total frame time misses 3.33 ms.

- [pre-mortem] Regression: storm clouds pass coverage but look flat/milky and their terrain shadows drift out of phase. Most likely cause: the gate measured cloud presence, not 3D structure or shadow/coverage coherence. Guard: storm fixtures with structure variance, vertical-depth proxy, and cloud-shadow correlation metrics tied to the same weather/phase inputs.

## wave-c-erosion-gaps-and-far-lod-meshing.md

- [devil's-advocate] This spec bundles the live mountain horizon fix, erosion/worldgen hash discipline, far-LOD meshing policy, and SHIELD-RT resume policy. The immediate blocker is horizon composition; erosion and SHIELD-RT are separate risk classes. Dispatching them together increases the chance that a task "fixes Wave C" by touching the wrong layer.

- [devil's-advocate] Threshold edits are allowed if evidence proves a non-defect, but the spec does not require an independent geometry/residency proof before changing classifier bands. That leaves room to reclassify real holes as sky/terrain and close the blocker on paper.

- [devil's-advocate] The erosion halo fixture is strong but narrow. H1/H2 cropped byte equality does not cover edited far tiles, hydro-parameter invalidation, mixed old/new cache state, or a saved world loading with old far-store data after new erosion settings.

- [red-team] The shipping failure is a mountain sliver fixed by attribution rather than geometry. `FarLodHorizon` goes green, `world_hash` stays canonical, but fast pans, lightning flashes, or different stations expose real holes and stale far tiles.

- [red-team] SHIELD-RT resume has a known 115 second live assembly hazard. Median/p95 draw timers will not catch a rare 49-tile rebuild or stale-epoch integration unless the gate records max build/upload hitch and all-or-nothing batch state under camera movement.

- [red-team] A cache-backed `FarLodHeightProvider` is a determinism and coupling hazard. If it uses mutable epoch state, unordered parallel writes, or stale hydro params, far mesh, raymarch depth, collision, and authoritative height can disagree without moving `world_hash` in the expected place.

- [pre-mortem] Regression: `FarLodHorizon` passes but mountains show cracks during fast pan/lightning or after hydro tuning. Most likely cause: classifier/attribution changes hid a real residency or mesh seam defect, and stale far tiles were not invalidated. Guard: live seam-crossing and fast-pan captures, raymarch-vs-authoritative-height parity, far-tile epoch/invalidation artifact, and a classifier diff audit before threshold changes.

## wave-c-multi-anchor-streaming-and-server-scale.md

- [devil's-advocate] This Wave C spec overlaps heavily with the multiplayer accept and lifecycle specs. It should declare those as prerequisites, otherwise runtime scale can be dispatched before the server can accept N real clients or cleanly process churn.

- [devil's-advocate] `ReplicationSmoke --avatars 20` with deterministic avatars is not the same as 20 live clients. It proves replication state shape, not per-client transport queues, acks, backpressure, packet loss, accept order, or disconnect behavior.

- [devil's-advocate] The active-region fairness target says near-floor protection for at least three far-apart anchors, but the scale target is 20. A design can satisfy the three-anchor fairness test and still starve far-apart players 4 through 20 under the 8192 active-region budget.

- [red-team] AOI and streaming share chunk/grid machinery, but the spec does not require AOI to be a subset of actually admitted/resident chunks. The server can replicate an entity in a desired-but-not-resident chunk, creating invisible/invalid client state and bandwidth waste.

- [red-team] Hotspot is the AOI invariant breaker. In a group-photo scenario all clients and props share one AOI, so snapshot cost becomes O(N * local density) where local density is effectively the whole group. Without hard per-snapshot entity/byte caps and rate tiers, the p95 <= 8 ms target will fail after spread-out tests pass.

- [red-team] Socket accept/packet arrival order must not change anchor artifacts, but stable client ids are still assigned by runtime admission elsewhere. Without scripted deterministic join slots or explicit sorting after assigned ids, two valid runs can produce different anchor ordering and different chunk residency telemetry.

- [pre-mortem] Regression: 20-player hotspot tanks server tick and some clients receive entities for chunks they have not streamed. Most likely cause: AOI uses desired chunks or full local density, not the admitted near-floor resident set, and lacks per-snapshot caps. Guard: spread and hotspot scale artifacts asserting AOI subset-of-resident, per-client entity/byte caps, despawn behavior for out-of-AOI entities, p95 AOI build time, and drained queues after churn.

## wave-d-closeout.md

- [devil's-advocate] Closeout can become a paperwork gate because it permits stale gates to be repaired, replaced, or retired "through project process" without defining equivalence. Retiring `NetworkedSession` is safe only if each old invariant is mapped to a successor assertion and artifact.

- [devil's-advocate] The spec says Wave D owns the final 300 fps ledger, but also allows a green release lane that records measured fps/frame time without 300 fps. That may be pragmatic, but it should be explicitly framed as "iteration closeout without 300 fps achieved" rather than letting a non-regression lane satisfy a headline performance target.

- [devil's-advocate] Warning disposition can hide real validation debt. `forge verify --validation-policy warn --testing-policy warn` plus documented owners is not enough if warnings cover schema/task-type problems that affect dispatch coverage or task execution semantics.

- [red-team] The canonical hash `f17726d44054d133` is repeated across many specs. If Wave C accepts a deliberate terrain/worldgen bump, stale closeout expectations can either fail correctly or, worse, be updated inconsistently. Closeout needs one canonical hash source and old->new provenance, not copied literals.

- [red-team] A 300 fps claim can be wrong if timers are not composable. GPU per-pass timings may overlap, CPU/GPU queues may hide stalls, and median frame time can pass while p95/p99 cliffs break the target. The closeout artifact must prove total present-to-present frame time, not just summed blessed pass timers.

- [red-team] Visual debt can regress through reclassification. Wave D explicitly forbids it, but the failure that ships is a threshold/classifier update in Wave C or Wave B followed by a green 48/48 sweep. Closeout should audit threshold provenance and before/after failing cells, not only final pass counts.

- [pre-mortem] Regression: iteration 6 is declared closed with green artifacts, but a stale network/session gate was retired and the successor gates did not cover the same invariant. Most likely cause: replacement was accepted as a list of modern gate names rather than an equivalence matrix. Guard: a gate-lineage table mapping each retired gate invariant to successor assertions, commands, artifacts, pass conditions, and explicit residual risks.

- [pre-mortem] Regression: closeout claims or implies 300 fps while users see lower performance or frame-time cliffs. Most likely cause: per-pass median timers and non-regression margins were treated as total-frame proof. Guard: release artifact with target hardware/driver, total CPU+GPU frame time, p95/p99, present-to-present timing, per-pass timers, and explicit "300 fps achieved" only when total frame time is <= 3.33 ms.
