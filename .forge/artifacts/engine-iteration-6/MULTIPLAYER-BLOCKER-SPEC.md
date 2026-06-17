# Multiplayer blocker spec — gating the Wave C multi-anchor driver (#5)

**Slug:** `multiplayer-blocker` · **Side:** SIM + NET (engine-generic) · **Iteration:** 6, Wave C
**Status:** SPEC (owner-gated: "5 needs the multi-player blocker properly specd out first")
**Determinism posture:** the streaming change itself is residency-only (world_hash-neutral),
but the player-avatar + input pieces are SIM state → **deliberate `world_hash` bump territory**;
this spec pins which parts bump and which do not.

---

## 1. What "#5" is, and why it is blocked

**#5 = the multi-anchor streaming DRIVER:** multiple lockstep players, each driving their own
streaming anchor, over a shared world, with per-anchor streaming budgets and a
`HeadlessServerTick` multi-anchor mode. The owner's directive (2026-06-15) was: land #1–#4,
and **spec the multiplayer blocker before building #5** — because the driver cannot be built
until the session layer can actually carry N players deterministically.

**The FOUNDATION already exists** (`0113a60`, test `208eb7c`): `SHIELD_WorldSystem::update` /
`update_chunk_activation` take a `std::vector<Vec3>` of anchors (union wanted-set, evict-if-near-
ANY-anchor, closest-anchor LOD/meshing priority, shared 8192-chunk budget), with a `Vec3`
forwarding overload that keeps single-anchor behaviour byte-identical (verified hash-unchanged).
**So the streaming engine can already serve N anchors.** What is missing is *everything that
would produce N anchors*: N deterministic player avatars fed by N input streams over an N-peer
session.

**The blocker is therefore NOT the streaming layer — it is the session/sim layer below it.**
Five concrete sub-blockers, in dependency order:

| # | Blocker | Current state | Why it blocks #5 |
|---|---------|---------------|------------------|
| B1 | **Session is hardwired to ≤2 peers, ONE remote** | `LockstepConfig` carries a single `peer_client_id`; `TcpTransport` accepts ONE client; docstring says "v1 scope: <= 2" | N players need an N-peer session: N input streams merged per tick, N-way handshake, N-way desync-oracle exchange |
| B2 | **No player avatar in the sim** | `ServerWorldRunner` streams around a FIXED spawn anchor; the ECS entity snapshot is "currently empty, terrain/water-only headless" (`ComputeWorldSubHashes` docstring) | An anchor must be a *player position*; without per-player avatar entities there is nothing to anchor on and nothing to move |
| B3 | **Input blob is empty / no movement schema** | `LockstepHooks::collect_local_input` returns empty for the headless server; "the headless server has none today" | Players must MOVE to drive distinct anchors; needs a deterministic quantized movement/action input schema applied per-avatar |
| B4 | **No join/leave-during-session model** | "no rejoin in v1"; a clean disconnect ENDS the session | Real multiplayer needs a defined lobby + mid-session leave that does not desync (dynamic peer set is a determinism hazard) |
| B5 | **No transport topology for N peers** | `TcpTransport` is one-remote, point-to-point | N delay-based-lockstep peers need a relay/topology decision (star-through-host vs mesh) |

---

## 2. Load-bearing invariant (must survive #5)

The determinism contract (design-decisions §6/13, `LockstepSession.h:20-27`) is the thing #5 is
most likely to break and the reason this is gated. The simulation a peer runs depends ONLY on
`(seed, preset, the ordered set of per-tick inputs)`. Everything #5 adds must preserve that:

- **Player avatars are SIM state.** Their spawn assignment, entity-id allocation, movement
  integration, and the order they are folded into `world_hash` MUST be a pure function of the
  ordered input set — never of wall-clock, socket arrival order, or peer connection order.
- **Anchors are DERIVED from avatar positions**, read one-way into the streaming layer (the
  existing render/stream one-way rule). Streaming residency stays world_hash-neutral; the
  avatar *positions* that drive it are the part that bumps the hash.
