# HANDOFF — post-Wave-E, the ranked band beyond, and the composability program

**Date:** 2026-07-04
**Branch:** `feat/polyglot-audit-roadmap` (NEVER push; commit per green gate only)
**Author of this handoff:** the Wave-E orchestrator (Opus 4.8)
**For:** the next autonomous agent continuing the spec-021 engine backlog

---

## 0. TL;DR — where we are, in one paragraph

Spec-021 is a ranked 190-item engine backlog (`docs/audit/021/` is the CANONICAL engine
state). Waves **A–D are CLOSED**; **Wave E (016 render-framework band, ranks 71–73) just
closed** this session. The engine is deterministic-locked: DEBUG static `--smoke ==
6f008a9f637c40b7`, moving `--smoke-moving == 0431682a3f8a8a24`; RELEASE static
`ea9a0121d13bc3bd`, moving `d79fdbbdbfe6580f`. Every change this session was **render-only /
hash-neutral** and both smokes held byte-identical. The next work is **Wave F** (the 015
Pillar B/C render band + RENDER-11 execution migration) and then the ranked band fans out
across pillars (atmo/aether/instinct/water/audio/ui). Two owner direction questions came up
mid-session and are specced in §7 below as **the composability program** (celestial-body
generalization + live shader authoring) — the owner asked these be captured descriptively,
NOT built yet. Standing law and toolchain gotchas are in §8. Read §8 before touching code.

---

## 1. Source-of-truth pointers (read these first, in order)

1. **`docs/audit/021/backlog.json`** — the canonical 190-item backlog. Every item has an
   `id`, `rank`, `status` (`todo`/`in-progress`/`done`), `proving_signal`, and (for landed
   items) a `note`. Validate with `python docs/audit/021/validate_backlog.py` (must print
   `OK: 190 items valid`). This is the machine-checked source of the ranking.
2. **`docs/audit/021/priority-ranking.md`** — the human ranking + the **Wave A/B/C/D/E
   execution records** (prose, at the bottom). The Wave-E record (2026-07-04) is the most
   recent; match its format when you append Wave F.
3. **`docs/audit/021/pillar-*.md`** — per-pillar deep briefs (render/shield/gpu/atmospheric/
   aetheric/instinct/water/audio/ui/networking/foliage/buildtestops). The ranked table for
   each pillar's items with evidence pointers.
4. **`docs/audit/021/gpu-modernization-plan.md`** — the RHI → Vulkan/DX12 → DLSS/FSR/XeSS →
   HW-RT track (specs 014/016, Diligent pilot). The RHI pilot go/no-go was **GO** (Wave C).
5. **Your auto-memory `MEMORY.md`** — one-line pointers to ~70 durable facts. Load-bearing
   ones for this work: determinism baselines, `ctest-lane-serial-only`,
   `toolchain-path-contamination`, `bash-tool-sandboxed-use-powershell`,
   `build-tree-gotcha`, `dispatch-shaders-inline`, `ultracode-parallel-pass-divergence`,
   `render-flip-gate-run-to-run-noise`, `headless-ingame-render-capture-stall`.

---

## 2. Verified state at handoff

- **Latest commits (this session, Wave E):**
  - `9762ddac`, `1c505952`, `9b751ef4`, `5ce975dd` — RENDER-14 (update_time_of_day decomposed).
  - `8c3deb03` — RENDER-11 i1 (RenderGraph.h + unit tests).
  - `453c8e97` — RENDER-11 i2 (render_frame trace emit + drift guard).
  - (this handoff + the Wave-E backlog flips + record commit follows.)
- **Determinism law (must hold after every change unless a re-bless is deliberately logged):**
  - DEBUG: static `--smoke == 6f008a9f637c40b7`, moving `--smoke-moving == 0431682a3f8a8a24`.
  - RELEASE: static `ea9a0121d13bc3bd`, moving `d79fdbbdbfe6580f`.
  - GPU-derived values must NEVER feed `world_hash` (spec 018 FR-E-003). All render work is
    hash-neutral by construction (the headless server renders nothing).
