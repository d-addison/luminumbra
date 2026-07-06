# Spec 024 — Aether Field Completion: the stateful energy layer (A1 arc)

**Status:** DRAFT (AETHER-05, 2026-07-05). Items: AETHER-05/06/07/11/12/08 (ranks 109–112).
**Charter:** spec-021 backlog A1 arc — all default-OFF, ZERO re-pins; activation
(`sim.aether_state` ON) is a deliberate owner-menu hash bump with a full evidence bundle.

## Problem

The engine has the RE-DERIVABLE half of the aether field: `AetherFieldSystem`
(T-I6-A1, seed +14) is a pure function of (seed, tick, region origin) — ambience only.
Nothing a player or creature DOES can leave a trace in it: no emitters, no absorption,
no persistence, no gameplay causality. The game pillar (LuminCrystal attunement,
photography of energy phenomena, creature aether sensitivity) needs a STATEFUL layer:
gameplay-driven deposits that persist, decay, and diffuse deterministically — while the
canonical world stays byte-identical until the owner activates it.

## Requirements

- **FR-024-1 (keystone, AETHER-06):** a stateful energy field layer
  (`EnergyFieldState`, engine-generic naming — the split-lint bans "aetheric" under
  `src/`) composing the existing solver primitives: id-ordered integer-quantized
  deposits → integer decay → pinned-iteration diffusion. Grid geometry mirrors
  `AetherFieldSystem` (24 m cells, 64×64, region-anchored) so the two halves sample the
  same identity. SysKey `sim.aether_state` (hashed, default-OFF) gates the tick; when
  OFF the layer allocates nothing and computes nothing.
- **FR-024-2 (determinism truth domain):** field truth is INTEGER fixed-point
  (`std::uint16_t` raw units per cell per channel, 1 gameplay unit = 256 raw ⇒
  0–255.996 gameplay units per cell — headroom pinned; Codex flagged 1024-raw's
  ~64-unit ceiling as too tight vs. deposit clamps; saturating adds) — the water-system lesson verbatim: float mirrors are for Render
  only, and per the research brief NO float exists anywhere in the hashed kernel.
  Diffusion is a NEW integer conservative gather kernel in the fields library (beside
  `ScalarFieldDiffusion`, reusing its fixture/gate conventions): double-buffered read
  snapshot, per-neighbor truncating outflow `out = (v * rate) >> k` with the RESIDUE
  STAYING IN THE SOURCE cell — number-conserving by construction (the
  number-conserving-CA result), pinned iteration count. Decay is exponential integer
  multiply-shift `v' = (v * (2^k − d)) >> k`, which provably reaches EXACTLY 0 (floor
  division guarantees monotone decrease ≥ 1 once v < 2^k/d — no epsilon tail).
  Deposits apply every tick; diffusion+decay run every 8 ticks on a FIXED stagger
  schedule `(region_index + tick/8) % N` (tick-derived, never load-derived — the ONI
  lesson). All cadence/solver constants are pinned (changing any = deliberate bump).
- **FR-024-3 (sub-hash, zero re-pin — Codex-revised):** `aether_state:v1:` additive
  sub-hash, empty-neutral: contributes ZERO bytes when the flag is OFF or the field is
  all-zero. It hashes the ENTIRE nonzero sparse page set — paged-out cells AND their
  last-touched epochs included, never just the active window (two sessions with equal
  windows but different paged-out state MUST hash differently). Fold: CONDITIONALLY
  into the EXISTING `|aether:` term of `ComposeWorldHash` — when ON, the aether slot
  becomes `StableChecksum(rederivable_bytes + aether_state_tag_bytes)`; when OFF the
  slot is byte-identical to today. NO new ComposeWorldHash term (the `|plants:`
  precedent proves a new term literal moves the composite even when empty). IN
  ADDITION (Codex finding A): `aether_state` is surfaced as its OWN named
  sub-hash/diagnostic field beside scent/ecology in the runner's sub_hashes set — the
  heavy oracle compares it directly across save/load + resim when ON (the folded
  `|aether:` composite embeds the tick-dependent re-derivable half and is excluded by
  heavy's recompute-and-exclude rule, so without the state-only field a broken
  persisted state could pass heavy), and LREC1 localization carries it. Activation =
  the aether slot moves = the owner-menu bump, no code change beyond flip + re-pin.