- **N-peer input merge order = ascending client-id** (the existing `std::map`-ordered merge in
  `LockstepSession.h:347` already does this for 2; it scales to N unchanged — the Factorio
  unordered-map lesson is already encoded). Client-id assignment must itself be deterministic
  (host = 0, remotes numbered by a deterministic join order recorded in the session, NOT by
  socket accept order leaking into the tick path).

---

## 3. Design (the driver, once unblocked)

Ordered so each step is independently gate-validatable and the determinism bump is isolated.

### Phase M1 — Player avatar as deterministic sim entity (world_hash BUMP)
- Add a `PlayerAvatar` component/entity to the ECS: stable per-player id, position, facing,
  velocity. Spawn position = a deterministic function of `(seed, preset, player_id)` (e.g. a
  fixed ring of spawn points indexed by player_id), NOT a runtime query race.
- Fold avatars into the `entities` sub-hash in a STABLE order (by player_id). This is the
  **deliberate `world_hash` bump** (the entities sub-hash stops being empty). Bump in its OWN
  commit with the LREC1 replay + lockstep + SimDeterminismLint re-bless, per the two-ordered-
  bumps discipline (this would be a THIRD bump after Aetheric + erosion — sequence it explicitly
  in the handoff so it stays attributable).
- `HeadlessServerTick` gains a fixed-N avatar mode: N avatars at deterministic spawns, stepped
  by scripted inputs, hash asserted run==replay.

### Phase M2 — Movement input schema (engine-generic opaque blob)
- Define the quantized per-tick input: movement axes (fixed-point), look is RENDER-side only
  (camera look never enters the hash — already the rule), action bits. This is the opaque blob
  `collect_local_input` returns and `apply_and_step` applies to the LOCAL avatar; the merged set
  applies each peer's blob to ITS avatar (client-id → avatar map).
- Integrate movement with the SAME `DeterministicMath` the rest of the sim uses (no `libm`
  transcendentals in the tick path; SimDeterminismLint scopes this).
- Gate: a 2-avatar lockstep loopback where the two avatars walk APART and both peers converge to
  the same `world_hash` (extends `LockstepLoopback`).

### Phase M3 — N-peer session (generalize ≤2 → N)
- `LockstepConfig`: replace the single `peer_client_id` with a peer-id SET; `LockstepSession`
  buffers inputs for all client-ids (the `std::map` already keys by client-id), advances a tick
  only when EVERY connected peer's input for it is present, exchanges + compares hashes against
  ALL peers at the cadence (any mismatch → Desync + dump).
- Handshake: N-way Hello validation (all peers agree seed/preset/tick-rate); reject loudly.
- Gate: `LockstepLoopback` extended to 3- and 4-peer in-process loopback (no sockets), plus a
  `LockstepFaultInjection` N-peer desync-localization case.

### Phase M4 — Multi-anchor driver wiring (residency-only, world_hash-NEUTRAL)
- `ServerWorldRunner` (or a new `SessionWorldDriver`) feeds the N avatar positions as the anchor
  vector into the EXISTING `update_chunk_activation(std::vector<Vec3>)`. No streaming-engine
  change — the foundation is done.
- Add per-anchor streaming budgets IF union pressure warrants (the shared 8192 budget may starve
  a far player; spec a per-anchor floor). Measure first; do not pre-optimize.
- Gate: `HeadlessServerTick` multi-anchor mode — N avatars at distinct positions, assert the
  union wanted-set is served, each avatar's near-surface is resident, and `world_hash` is
  UNCHANGED vs the same avatars under the single-anchor forwarding path at N=1 (residency-only).

### Phase M5 — Lobby + join/leave (the B4 hazard)
- v1 multiplayer: a FIXED roster agreed at handshake (no mid-session join) — the safe first cut,
  matching the current "no rejoin" stance but for N players. Leave = that avatar freezes
  (deterministic: a left player's input blob becomes a defined "idle" set, NOT absent — absence
  would stall the tick) until session end. This keeps the tick path fed and deterministic.