- **The full gate bundle (run the ucrt64 PATH prepend on EVERY build/ctest/validator):**
  ```powershell
  $env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
  cmake --build --preset debug                                   # the tree the gates read
  build\debug\bin\luminumbra_server_app.exe --smoke             # 6f008a9f637c40b7 run==replay
  build\debug\bin\luminumbra_server_app.exe --smoke-moving      # 0431682a3f8a8a24 run==replay
  ctest --test-dir build/debug --output-on-failure             # SERIAL only; green exc ForestPerfBudget
  python docs\audit\021\validate_backlog.py
  # render gates (need the 5070 Ti):
  powershell -File .forge\scripts\validate-engine-frontier.ps1 -Mode RenderHealth -BuildPreset debug
  powershell -File .forge\scripts\validate-engine-frontier.ps1 -Mode RenderBudget -BuildPreset release
  ```

---

## 3. Wave E close (what just landed) — and the constraint that governs Wave F

**Wave E = 016 render-framework band (ranks 71–73), render-only / hash-neutral.**

- **RENDER-14 (rank 72) — DONE.** `update_time_of_day` decomposed 270 → 201 lines; ~69 lines
  of pure render-derived math moved VERBATIM into header-only pure functions in
  `rendering/TimeOfDayModel.h` + `rendering/ExposureModel.h`, each pinned bit-exact against
  the canonical library primitives by 6 gtests. The byte-fragile trig asymmetry is preserved
  (sun uses unqualified `::sin`; moon uses `std::sin` float overload) — DO NOT "normalize" it.
- **RENDER-11 (rank 71) — DECLARATION HALF DONE; status `in-progress`.**
  `rendering/RenderGraph.h` declares render_frame's 23-stage dispatch as DATA (nodes with
  named resource reads/writes + the god-rays "latest opaque snapshot" as an explicit
  latest-writer edge); a topo-scheduler + validator prove consistency; render_frame emits its
  real stage trace (`record_frame_stage`); `validate_render_health` asserts `schedule() ==
  trace` (drift guard). Byte-identical (no GL call moved).
- **RENDER-16 (rank 73) — DONE.** `-Mode RenderBudget` (release, 5070 Ti): half-res GTAO holds
  post-Pillar-A (ssao 0.407 ≤ 0.70 GREEN). The `total` 4.043 > 3.33 is the pre-existing
  aspirational 300-fps budget (never green; by-design-RED like ForestPerfBudget), not a
  regression.

**⚠️ THE GOVERNING CONSTRAINT (read this — it dictates the whole render track):**
There is **no byte-exact whole-frame gate** on this engine. The headless server `--smoke`
does NOT render (RenderPipeline is client-only). Whole-frame FLIP floors at ~0.057 run-to-run
noise (memory: `render-flip-gate-run-to-run-noise`). So a blind refactor of the GL-state-dense
`render_frame` hot path is **unverifiable** on a Frostbite fidelity floor. This is exactly why
RENDER-11 shipped as declaration-only. **The unlock is an in-process A/B harness** (render the
frame two ways in ONE process, same frame state, `flip_diff == 0` — deterministic; GPU-09's
dual-render harness + the `--render-parity-finalblit`/`--render-parity-ssao` modes already
prototype the same-process zero-variance FLIP). Build that harness FIRST before migrating any
render execution. This applies to RENDER-11 execution migration AND to RENDER-15/17/18.

---

## 4. Remaining spec-021 ranked band (Wave F and beyond)

The ranking fans out across pillars after the render band. Grouped by natural wave:

### Wave F — the 015 render band (ranks 74–75) + RENDER-11 execution migration
- **RENDER-15 (rank 74, todo)** — 015 C-1 colored shadow maps (tinted-transmission
  attachment) built through the 016 pass/resource contract. The "light is the subject" quick
  win leading Pillar C. **Determinism-sensitive shader work → write INLINE** (memory:
  `dispatch-shaders-inline`, `ultracode-parallel-pass-divergence`). Gate: WorldVisualSweep +
  the in-process A/B (see §3) — do NOT lean on whole-frame FLIP alone.
