# Design note: water-sim lockstep determinism (streaming-arrival coupling)

**Status:** interim **C landed** (`254cec1f`) — full B′ still open · **Author:** determinism investigation (2026-06-25)
**Related:** spec 009 (flowing water), `water-heightmap-read-race` memory, commits `7d605399` (bed), `254cec1f` (C)

> **Update (C landed):** `ServerWorldRunner::Boot` now settles streaming + water to a
> steady state (water reaches a static fixed point — it sleeps) before the counted sim,
> so the start state is the trajectory-independent equilibrium. Default `--smoke` is now
> **0 flakes in 24 cold boots** (was ~66%, ~15% after the bed fix); `--planted-roster` /
> `--ecology-roster` stay run==replay. Canonical hashes changed (water starts at
> equilibrium): default `b05b642e → 6f008a9f`, planted `47f237c7`, ecology `9f0dd5b9`.
> No code pins these literals, so no code re-pin was needed. **C settles the INITIAL
> residency only** — chunks streaming in *during play* (moving) still hit the per-tick
> trajectory coupling, so **B′ (below) remains the full fix.** Trade-off: adds server
> boot warm-up time.

## Problem

The headless `--smoke` determinism gate intermittently fails on the **water** sub-hash
(cold first boot, ~15% of runs after the bed-value fix below; ~66% before it). The
mesh sub-hash also differs but is intentionally excluded; every *other* sim sub-hash
(terrain/entities/wind/weather/aether) is rock-stable.

This is **not** a headless-only artifact. It is a **real (latent) lockstep desync risk**:

- `LockstepSession` exchanges `world_hash` + sub-hashes (incl. `water`) between peers at
  a 30-tick cadence and **HALTS the session on mismatch** (`LockstepSession.h:29-34, 251`;
  `desync_section` can be `"water"`).
- Both peers run the water sim **inside the deterministic tick**
  (`ServerWorldRunner::RunFixedTicks` → `GameSession::TickSimulation` →
  `SHIELD_WorldSystem::update` → `WaterSystem::update`, `SHIELD_WorldSystem.cpp:2014`).
- It is masked today only by the **single-PC testing constraint** (two boots on one
  machine have similar timing). On two real Steam clients with different CPU/scheduling/
  cache behaviour, the divergence would surface as a desync halt.

## Root cause

A chunk's `water_depth_mm` at a checkpoint tick depends on **how many ticks it has been
water-initialized and simulated**, which depends on **which tick it became eligible** —
and that couples to **async generation/meshing completion timing**, not to the
deterministic tick alone.

Two distinct contributors:

1. **Bed value (FIXED, commit `7d605399`).** `water_bed_mm` reused `cp->heightmap_data`
   gated only on array size; a chunk water-initialized while still at a coarse/
   not-yet-LOD0-promoted heightmap seeded a bed that didn't match `GetTerrainHeightAt`,
   and whether it had reached LOD0 by init time was streaming-timing-dependent. Now gated
   on `current_lod == 0` (byte-identical to the sampler per the parity gate), else the
   pure sampler — so the bed is timing-independent. (`WaterSystem.cpp:369-382`.)

2. **Depth trajectory (REMAINING).** Water-init is rate-limited
   (`MAX_WATER_INITS_PER_TICK = 6`) and selected from `m_active_chunks` (a
   `std::unordered_map`); the sim is rate-limited (`MAX_WATER_SIMS_PER_TICK = 64`) via a
   rotating cursor whose evolution tracks the active-set-*size* sequence. When a chunk
   enters the active set — and therefore the tick its water sim starts accumulating — is
   coupled to async generation completion. Different arrival ticks → different step count
   by the checkpoint → different `water_depth_mm`.

**Key empirical constraint for any fix:** an attempt to make the *init selection order*
deterministic (sort the per-tick candidates by chunk id, mirroring the resize path at
`WaterSystem.cpp:320`) made the flake **worse** and destabilized the canonical hash
(`b05b642e` → several values). That rules out "selection order" as the cause: the
nondeterminism is in **which chunks are present/eligible at each tick** (activation +
generation-completion timing), not in how a fixed candidate set is ordered. The final
chunk set is deterministic (terrain never flakes; both hashes read the same
post-`wait_for_streaming_jobs` snapshot) — it is the **per-tick trajectory** that varies.

## Fix options