- **FR-024-4 (persistence, LOAD-BEARING DECISION):** the stateful layer PERSISTS in
  saves — deposits are input-history hysteresis (the RimWorld-pollution class: no
  seed+tick function regenerates them; replay-rederivation couples load cost to session
  length). Format: a version-tagged per-region record with the CHANNEL COUNT in the
  header (so AETHER-08's polarity channel is a version field, not a new format),
  sparse non-zero encoding, and **absent record ⇒ all zeros** — which makes default-OFF
  free (saves byte-identical when the flag is OFF) and matches the additive sub-hash
  contract. **Tick-epoch rebasing (Codex finding D — LOAD-BEARING):** existing load
  paths reset the sim tick to 0, so absolute last-touched ticks would underflow and
  the tick-derived stagger phase would shift. The record therefore persists its SAVE
  EPOCH (sim tick + cadence phase at save), and load REBASES: catch-up decay for each
  cell's elapsed steps is applied AT LOAD (the bounded sequential loop, FR-024-9),
  last-touched := the loaded tick base, and the stagger phase restores from the
  persisted cadence phase so the next diffusion pass fires at the same relative step.
  Proving signal (c) exists precisely to pin this. When `sim.aether_state` is ON, the
  heavy oracle treats `aether_state` like water/terrain/entities (authoritative
  round-trip state) via the state-only sub-hash (FR-024-3), NOT like the re-derivable
  wind/weather/aether trio it recomputes-and-excludes.
- **FR-024-5 (emitters, AETHER-07):** engine `FieldEmitterComponent` — additive opt-in
  idiom (zero-default, participant-gated): `{channel, rate_raw_per_tick, radius_cells}`
  (channel as a data key, the Factorio `emissions_per_minute` precedent, so polarity is
  data not API). Deposit application is TWO-PHASE per tick: gather `(emitter_id, cell,
  amount)` into a buffer, sort by (cell, emitter_id), apply with saturation — never
  iterate a hash container into the field (the FFF-52/WATER-17 ordering law); a
  max-cell clamp at deposit time (ONI back-pressure) bounds accumulation.
  No component ⇒ no work, no hash bytes. Lua binding `sample_energy_field(x, y, z)`
  (read-only sampler) registered in LuaState + added to the LuaApiManifest baseline
  (manifest gate enforces the contract). Game-side alias `get_aetheric_value` lives in
  `scripts/` only.
- **FR-024-6 (render modulation, AETHER-11):** `u_aetherMaterialModulation` uniform,
  default 0.0 ⇒ pixel-identical (RenderParityFrame EXACT 0.0). Monotonic
  RenderSmokeTest: modulation 0 → luminance L0, modulation up → luminance
  non-decreasing at the probed emissive texel.
- **FR-024-7 (stimulus, AETHER-12):** `StimulusChannel::Aether = 5` (append-only;
  `kStimulusChannelCount` → 6; Sample + name-table cases). The channel samples the
  STATEFUL layer when ON, else the re-derivable field (ambience fallback) — creatures
  react to the composite energy environment. Photo scorer gains an `aether_glow` axis
  (pure function; seed +24 registry unchanged). Lazy registry ⇒ hash-neutral for
  non-subscribers (the canonical roster subscribes to nothing).
- **FR-024-9 (world-anchored truth — Codex-revised):** the re-derivable field lives on
  a REGION-ANCHORED scrolling grid (pure function of origin — scrolling is free).
  Stateful truth CANNOT: a scrolling grid would drop deposits when the window moves.
  The stateful layer's truth is WORLD-ANCHORED sparse pages (cells keyed by
  world-quantized coords, Factorio's generated-chunks model); the ACTIVE window (the
  64×64 region around the SERVER's authoritative streamed anchor — never client
  camera state) is the only part that ticks. **Window-boundary rule (pinned):** the
  window edge is SEALED — no outflow across it; the truncating-outflow kernel already
  keeps residue in the source cell, so a sealed edge conserves exactly (mirrors
  `ScalarFieldDiffusion::seal`); conservation AT THE EDGE is a proving signal. Cells
  paged out are FROZEN with their last-touched cadence step recorded; page-in applies
  CATCH-UP DECAY as the SEQUENTIAL per-step multiply-shift loop — NOT pow-by-squaring,
  which truncates once instead of per step and is NOT bit-equal to the in-window
  sequence — bounded and cheap because the exact-zero property caps the loop at a few
  hundred steps for any uint16 value (once v < 2^k/d it strictly decreases ≥ 1/step;
  cap = first step where v == 0). Diffusion does NOT catch up (documented, Factorio's
  inactive-chunk behavior). The anchor derives from replicated sim input
  (lockstep-safe).