- **RENDER-17 (rank 75, todo)** — 015 Pillar B froxel volumetric participating media (extends
  the analytic aerial; C-1 tint compose for colored shafts). XL/high. Gated behind 016 + the
  014 RHI pilot.
- **RENDER-18 (rank 75, todo)** — 015 C-2 full OIT colored glass + screen-space refraction.
  Lands last in the render band; OIT variant decided by measurement (015 OQ).
- **RENDER-11 execution migration (the in-progress remainder)** — make the scheduler DRIVE the
  passes, replacing the hand-scripted sequence. GATE-BLOCKED on the in-process A/B harness
  (§3). Migrate the clean pass-object stages first (shadow/gbuffer/ssao/lighting/skybox/water/
  foliage/particles/final_blit); the GL-state-dense inline stages (waterfall/cloud-composite/
  decals/far-field) come last. The declaration (`RenderGraph.h`) is the target order.

### Beyond Wave F — the ranked band (76+), by pillar cluster
- **Atmosphere live-play bridges (76, 78, 86, 98, 99):** ATMO-07 (per-frame WeatherSystem →
  set_weather_state), ATMO-08/09 (lightning StrikeSchedule + season tick from sim), ATMO-11
  (drive spec-010 hydrology rain from WeatherSystem), ATMO-10 (time-of-day a pure function of
  sim tick — see the composability §7A note), ATMO-12 (wire the built WeatherEventSystem).
- **Aether client bridges (79, 80):** AETHER-04 (the never-landed A1d 2/2 sim→render aether
  bridge), AETHER-10 (drive u_aetherGlow* from RenderContext + tuning).
- **Emergent ecology / Instinct (81–85, 96, 97):** INSTINCT-04 (feeding loop:
  Grazeable/FoodSource), INSTINCT-07 (deep-ecology components on the living set), INSTINCT-06
  (needs consequences), INSTINCT-08 (vertebrate scent tracking), INSTINCT-05 (IAUS
  Drink/Forage), INSTINCT-10/11 (ecology sub-hash v2 + real world seed), INSTINCT-15 (ecology
  perf budgets). This is the owner's **emergent-ecology direction** (memory:
  `emergent-ecology-ai-direction`) — substrate landed; these wire live participants.
- **Water (77, 100–105):** WATER-17 (heavy-oracle roundtrip — see §5), WATER-12 (dam half of
  009 AC-5), WATER-13 (flow-momentum save/load contract), WATER-09/10 (sub-hash localization +
  host==peer cross-build gate), WATER-08 (sever float→sim feedback edges), WATER-11
  (waterfalls respond to live water/terraform), WATER-07 (rain/evap from WeatherSystem).
- **Audio (87–90, 106, 107):** AUDIO-05 (SFX bus volume), AUDIO-07 (night soundscape),
  AUDIO-06 (waterfall roar — the never-called ComputeWaterfall path), AUDIO-08 (thunder cue
  distance-delayed), AUDIO-09/10 (environmental reverb + category buses/ducking). Memory:
  `audio-everything-maps-to-sound`.
- **UI (91–95):** UI-06 (world-selection → real saves), UI-09 (settings completeness:
  resolution/sfx/music rows), UI-08 (fidelity baseline to all screens), UI-10 (Codex browse v2
  photo thumbnails), UI-11 (first-session tutorial/onboarding). Memory:
  `polish-to-full-game-roadmap` (Phase 2+ is the tutorial + full loop).
- **GPU (78):** GPU-14 (RenderDoc capture wired via GPU-06 — real StartFrameCapture bracket).

Pull the live list any time with:
```bash
python -c "import json;d=json.load(open('docs/audit/021/backlog.json'));
xs=sorted([i for i in d['items'] if i['status']!='done' and isinstance(i.get('rank'),int)],key=lambda x:x['rank']);
[print(x['rank'],x['id'],x['status'],x['summary'][:90]) for x in xs[:50]]"
```

---

## 5. Deferred / open items register (explicit — nothing silently dropped)

