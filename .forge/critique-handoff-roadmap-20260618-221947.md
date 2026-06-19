# Forge-Critique — HANDOFF "Forge-driven roadmap driver (configurable systems edition)"

Target: `.forge/artifacts/handover/HANDOFF-2026-06-18-forge-roadmap-driver.md`
Date: 2026-06-18 · Branch: `feat/polyglot-audit-roadmap` (LOCAL-ONLY)
Method: 3-agent devil's-advocate pass (2 fact-check, 1 plan-attack), consolidated.

## Bottom line
- **The handoff's factual claims are sound.** Tick order, RNG seed offsets (wind+11,
  weather+12/13, aether+14, plant+15), the `Compute*SubHash` folding pattern, every cited
  spec (TDD-LOCK, WAVE-A-SPEC §A2, emergent-ecology/spec, MULTIPLAYER-BLOCKER-SPEC v2,
  long-range-roadmap), test infra (54 test files, `test/ai/plant_growth_test.cpp`,
  `visual_critique.py` + fixtures, `validate-engine-frontier.ps1` with a `FarLodHorizon`
  mode), forge config (TDD mode ON, coverage_gate 95), and all 7 foliage commits — verified.
  The **only greenfield piece** is `src/luminumbra_common/core/SystemConfig.{h,cpp}` +
  `data/common/systems.json` (nlohmann is vendored at `vendor/nlohmann/`).
- **Structural problems:** three contradictions + a missing schedule. Rigor applied to a
  self-contradictory invariant, a serialized foundation, and an all-OFF baseline yields
  *confident green on the configuration nobody ships*.

Fact-check corrections to fold in: no discrete "foliage brief"/"procgen brief" — foliage
research is in `research/gpu-grass.md`, procgen is implied in `shieldrt-gpu-sdf.md`;
`FarLodHorizon` is a **gate mode in `validate-engine-frontier.ps1`**, not a
`visual_critique.py` detector.

## Top objections (severity-ordered)

1. **[BLOCKER] "Byte-identical baseline" vs. "fold a config sub-hash into world_hash" collide
   at first introduction.** The first commit mixing any `ComputeConfigSubHash` into
   `world_hash` moves the hash even at all-defaults (`H(s) ≠ H(s‖sub)`). → Make the sub-hash
   **versioned + order-independent + default-constant**: `world_hash = H(sim_state) ⊕ H(sorted
   non-default sim flags)`; one intentional re-pin at §1, zero thereafter at defaults. Rewrite
   the doctrine line.
2. **[BLOCKER] Ordering contradiction:** D (erosion) is a stated prerequisite for C (SHIELD-RT)
   bless, but §6 lists C before D → C's hardest baselines get blessed against wrong terrain,
   re-blessed twice. → **D before C, unconditionally.**
3. **[BLOCKER] "Land EVERYTHING" ≈ 18 multi-week systems, no per-item budget** → silent
   de-scope of load-bearing-but-boring work. → Prioritization pass (game-critical /
   foundational / gold-plating); treat as a backlog to pull from.
4. **[HIGH] SystemConfig-first = big foundational rewrite before any feature**, serializing
   risk through working green code (the just-tuned palette/atmosphere). → Split §1: **§1a**
   minimal mechanism first; **§1b** lazy migration, one subsystem at a time with behavior-diff.
5. **[HIGH] "Everything OFF by default" → integration never exercised**; gates validate the
   all-OFF corner only. → Mandate a gated **"game profile" ON baseline** + named bundles.
6. **[HIGH] Per-tick `enabled()` across 36k entities has no perf budget** (300fps target). →
   Resolved immutable per-tick snapshot: packed bitset + flat enum-indexed params; O(1) bit
   test; explicit perf AC.
7. **[MEDIUM] Full lifecycle on all ~18 items = ceremony > value on small ones.** → Tier the
   process (T1 full / T2 spec+tests+verify / T3 test+implement); determinism tests mandatory
   for all sim; visual re-bless only on render-touching slices.
8. **[MEDIUM] Networking (F) can't be over-the-wire validated on one PC** → real ACs can't go
   green. → Scope F to the `ILockstepTransport` seam + TCP local path; defer
   delta-snapshot/prediction until a 2nd box or a deterministic net-sim harness exists.
9. **[MEDIUM] Procgen/erosion "pure function" rests on float math** (cross-platform desync
   risk if sim-read). → Enforce VISUAL-ONLY / fixed-point-at-sim-boundary as an AC with a
   parity test; bake sim-read erosion to integers + commit + hash as data.
10. **[MEDIUM] No rollback protocol, no prioritization criterion** (G *is* the game, scheduled
    last). → Rollback rule (break → revert slice by default; re-pin only with written
    justification) + per-commit baseline-hash tag; prioritization tags.

## Disposition — revisions folded into the handoff
1. Corrected hash contract (versioned/order-independent/default-constant); doctrine line fixed.
2. §1 split into §1a (minimal mechanism + resolved snapshot, O(1) perf AC) / §1b (lazy migration).
3. Reorder **D before C**.
4. Game-profile ON baseline + named bundles, gated alongside all-off.
5. Tiered forge lifecycle.
6. F scoped to the transport seam; rest gated behind a net-sim harness / 2nd box.
7. VISUAL-ONLY / fixed-point-at-sim-boundary AC + parity test for A and D.
8. Rollback rule + per-commit baseline-hash tag + prioritization tags.

## Non-goals recorded
- Big-bang migration of all existing toggles in §1 (→ lazy §1b).
- Full delta-snapshot/prediction/reconciliation for F until testable.
- Uniform full-lifecycle ceremony on Tier-2/Tier-3 items.

## Owner decisions captured (2026-06-18)
- Adopt revised scope; revise handoff; spec everything; continue autonomously.
- G (photography) ordering vs. "engine first": honor engine-first for the actual photo loop;
  only a **pure-sim capture-scoring determinism fixture** is pulled forward as a cheap derisk.
