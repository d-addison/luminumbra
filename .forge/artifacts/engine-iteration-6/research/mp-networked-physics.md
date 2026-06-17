# Server-Authoritative Networked Physics at Scale — Research Brief

**Slug:** `mp-networked-physics` · **Iteration:** 6, Wave C · **Side:** SIM + NET (engine-generic)
**Date:** 2026-06-17 · **Author:** research dispatch (Opus 4.8)

## Scope

How to run an **authoritative dedicated server** that simulates a single Jolt physics world
for **20+ players plus dynamic props** (the owner's explicit "Garry's Mod model": players AND
props collide, the server owns one physics world), and how to **replicate** that rigid-body
state to clients. This brief recommends a concrete approach tied to our existing engine
(deterministic 30 Hz `SimulationClock` + `GameSession::TickSimulation`, Jolt `PhysicsSystem`,
headless `ServerWorldRunner`, `world_hash` machinery), and explains why **state-synchronization
replication** beats deterministic lockstep for these requirements. It aligns with the live
v2 plan in `MULTIPLAYER-BLOCKER-SPEC.md` (authoritative server + snapshot replication + client
prediction) and supplies the physics-specific detail that plan defers to research.

The recommendation is grounded in primary sources: Glenn Fiedler's *Networked Physics* series
and his Oculus/GDC *Networked Physics in Virtual Reality* writeup, the Jolt Physics
architecture/determinism documentation and island-solver design, and the Valve Developer
Community wiki on Source multiplayer networking and VPhysics.

---

## Recommended approach

**State-synchronization replication of authoritative Jolt bodies**, server-owned, snapped on the
client, with priority-accumulator scheduling, quantized + delta-compressed transforms, sleeping
bodies excluded, and per-prop ownership/authority tracked by sequence number. Concretely:

### 1. One authoritative Jolt world on the server
The server runs the single true `JPH::PhysicsSystem` inside `ServerWorldRunner`'s existing fixed
30 Hz loop. Clients never simulate props authoritatively — they send quantized input (`usercmd`),
the server steps physics, and the server is the sole source of truth for every dynamic body.
This is exactly the Source/Quake/Garry's-Mod lineage: *"the server simulates the game in
discrete time steps called ticks … During each tick, the server processes incoming user commands,
runs a physical simulation step, checks the game rules, and updates all object states"*
[Valve, Source Multiplayer Networking].

### 2. State sync, not input replay
We send **both input and state**, and run a (light) simulation on the client too. Fiedler:
*"Because we send state, we don't need perfect determinism to stay in sync, and because the
simulation runs on both sides, objects continue moving forward between updates"*
[Fiedler, State Synchronization]. Each per-body update carries **position, orientation, linear
velocity, angular velocity**; the client extrapolates from the last update applied to each body.
This is the model Fiedler shipped in the Oculus VR sample over non-deterministic PhysX, proving
*"PhysX non-determinism could be managed through regular state snapshots rather than requiring
deterministic simulation"* [Fiedler, Networked Physics in VR].

### 3. Priority accumulator (bandwidth-bounded scheduling)
We cannot fit every body in every packet at 20+ players. Use Fiedler's **priority accumulator**:
each body accumulates a per-frame priority; each packet sorts by accumulated value, includes the
top-N that fit the MTU budget, **resets the accumulators of included bodies**, and leaves the
rest to rise next packet [Fiedler, State Synchronization]. Priority is boosted for bodies near a
client (interest management) and for recently disturbed bodies; resting/distant bodies decay to
near-zero send rate. Fiedler's demo packed up to **64 state updates per packet** out of **901
cubes**.

### 4. Quantize on both sides
Quantize state identically on server and client **before each step** so both integrate from the
same rounded inputs: *"quantize the state on both sides … before each simulation step you
quantize the entire simulation state as if it had been transmitted over the network"*
[Fiedler, State Synchronization]. This keeps the client's local extrapolation from drifting away
from what the wire can represent.

