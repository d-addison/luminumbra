# HANDOFF — post-Waves F/G/H(core): the remaining 54 items, open decisions, and deferred register

**Date:** 2026-07-05 (supersedes HANDOFF-2026-07-04-post-wave-E-and-composability-program.md)
**Branch:** `feat/polyglot-audit-roadmap` (NEVER push; commit per green gate only)
**Author:** the full-campaign orchestrator (Claude Fable 5, session 0191C6QwPSR8Nq8DFjojLjDs)
**For:** the next autonomous agent continuing the spec-021 campaign

---

## 0. TL;DR — where we are, in one paragraph

The spec-021 190-item backlog stands at **136 done / 54 remaining** after one autonomous
session landed Phase 0 (WATER-17 → **Bump A**), **Wave F** (the whole render band incl. the
whole-frame A/B harness — `-Mode RenderParityFrame` EXACT 0.0 — RENDER-11 graph-driven
execution, colored shadows, froxel volumetrics, WBOIT glass, the celestial seam, flag-OFF
auto-exposure), **Wave G** (24 items: water evidence, all seven sim→render bridges, the
default-OFF sim couplings, instinct groundwork, the six-item audio cluster, and the R1.X
flip that made `render.live_weather`/`aether_tap`/`snow_cover` **DEFAULT-ON** under a 0/48
sweep), and **Wave H's core** (WATER-08 → **Bump B**, WATER-11 live waterfalls, the full
I2/I3 instinct band → **Event P**). CANONICAL BASELINES (Bump B, 2026-07-05): DEBUG static
`a66ab4d049ba9228`, moving `a91098d71d742567`; RELEASE static `045f7c2f0645bcce`, moving
`85c0842944f86563`; populated golden `d281053b8de4b891` (ecology v2). What remains: the
**A1 aether arc** (6 items), then **Waves I–K modernization M0–M6** (~45 items: GPU
Vulkan/DX12/DLSS/HW-RT, NET scale-out, UI, FOLIAGE, OPS), plus the open owner menus below.
The approved campaign plan lives at
`C:\Users\David\.claude\plans\ultracode-land-all-of-joyful-matsumoto.md` — Waves I–K
execute per its M0–M6 section verbatim.

## 1. Source-of-truth pointers (read in order)

1. `docs/audit/021/backlog.json` — 190 items, statuses + notes; validate with
   `python docs/audit/021/validate_backlog.py` (must print `OK: 190 items valid`).
2. `docs/audit/021/priority-ranking.md` — the ranking + Wave A–G execution records
   (append Wave H's record next, matching the skeleton; Event P's bundle results belong in it).
3. The campaign plan (path above) — Waves I–K's M0–M6 detail, the shared-file serialization
   rules, and the per-commit sequences.
4. Auto-memory (`MEMORY.md`) — baselines + standing gotchas, updated through Bump B.

## 2. The hash-bump ledger (the campaign economy) — STATUS

- **Bump A (WATER-17)** — DONE (`d1ec2cb0`). Settle idempotency + flux persistence + water
  sub-hash regroup.
- **Bump B (WATER-08)** — DONE (`bfa9cea3`). mm-domain reroutes; float mirrors → Render.
- **Event P (instinct band)** — code DONE (`98f3c19d`→`3e54328a`); the closing evidence
  bundle (heavy oracle, LREC1, lockstep, ReplicationSmoke, 26-cell matrix, release rebuild)
  was RUNNING at handoff time as background task `b1evubn8c` — fold its results into the
  Wave H record. Every leg passed individually during the band; the bundle is confirmatory.
- **Remaining deliberate bumps are ALL owner-menu activations** (§4) — no further
  engineering bumps are planned.

## 3. Remaining work (54 items), in execution order

### 3.1 Wave H remainder — the A1 aether completion arc (6 items, next up)

Per the plan's A1 section; all default-OFF, zero re-pins; seed offset +35 chartered
(grep-verify before claiming); the engine/game split-lint bans "aetheric" under src/.

| Rank | Item | One-liner |
|---|---|---|
| 109 | AETHER-05 | Author spec 024 (research-first: stateful energy model, emitters/absorbers, polarity, persistence-vs-rederivable decision) |
| 110 | AETHER-06 | The keystone: stateful field layer on ScalarFieldDiffusion, SysKey `sim.aether_state` OFF, additive `aether_state:v1:` empty-neutral sub-hash |
| 111 | AETHER-07 | Engine FieldEmitterComponent + Lua `sample_energy_field` binding (+ manifest gate); game-side alias in scripts/ |
| 111 | AETHER-11 | `u_aetherMaterialModulation` default 0.0 (pixel-identical) + monotonic RenderSmokeTest |
| 111 | AETHER-12 | StimulusChannel::Aether=5 (append-only) + the photo-scorer aether axis |
| 112 | AETHER-08 | Channel B (Lumin/Umbra polarity), same flag; RG32F tap when active |

### 3.2 Waves I–K — modernization M0–M6 (execute per the plan verbatim)

- **M0 hygiene (4):** OPS-05 (root build/ retirement + -Strict, FIRST — protects every
  later gate), OPS-06, NET-10, SHIELD-10.