| Item | State | What / why deferred | Unblock |
|------|-------|---------------------|---------|
| **AC-A-001 night-floor** | **OWNER-PENDING** | Wave D amend surfaced two findings: night lighting is inherently authored (no in-scatter model at night), and the wrap-floor VALUE is a taste lever. **brighter-floored [current build, luma 84] vs moodier-floorless [luma 46]** — both navigable by the AC's ≳8 bar. | **Owner ratifies** which look ships. Blocks the WorldVisualSweep re-bless. |
| **Rank 69 GPU auto-exposure** (ATMO-05 + RENDER-07) | todo, split from Wave D | A refinement, not an AC bar (FR-A-004 permits the analytic curve already shipped); building it forces a 2nd visual re-bless. | Standalone; do when the async-readback exposure metering is prioritized. |
| **RENDER-11 execution migration** | in-progress | Declaration done; scheduler-driving-passes needs a byte-exact frame gate that doesn't exist. | Build the in-process A/B harness (§3), then migrate clean pass-object stages. |
| **RENDER-15 colored shadows** | todo (rank 74) | Wave F lead item. Shader work → INLINE. | Wave F. |
| **WATER-17** (rank 77) | todo, filed Wave B | Heavy-oracle water roundtrip failure, proven PRE-EXISTING (boot water settle exits at its 400-cap with 2577/5433 chunks never water-inited + all inited chunks awake → settle reproducible but NOT idempotent → save/load water can't roundtrip). Terrain/entities legs green. | Root-fix the boot water settle idempotency (make the calm check see all chunks, or reach sleep). |
| **RENDER-20** (rank 76) | todo | Amortize the one-time ~30s (debug) main-thread ambient-wildlife/procgen-tree bring-up stall. | Move bring-up off the main thread (cf. RENDER-19 world-entry scan pattern, commit 1901c5a7). |
| **ForestPerfBudget** | chartered RED | The one expected ctest failure; run the lane SERIALLY (memory: `ctest-lane-serial-only`). | The 300fps forest campaign (spec-004 territory). |
| **RenderBudget `total` ≤ 3.33** | aspirational RED | The 300fps whole-frame budget; never green. Present-bound (present ~2.27ms). Real lever = CPU-submit + present (spec-004), not GPU passes. | spec-004 Phase 1+. |

---

## 6. What came up this session (owner interjections — captured, NOT built)

Mid-Wave-E the owner raised two forward-looking directions and then chose: *"Finish Wave E
then create a handoff … Be descriptive for the next agent."* So they are specced below (§7),
not implemented. Both are **engine-composability** themes — they generalize what the engine
already does into data-driven, experiment-friendly primitives.

1. *"shouldn't we generalize concepts like sun/moon/etc even further within the engine?"*
2. *"same with shaders … a way (like minecraft) to tweak things, drop things in, experiment,
   ideally even a panel for dev changing shaders on the fly and seeing its effects (look at
   blender)"*

---

## 7. THE COMPOSABILITY PROGRAM (the spec the owner asked to capture)

Design intent: the engine should expose its rendering/lighting concepts as **composable,
data-driven primitives you can drop in and experiment with live** — without a rebuild, and
without ever endangering determinism. Two tracks. Both are grounded in substrate that ALREADY
EXISTS — the work is closing the gap, not rebuilding.

### 7A. Celestial-body generalization (sun / moon / … → engine primitives)

**Today (verified):** sun and moon are hard-coded concepts inside `update_time_of_day` +
`RenderPipeline` members (`m_sun`, `m_moonLightDir`, `m_moonUpFactor`, `m_moonIllumination`,
season declination, lunar phase). Wave-E RENDER-14 already extracted the *pure math* for both
into `rendering/TimeOfDayModel.h` (`ComputeSunGeometry`, `ComputeMoonGeometry`,
`ComputeMoonIllumination`, `LunarIllumination`) + `ExposureModel.h`. **That extraction is the
groundwork for this** — the sun/moon are now pure functions of (tick, orbital params).

**Proposed abstraction — a `CelestialBody` / `SkyLight` primitive:**
```
struct CelestialBody {
    orbit:            fn(tod, seasonTick, params) -> direction   // sun/moon already have this
    radiance_model:   enum { TransmittanceCoupled(sun), AuthoredNightFill(moon), ... }
    phase:            optional (moon lunar phase; sun = none)
    shadow_role:      enum { PrimaryCascade, Secondary, None }
    exposure_hook:    contributes to the auto-exposure elevation curve
};
```
Sun and moon become CONFIGURED INSTANCES of this primitive; the pipeline iterates a list of
active bodies instead of two hard-coded members. New bodies (a second moon, a bright planet,
an aurora-driving source) become DATA.

**Tiering (do NOT skip the byte-identical tier):**
- **Tier 1 (generic-ready seam, byte-identical):** refactor the two existing bodies to flow
  through a `CelestialBody`-shaped seam WITHOUT changing the lighting/shadow passes (still one
  primary directional light + the moon fill). Gate: both smokes byte-identical + RenderHealth
  + a TimeOfDayModel-style bit-exact gtest (same technique RENDER-14 used). This is safe and
  landable under the current gates.
- **Tier 2 (full N-body):** a genuinely new multi-light lighting + shadow pass (>1 shadow-
  casting celestial light). This is a NEW render feature — needs the in-process A/B harness
  (§3) and a WorldVisualSweep, and it interacts with RENDER-11/12 (resource ownership) and the
  RHI pilot. Rank it in the render band, not before RENDER-15.
- **Related ranked item:** ATMO-10 (rank 98, "time-of-day a pure function of the sim tick")
  is the determinism-clean prerequisite for making celestial state fully sim-derived. Do
  ATMO-10 first if celestial state should ever be authoritative (today TOD is scenario/wall).

**Determinism constraint:** celestial *render* state (direction, color, exposure) is
render-only and may use libm freely (it never feeds world_hash). But if any celestial quantity
becomes SIM-authoritative (e.g., season tick driving ecology), it must route through the
integer-deterministic sim path (DeterministicMath, tick-keyed), NOT the render floats.

### 7B. Live shader authoring (Minecraft drop-in + Blender-style dev panel)

**Today (verified substrate — build the gap, do not rebuild):**
- **`Shader::Reload()` EXISTS** (spec 016 FR-D-003, `rendering/Shader.h:43`): recompiles to a
  SEPARATE program, reflects, re-validates the declared layout, and ROLLS BACK on mismatch.
  This is the safe hot-reload primitive — it already exists per pass.
- **ImGui dev overlay is wired** (`main_client.cpp` + `imgui.ini`; F3 GPU profiler overlay
  landed at `90406685`). The panel host exists.
- **Slang reflection** emits per-pass uniform JSON (`PassShaderLayouts`) — memory
  `shader-toolchain-slang`. So the set of tweakable uniforms per shader is already machine-
  readable.
- **SystemConfig** is the data-driven, gated/hashed flag+tuning registry (memory
  `system-config-substrate`) — the determinism-safe path for anything sim-affecting.
- Shaders are runtime-loaded from `res/shaders/` (no rebuild for a `.frag` edit).

**What's MISSING (the gap to build) — a crawl/walk/run:**
- **Crawl:** a hotkey that calls `Shader::Reload()` across all passes (re-reads `res/shaders/`
  from disk). Instant "edit .frag in your editor → press key → see it" loop. Lowest risk;
  reuses the existing rollback-safe reload. ~a day.
- **Walk:** a file-watcher on `res/shaders/` that auto-triggers the reload, PLUS an ImGui panel
  that lists the reflected uniforms (from the Slang reflection JSON) as live sliders/color-
  pickers, writing straight to the pass uniforms. This is the "Blender-style dev panel." The
  values are render-only tweaks (determinism-SAFE — they feed only GPU uniforms, never
  world_hash).
- **Run:** data-driven DROP-IN — material/shader/pass definitions loaded from data files (a
  `res/materials/*.json` or similar), so a new visual effect is added without C++. The
  RenderGraph declaration (RENDER-11, `RenderGraph.h`) is the natural home for a data-driven
  pass list once execution migration lands — a dropped-in pass becomes a new node.

**The determinism firewall (LOAD-BEARING — state it in the spec):**
- Render-side hot-reload + uniform tweaks are **determinism-safe** because RenderPipeline is
  client-only and never feeds world_hash (018 FR-E-003). A designer can tweak all day; the sim
  is untouched.
- Any drop-in that affects SIM (a new material with sim properties, a field that feeds
  gameplay) MUST route through SystemConfig's gated/hashed path (additive sub-hash, default-
  OFF, re-pin discipline) — memory `system-config-substrate`. The two worlds must not blur:
  render tweaks are free; sim drop-ins are hashed and gated.