- **FR-024-8 (polarity, AETHER-08):** channel B (Lumin/Umbra polarity) as a second
  component of the same layer under the SAME flag: cell state becomes {energy_mE,
  polarity_mB}. Sub-hash v1 covers both channels from day one (land B before any
  activation so v1 never needs a v2 for polarity). Render tap upgrades R32F → RG32F
  when active.

## Seed offset

**+38** (this spec re-charters: the handoff's +35 is TAKEN by PhotoFilters.h:50; +28 =
LightTools, +36 = germination — grep-verified 2026-07-05; +38 has zero hits). The
append-only registry comment gains `+38 energy-field-state`. INSTINCT-14's stale +36
reservation must likewise re-charter (next free ≥ 39) when it unblocks.

## Determinism firewall (LOAD-BEARING)

- OFF (canonical): no allocation, no tick work, zero sub-hash bytes, saves unchanged,
  ComposeWorldHash aether slot byte-identical → both smokes byte-identical at the Bump-B
  canonicals; that IS the landing gate for every A1 commit.
- ON (test-only until activation): id-ordered deposit application (entity ids sorted
  ascending, the EcologyHash rule) so registry iteration order never leaks; integer
  truth domain (FR-024-2); DeterministicMath only, no libm on the sim path
  (SimDeterminismLint must stay green); pinned iteration counts (changing any solver
  constant = deliberate bump, same as `kAetherDiffuseIterations`).
- Render/Lua sampling is one-way read-only (the T-I6-A1d bridge rule): render and
  scripts consume, NEVER write back outside the emitter component path.
- The re-derivable `AetherFieldSystem` is UNTOUCHED — same seed +14, same constants,
  same sub-hash when the stateful flag is OFF.

## Proving signals

- `AetherEmitterDeterminism.*` (NEW, AETHER-06), the brief's three proving signals:
  (a) **conservation invariant (no-saturation regime)** — closed field, decay off,
  id-ordered deposits sized to never clip, 10k ticks: `sum(cells) == sum(deposits)`
  EXACTLY after every diffusion pass (catches any rounding leak); (a2) **saturation
  accounting** (Codex finding B) — deposits that DO clip, with the clipped amount
  returned by the apply path: `sum(cells) == deposits − clipped − decayed` exactly;
  (b) **order-independence** — two emitters on the same cell registered in opposite
  orders ⇒ identical field bytes + `aether_state:v1:` sub-hash (catches unordered
  application / saturation-order bugs); (c) **stagger/save-resume equivalence** — 1k
  ticks continuous vs. save+load mid-stagger-cycle mid-decay ⇒ identical sub-hash
  trajectory after (pins the FR-024-4 epoch rebase + FR-024-9 sequential catch-up);
  (d) **window-edge conservation** — deposits adjacent to the sealed window edge, no
  decay: total conserved across passes (pins the sealed-boundary rule); (e)
  **paged-out coverage** — two sessions, identical active windows, different paged-out
  cells ⇒ DIFFERENT `aether_state:v1:` (pins whole-page-set hashing). Plus: empty
  field ⇒ empty sub-hash; exact-zero decay (a seeded cell reaches literal 0, no tail).
- `AetherScriptBinding.*` (NEW, AETHER-07): manifest gate green with the new entry;
  sampler returns quantized truth; game alias resolves in scripts/.
- Monotonic RenderSmokeTest (NEW, AETHER-11) + RenderParityFrame EXACT 0.0 at default.
- `AetherStimulusDeterminism.*` (NEW, AETHER-12): channel 5 pure-function; canonical
  roster unaffected (smokes byte-identical).
- `AetherDualChannelDeterminism.*` (NEW, AETHER-08): polarity round-trip + sub-hash
  covers channel B; single-channel worlds hash unchanged when B is all-zero.