- **M1 scale seam + salvo (5):** GPU-P09/GPU-07 (the render-scale seam — scale=1.0
  byte-identical is the hard gate), NET-06, UI-06, AUDIO-11, **FOLIAGE-05 (re-tasks
  ForestPerfBudget GREEN — do EARLY; it retires the lane's only chartered RED)**.
- **M2 HLSL port + salvo (8):** GPU-P06/GPU-08 (~53 → ~43 shaders after Wave G's dead-file
  deletions; slangc toolchain per memory; batches F1–F6, intricate shaders INLINE),
  FOLIAGE-09 (before the compute batch), NET-07, NET-08, UI-09, UI-10, AUDIO-12, FOLIAGE-07
  (before M3's plant-procgen port).
- **M3 native Vulkan + salvo (6):** GPU-P07 (pass-by-pass, increasing coupling, the Wave-F
  A/B harness gates every port), NET-11, NET-09, UI-11, AUDIO-14 (mp3 only; assets need
  ELEVENLABS_API_KEY — code lands regardless).
- **M4 upscaler + DLSS (5):** GPU-P10/GPU-11 (IUpscaler contract; FSR2/XeSS stubs only),
  GPU-P11/GPU-13 (Streamline, runtime-loaded DLLs only, RTX-gated gate), NET-13 (POSIX
  transport via the Docker proxy), NET-12 (**hardware-blocked** — GNS loss matrix is the
  documented proxy), OPS-07. **VISUAL PAUSE #3** (DLSS presets + FOLIAGE-10 morphology).
- **M5 DX12 + nightly (4):** GPU-P08 (**timeboxed**; fallback = parse-only + residual),
  OPS-08, OPS-09 (nightly full-gate — after FOLIAGE-05 so the suite is all-green),
  FOLIAGE-10 (WorldVisualSweep re-bless + owner screenshots).
- **M6 hardware RT + tail (9):** GPU-P12 (BLAS/TLAS, render-residency only, amortized
  budgets — the EnsureSurfaceReadyNear lesson), GPU-P13/GPU-10 (RT-GI/AO, NFR-4 carve-out,
  the ShieldRt trio must stay green — augments-never-replaces), GPU-P14 (RT water
  reflections), SHIELD-08 (XL — start design in M4/M5; smoke + avail-trace + matrix after
  EVERY increment), OPS-16 (unblocked — INSTINCT-12 landed in Wave G), AUDIO-13, UI-08
  (LAST in UI; bless once at 3840×1600), OPS-15 (dead last). **VISUAL PAUSE #4** (RT-GI
  cave, RT water reflections) + the campaign final report.
- **Parallel sim-side tails (file-disjoint, ride Waves I–K):** INSTINCT-09 (T.2 — the
  two-stack unification; equivalence commit byte-identical → switch commit golden refresh;
  NOTE: also the chartered answer to the measured ecology O(N²) hot spot, §5),
  ATMO-13 (T.3 — region-follow grids; activation = a deferred moving-baseline bump),
  WATER-15 (T.4 — research spike; recommend render-only channel detail first),
  INSTINCT-14 (**blocked** on the spec-019 substrate; reserve seed +36, record blocked).

## 4. OPEN OWNER DECISIONS (two menus — nothing blocks engineering until chosen)

**PAUSE #1 menu (visual, delivered 2026-07-05; owner said "keep it all going" → current
defaults stand until explicitly ratified):**
1. **AC-A-001 night floor** — `u_moonWrapFloor` 0.25 (current, brighter/navigable) vs 0.0
   (moodier). ONE build stages both: `LUMIN_MOON_WRAP_FLOOR=0.0` for the moody capture.
2. **GPU auto-exposure** — `--auto-exposure-metered` (rank 69, landed flag-OFF): adopt as
   default or keep the analytic curve.
3. **Froxel volumetrics default tier** — `--volumetric-quality 1` opt-in today; making
   tier 1 default = a WorldVisualSweep re-bless; the temporal tier (quality 2) + the
   RenderBudget mode-1 measurement are chartered follow-ups of this decision.

**Activation menu (each = ONE deliberate hash bump w/ full evidence bundle + re-pin):**
- `sim.hydrology_weather` ON (weather-driven rain; implies finite-hydrology semantics).
- `sim.weather_events` ON (Markov epochs via GameSession::CurrentWeatherEvent).
- A THIRSTING sim roster (ThirstComponent + water holes on the server ecology roster) —
  the INSTINCT-05 populated transition (landed hash-neutral because no sim roster thirsts).
- ATMO-13 region-follow ON (once built; a moving-baseline bump).
- Full spec-010 finite hydrology ON.

## 5. Deferred / follow-up register (recorded, not dropped)