### 5. Transform compression
- **Position:** bound the world and quantize. Fiedler's snapshot work used ~512 values/m (≈2 mm)
  → *"18 bits for x, 18 bits for y and 14 bits for z giving a total of 50 bits per-position
  (originally 96)."* State-sync needs finer precision to avoid extrapolation drift —
  **4096 values/m** [Fiedler, Snapshot Compression; State Synchronization].
- **Orientation:** **smallest-three quaternion**. Drop the largest component (reconstruct from
  unit-length constraint), send a 2-bit index of which was dropped plus the other three in
  `[-0.707107, +0.707107]`. At 9 bits/component that is **29 bits/orientation** (down from 128);
  state-sync uses **15 bits/component** for stability [Fiedler, Snapshot Compression; State
  Synchronization].
- **Delta compression:** encode against a recently-acked baseline per body. Fiedler reached
  **~80 bits (10 bytes) per cube** after delta + quantization, taking the demo from 17.37 Mbit/s
  down to **under 256 kbit/s per player** [Fiedler, Snapshot Compression; Networked Physics in VR].

### 6. Sleeping bodies are not replicated
Jolt deactivates bodies that come to rest, and *"Bodies will not automatically wake up when
created … Neighboring bodies will not be woken up when bodies are removed"* [Jolt, README]. A
prop at rest gets one final "now at rest" update and then **stops consuming bandwidth** until it
wakes. Fiedler encodes an at-rest body as a **single bit** instead of velocity vectors, and
forces stability by *"forcing cubes to rest if unmoving for 16 frames"* [Fiedler, Networked
Physics in VR]. For a 20-player sandbox where most props are settled most of the time, this is
the single biggest bandwidth win.

### 7. Ownership + authority for props
Adopt Fiedler's two-concept scheme, both carried as sequence numbers in the body's state:
- **Authority** transfers to whoever last interacted with a body and propagates through
  collisions: *"a cube thrown by player 2 could take authority over any objects it interacted
  with, and in turn any objects those objects interacted with, recursively"* — this lets a
  client predict its own thrown props smoothly.
- **Ownership** is exclusive grab-lock: *"Once a cube is owned by a player, no other player could
  take ownership until that player relinquished ownership"* [Fiedler, Networked Physics in VR].