| # | Approach | Determinism | Risk | Notes |
|---|----------|-------------|------|-------|
| **A** | **Settle generation before water-init each tick** — guarantee all generation/activation scheduled for tick T is completed AND reflected in `m_streaming_state.chunks` *before* `WaterSystem::update` runs (move the streaming-jobs wait/emplace ahead of water-init within the tick). | Makes the water candidate set deterministic per tick **iff** generation *scheduling* is already deterministic (it is: sorted candidates + per-tick budget, `SHIELD_WorldSystem.cpp:2585-2620`). | Medium — re-orders the tick; possible mid-tick stall cost; must confirm the completion-emplace path. | Most targeted. Verify the exact ordering of generation-completion emplace vs `water_system->update` first. |
| **B** | **Deterministic water activation, decoupled from meshing** — water-init triggers on deterministic *radius* membership around the deterministic anchor, using the pure sampler (`GetTerrainHeightAt`), independent of generation/LOD readiness. A chunk's water starts at the tick it enters the radius. | Strongest — water no longer depends on async streaming at all (the sampler needs no generated data). | Higher — larger change to when/where water-init fires; must ensure no double-init / interaction with unload. | Cleanest lockstep-correct model. Pairs well with removing the init cap (D) for the in-radius set. |
| **C** | **Boot load+init barrier** — generate + water-init all in-range chunks before the first sim tick (headless/lockstep boot). | Fixes initial-load determinism (the smoke). | Low for boot; does **not** cover chunks streaming in during play (moving camera). | Partial; good stopgap for the gate but not the moving case. |
| **D** | **Remove per-tick water rate caps** (init/sim unbounded) so every present chunk inits+sims every tick. | Removes the cursor-window trajectory dependence; with deterministic presence (A/B) → fully deterministic. | Perf — the caps exist to amortize worldgen sampling / sim cost (spec 008); unbounded bursts spiked frames. | Only viable combined with a presence fix; likely needs to stay capped for perf. |

## Recommendation

Pursue **B** (deterministic radius-based water activation via the pure sampler) as the
lockstep-correct target, prototyped behind heavy verification; fall back to **A** if B's
blast radius proves too large. **C** is a reasonable interim to green the gate if a full
fix is deferred. Avoid **D** alone.

Before implementing, confirm two things in code:
1. The exact point the generation-completion handler emplaces chunks into
   `m_streaming_state.chunks` relative to `WaterSystem::update` (decides A's viability).
2. Whether `m_active_chunks` membership during the 90-tick smoke is already deterministic
   for the *initial horizon* (synchronously emplaced at boot, `SHIELD_WorldSystem.cpp:2742-2746`)
   and only the *streamed-in-during-play* chunks are async — which would narrow the fix.

## Critical refinement (found while confirming code point #2)

`update_chunk_activation` (`SHIELD_WorldSystem.cpp:2430-2451`) computes the per-tick
`target_radius` via `streaming_radius_for_pressure(...)`, which **takes
`has_active_job(generation)` and `meshing_jobs_active()` as inputs**, and sets
`generation_budget = 0` whenever a generation job is active. So the *wanted/scheduled
chunk set itself adapts to async job-activity timing* — not just chunk-arrival timing.
This means the determinism leak is upstream of water entirely: **the sim-relevant chunk
set per tick is throttled by async streaming pressure.**

Consequence for Option B: it is not enough to change *when water-init fires*. The fix
must give the **simulation** a chunk set that is a pure function of (anchor, tick) and
**independent of the adaptive/async render-streaming pressure**. Concretely:

- **B′ (refined):** separate a **deterministic sim residency set** (fixed sim radius
  around the deterministic anchor, chunk objects emplaced synchronously per tick like the
  boot horizon at `2742-2746`) from the **adaptive render-streaming set** (the existing
  pressure-throttled, async path — keeps driving LOD/meshing for visuals). Water (and any
  hashed sim) operate on the deterministic sim set via the pure sampler; rendering keeps
  the adaptive set. This is the lockstep-correct separation: **sim residency is
  deterministic; render residency is best-effort.**

This is a meaty, perf-sensitive change to the streaming core (the adaptive pressure logic
exists to avoid frame spikes — spec 008). It should land behind a flag / be validated for
both determinism (the verification plan below) and streaming perf (no regression in the
moving-frame / streaming budgets).

## Verification plan

- **Cold-boot flake rate:** loop `luminumbra_server_app --smoke --artifact` ≥20× under
  load; require **0 flakes** (the bug is timing/contention-sensitive, so sample under a
  busy machine, not idle).
- **Sub-hash localization:** assert the `water` sub-hash matches run-to-run via the
  artifact diff (the harness already dumps `sub_hashes` vs `sub_hashes_replay`).
- **Gates:** `WaterDeterminism.*` (4 tests) stay green; `--smoke --planted-roster` and
  `--smoke --ecology-roster` stay run==replay.
- **Re-pin:** expect the canonical `world_hash` to change (trajectory changes); re-pin
  baseline literals and confirm a new *stable* value across ≥20 cold boots.
- **(Ideal, blocked):** two-process compare on separate machines — blocked by the
  single-PC constraint; the under-load cold-boot loop is the best available proxy.