**Proving signals for this track:** the crawl/walk tiers are render-only → gate on RenderHealth
+ both smokes byte-identical (proving no sim leak) + a "reload rolls back on a broken shader"
test (the Shader::Reload contract). The run tier (sim drop-ins) gates on the SystemConfig
sub-hash discipline.

**Where to rank it:** this is net-new tooling, not a spec-021 backlog item yet. Recommend
authoring a small spec (`docs/specs/0XX-live-shader-authoring/`) per the standing
`research-before-spec` rule, then ranking the crawl tier as a low-risk quick win and the
walk/run tiers behind the RENDER-11 execution migration (so the data-driven pass list has the
RenderGraph to plug into).

---

## 8. How to work here (standing constraints — READ BEFORE CODING)

- **NEVER push.** Commit per green gate (`git commit -F <file>` — no `"` inside PS
  here-strings). The autonomous mandate is commit-only; the owner reviews at wave boundaries.
- **Determinism-sensitive / intricate GPU / shader code is written INLINE** (memory
  `dispatch-shaders-inline`, `ultracode-parallel-pass-divergence`). Do NOT fan out parallel
  agents onto interdependent code sharing a seam/ABI — they diverge. Fan out only file-disjoint
  / committed-interface work. `git status` after every generation workflow.
- **The determinism law** (§2) holds after every change. A deliberate re-bless is ≤1 logged
  bump with the full evidence bundle (heavy oracle + LREC1 + LockstepLoopback + matrix). Local-
  dev hash bumps are fine if run==replay holds and baselines are re-pinned (memory
  `local-dev-world-hash-bumps-ok`), but render work should be hash-neutral by construction.
