# Networking Heterogeneous Entities — AI/NPCs, Animals, Projectiles (research brief)

**Scope:** How an authoritative dedicated server with STATE REPLICATION (Source / Garry's-Mod model)
replicates entities that are *not* the local player — server-ticked AI/NPCs and animals, and short-lived
ballistic PROJECTILES (a shot arrow) — to 20+ persistent join/leave clients, and the concrete design that
fits Luminumbra's existing pipeline. Builds directly on the P3 replication leg already shipped
(`ReplicationProtocol` Usercmd/Snapshot/Ack, `SnapshotReceiver`/`UsercmdReceiver` reliability,
`ReplicationServer`/`ReplicationClient`, AOI scoping, `SnapshotInterpolator` render-behind interpolation,
`LocalPlayerPredictor` predict/reconcile). Grounds the next sub-phase of `MULTIPLAYER-BLOCKER-SPEC.md`.

This brief calibrates everything to a **non-competitive ZEN CO-OP photography game** — that single fact
collapses most of the hard FPS netcode (favor-the-shooter lag comp, predicted-projectile reconciliation) to
"optional / minimal", and that calibration is the most load-bearing recommendation here.

---

## Recommended approach

**Our generic pipeline is 90% sufficient. Three wire additions and a server-side spawn change carry it.**

The entire snapshot → reliability → AOI → interpolation machine we built for player avatars is, by design,
*entity-agnostic*: a `ReplEntityState` is just a quantized transform with an id and flags, and
`SnapshotInterpolator` already lerps *any* entity that appears in two bracketing snapshots. AI/NPCs, animals,
and projectiles are all "things with a transform the server owns and the client displays" — the same case as a
remote avatar. This is the canonical authoritative-server result: **the local player is seen *in the present*
(predicted); every other entity is seen *in the past* (interpolated from snapshots)** (Gambetta). NPCs and
animals are "every other entity." So:

1. **Add an entity TYPE/archetype field + an anim-state field to `ReplEntityState`** so a client can
   instantiate the right mesh+animation set and play the right clip. This is the one true gap: today a client
   cannot tell a player from a grovestrider from an arrow.
2. **Spawn AI/animals on the SERVER tick.** Move the grovestrider (planner + skinned mesh + idle/walk clips)
   out of the client scenario and into the authoritative `GameSession`/`ServerWorldRunner` world, where
   `RunInstinctSystemOnTick` already runs every fixed tick. The AI *decision-making* (GOAP planner, needs,
   stimulus) runs **server-only**; only the *resulting* transform + anim-state replicate. Clients run **zero**
   AI — they interpolate the creature exactly like a remote player. This is the universal pattern (Source,
   Unreal, Halo): "the server controls all AI since it owns them"; "you should never play animations
   predictively for non-owned objects — always wait for the server's authority."
3. **Projectiles (the arrow): server-authoritative, interpolated, NOT client-predicted, NO lag compensation**
   for a co-op game. The shooter's `Usercmd` action bit fires the arrow on the server; the server spawns a Jolt
   ballistic body, simulates it on the tick, replicates it as a typed `ReplEntityState` like any other entity,
   and the client interpolates it. The arrow is a short-lived entity: spawn → fly → hit/expire → despawn.
   Because there is no adversary to "feel cheated", we do **not** need favor-the-shooter prediction or server
   rewind; a ~100 ms render-behind arrow is invisibly fine when you are photographing wildlife, not fragging.

**Update-rate tiering (relevancy + cadence).** Reuse and extend AOI: distant/idle NPCs replicate at a *lower
rate* than near/active ones. Unreal calls this `NetUpdateFrequency` + `MinNetUpdateFrequency` + adaptive
"meaningful update" detection + dormancy (a stationary idle creature replicates almost never); Source decouples
snapshot rate from tick rate (`cl_updaterate` ≪ tickrate). Our `SnapshotInterpolator` already tolerates
irregular per-entity cadence (it lerps whatever it has and clamps), so a per-entity rate tier needs only a
server-side "should I include this entity in this client's snapshot this tick?" decision keyed on
distance + a per-archetype base cadence + a dirty/idle flag.

**Determinism note (unchanged posture).** Server-authoritative ⇒ the AI and projectile sims need **NO**
cross-machine bit-exactness — clients never run them, they only display. `world_hash` stays the *server's*
replay/save/debug invariant. But the moment AI creatures live on the headless server, the `entities` sub-hash
(currently empty: terrain/water only) becomes **non-empty** — a *deliberate* bump, exactly like the player-avatar
bump already planned (spec §4). Land it in its own commit with the heavy-oracle + LREC1 + lockstep re-bless.

---

## Wire / model changes

### `ReplEntityState` (today → proposed)

Today: `{ u32 entity_id; i32 px/py/pz_mm; i16 yaw_mrad; u8 flags }` — **no type, no anim**.

Proposed additions (keep the quantized-transform discipline; bit-pack later if budget needs it):

| Field | Type | Purpose |
|---|---|---|
| `entity_id` | `u32` | unchanged (server-assigned; arrow ids drawn from a high range to avoid clashing with avatar/player ids) |
| `type_id` | `u16` | **NEW.** Compact archetype/class id (player, grovestrider, arrow, …). Client maps it to a mesh + animation set + behavior-display config via a shared archetype table. This is Quake3's `eType`, Source's server-class index, Unreal's replicated actor class. Send it *every* snapshot (1–2 bytes, trivial) so a fresh-joiner / packet-loss recovery never sees a typeless entity — simpler than a separate spawn message and idempotent. |
| `anim_state` | `u8` | **NEW.** Small clip enum (idle / walk / run / fly / impact…). Client plays the mapped clip. Cheap pose conveyance — **never** send joint data. |
| `anim_phase` | `u8` | **NEW (optional).** Normalized clip time [0,255] → [0,1) so a replicated creature's clip is roughly phase-aligned across clients and resyncs after a dropped snapshot. Omit for v1 if budget-tight (client can free-run the clip from `anim_state`). |
| `px/py/pz_mm` | `i32` | unchanged quantized position |
| `yaw_mrad` | `i16` | unchanged facing |
| `flags` | `u8` | unchanged; reserve a bit for "this is the client's own avatar" so the client predicts that one and interpolates the rest |

Cost: +4 bytes/entity worst case (+~3 if `anim_phase` dropped). At our measured ~65 kbps/20-avatar baseline
this is a small, bounded increase; AOI + rate-tiering offset it for the larger entity counts AI/animals add.

### `SnapshotMsg` — spawn vs update vs despawn

- **Spawn / update:** *No new message needed.* A typed `ReplEntityState` that the client has not seen before
  *is* the spawn (instantiate `type_id`'s mesh at the transform); a subsequent one is an update. Carrying
  `type_id` every snapshot makes spawn idempotent and join/recovery trivial (Quake3 "baseline" idea, Source
  "full snapshot on connect"). This matches our full-set-then-delta plan.
- **Despawn:** **Add an explicit, RELIABLE despawn signal for short-lived entities.** Today a full snapshot
  drops absent entities implicitly (client removes any entity not in the newest snapshot) — fine for a player
  who leaves, *not* fine for an arrow. An arrow that spawns and despawns *between* two snapshots a client
  actually receives (lost datagram, low cadence) would be **missed entirely** or **linger as a ghost**
  (interpolator clamps to the last-seen state). Two robust options, pick per reliability appetite:
  1. **`despawn` field/list in the snapshot** (a small set of recently-removed entity_ids carried for a few
     snapshots so a single dropped packet can't strand a removal) — simple, AOI-friendly, no new channel.
  2. **A tiny RELIABLE event channel** carrying spawn/despawn for short-lived entities (the reliable-ordered
     sibling to the unreliable snapshot stream that the replication research already calls for). Halo's lesson:
     "cosmetic events … are only sent once, so there is no guarantee they will arrive" — so anything whose
     *absence* would be visibly wrong (a stuck arrow) must not ride a fire-and-forget path.

  **Recommendation:** option (1) for v1 (a `removed_ids` vector in `SnapshotMsg`, retained for ~N snapshots)
  — it stays on the existing unreliable-but-most-recent-wins path and needs no new transport, while making
  removal robust to single-packet loss. Promote to (2) only if a reliable event channel lands for other reasons.

### `BuildAvatarReplStates` → `BuildEntityReplStates`

Generalize `World::BuildAvatarReplStates(avatars)` (in `PlayerAvatar.cpp`) into a
`BuildEntityReplStates(registry, ...)` that walks the `entt` registry's replicated entities (avatars + AI
agents + animals + live projectiles), reads each one's transform + archetype tag + current
`ActionPlanComponent`/animation state, and emits a typed `ReplEntityState`. The avatar path becomes one
contributor. AOI cull + per-entity rate tiering apply at this layer (or inside `ReplicationServer::BuildSnapshot`,
which already does the AOI radius filter).

---

## Alternatives considered (and why rejected)

| Approach | Verdict | Why |
|---|---|---|
| **Run AI/animal logic on the client too** (client-side AI, server only corrects) | **Rejected** | Reintroduces the cross-machine determinism tax we explicitly dropped, lets clients disagree about creature behavior, and contradicts the authority model. Source/Unreal/Halo all run AI server-only; clients interpolate. "Never play animations predictively for non-owned objects." Zero benefit for a co-op game where creatures aren't latency-critical. |
| **Client-PREDICTED projectiles (favor-the-shooter)** — shooter spawns a local arrow instantly, reconciled with the server's authoritative one (Valve "predicted entities" / Overwatch "predicted rockets") | **Rejected for v1 (zen co-op)** | This is *hard* netcode: prediction-eligible shared code on client+server, predicted-vs-authoritative reconciliation, suppressing the duplicate visual when the authoritative arrow arrives. Overwatch shipped predicted rockets and noted "other major FPS studios said it's not possible." It exists to make *competitive* shooting feel instant. A photographer shooting an arrow has no opponent to out-click; a ~100 ms delay before the arrow appears is imperceptible in intent. Keep the door open (the `LocalPlayerPredictor` pattern + a "predicted entity" flag could extend here later) but **do not build it now**. |
| **Lag compensation / server rewind for arrow hits** (server rewinds targets to the shooter's render time, Valve lag comp / Gambetta Part IV) | **Rejected for v1 (zen co-op)** | Server rewind exists so a competitive shooter who aimed at where the enemy *was* (interpolated, in the past) still registers the hit. In co-op, hits are on the environment / passive creatures with no fairness dispute; sub-100 ms "you hit slightly behind the deer" is a non-issue and may even be desirable (the creature isn't an adversary dodging you). The machinery (per-entity position history ring, rewind/restore around hit checks) is real complexity. **Skip it.** If a future PvP-ish mode needs it, store a short transform-history ring per entity then. |
| **Send full joint/pose data for creature animation** | **Rejected** | Bandwidth-prohibitive and unnecessary. A small `anim_state` enum (+ optional normalized phase) lets the client drive its own local skinning from the same clips the server picked. Standard practice everywhere. |
| **Separate reliable spawn message per entity** (instead of typed-state-is-spawn) | **Rejected as the primary mechanism** | Adds round-trips and ordering coupling against the unreliable snapshot stream. Carrying `type_id` in every snapshot makes spawn idempotent and recovery-trivial (Quake3 baseline / Source full-on-connect). Reliable signalling is reserved for *despawn* of short-lived entities (the genuinely lossy case). |
| **No type field; infer type from id ranges or flags** | **Rejected** | Fragile, conflates id allocation with typing, and can't express archetype variety (multiple creature species, projectile kinds). An explicit `type_id` is 1–2 bytes and is what every shipping engine does. |

---

## Per-class netcode table

| Class | Authority | Sim location | Predicted on client? | Interpolated on client? | Snapshot/update rate | Despawn handling |
|---|---|---|---|---|---|---|
| **Local player avatar** | Server (Jolt CharacterVirtual) | Server tick | **Yes** — `LocalPlayerPredictor` predict + reconcile | No (it's the local one) | Tick-cadence usercmd up; snapshot down | Reliable on leave (player-leave lifecycle, already built) |
| **Remote player avatar** | Server | Server tick | No | **Yes** — `SnapshotInterpolator` render-behind | Full / high (players are interesting) | Implicit on full snapshot (player gone) + leave event |
| **NPC / AI creature** (grovestrider) | Server | Server tick — `RunInstinctSystemOnTick` (GOAP planner, needs, stimulus) | No | **Yes** — same interpolation as remote avatar | **Tiered:** near/active high; **distant/idle low (or dormant)** per AOI + per-archetype cadence | Implicit on snapshot; explicit `removed_ids` if it can vanish abruptly |
| **Animal** (passive) | Server | Server tick (same InstinctSystem) | No | **Yes** | **Tiered**, typically lower than NPCs (idle grazers → near-dormant) | As NPC |
| **Projectile (arrow)** | Server (Jolt ballistic body, spawned from shooter's fire usercmd) | Server tick | **No** (v1; predicted is a rejected-for-now option) | **Yes** (short flight, render-behind is fine) | **High while alive** (fast mover, every snapshot it's in AOI) — short-lived, so cost is transient | **Explicit/reliable** — `removed_ids` (v1) so a single dropped packet can't strand a ghost arrow |

---

## Integration notes (our files)

- **`ReplicationProtocol.h` — `ReplEntityState`:** add `u16 type_id`, `u8 anim_state`, optional `u8 anim_phase`;
  update encode/decode + `operator==` + the `ReplicationProtocol_test` round-trip. Add a `removed_ids` vector to
  `SnapshotMsg` (encode/decode + a retention window in `ReplicationServer`).
- **`ReplicationEndpoint.h` — `SnapshotInterpolator::Sample`:** already lerps shared entities and passes through
  single-snapshot entities — extend so `type_id`/`anim_state` pass through unchanged (don't lerp the enum; carry
  newest), and so `removed_ids` evict an interpolated entity. `ReplicationServer::BuildSnapshot`/`BroadcastSnapshot`
  gains the per-entity rate-tier decision and the despawn-id retention ring. AOI radius filter is reused as-is.
- **`PlayerAvatar.cpp` — `BuildAvatarReplStates` → `BuildEntityReplStates`:** generalize to walk the registry's
  replicated set (avatars + AI agents + projectiles), reading transform + `type_id` (archetype) +
  `anim_state` (from `ActionPlanComponent` / an animation component). Avatar build becomes one path inside it.
- **`GameSession::TickSimulation` / `ServerWorldRunner`:** spawn the grovestrider (and animals) **on the server**
  in the authoritative `entt::registry` at world boot (mirror how `avatar_count` seeds avatars). `RunInstinctSystemOnTick`
  already executes per fixed tick — no change to the AI itself; it just now runs in the headless server world.
  Add a projectile system step: on a shooter's fire action-bit (decoded from `Usercmd`, like `SetAvatarMove`),
  spawn a Jolt ballistic body + a projectile archetype entity; step it each tick; on hit/expiry, destroy it and
  push its id into the despawn ring.
- **Jolt:** projectiles are short-lived dynamic bodies (the player avatar is already a Jolt `CharacterVirtual`,
  so the physics seam exists). Server-only — no client Jolt for projectiles.
- **`entities` sub-hash (`ServerWorldRunner::ComputeWorldSubHashes`):** goes non-empty once AI creatures live on
  the server. Treat as a single deliberate `world_hash` bump (own commit + heavy-oracle/LREC1/lockstep re-bless),
  identical to the player-avatar bump already in the spec. Projectiles, being non-deterministic-relevant and
  transient, should be **excluded** from the `entities` sub-hash unless they must be replay-reproducible — prefer
  excluding them so the hash stays a stable terrain+creature+avatar invariant (decide explicitly and document).
- **Archetype table:** clients and server share a `type_id → {mesh, anim set, behavior-display}` table. The
  existing `scripts/common/archetypes/*.json` (grovestrider.json, shadowstalker.json) is the natural source —
  assign each a stable `type_id`.

---

## Open risks

1. **Short-lived-entity loss over unreliable snapshots.** An arrow spawning and dying inside one snapshot gap
   could be missed or stranded. Mitigated by `removed_ids` retention + carrying type every snapshot, but the
   retention window (N snapshots) must be tuned against worst-case loss; verify with a packet-loss test.
2. **`type_id` / archetype-table drift** between client and server (or across versions). A mismatched id →
   wrong mesh or a crash. Needs a versioned/validated archetype table (protocol-version-gated, like
   `kReplProtocolVersion`).
3. **Rate-tiering correctness.** A creature that goes idle-dormant then suddenly acts must wake its replication
   promptly (Unreal's `FlushNetDormancy` problem) or clients see a teleport. Need an "activity changed → force
   include next snapshot" rule.
4. **`entities` sub-hash scope decision.** Whether projectiles count toward the replay hash is a real fork;
   including non-deterministic transient bodies could make the hash noisy. Recommend excluding — but it must be
   a deliberate, documented choice, not an accident.
5. **anim_phase desync.** Free-running clips on the client drift from the server over a long-lived creature;
   `anim_phase` resync mitigates but costs a byte. Acceptable to omit for v1 and revisit if drift is visible.
6. **Future scope creep toward FPS netcode.** If a later mode wants competitive shooting, predicted projectiles
   + lag comp become real work. Keep the `flags` "owned/predicted" bit and a per-entity history hook in mind so
   the v1 design doesn't *preclude* them — but explicitly don't build them now.

---

## Citations

1. **Gabriel Gambetta — Fast-Paced Multiplayer (Entity Interpolation; Lag Compensation; Client-Side Prediction
   & Server Reconciliation).** https://www.gabrielgambetta.com/entity-interpolation.html ·
   https://www.gabrielgambetta.com/lag-compensation.html ·
   https://www.gabrielgambetta.com/client-side-prediction-server-reconciliation.html — Canonical statement of
   the asymmetry our whole design rests on: **"the user's player is seen *in the present* and the other entities
   are seen *in the past*"** — i.e. predict your own avatar, **interpolate everything else** (remote players, AI,
   animals, projectiles). Lag compensation: the server "rewinds" targets to the shooter's view-time so a shot at
   an interpolated (past) target still registers — the technique we deliberately **skip** for zen co-op.
2. **Source Multiplayer Networking / Networking Entities / Prediction / Lag Compensation / Weapon Prediction —
   Valve Developer Community.** https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking ·
   https://developer.valvesoftware.com/wiki/Networking_Entities ·
   https://developer.valvesoftware.com/wiki/Prediction ·
   https://developer.valvesoftware.com/wiki/Lag_compensation ·
   https://developer.valvesoftware.com/wiki/Weapon_Prediction — Entities link to classes via
   `LINK_ENTITY_TO_CLASS` and are created by `CreateEntityByName`; **SendProps** describe each networked field's
   bit-encoding/min-max; client classes stay in sync with the server class via the **server-class index**
   (our `type_id`); **baseline + delta** (full snapshot on connect, deltas after). **Prediction is only for the
   local player and entities he alone affects** — NPCs are server-authoritative and not predicted. **Weapon
   prediction** (predicted entities) is the optional, complex shooter feature we reject for v1.
3. **Unreal Engine — Actor Relevancy & Priority; Property Replication; Network Dormancy; Detailed Actor
   Replication Flow.** https://dev.epicgames.com/documentation/en-us/unreal-engine/detailed-actor-replication-flow-in-unreal-engine ·
   https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-network-dormancy-in-unreal-engine ·
   https://www.mattgibson.dev/blog/unreal-replication-settings — `NetUpdateFrequency` /
   `MinNetUpdateFrequency` + **adaptive "meaningful update" frequency** + **dormancy** are the production form of
   our per-entity **update-rate tiering** (idle/distant NPCs replicate rarely or not at all); the server gathers
   only each connection's **relevant** actors and the **actor class is replicated** so the client spawns the
   right type.
4. **Quake 3 Network Model — Fabien Sanglard.** https://fabiensanglard.net/quake3/network.php — Snapshots carry
   only entities the client could need (PVS/relevancy); `entityState_t` is a fixed field set delta-compressed
   against the **last *acked*** snapshot; entity presence/absence in the snapshot set *is* the
   create/remove signal (the model we extend with `type_id` + an explicit `removed_ids` for short-lived arrows).
5. **GDC — Overwatch Gameplay Architecture and Netcode (Tim Ford) & "I Shot You First: Networking HALO: REACH"
   (David Aldridge), with Wolfire's Halo summary.** https://www.youtube.com/watch?v=vTH2ZPgYujQ ·
   https://www.gdcvault.com/play/1014345/I-Shot-You-First-Networking ·
   http://blog.wolfire.com/2011/03/GDC-Session-Summary-Halo-networking — Overwatch built **predicted projectiles
   ("predicted rockets")** for a *competitive* shooter and noted others called it impossible — evidence the
   feature is hard and exists for competitiveness we don't have. Halo is **server-authoritative** (server is the
   source of truth, simulates projectiles + hit detection, client predicts and is overridden); **cosmetic
   events are sent once with no delivery guarantee** — the explicit warning behind our "despawn must be reliable"
   recommendation.
6. **Glenn Fiedler (Gaffer On Games) — State Synchronization & Snapshot Compression.**
   https://gafferongames.com/post/state_synchronization/ · https://gafferongames.com/post/snapshot_compression/ —
   The **priority accumulator under a per-packet byte budget** that bounds bandwidth as entity count (incl. AI +
   projectiles) grows, and the quantized bit-packing (smallest-three quaternion, delta positions) sizing the
   per-entity cost of adding `type_id`/`anim_state`. Confirms state-replication (not lockstep) is right "when
   bit-level determinism across platforms is impractical."