| # | Item | Where recorded |
|---|---|---|
| D1 | `-Mode InteractiveBootLeash` frame-time gate (RENDER-20's leash half; determinism half fully gated) | Wave F record |
| D2 | Colored-shadow BEAUTY pool staging (needs bare DEFERRED-lit terrain — grass cards are forward-lit) + centered glass-stack strip + colored-shaft pane-adjacent framing | Wave F record + memory |
| D3 | Froxel temporal tier (quality 2) + RenderBudget mode-1 measurement | F7 commit + PAUSE #1 |
| D4 | Night-bed + waterfall-roar audio assets are PLACEHOLDERS (reused mp3s; real assets = AUDIO-14 ElevenLabs, needs $env key) | bank JSON `_placeholder_note`s |
| D5 | Audio duck tuning (floor/attack/release) not exposed through SystemConfig | AUDIO-10 agent report |
| D6 | Forage's `food_availability` sensing unwired (the arbiter seam exists; Forage never wins until fed) | INSTINCT-05 commit |
| D7 | **Ecology N=4000 = 25.7 ms/tick (77% of the 33 ms budget)** — the measured O(N²) opposite-role scan; chartered response = INSTINCT-09 + a spatial partition; NEVER a time-based cap (rule recorded at both budget sites) | Wave G record + blessed budgets |
| D8 | Cross-BUILD water-hash equality is IMPOSSIBLE (float terrain seeds beds; same reason world_hash is per-build) — the gate is same-build cross-process by design | WaterCrossBuild gate comment + memory |
| D9 | Water resolution is uniform/fixed (camera-independent) but a TRUE host==peer sim-grid decoupling from visual LOD remains a spec-009 NFR-DET architectural note | ResizeSimulationGrid comment |
| D10 | Ambient/wildlife thirst behavior (client) now arbiter-correct; server rosters don't thirst yet (see activation menu) | INSTINCT-05 commit |
| D11 | The species JSON `brain`/`genome_ranges` blocks ship empty on the 10 shipped species (defaults == compiled); authoring real per-species values is content work | INSTINCT-12 notes |
| D12 | `systems.game.json` (the intended-ON profile) not yet updated for the Wave-G keys | systems.json comment |

## 6. Blocked / residual register (verbatim from the plan, still true)

R1 NET-12 SDR second-machine: hardware-blocked → GNS loss-matrix proxy + residual doc.
R2 NET-13: transport-seam proof via Docker only. R3 DX12 mingw timebox → parse-only
fallback. R4 FSR2/XeSS stubs only. R5 DLSS exposure: Streamline auto-exposure until the
PAUSE-#1 metered-exposure decision. R6 OPS-16: RESOLVED (INSTINCT-12 landed). R7 AUDIO-14
assets need ELEVENLABS_API_KEY. R8 AudioBankIntegrity: done. R9 Streamline ship-signing =
follow-up. R10 the whole-frame harness exists (Wave F); per-pass A/B extension, if needed,
opens M2.

## 7. Standing law (unchanged) + NEW standing lessons from this session

**Unchanged:** ucrt64 PATH prepend on every build/ctest/validator; ctest SERIAL only
(ForestPerfBudget = the only chartered RED until FOLIAGE-05); full-tree rebuild after
header changes; shaders INLINE; commit per green gate, NEVER push; `git commit -F <file>`;
visual changes require a looked-at capture; backlog validate after every flip; Codex
advisor = gpt-5.5 high, read-only, -o final message.

**New this session (also in auto-memory):**
1. **Water grids live on the 2.5D column's y=0 chunk** regardless of terrain sign — any
   chunk lookup from a position with terrain<0 (river beds!) resolves to y=-1 and MISSES.
   Probe water chunks at y≈0.5; the injection path has a y=0 fallback.
2. **River sink cells swallow same-tick injections** (Phase-0 boundary clamp) —
   hash-invisible; stage injection tests with dug pits, never on the river band.
3. Gridded water chunks in the default preset sit ~128–176 m from spawn; nearer
   `WaterLevelAt` wetness is ungridded worldgen lakes.
4. **Never run two ninja builds concurrently** (the agent-build + orchestrator-build
   collision corrupts the regen step); when fanning out agents, forbid them from building —
   verify centrally.
5. Agents must NOT touch `main_client.cpp`/`GameSession` in a fan-out wave — the
   orchestrator owns those seams and applies agents' wiring instructions verbatim.
6. The additive opt-in component pattern (zero-default senses, participant-gated ticks)
   lets sim features land hash-neutral and activate by roster/data — use it for every
   remaining sim item.
7. PowerShell `python -c "<multiline>"` breaks — write scratchpad .py files.
8. RenderHealth after ANY client rebuild: rerun `ctest -R RenderSmoke`, delete the
   `RenderHealth*` provenance sidecars, then run the mode (FR-A-005).

## 8. Immediate next actions (the first hour of the next session)

1. Check background task `b1evubn8c` (Event P bundle) results; append the **Wave H record**
   to priority-ranking.md (W2/Bump B + WATER-11 + I2/I3 + the bundle evidence) and update
   memory if anything moved.
2. Start **A1**: research brief → author `docs/specs/024-aether-field-completion/spec.md`
   (AETHER-05), then AETHER-06 the keystone (grep-verify seed offsets 28/35/36/38 free
   before claiming +35).
3. After A1: open **Waves I–K** with M0 (OPS-05 first — it protects every subsequent gate
   run), then M1 with FOLIAGE-05 early.