- **ucrt64 PATH prepend on EVERY build/ctest/validator:**
  `$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"` (memory `toolchain-path-contamination` —
  KiCad/mingw64 poison PATH → silent compile fails).
- **ctest lane is SERIAL-ONLY** (memory `ctest-lane-serial-only`) — `-j` gives false failures
  (WaterDeterminism SEGFAULT class). Only expected RED = `ForestPerfBudget`.
- **Build the tree you test** (memory `build-tree-gotcha`) — gates read `build/debug`; a
  `--target X` build does NOT recompile dependencies of X in other TUs (RenderPipeline.cpp
  needs a full `cmake --build --preset debug`). Adding a member to a widely-included header
  can cause the stale-obj SIGSEGV class (memory `stale-obj-concurrent-header`) — a clean full
  build fixes it.
- **Bash tool is sandboxed** — it can't run the GPU client or compiler; use PowerShell for
  builds/client/gates (memory `bash-tool-sandboxed-use-powershell`).
- **Visual changes REQUIRE a looked-at capture** (memory `density-convention-blackout-
  incident`, `show-visual-progress`). The headless IN_GAME `--frame-scan`/`--scene-config`
  capture HANGS on frame 1–2 after world-load (memory `headless-ingame-render-capture-stall`
  = RENDER-01) — `--ui-screenshot` works. This blocks automated visual re-bless; plan around
  it or fix RENDER-01 first.
- **Advisor before substantive work + before declaring done.** For architectural sign-off when
  stuck, Codex gpt-5.5 `model_reasoning_effort="high"` (NOT xhigh — it wedges), read-only
  sandbox, deliverable as final message via `-o` (memory `codex-critique-signoff-helper`,
  `codex-high-not-xhigh`).