- Mid-session JOIN is explicitly DEFERRED to a later iteration: it requires a deterministic
  world-state handoff (the joiner must adopt the exact tick-T state) + roster-change-without-
  desync, which is a separate research+spec item. Flag it; do not smuggle it into #5.

### Phase M6 — TCP topology for real N-peer (B5)
- Adopt **star-through-host**: the host relays every peer's input to every other peer (delay-
  based lockstep tolerates the extra hop; the horizon absorbs it). Avoids an N² mesh and a
  separate NAT story per pair. The `ILockstepTransport` seam is unchanged per-link; add a host
  relay that fans Input/Hash/Bye frames out. LAN/loopback first; internet/NAT is later.

---

## 4. Determinism bump ledger (explicit, per the two-bumps discipline)

Current chain: `d950a6afc12a5cdc` (iter-5) → `f17726d44054d133` (A1 aether, bump #4). Erosion is
per-preset (off the default chain). **#5 adds ONE more deliberate bump:**

- **Bump #5 (M1): non-empty `entities` sub-hash** when player avatars exist on the default
  headless lane. Its OWN commit, heavy-oracle + LREC1 replay + lockstep + lint re-bless in that
  commit. M2–M6 are then world_hash-neutral GIVEN M1's canonical (movement changes WHAT avatars
  do, but the contract is "same inputs → same hash", which the M2 gate asserts).
- M4 streaming is residency-only → NEUTRAL (the foundation already proved this).

---

## 5. Risks / open questions for owner

1. **Bump sequencing.** M1 is a third world_hash bump this arc. Confirm it lands AFTER any
   remaining Wave C erosion/worldgen bump so the chain stays one-bump-per-commit attributable.
2. **Per-anchor budget starvation.** Shared 8192 budget across N far-apart players may under-
   serve everyone. Spec'd as "measure then add per-anchor floor" — owner OK with deferring the
   floor until a 3–4 player capture shows starvation?
3. **Fixed roster v1.** Mid-session join is deferred (needs deterministic state handoff).
   Confirm a fixed-roster-at-handshake first cut is acceptable for the first multiplayer
   milestone.
4. **Player count target.** Spec assumes a small co-op N (2–4, matching a zen co-op photography
   game). Confirm the intended max — it sizes the budget + topology decisions.
5. **Avatar physics scope.** Do players collide with each other / with world physics on the
   server, or is movement kinematic (positions integrated, terrain-clamped, no inter-player
   collision) for v1? Kinematic is the lower-determinism-risk first cut.

---

## 6. Gate plan (every phase validated; no silent scope cuts)

| Phase | New/extended gate | Asserts |
|-------|-------------------|---------|
| M1 | `HeadlessServerTick` N-avatar mode; bump re-bless | entities sub-hash non-empty + run==replay; new canonical |
| M2 | `LockstepLoopback` 2-avatar-walk-apart | both peers converge to one world_hash with distinct movement |
| M3 | `LockstepLoopback` / `LockstepFaultInjection` 3–4 peer | N-peer convergence + N-peer desync localization |
| M4 | `HeadlessServerTick` multi-anchor | union served, per-avatar near-surface resident, residency-only (hash == N=1 forwarding) |
| M5 | new `SessionRosterLeave` | a left player's idle-input keeps the tick fed; no stall, no desync |
| M6 | `NetworkedSession` star-relay | host fans inputs to all peers; loopback + LAN |

**Sequencing rule (unchanged):** a determinism bump is discharged ONLY by a passing re-run of the
heavy oracle + LREC1 replay + lockstep in the bump commit — never by reclassification.

---

## 7. Recommendation

#5 is **well-defined and unblocked at the streaming layer**; the real work is M1–M3 (player
avatars + movement input + N-peer session), which is a SIM/NET feature with one deliberate
world_hash bump, not a rendering task. Suggest scheduling #5 as its own focused leg AFTER the
owner answers §5 (player-count target, fixed-roster-v1, kinematic-vs-physics), since those
answers size M1/M4/M6. The four render debt items (#1–#4) are landed and gate-green; this spec
discharges the owner's "spec the blocker first" precondition for #5.