- Every commit: both smokes byte-identical (debug a66ab4d049ba9228/a91098d71d742567),
  serial ctest lane, validate_backlog.py OK.

## Research basis (AETHER-05, cited brief 2026-07-05)

Prior art surveyed (full brief in the wave record): **Factorio pollution**
(chunk-grid field, 1/64-tick cadence, threshold-gated 2% cardinal diffusion, data-driven
`ageing` decay + two-tier sinks — wiki.factorio.com/Pollution,
lua-api.factorio.com/latest/types/PollutionSettings.html); **Minecraft light** (0–15
integer BFS steady-state — re-derivable, persisted only as a load cache with
absent-section-recompute tolerance — minecraft.wiki/w/Light, /Chunk_format);
**Dwarf Fortress flows** (3-bit integer levels prove coarse quantization is legible —
dwarffortresswiki.org DF2014:Flow); **RimWorld pollution** (pure per-tile hysteresis —
MUST persist — rimworldwiki.com/wiki/Pollution); **ONI** (per-cell element sim = the
expensive end; back-pressure emission caps; load-derived throttling desyncs —
oxygennotincluded.wiki.gg). Numerics: **Stam GDC 2003** (pinned Gauss-Seidel counts,
never iterate-to-epsilon; semi-Lagrangian advection is non-conservative — skip it on
the stateful layer); **Gaffer on Games** float-determinism/lockstep (integer substrate
for hashed state); **number-conserving CA** (deterministic residue-carrying replaces
probabilistic rounding — sciencedirect.com S0890540120300225); **FFF-52 + GALSOV**
(iteration-order desyncs ⇒ stable-ID ordering before any sim-visible enumeration);
**Noita GDC** (dirty-rect + update-counter boundary discipline, the amortization
pattern if perf ever demands it). Key decisions all trace to a cited precedent:
integer kernel (Gaffer/NCCA), residue conservation (NCCA), two-phase sorted deposits
(FFF-52), exponential shift decay with exact zero (NCCA + DF quantization), persist
(RimWorld class), absent-record-equals-zeros (Minecraft chunk tolerance), fixed
stagger (ONI anti-lesson), every-tick affordability with cadence-8 headroom (Stam +
Factorio).

## SDD trace (TDD-LOCK: every FR → one named proving signal)

| FR | Proving signal (ctest / gate) |
|---|---|
| FR-024-1 keystone gating | `AetherEmitterDeterminism.OffPathAllocatesNothing` + both smokes byte-identical (Bump-B pins) |
| FR-024-2 integer kernel | `AetherEmitterDeterminism.ConservationNoSaturation` (a) + `.ExactZeroDecay` |
| FR-024-3 sub-hash | `AetherEmitterDeterminism.EmptyFieldEmptySubHash` + `.PagedOutStateDiverges` (e); heavy-oracle `aether_state` compare (`-Mode HeadlessServerTickHeavy`, ON fixture) |
| FR-024-4 persistence | `AetherEmitterDeterminism.SaveResumeEquivalence` (c, epoch rebase) |
| FR-024-5 emitters | `AetherEmitterDeterminism.DepositOrderIndependence` (b); `AetherScriptBinding.*` + `-Mode LuaApiManifestGate` |
| FR-024-6 render modulation | `RenderSmokeTest.AetherMaterialModulationMonotonic` + `-Mode RenderParityFrame` EXACT 0.0 |
| FR-024-7 stimulus | `AetherStimulusDeterminism.*` + `-Mode StimulusChannelGate` |
| FR-024-8 polarity | `AetherDualChannelDeterminism.*` |
| FR-024-9 window/pages | `AetherEmitterDeterminism.WindowEdgeConservation` (d) + `.SequentialCatchUpBitEqual` |

Sign-off: Codex gpt-5.5 (high, read-only) NO-GO r1 → all findings folded → **GO r2**
(`.forge/artifacts/engine-framework-roadmap/codex-signoff-aether06{,-r2}.md`).

## Run tier (charted, NOT built)

Activation (`sim.aether_state` ON in systems.game.json) = the owner-menu deliberate
bump with the full evidence bundle + re-pin. Content work (crystal emitter archetypes,
per-species aether sensitivity values) rides the game-content track, not this spec.