- **Fidelity floor = BF4/BF1 Frostbite** (memory `visual-fidelity-target`); **engine stays
  generic** (emissive materials / scalar fields — LuminCrystal/Aetheric lore is game content;
  memory `engine-game-decoupling`); **most powerful/composable/scalable, productionized over
  stopgap** (memory `engine-power-scalability-principle`).
- **Commit trailer:**
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_0191C6QwPSR8Nq8DFjojLjDs
  ```

---

## 9. Key seam / file map (the render band you're most likely to touch)

- `src/luminumbra_client/rendering/RenderPipeline.cpp` — the 5,000-line pipeline;
  `render_frame()` (~2070–2675) is the 23-stage dispatch; `update_time_of_day()` (~5024–5225)
  is now the assembler over the extracted facets; `validate_render_health()` (~1500–1600) hosts
  the RENDER-11 drift guard + the 8 required_passes check + `refresh_render_pass_metadata()`.
  Two honest scoping notes on the RENDER-11 drift guard so nobody reopens it: (a) the drift
  check is **skipped when the trace is empty** (pre-first-frame / post-shutdown) — it is a
  "checked when a frame ran" guard, never a false positive; a skip is not a pass. (b) because
  the trace emit is UNCONDITIONAL, `schedule() == trace` guards **drift between the declaration
  and render_frame's stage STRUCTURE** (they live in different files, so one careless edit can't
  desync them) — it does NOT prove the declaration matches the actual per-frame EXECUTED passes;
  that gap closes only with execution migration. `schedule()` itself is effectively tautological
  (forward-only edges can't reorder or cycle); the real teeth are `validate()` + the god-rays
  branch test.
- `src/luminumbra_client/rendering/RenderGraph.h` — the RENDER-11 declarative frame graph
  (nodes/edges/scheduler/validator + `BuildLuminumbraFrameGraph()`). The target order for
  execution migration.
- `src/luminumbra_client/rendering/TimeOfDayModel.h` / `ExposureModel.h` / `SunLightModel.h` —
  the pure, bit-exact-gtested render-math seams (celestial groundwork §7A).
- `src/luminumbra_client/rendering/Shader.h` — `Shader::Reload()` (hot-reload primitive §7B).
- `src/luminumbra_client/rendering/AsyncReadbackRing.{h,cpp}` — the readback path (FR-G-001
  allowlist now empty); any GPU→sim result must route through it.
- `res/shaders/` — runtime-loaded shaders (no rebuild for a `.frag` edit).
- `test/rendering/render_capture_test.cpp` — the render gtests (RenderGraph + TimeOfDayModel +
  ExposureModel + SunLightModel + the capture/SDK tests). Model tests need no GL context.
- `.forge/scripts/validate-engine-frontier.ps1` — the mode gate runner (`-Mode RenderHealth`,
  `-Mode RenderBudget`, `-Mode MovingResidency`, etc.).

---

## 10. Immediate next actions for the next agent

1. **Confirm the handoff state:** run the full gate bundle (§2). Both smokes byte-identical,
   ctest green exc ForestPerfBudget, backlog valid. Read the Wave-E record in
   `priority-ranking.md`.
2. **Surface the AC-A-001 owner decision** (§5) if the owner is available — it unblocks the
   render visual re-bless and is the one true owner-gated item.
3. **Start Wave F with RENDER-15 (colored shadows, rank 74)** — shader work INLINE; but FIRST
   consider building the **in-process A/B harness** (§3), because RENDER-15/17/18 AND the
   RENDER-11 execution migration all need it. That harness is the highest-leverage next piece
   of render infrastructure.
4. **If the owner wants to pursue the composability program (§7):** author the small specs
   (`research-before-spec`), land the celestial Tier-1 seam (byte-identical, safe) and the
   shader-authoring "crawl" tier (reuses `Shader::Reload()`) as low-risk quick wins.
5. **Pause at the Wave F boundary** for owner review, per the wave discipline.

**The prime directive stands:** drive the ranked backlog spec→plan→execute→verify, TDD, commit
per green gate, hold determinism, pause at wave boundaries. Everything above is navigation —
the backlog + the gates are the source of truth.