Because we run a **dedicated authoritative server** (not Fiedler's peer host-arbiter), the server
is the final arbiter: client authority is a *prediction hint* the server validates, not a
hand-off of truth. Held props are excluded from the generic physics broadcast and ride the
owning player's avatar state.

### 8. Client snap + visual smoothing, jitter buffer
On receipt the client **snaps** the rigid-body state hard into its local sim, then hides the
visible pop by carrying a decaying **position/orientation error offset at render time** (not in
the sim): Fiedler blends two smoothing factors — *"0.95 for small errors (≤25cm)"* and
*"0.85 for large errors (≥1m)"* [Fiedler, State Synchronization]. A **jitter buffer** holds
incoming packets so they are delivered on the correct frame/sequence — Fiedler used 4–5 frames at
60 Hz for state sync, and a **100 ms** buffer for VR avatars [Fiedler, State Synchronization;
Networked Physics in VR]. At our 30 Hz this is ~2–3 ticks (~66–100 ms).

---

## Alternatives considered (and why rejected)

### Deterministic lockstep physics — REJECTED (primary alternative)
Send only inputs; every machine re-simulates the identical world [Fiedler, Deterministic
Lockstep]. Rejected for all three of our hard requirements (and this matches the v2 pivot already
recorded in `MULTIPLAYER-BLOCKER-SPEC.md`):

1. **Stalls on the slowest peer / tail latency.** A lockstep tick cannot advance until every
   participant's input arrives. Fine at 2 co-op players; fragile at 20 — one hiccup freezes
   everyone.
2. **No clean mid-session join/leave.** A joiner must replay from tick 0 or transfer full state;
   a leaver's missing input stalls the tick. Our requirement is an open persistent server.
3. **Requires bit-exact cross-MACHINE determinism.** Jolt *can* do this — there is a
   `CROSS_PLATFORM_DETERMINISTIC` build (*"approximately 8% slower … deterministic regardless of
   compiler … OS … architecture … word size"*) — but it demands strict discipline: identical API
   call order, `/fp:precise`, `-ffp-contract=off`, Jolt's own `Sin`/`Cos`, `QuickSort` not
   `std::sort`, no `std::hash`, and it **explicitly excludes** broadphase queries and listener
   callbacks (multithreaded → non-deterministic) [Jolt, Architecture/Determinism]. That is a
   heavy perpetual tax that state-sync simply does not need, because **the server is the only
   authority and we ship state, not just input** [Fiedler, State Synchronization].

State-sync wins precisely because it decouples correctness from cross-machine FP determinism:
join = stream a baseline and start sending snapshots; leave = drop the client; no hash
convergence on the hot path.

### Pure snapshot interpolation (no client sim) — REJECTED as the *primary* prop mechanism
Sending whole-world snapshots at a low rate (Fiedler's demo: 10 Hz) and interpolating is simple
and is exactly what Source does for *remote* entities (snapshots ~20/sec, `cl_interp` 100 ms
default) [Fiedler, Snapshot Interpolation; Valve, Source Multiplayer Networking]. But raw
interpolation adds **interpolation delay on top of latency** and, without a client-side sim,
props between updates look stepped. We **keep interpolation for remote entities** but layer
state-sync extrapolation on top for physics props so they keep moving between updates. This is a
hybrid, not a rejection — interpolation is the fallback presentation, state-sync is the physics
path.

### Client-authoritative props (Fiedler's peer model verbatim) — REJECTED
Fiedler's VR sample is peer-to-peer with a host arbiter and full authority hand-off to clients.
For an open server with untrusted/variable-latency clients we keep authority on the **dedicated
server** and treat client authority only as a prediction hint, to prevent cheating and resolve
conflicts deterministically.

---

## Perf / scale budget

**Server physics step.** Jolt is built for exactly this: the constraint solver's parallelism
comes from **simulation islands** — *"a maximal set of active bodies connected … through contacts
or constraints"* — built with a **lock-free union-find** across threads, then solved by jobs that
atomically claim islands; oversized islands are subdivided by the **LargeIslandSplitter** into up
to **31 parallel slots** [Jolt DeepWiki, Island Building & Parallel Solving]. Static/kinematic
bodies (our voxel-chunk collision, the existing `m_chunk_bodies`) anchor islands but are not
solved, and **sleeping props cost ~0** until disturbed [Jolt, README]. Jolt scales to thousands
of active bodies — the shipped benchmark scenes run **3,680-body ragdoll piles**, a **1,240-box
pyramid**, and **4,410 boxes on a mesh** [Jolt, PerformanceTest.md], and Jolt powers Horizon
Forbidden West and Death Stranding 2 [jrouwe/JoltPhysics].

*Order-of-magnitude budget (FLAG: needs profiling on our RTX 5070 Ti target box):* a 30 Hz tick
gives a **33.3 ms** frame; physics should stay a fraction of that. For a sandbox of ~20 players +
a few hundred active props, expect the **active** body set in the low hundreds at any instant
(most props sleeping), well inside Jolt's multicore envelope of low-single-digit milliseconds per
step on a modern multicore CPU. The real risk is a single **giant island** (e.g. one huge prop
stack) serializing the solver — LargeIslandSplitter mitigates but does not eliminate this; cap
stack/island size as a design lever. **FLAG: measure step cost vs. active-island count;
re-derive the active-prop budget from a profiled scene before committing the cap.**

**Replication bandwidth (per player, downstream).** Using Fiedler's measured
**~80 bits ≈ 10 bytes per fully-updated body** [Fiedler, Snapshot Compression]:
- 30 active/visible props refreshed each tick × 10 B × 30 Hz ≈ **~9 KB/s ≈ 72 kbit/s**.
- The priority accumulator caps this regardless of total prop count: pick a per-packet body
  budget (e.g. 32–64 updates) and the rest rises in priority over subsequent ticks.
- Sleeping props ≈ **1 bit each** when touched at all, effectively free [Fiedler, VR].
- Fiedler's whole 901-cube VR demo held **under 256 kbit/s per player** [Fiedler, VR] — a useful
  ceiling. Upstream is tiny (quantized `usercmd` per tick).
- **FLAG:** downstream scales with *visible* prop churn, not roster size, thanks to interest
  management + the accumulator; budget ~256 kbit/s/player as the working ceiling and validate.

---

## Determinism implications

We **keep** our deterministic 30 Hz sim and `world_hash`/sub-hash machinery, but **repurpose**
them. They are no longer a *transport requirement* (state-sync does not need cross-machine
determinism), so we **do not** pay the `CROSS_PLATFORM_DETERMINISTIC` 8% tax or the discipline
burden it imposes [Jolt, Architecture/Determinism]. Instead:

- **Server-side determinism within one binary still matters** for save/load round-trips, replay,
  and desync debugging. Jolt is deterministic on the **same binary** provided *"the APIs that
  modify the simulation are called in exactly the same order"* [Jolt, Determinism]. Our existing
  `ServerWorldRunner` determinism contract (one fixed tick per frame, streaming quiesced, same
  seed → same `world_hash`) and `NetworkStateHash` infra remain valid and useful as a
  **replay/integrity/debug oracle**, not a multiplayer sync gate.
- **Quantize-on-both-sides** (above) is the only "determinism-adjacent" requirement on the hot
  path, and it is local, not cross-machine.
- Beware Jolt's documented non-deterministic surfaces if we ever lean on them for replication:
  broadphase queries and `ContactListener`/`BodyActivationListener`/`PhysicsStepListener`
  callbacks and `GetActiveBodies` run off multiple threads and are **not** order-deterministic
  [Jolt, Determinism]. The replication snapshot must read settled body state, not callback order.

Bottom line: **state-sync makes cross-machine physics determinism unnecessary**; our determinism
work is retained as a single-binary save/replay/debug asset, which is where its value actually is.

---

## Integration notes (our engine)

- **Jolt `PhysicsSystem` (`src/luminumbra_common/systems/PhysicsSystem.{h,cpp}`):** today it owns
  the single `JPH::PhysicsSystem`, a `CharacterVirtual` player, and static `m_chunk_bodies` voxel
  collision. Extend with a **dynamic-prop registry** (`BodyID` → replication id + ownership/
  authority sequence) and a `SnapshotActiveBodies()` read that returns position/orientation/
  linvel/angvel + sleep flag for currently-active dynamic bodies. Drive `update(dt)` from the
  server tick with the canonical fixed `dt` (1/30 s); use the Jolt collision-steps setting for
  sub-stepping if fast props tunnel.
- **`ServerWorldRunner` (`src/luminumbra_server/`):** its fixed-tick loop
  (`physics → TickSimulation → streaming → quiesce`) becomes the authoritative server frame.
  Streaming anchors expand from a single spawn anchor to the **vector of connected players'
  avatar positions** (the multi-anchor foundation `0113a60` already exists per the spec). After
  `TickSimulation`, the replication layer reads `PhysicsSystem::SnapshotActiveBodies()`.
- **Replication layer (new, NET side):** per-connected-client, run the **priority accumulator**
  over (a) that client's avatar, (b) props in its interest set, build a delta+quantized packet
  against the last acked baseline, and ship it. Reuse the existing `ILockstepTransport` framing
  discipline (length-prefixed, no struct padding, hashes/ids as fixed-width) noted in the spec
  for the wire encoding. Clients snap state, smooth render error, run the jitter buffer.
- **`NetworkStateHash` / `world_hash`:** keep as the server replay + save-integrity + desync-DEBUG
  oracle (per spec); **remove it from the multiplayer hot path**.
- **Quantization module (shared common):** one codec used by both server-encode and client-decode
  *and* by the on-both-sides pre-step quantizer (position 4096/m bounded, smallest-three quat at
  15 bits/component) so encode/decode/extrapolate all agree.

---

## Open risks

1. **Giant-island serialization.** One huge connected prop stack can serialize Jolt's solver
   despite LargeIslandSplitter. Need a design cap on island/stack size and a profiled worst case.
   **FLAG: profile.**
2. **Active-prop budget unknown.** The bandwidth and step-cost numbers above are Fiedler/Jolt
   reference figures, not our measurements. Must profile a representative 20-player + props scene
   on the RTX 5070 Ti target before committing per-packet body budgets and prop caps.
3. **Authority conflict resolution on an untrusted server.** Fiedler's recursive authority
   spread is peer-trusting; on a dedicated server it must be a *hint* the server validates, with
   tie-breaks (ownership seq > authority seq) enforced server-side to prevent griefing.
4. **Client mis-prediction of owned props vs. server correction.** Snapping + render-smoothing
   hides small errors; large server corrections (collision the client didn't see) can still pop.
   Tune the two-factor smoothing thresholds against real latency.
5. **Voxel-terrain edits during play.** Our world is editable SHIELD voxels; changing static
   chunk collision under active islands must re-wake affected bodies correctly (Jolt does not
   auto-wake neighbors on body removal) and re-stream collision around all anchors.
6. **Interest management correctness.** Mis-scoped PVS/area-of-interest will either leak
   bandwidth (everything to everyone) or pop props in as players approach. Needs explicit
   per-tick interest sets keyed to streaming anchors.

---

## Citations

1. Glenn Fiedler, **"State Synchronization."** Gaffer On Games. Primary spec for state-sync over
   lockstep, priority accumulator, quantize-on-both-sides, two-factor render smoothing, jitter
   buffer. *"Because we send state, we don't need perfect determinism to stay in sync."*
   https://gafferongames.com/post/state_synchronization/
2. Glenn Fiedler, **"Networked Physics in Virtual Reality"** (Oculus-sponsored; GDC lineage).
   Authority vs. ownership model, recursive authority spread, at-rest = 1 bit, force-rest after
   16 frames, <256 kbit/s/player, managing non-deterministic PhysX via state snapshots.
   https://gafferongames.com/post/networked_physics_in_virtual_reality/
3. Jolt Physics — **Architecture / "Deterministic Simulation"** and **Island Building & Parallel
   Solving.** Same-binary determinism rules, `CROSS_PLATFORM_DETERMINISTIC` (~8% cost),
   non-deterministic broadphase/listeners, lock-free union-find islands, LargeIslandSplitter
   (31 slots), sleeping bodies.
   https://github.com/jrouwe/JoltPhysics ·
   https://deepwiki.com/jrouwe/JoltPhysics/2.6-contact-and-constraint-system

### Supporting sources
4. Glenn Fiedler, "Snapshot Compression." Position quantization (512/m → 50 bits), smallest-three
   quaternion (2-bit index + 3×9 bits = 29 bits), delta to ~80 bits/cube, 17.37 Mbit/s → <256
   kbit/s. https://gafferongames.com/post/snapshot_compression/
5. Glenn Fiedler, "Deterministic Lockstep" and "Snapshot Interpolation" (rejected alternatives).
   https://gafferongames.com/post/deterministic_lockstep/ ·
   https://gafferongames.com/post/snapshot_interpolation/
6. Valve Developer Community, "Source Multiplayer Networking." Server tick (15 ms / 66.67 Hz),
   per-tick simulate + update object states, snapshots ~20/sec, `cl_interp` 100 ms default,
   physics-entity replication. https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking
7. Valve Developer Community, "VPhysics" / "prop_physics." Source/GMod physics props are
   server-VPhysics-simulated; prop velocity is engine-handled and not separately networked.
   https://developer.valvesoftware.com/wiki/VPhysics ·
   https://developer.valvesoftware.com/wiki/Prop_physics
8. Jolt Physics, "PerformanceTest.md" + multicore scaling doc. Benchmark body counts (3,680-body
   ragdoll piles, 1,240-box pyramid, 4,410-box mesh); shipped in Horizon Forbidden West, Death
   Stranding 2. https://github.com/jrouwe/JoltPhysics/blob/master/Docs/PerformanceTest.md ·
   https://jrouwe.nl/jolt/JoltPhysicsMulticoreScaling.pdf
9. Guerrilla Games, "Architecting Jolt Physics for Horizon Forbidden West." Production validation:
   doubled simulation frequency at less CPU vs. prior commercial engine.
   https://www.guerrilla-games.com/read/architecting-jolt-physics-for-horizon-forbidden-west
