# Client prediction, server reconciliation, entity interpolation & lag compensation

**Slug:** `mp-prediction-reconciliation` · **Side:** NET + SIM (client-side latency hiding over an authoritative server) · **Iteration:** 6, Wave C (P3 replication leg)
**Status:** research brief v1 · 2026-06-17
**Audience:** the P3 replication-protocol build (`MULTIPLAYER-BLOCKER-SPEC.md` §0 v2)

## Scope

The `MULTIPLAYER-BLOCKER-SPEC.md` v2 pivot commits Luminumbra to an **authoritative dedicated
server** (Source / Quake / Garry's-Mod lineage): the server runs the one true 30 Hz simulation
(full Jolt physics for player avatars + props), clients send quantized `usercmd` input upstream,
the server returns per-client entity-state snapshots. This brief covers the **client-side half**:
how a client hides the round-trip latency to that server while the server stays authoritative —
four techniques, each calibrated to the fact that **Luminumbra is a zen co-op photography game,
not a competitive shooter.** That calibration is the load-bearing conclusion: most of the
shooter-grade machinery (lag compensation rewind, sub-frame hit validation, time dilation) is
*either unnecessary or a much-reduced version* here, and the brief says so explicitly per section.

The four techniques (Gambetta's canonical decomposition, which mirrors Bernier's four-part Source
model):
1. **Client-side prediction** of the *local* player avatar — apply local input immediately, don't wait for the server.
2. **Server reconciliation** — accept the authoritative snapshot, replay unacknowledged inputs on top of it, smooth the error.
3. **Entity interpolation** of *remote* avatars/props — render them "in the past" between two received snapshots.
4. **Lag compensation** — server rewind for hit validation (assessed as *near-unnecessary* for us).

---

## Recommended approach

### TL;DR for a zen co-op game
Predict **only the local avatar's movement**; interpolate **everything remote**; do **NOT** build
lag-compensation rewind, time dilation, or favor-the-shooter hit registration in v1. The
photography loop has no instant-hit, millisecond-sensitive PvP interaction — the thing
players care about is *their own camera/movement feeling instant* and *other players/creatures
moving smoothly*, both of which prediction + interpolation deliver without server rewind.

### 1. Local-player prediction + reconciliation (BUILD — the core of the feel)

The client and server run **the same deterministic movement code** on the same 30 Hz fixed tick.
Gambetta and Bernier both describe this identically: the client "runs exactly the same code and
rules the server will use to process the user commands," predicting locally, while the server
remains the authority. Concrete mechanics:

- **Sequence-numbered usercmds.** Every `usercmd` the client sends carries a monotonically
  increasing **input sequence number**. The client keeps each un-acknowledged usercmd in a local
  ring buffer.
- **Predict immediately.** On producing usercmd `N` at tick `T`, the client *immediately* applies
  it to its local copy of its own avatar (step the same movement/physics integrator the server
  uses) and renders the result. No waiting for the server.
- **Server acks the last input it processed.** Each snapshot the server sends to a client includes
  "the sequence number of the last input it processed" (Gambetta) plus that client's authoritative
  avatar state at that tick.
- **Reconcile by replay.** When snapshot arrives acking input `K`: (a) snap the local avatar to the
  authoritative state in the snapshot; (b) discard buffered usercmds `<= K`; (c) **re-apply the
  still-unacknowledged usercmds `K+1 .. N`** on top of the authoritative state to recompute the
  present. If the prediction was correct (the common case — no collision surprise, no server-side
  force) the replayed result equals what was already on screen and nothing visibly changes.
- **Error smoothing.** When prediction and authority disagree (a misprediction), do **not** hard-
  snap the rendered avatar — that is a visible pop. Snap the *simulation* state to authority, then
  smooth the *rendered* position toward it over a few frames (exponential decay / short lerp of the
  position error). This is exactly the render-vs-sim split Luminumbra already has (see Integration).

This is the entire "feels instant" budget for the local player. It is non-negotiable for a
playable feel even in a calm game: a co-op photographer walking around a voxel world with 60–120 ms
RTT would feel like wading through mud if movement waited for the server.

### 2. Entity interpolation of remote entities (BUILD — the smoothness budget)

Remote avatars, props, and creatures are **never predicted**; they are **interpolated**. Gambetta:
"you show the other players *in the past* relative to the user's player." Bernier/Source default
this **interpolation delay to 100 ms** (`cl_interp 0.1`). Mechanics:

- Client keeps a small **interpolation buffer** of the last few authoritative snapshots per remote
  entity, each tagged with its server tick/timestamp.
- The client renders remote entities at **render-time minus an interpolation delay** (the "render
  behind" window), interpolating position/orientation between the two buffered snapshots that
  straddle that past time. Source's formula: target render time = now − interp delay; pick the two
  snapshots around it; lerp by the fractional time between them.
- The delay must be **≥ the snapshot interval** so there are always two snapshots to interpolate
  between even with one dropped packet. At a 30 Hz server tick and (say) a 15–20 Hz snapshot send
  rate, a snapshot lands every ~50–66 ms, so a **100 ms interpolation buffer** (two snapshot
  intervals) is the right starting value — identical to Source's default, and it tolerates a single
  lost snapshot without a hitch.

**Extrapolation / dead-reckoning is REJECTED as the default.** Dead reckoning extrapolates from the
last known velocity/heading; Gambetta notes it works for "highly dependent" motion (a racing car)
but "fails when these conditions aren't met" — e.g. a player who can change direction instantly,
which is exactly a walking voxel avatar. Extrapolation guesses the future and is *always wrong* the
instant the entity changes course, producing rubber-banding. Interpolation only ever shows *actual
server data, 100 ms late*, which for a zen game is the right trade (a tiny, unnoticeable
remote-entity lag in exchange for zero rubber-banding). We may keep a *brief* clamped extrapolation
(≤1 snapshot interval) purely as a stall-cover for a missing packet, then freeze — but it is a
fallback, not the model.

### 3. Lag compensation / server rewind (DO NOT BUILD in v1 — assessed)

Lag compensation is the server "rewinding time" to reconstruct where entities were *at the moment
the client acted*, to validate time-and-space-sensitive events. Bernier's Source formula:
`Command Execution Time = Current Server Time − Packet Latency − Client View Interpolation`, after
which the server restores historical hitboxes (Source keeps ~1 s of position history) to test the
hit. Gambetta is explicit about *what it is for*: "time- and space-sensitive events; for example,
**shooting your enemy in the head**." It carries a well-known cost — the "shot around a corner"
unfairness where a target "got behind a wall, and *then* got shot, a fraction of a second later,
when they thought they were safe." Overwatch (Tim Ford, GDC 2017) calls this favor-the-shooter and
spends real engineering on rewind volumes precisely because it is a precision competitive shooter.

**Luminumbra has no such event.** It is co-op photography: there is no PvP instant-hit weapon, no
headshot validation, no competitive fairness contract between players to protect. The only
"interactions" between players are bumping into each other (handled by server physics at whatever
tick they collide — no rewind needed; the server is authoritative and both are slightly latent,
which is fine) and shared-world events (a creature, a photo target) that are not millisecond-
sensitive. **Building server rewind here would be pure cost for no player-visible benefit, and it
would *add* the corner-death unfairness for nothing.**

**Minimal-correctness version (if any interaction ever needs it):** for the rare case of a
deliberate close-range cooperative interaction (e.g. "tag" a creature another player is looking at,
or a co-op trigger), accept it at **server-present time** with a generous spatial tolerance, or do
a *cheap, bounded* one-shot rewind to the usercmd's tick using the same position history the
snapshot system already keeps — without the full per-bone hitbox machinery. There is no need for
the persistent 1 s rewind buffer, sub-tick interpolation of hitboxes, or favor-the-shooter
arbitration. Default: skip it entirely.

### 4. Reconciling PHYSICS-driven prediction (the genuinely hard part — BUILD carefully)

Our avatars are **full server-authoritative Jolt physics** (Garry's-Mod model), which is harder to
predict than the kinematic capsule of a typical FPS. The discipline:

- **Predict what is locally determinable; defer what depends on remote state.** The local avatar's
  *own* movement against the *static* world (terrain, settled voxels, its own gravity/jump/walk
  integration) is highly predictable — the client has the collision geometry and runs the same
  integrator, so prediction is correct almost always. **What must defer to the server:** outcomes
  that depend on *other dynamic bodies the client can't authoritatively know* — a collision with
  another player's avatar, a pushed prop, a server-spawned force. The client may predict a
  best-effort result, but these are the cases that *will* mispredict and must reconcile to
  authority. Overwatch's stance (Ford) is "predict everything, accept occasional mispredictions";
  for us the safer default is **predict local movement confidently, treat inter-body physics as
  reconcile-on-correction** (don't fight the server over a prop you don't own).
- **Determinism is required only client↔server for the local avatar, not cross-machine.** The v2
  pivot already removed the heavy cross-machine bit-exact tax (`MULTIPLAYER-BLOCKER-SPEC.md` §0).
  Prediction replay only needs the client's *own* integrator to match the server's *closely enough*
  that the common case reconciles to ~zero error; small float divergence is absorbed by error
  smoothing. We do **not** need the lockstep desync-oracle here.
- **Smoothing physics corrections specifically.** Physics corrections can be larger and lumpier than
  kinematic ones (a body that the server resolved out of a penetration, a velocity the server
  clamped). Two safeguards: (1) **velocity-aware smoothing** — blend both position *and* velocity
  error so the smoothed avatar doesn't visibly decelerate/snap; (2) **a snap threshold** — if the
  error exceeds a large bound (teleport, server-side death/respawn, getting launched), hard-snap
  instead of smoothing (smoothing a 10 m correction looks worse than a clean cut). This mirrors
  Source's prediction-error decay with a teleport escape hatch.
- **Re-simulation cost.** Reconciliation replays the unacked usercmds through the *physics* step,
  not just a position lerp. At 30 Hz with a small input horizon the unacked window is only a handful
  of ticks (e.g. RTT 100 ms ≈ 3 ticks), and replay is **single-body** (just the local avatar
  against static geometry), not a full-world re-sim — orders of magnitude cheaper than rollback
  lockstep's full-world snapshot/restore (which the spec already rejected for exactly this reason).

---

## Alternatives considered (and why rejected)

| Alternative | Why rejected for Luminumbra |
|---|---|
| **Delay-based lockstep** (the existing `LockstepSession`) | Already rejected by `MULTIPLAYER-BLOCKER-SPEC.md` v2: stalls on the slowest of 20 peers, can't do mid-session join/leave, needs cross-machine bit-exact determinism. Park it for 2-player replay/loopback testing only. |
| **Rollback netcode** (full-world snapshot + N-tick re-sim every frame, GGPO/Overwatch-style) | Rejected in `LockstepSession.h:5-18`: our world tick includes chunk streaming + water + physics, so full-world re-sim is brutal, and rollback exists to hide latency in *reaction-critical competitive* play we don't have. Our reconciliation re-sims only the *single local avatar*, not the world. |
| **No prediction (wait for server)** | Movement would feel laggy at any real RTT; unacceptable even for a calm game. Local prediction is the minimum bar. |
| **Predict remote entities too / extrapolation as default** | Dead reckoning rubber-bands when an entity changes course (Gambetta); a walking avatar changes course constantly. Interpolation shows real data 100 ms late with zero rubber-banding — the right trade for smoothness over a non-existent latency-of-others requirement. |
| **Full lag-compensation server rewind** | No instant-hit PvP, no headshot validation, no competitive-fairness contract. Pure cost; would *introduce* corner-death unfairness for no benefit. Skip in v1. |
| **Time dilation / input-starvation clock control** (Overwatch's "most important problem") | A competitive-shooter optimization to keep server input buffers full at 60–128 Hz under packet loss. At 30 Hz, calm movement, and a generous interpolation buffer, simple input buffering + a small adaptive horizon suffice; full time dilation is over-engineering for v1. Revisit only if 20-player tail latency proves it needed. |

---

## Perf / latency budget

- **Server tick:** 30 Hz fixed (`SimulationClock::kCanonicalTickRateHz = 30.0`, `fixed_dt ≈ 33.3 ms`). Unchanged; this is the authority.
- **Snapshot send rate:** start 15–20 Hz per client (every 1–2 sim ticks), delta-compressed against a per-client baseline, scoped by PVS/area-of-interest (already the P3 plan) so a 20-player world doesn't broadcast everything.
- **Interpolation buffer (remote entities):** **100 ms** default (`cl_interp 0.1` equivalent) — two snapshot intervals, tolerates one lost snapshot. This is the only *added* latency a player perceives, and only for *other* entities, not their own avatar.
- **Local-avatar perceived input latency:** ~**0 ms** (prediction makes local input instant on screen).
- **Reconciliation replay cost:** re-sim of the unacked usercmd window for the **single local avatar** only. Window ≈ `ceil(RTT / 33.3 ms)` ticks (100 ms RTT → ~3 ticks); negligible per frame. No full-world re-sim.
- **Server position history (if minimal rewind is ever added):** a few hundred ms at most, vs Source's full 1 s — and *default is to keep none for rewind*.
- **Misprediction smoothing window:** a few render frames (exponential decay of position+velocity error), with a hard-snap threshold for teleports/large corrections.

---

## Integration notes (what we already have that fits)

Luminumbra is unusually well-positioned because the render/sim decoupling prediction needs **already exists**:

- **`SimulationClock` (30 Hz fixed tick).** `src/luminumbra_common/core/SimulationClock.h`. The
  server's authority clock. The client runs the *same* clock for its local prediction so predicted
  ticks line up 1:1 with server ticks. The clock's `tick_count()` (1-based tick ids) is the natural
  basis for the **input sequence number** — a usercmd is "input for tick T," exactly as the existing
  lockstep input model already frames it.
- **Render-side camera already decoupled from sim.** This is the single most important asset for
  this work. Camera *look* is render-only and "never enters the hash" (`MULTIPLAYER-BLOCKER-SPEC.md`
  §3 M2; memory: render-side camera decouples look from sim). That means **look needs no prediction
  or reconciliation at all** — it's already instant and local. Only *movement* (position/velocity)
  is predicted and reconciled. This also gives us the exact seam for **error smoothing**: the sim
  avatar position snaps to authority while the *rendered* avatar position smooths toward it, using
  the existing one-way sim→render read.
- **Input schema (opaque quantized blob).** `MULTIPLAYER-BLOCKER-SPEC.md` §3 M2 / `LockstepSession.h:82-86`
  already define a quantized per-tick input blob: fixed-point movement axes + action bits, **look is
  render-side only**. This is *precisely* the `usercmd`. For the authoritative-server model we add:
  (a) an **input sequence number** (= target tick id), (b) **client-side buffering of unacked
  usercmds** for replay, (c) bundle multiple usercmds per packet (Bernier/Source send command
  packets ~30/s carrying all commands since last ack) so packet loss is covered.
- **`LockstepSession` framing discipline reused, session logic retired.** The wire encoding style
  (length-prefixed, no struct padding, little-endian, hashes as hex — `LockstepSession.h:55-116`) is
  the encoding the replication protocol reuses (`MULTIPLAYER-BLOCKER-SPEC.md` §0 "what this KEEPS").
  The *peer-lockstep session logic* (`PumpTick`, desync oracle, per-tick all-peer barrier) is **not**
  used for the authoritative path — it's parked.
- **`world_hash` / sub-hashes repurposed.** No longer a multiplayer transport requirement (server is
  authoritative; clients can only mispredict, not desync the world). Kept as the server's
  replay/save-integrity/debug invariant only (`MULTIPLAYER-BLOCKER-SPEC.md` §0). Client prediction
  does **not** need it.
- **Server avatar entity (P1 prep).** The `PlayerAvatar` ECS entity (stable id, position, facing,
  velocity; deterministic spawn) is the entity the client predicts locally and the server replicates
  in snapshots. Avatar position is also the streaming anchor (P0 multi-anchor budget).

**Suggested build order inside P3:** (1) usercmd = existing input blob + sequence number + client
buffering; (2) server applies usercmds to the avatar and sends per-client snapshots (state + last-
acked-input); (3) client prediction + replay reconciliation of the *local avatar* (the feel); (4)
interpolation buffer for *remote* entities (the smoothness); (5) physics-correction smoothing +
snap threshold. Lag-compensation rewind is **not** in P3 scope.

---

## Open risks

1. **Client↔server physics divergence.** The client's local Jolt prediction must match the server's
   integrator closely enough that the common case reconciles to ~0 error. Jolt is deterministic
   per-build but the client and server must use the *same* build/config and the *same* fixed dt.
   Risk: persistent small corrections (constant micro-smoothing) if integrators drift. Mitigation:
   shared integrator code path, identical `fixed_dt`, validate "no-input-change → zero correction"
   as a gate.
2. **Prediction of inter-body collisions.** Predicting a collision with another player/prop the
   client doesn't authoritatively own *will* sometimes mispredict (the other body's true state is
   100 ms stale via interpolation). Risk: a visible correction when two avatars touch. Mitigation:
   predict local movement confidently, treat inter-body resolution as reconcile-on-correction;
   accept occasional small smoothing. Acceptable for a calm game; would be a problem in a shooter.
2b. **Interpolation delay vs co-op "togetherness."** 100 ms render-behind means you see your co-op
   partner slightly in the past. For photography (composing a shot *with* someone in frame) this is
   almost certainly fine, but if "line up a photo of my friend" feels off, the buffer is a tunable
   knob (lower it toward one snapshot interval, accepting more jitter).
3. **20-player snapshot bandwidth / AOI.** Full prediction+interpolation is per-entity; at 20+
   players the snapshot/PVS/AOI budget (P5) dominates, not the prediction math. Risk lives in the
   replication-scope design, not this brief — but interpolation correctness depends on snapshots
   arriving regularly, so AOI churn (entities entering/leaving a client's set) needs a clean
   spawn/despawn-in-interpolation story.
4. **Misprediction smoothing tuning.** Over-smoothing feels floaty; under-smoothing pops. The
   threshold between smooth-vs-snap and the decay rate are feel-tuned, not derivable — budget a
   tuning pass with the render-side camera in the loop.
5. **Cheating / authority.** Server-authoritative means a client can lie in its usercmd but the
   server validates movement (it runs the real physics). Low stakes for a co-op game, but the server
   must clamp/validate predicted positions it receives implicitly (it only trusts *input*, never
   client-reported *position*) — which is exactly what the prediction model already enforces.

---

## Citations

**Primary — canonical technique sources:**

1. Gabriel Gambetta, *Fast-Paced Multiplayer (Part II): Client-Side Prediction and Server Reconciliation* — https://www.gabrielgambetta.com/client-side-prediction-server-reconciliation.html — *Client sequence-numbers each input; server acks "the sequence number of the last input it processed"; client snaps to authority then re-applies all still-unacknowledged inputs.* (The exact local-player prediction+replay model we adopt.)
2. Gabriel Gambetta, *Fast-Paced Multiplayer (Part III): Entity Interpolation* — https://www.gabrielgambetta.com/entity-interpolation.html — *"Show the other players in the past relative to the user's player"; interpolate between two received snapshots; dead reckoning "fails" when direction/speed can change instantly (i.e. for walking avatars).* (Why we interpolate remotes and reject extrapolation as default.)
3. Gabriel Gambetta, *Fast-Paced Multiplayer (Part IV): Lag Compensation* — https://www.gabrielgambetta.com/lag-compensation.html — *Server "can authoritatively reconstruct the world at any instant in the past" to validate "time- and space-sensitive events; for example, shooting your enemy in the head"; carries the "shot around a corner" unfairness.* (The technique we assess as unnecessary for non-PvP co-op.)
4. Gabriel Gambetta, *Fast-Paced Multiplayer (Part I): Client-Server Game Architecture* — https://www.gabrielgambetta.com/client-server-game-architecture.html — *The authoritative-server + dumb-client framing the whole series builds on.*

**Primary — Valve / Source (Bernier):**

5. Valve Developer Community, *Source Multiplayer Networking* — https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking — *Server simulates in 15 ms ticks (~66/s) and is authoritative; client sends command packets ~30/s bundling usercmds; client prediction (`cl_predict`) runs "exactly the same code and rules the server will use"; entity interpolation defaults to 100 ms (`cl_interp 0.1`).* (Tick/snapshot/interp numbers; the usercmd + prediction + interpolation combination.)
6. Yahn W. Bernier (Valve), *Latency Compensating Methods in Client/Server In-game Protocol Design and Optimization*, GDC 2001 — https://developer.valvesoftware.com/wiki/Latency_Compensating_Methods_in_Client/Server_In-game_Protocol_Design_and_Optimization (also Semantic Scholar: https://www.semanticscholar.org/paper/330071040ca858ca710a24a03915366fcd46f021) — *The foundational four-part model: input prediction, entity interpolation, and lag compensation via server rewind; rewind formula `Command Execution Time = Current Server Time − Packet Latency − Client View Interpolation`, with ~1 s of position history.* (The original lag-compensation paper.)
7. Valve Developer Community, *Lag Compensation* — https://developer.valvesoftware.com/wiki/Lag_Compensation — *"The server using a player's latency to rewind time when processing a usercmd, in order to see what the player saw when the command was sent."* (Definition + the favor-the-attacker consequence.)

**Primary — Overwatch (GDC):**

8. Timothy Ford (Blizzard), *'Overwatch' Gameplay Architecture and Netcode*, GDC 2017 — https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and (deep-dive summary: https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback) — *16 ms command frames (~60 Hz); client clock leads server by ½ RTT + 1 buffered frame; "predict everything" with rollback-and-replay-buffered-inputs reconciliation; server-signaled time dilation on input starvation ("the most important problem"); favor-the-shooter rewind with spatial pre-filtering; prediction disabled above ~220 ms RTT.* (The competitive-grade end of the spectrum — explicitly the machinery we scope OUT for a calm co-op game.)

**Engine cross-references (local):**

9. `D:\Coding\luminumbra\.forge\artifacts\engine-iteration-6\MULTIPLAYER-BLOCKER-SPEC.md` §0 v2 — the authoritative-server pivot this brief serves (usercmd upstream, per-client snapshot downstream, predict-local / interpolate-remote, PVS/AOI for 20+).
10. `D:\Coding\luminumbra\src\luminumbra_common\core\SimulationClock.h` — 30 Hz fixed tick (`kCanonicalTickRateHz = 30.0`); tick ids as the basis for input sequence numbers.
11. `D:\Coding\luminumbra\src\luminumbra_common\net\LockstepSession.h` — rejected rollback/lockstep rationale (`:5-18`); reusable wire-framing discipline (`:55-116`); the quantized opaque input blob = our usercmd (`:82-86`).
