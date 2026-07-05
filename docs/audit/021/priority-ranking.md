# Spec 021 — Single Reconciled Priority Ranking (2026-07-02)

> One ranked order across ALL 183 backlog items (FR-D-001), including shipped items marked
> `done` (FR-D-003), reconciled against the 2026-06-28 merged execution roadmap's dependency
> spine (FR-D-002). Source of truth for item detail: `docs/audit/021/backlog.json`
> (validated by `validate_backlog.py`; every item carries a TDD proving-signal).
> Evidence for every claim lives in the per-pillar critique docs (`pillar-*.md`).

## Ranking criteria (FR-D-004)

Applied in this order, ties broken downward:

1. **Blocker-unblock value** — how many other ranked items (including the entire visual
   re-bless surface) the item unblocks. This is why the headless IN_GAME capture hang is the
   #1 actionable item.
2. **Dependency-spine position** — no item is ranked above an unmet hard dependency from the
   merged roadmap without an explicit justification note (AC-003). Notes are marked
   **[SPINE NOTE]**.
3. **Owner-stated GPU emphasis** — the 014 pilot-gate closure and the GPU deep track are
   pulled forward relative to a pure defect-first order (charter G-5: "get all the DLSS/RT
   support").
4. **Effort** — S/M quick wins float to the top of their tier.
5. **Risk** — HIGH hash-risk work (017-B, weather region-follow, water hash re-pins) is
   serialized, never fanned out, and placed where the landed gate suite guards it.

**Joint ranks:** several findings were independently filed by two pillar auditors (each sees
its own side). They are deliberately kept as separate backlog entries but ranked at ONE
position, written `A + B`. Treat them as one work item with two acceptance views. Multi-item
ranks written `A / B` are batches of *distinct* items sharing a rank (not duplicates); only
`A + B` pairs appear in the cross-pillar duplicate register.

## Spine reconciliation — verified state vs the 2026-06-28 roadmap (FR-D-002, FR-A-002)

The audit re-verified every edge of the merged roadmap's dependency spine against the tree.
**Most of the spine's prerequisites have already landed** — the roadmap snapshot is 4 days
stale and materially behind reality:

| Spine edge | Roadmap assumption | Verified 2026-07-02 |
| --- | --- | --- |
| 018-B strictly before 017-B | 018-B dormant | **LANDED** (727fbc8c, `ResidencyContract.h` + locked gtests → SHIELD-12 done). 017-B may legally start. |
| 017-A unblocks 015 A-T06 + 016 FR-E foliage | 017-A future | **LANDED** (ca2616d8/3bba2a52 → RENDER-02/GPU-01/FOLIAGE-02 done). Foliage readback rerouted; A-T06 still open (RENDER-07 + ATMO-05). |
| 017-B required by 016 FR-E *SDF* retirement | both future | 017-B **OPEN** (SHIELD-02→03). RENDER-06 stays gated behind it — honored below. |
| 018-E/F gates before readback consumers | future | **LANDED** (OPS-03 done: ReadbackDiscipline + DeterminismAudit + RenderReadbackAllowlist + MovingResidency). Consumers are now legal. |
| 016 seam + FR-D reflection + registry gate the 014 pilot | 016 partially done | Seam **LANDED** (RENDER-03: friend list empty), FR-D reflection **LANDED** (RENDER-04/GPU-02). **Registry ownership is the ONE unfinished gate leg** (RENDER-12 + GPU-12), plus pilot support legs GPU-04/GPU-05/GPU-09. |
| 019-C1 soak requires multi-connection accept | Wave-3 future | **LANDED EARLY** (NET-05 done, 45e6963f) via per-client-port multi-accept; the single-port full form is NET-11. |
| 020-A before 020-B | Wave-0 future | **BOTH LANDED in order** (OPS-01, OPS-02 done). |
| Determinism law | `--smoke == 6f008a9f637c40b7` | **RE-VERIFIED 2026-07-02** during this audit: run-1 == run-2 == replay, 90 ticks, 5433 chunks. |

**AC-008 compliance record (read-only audit):** the audit's own writes were confined to
`docs/audit/021/` (plus the charter itself, `docs/specs/021-engine-framework-audit-charter/`,
untracked before this session). The tracked files dirty in `git status` —
`res/shaders/waterfall.frag`, `data/common/foliage/scatter_set.json`,
`data/common/materials.json`, `.forge/config.yaml`, `imgui.ini`, and 12
`build/debug/test-artifacts/*` files — were all already dirty in the pre-audit baseline
snapshot and are themselves first-class findings of this audit: WATER-06 (the stranded
waterfall shader diff), FOLIAGE-08 (the scatter_set edit), and OPS-10 (test-artifact churn —
the structural reason `git status` is never clean). The `--smoke` half was re-verified
2026-07-02: `6f008a9f637c40b7`, run-1 == run-2 == replay.

Two live blockers the roadmap did not carry (both emerged after 2026-06-28) are the
FR-A-003 mandated findings and lead the actionable ranking (AC-007):

- **RENDER-01** — headless IN_GAME render-capture hang (frame-2 main-thread block; the
  charter's `main_client.cpp:4349` anchor has drifted — see `pillar-render.md` for the
  verified current anchor). Blocks the 015 re-bless, the 016 parity harness, and every
  `--frame-scan`/`--scene-config` visual proving-signal below.
- **SHIELD-01** — the world-load hang: `EnsureSurfaceReadyNear`'s three unbounded waits, now
  at `SHIELD_WorldSystem.cpp:2834/:2836/:2991` (the charter's `:2825/:2827/:2925` drifted).
  Stopgap landed (SHIELD-11 done); root fix is the 017-B chain (ranks 50–52).

---

## The ranking

### Ranks 1–33 — Landed foundation (all `done`; re-ranked, not dropped — FR-D-003)

Ordered by how much of the remaining spine each unlocked:

1. **OPS-03** — 018-C/E/F + FR-G-001 gate wave (MovingResidency, ReadbackDiscipline,
   DeterminismAudit, RenderReadbackAllowlist, per-build matrix baselines). The spine's
   "gates before consumers" prerequisite — everything below is guarded by this.
2. **SHIELD-12** — 018-B ResidencyContract landed + locked. Opens 017-B.
3. **SHIELD-13** — per-tick availability-set trace + static run==replay comparison
   (report-only by design) + MovingResidency gate. The baseline 017-B must reproduce.
4. **SHIELD-14** — 017-D wait instrumentation; measured the moving-anchor barrier at
   p99 239ms (~50% of wall) — the quantified motive for 017-B.
5. **SHIELD-15** — 017-B activation-queue code-grounded design + step-1 mechanism.
6. **SHIELD-11** — load-hang stopgap: malformed-SDF guard + LUMINUMBRA_JOB_WATCHDOG +
   phase breadcrumbs (hash-neutral).
7. **RENDER-02 + GPU-01 + FOLIAGE-02** — 017-A AsyncReadbackRing + FR-G-001 ban gate +
   foliage blade readback rerouted. The RHI-shaped readback seam.
8. **RENDER-03** — 016 pass contract COMPLETE (RenderPipeline friend list empty; every pass
   on RenderContext + typed handles). Two of the three 014 gate legs closed by this + next.
9. **RENDER-04 + GPU-02** — 016 FR-D shader reflection + layout validation + hot-reload
   rollback (LightingPass pilot).
10. **OPS-01** — 020-A operability core (tree preflight, provenance manifests,
    stale-capture refusal, manual-tier enumeration).
11. **OPS-02** — 020-B config codegen for the FULL registry, byte-identical `config:v1:`.
12. **ATMO-01 + ATMO-02 + RENDER-05** — 015 Pillar A core: LUT magnitude coupling,
    lunar-phase night modes, deterministic TOD exposure seam, dedicated moon radiance channel.
13. **WATER-04** — the water main-thread block (~450–1500ms) is DEAD (rotating sim window,
    inline init, 220ms gate ceiling). Corrects the stale "#1 moving-lag killer" memory.
14. **WATER-03** — water lockstep desync RESOLVED; canonical hash moved to `6f008a9f`.
15. **WATER-01** — spec 009 phases 1–3: fixed-point virtual-pipes solver + terraform coupling.
16. **WATER-02** — spec 010 finite hydrology landed (default-OFF, land-water-probe gated).
17. **WATER-05** — waterfalls-from-connections landed (lake/tarn rim outlets + surface
    connection). Corrects stale KNOWN-CONTEXT.
18. **NET-01** — replication core incl. the ack-driven re-delta loop (landed 2026-06-19;
    corrects stale memory).
19. **NET-05** — 019-C1 soak harness + in-process 32-client ReplicationScale (Wave-3 item,
    landed early; live run was N=4 — see NET-07).
20. **NET-03** — 019-D1 busy-spin kill via bounded no-drop OutboundByteQueue.
21. **NET-04** — 019-E1 per-client backpressure metrics + across-client p95s.
22. **NET-02** — 019 scale-path docs/routing; lockstep demoted to oracle/replay/small-co-op.
23. **UI-01** — Wave-0.3 create-world punch-list landed IN FULL (flex rows, tab panes,
    slider-drag fix, knob→preview proof, preview precipitation).
24. **UI-04** — settings screen built + fully wired (corrects "settings.rml not built").
25. **UI-02** — headless live-diorama capture (`--preview-live/--preview-weather`).
26. **UI-05** — creature-codex browse overlay v1 (corrects "codex UI = next").
27. **UI-03** — lake-preview null-water crash fix (landed 2026-06-25, pre-roadmap).
28. **FOLIAGE-04** — spec 006 farming loop phases 1–5 end-to-end (growth, germination,
    persistence, sim→render bridge, verbs + HUD).
29. **FOLIAGE-03** — octahedral tree impostors landed and flipped DEFAULT-ON
    (perf-validated; corrects "default-OFF pending validation").
30. **INSTINCT-01 / INSTINCT-02 / INSTINCT-03** *(batched — distinct done items)* — spec 005
    complete (boids, advected scent, double-bridge foraging, heritable sensory genes), spec
    011 ~two-thirds (energy, sleep, forager colony), species_id keystone + registry.
31. **AETHER-01 / AETHER-02 / AETHER-03** *(batched — distinct done items)* — deterministic
    aether field sim core, world_hash fold (bump #4, inside the current baseline), inert
    render emissive tap. The pillar is far more built than the charter assumed — only the
    client bridge is missing (rank 79).
32. **AUDIO-01 / AUDIO-02 / AUDIO-03** *(batched — distinct done items)* — one-shot UAF fix,
    post-audit coverage wave, and the three audio gate modes.
33. **OPS-14 / SHIELD-16** *(batched — distinct done items)* — crash-diagnostics substrate;
    preview render-radius decoupling.

### Ranks 34–49 — TIER A: live defects, red/latent-red gates, hygiene quick wins

34. **RENDER-01** — *(AC-007 blocker #1)* root-cause + fix the headless IN_GAME
    render-capture hang. Highest unblock value in the backlog: gates ranks 68–75 and every
    visual proving-signal. Signal: NEW HeadlessInGameCapture gate.
35. **FOLIAGE-01** — FoliageInstancing gate RED (0 blades, debug flat_lands; not a 017-A
    regression; first step = clean readback-enabled re-run). A red determinism-adjacent gate
    is a broken oracle — fix before it masks real regressions.
36. **FOLIAGE-11** — harden the FoliageInstancing analysis writer (readback-disabled runs
    must fail loudly, not write vacuous green artifacts). Guards 35's diagnosis.
37. **OPS-10** — stop tracked test-artifact churn (25 committed baselines overwritten per
    run; silent re-bless risk — confirmed defect; also the reason `git status` is never clean).
38. **WATER-06** *(in-progress)* — commit the stranded waterfall night-lighting shader fix
    (HEAD sets a uniform that only exists in an uncommitted `waterfall.frag` diff — a silent
    GL no-op until committed). Visual verify rides on rank 34.
39. **OPS-11** *(in-progress)* — extend LUMINUMBRA_JOB_WATCHDOG to all three
    EnsureSurfaceReadyNear waits (observability for the hang until ranks 51–53 land).
40. **FOLIAGE-08** — reconcile the uncommitted `scatter_set.json` working-tree edit
    (commit-with-re-bless or revert; visual state must not ride the tree).
41. **RENDER-10** — close the FR-G-001 ban-gate coverage hole (`.ipp` files escape the scan;
    the blocking readback in `SkyAtmosphereLut.ipp` is invisible to the gate).
42. **AETHER-09** — reconcile the EngineGameSplitLint 'aetheric' noun ban (the mode FAILS on
    the current tree — a latent red gate).
43. **SHIELD-04** — wrong-sized-SDF OOB guard on the streaming promotion path (the unguarded
    half of the load-hang hypothesis; cheap hardening).
44. **SHIELD-05** — quarantine wrong-sized SDF at the persistence boundary.
45. **AUDIO-04** — bank-integrity + ogg-guard ctest (mechanically enforce the
    "everything maps to sound" standing rule + the no-ogg gotcha).
46. **UI-12** — UI gate honesty tail (validator asserts 3/20 pinned tests; manifest
    hardcodes passed:true).
47. **UI-07** — `--ui-fixtures` + hermetic gallery e2e (an environmentally-RED pinned test).
48. **OPS-04** — wire the two-tree preflight into the frontier gate + extend provenance
    binding to RenderBudget/visual sweeps.
49. **OPS-12 / FOLIAGE-06** *(batched — distinct items)* — raise the ctest-manifest floor to
    the real roster; register the built-but-unregistered `octa_impostor_test`.

### Ranks 50–56 — TIER B: the 017-B concurrency keystone (HIGH hash risk; serialized)

The Wave-1 remainder. All prerequisites landed (ranks 2–6). Opus-inline, one increment at a
time, availability-trace-verified per increment — never fanned out.

50. **SHIELD-02** — 017-B step 1: decouple sim-truth publish from render meshing (the LOD0
    backfill puts meshing on the hash-critical path). Gate: byte-identical `--smoke` + static
    avail-trace MATCH.
51. **SHIELD-03** — the deterministic activation queue (fixed pipeline-latency-K, tick-keyed
    availability) replacing the per-tick barrier; targets the measured p99 239ms block.
52. **SHIELD-01** — *(AC-007 blocker #2)* world-load hang ROOT FIX: bound/replace the three
    EnsureSurfaceReadyNear waits with the activation model on the boot path. Signal: NEW
    Test-WorldLoadBounded (20× loads, zero watchdog wedges).
53. **SHIELD-07 + OPS-13** *(joint — same FR-D-002 axis filed from two pillars)* — the
    fast/slow-job determinism-matrix throttle axis; the adversarial-timing proof 017-B needs.
54. **RENDER-06** — 016 FR-E: retire the GPU-SDF synchronous readback onto the ring and
    empty the FR-G-001 allowlist. **[SPINE]** correctly gated behind 017-B (SDF = sim truth).
55. **SHIELD-06** — enforce 018-B in production (derive hash-exclusion scope from the
    declared contract; today no production TU includes `ResidencyContract.h`).
56. **SHIELD-09** — proper quiesce/epoch sync for the preview reinit-vs-far-LOD race
    (replaces the landed point-guard).

### Ranks 57–66 — TIER C: 014 pilot-gate closure → the RHI pilot (owner GPU emphasis)

**[SPINE]** The hard gate (Codex #8) = 016 seam ✅ + FR-D reflection ✅ + **registry** (rank
57) — plus the pilot support legs. The pilot itself sits at rank 66 and is not scheduled
above any leg (FR-E-002).

57. **RENDER-12 + GPU-12** *(joint)* — render-target ownership into the resource registry
    (lifetime/load-store/history semantics; Vulkan-shaped). THE unfinished 014 gate leg.
58. **GPU-04** — put the pilot passes on the seam: DebugViewPass, GroundDecalPass, and the
    inline TAAU/aerial/god-rays methods onto the RenderContext contract.
59. **GPU-05 + RENDER-13** *(joint)* — ExpectedLayout coverage 13/13 passes (reflection
    validation currency for the HLSL port).
60. **GPU-09** — the in-process dual-backend FLIP parity harness (offline flip_diff is
    cross-run noisy ~0.057 → unusable as the port gate; in-process same-frame is the rule).
61. **GPU-P01** — PilotReadiness frontier gate: machine-check all four gate legs; flips green
    only when the pilot may legally start.
62. **GPU-06** — link real capture SDKs (RenderDoc in-app API + Nsight path); today
    marker-only (charter FR-E-003).
63. **GPU-P02** — vendor Diligent (FetchContent) + `rhi/` device/swapchain bring-up +
    `LUMIN_RHI=gl|vulkan|dx12` flag, default gl. **[SPINE NOTE — deliberate, justified]**
    ranked before the gate closes: Codex #8's gate guards the *pilot pass ports*, not
    vendoring/bring-up, which is CMake+device-only, ports zero passes, and is proven
    hash-neutral by its own signal (`--smoke` unchanged). Keeps the GPU critical path warm
    while ranks 57–60 land.
64. **GPU-P03** — the Rhi* type set beneath the 016 handles + the RhiNoReexport mechanical
    gate (no Diligent header escapes `rhi/`).
65. **GPU-P04** — pilot-pair single-source HLSL (DXC→SPIR-V; SPIRV-Cross reflection diffed
    against the landed GL-introspected layouts).
66. **GPU-03 + GPU-P05** *(joint — P05 completes the auditor's GPU-03)* — **THE 014 PILOT
    go/no-go**: pilot pair on GL-via-Diligent AND native Vulkan through the 016 seam,
    in-process FLIP parity. Unblocks 015 B/C-2 downstream (ranks 74–75).

### Ranks 67–75 — TIER D: 015 finish + render-framework payoff (mostly gated on rank 34)

67. **RENDER-09 + ATMO-04** *(joint)* — A-T07 photo manual EV wired into the exposure seam
    (today metadata-only). Not blocked by the capture hang (in-process pair signal).
68. **RENDER-08 + ATMO-03** *(joint)* — moon-radiance calibration + true-midnight re-bless +
    stale visual-baseline refresh. Needs rank 34.
69. **RENDER-07 + ATMO-05** *(joint)* — A-T06 GPU auto-exposure through the ring. **[SPINE]**
    both prerequisites (017-A ring, 018-E/F gates) verified landed; only rank 34 blocks the
    re-bless.
70. **ATMO-06** — close or formally amend 015 FR-A-001 (the remaining authored constants vs
    full LUT-magnitude coupling).
71. **RENDER-11** — 016 FR-C declarative frame graph (ordering exceptions become explicit
    edges). Not a 014 gate leg; correctly after the pilot-gate tier. (RENDER-13's
    follow-through is already covered at rank 59.)
72. **RENDER-14** — 016 FR-F: decompose `update_time_of_day` + shrink RenderPipeline
    (5,311 lines vs AC-008's line-drop criterion).
73. **RENDER-16** — re-validate default-ON half-res GTAO under RenderBudget at native
    3840x1600 (the AO bless predates Pillar A).
74. **RENDER-15** — 015 C-1 colored shadow maps through the pass/resource contract.
    **[SPINE]** after registry (57), before Pillar B (75) — matches Codex #9.
75. **RENDER-17 then RENDER-18** — 015 Pillar B froxel volumetrics, then C-2 OIT/refraction.
    **[SPINE]** gated on 016 framework + the 014 pilot (rank 66) — the deferred payoff.

### Ranks 76–99 — TIER E: living-world payoff (high visible value per effort)

76. **ATMO-07** — the live-play weather bridge (sim weather is currently invisible in normal
    play — audio-only consumers). Machinery exists on both sides; needs rank 34 to prove.
77. **ATMO-08** — consume the sim lightning StrikeSchedule live (bolt + pulse + delayed
    thunder), replacing the random 22s timer.
78. **ATMO-09** — feed the sim tick to `set_season_tick` in live play (seasons currently
    frozen at tick 0) + couple the foliage autumn palette.
79. **AETHER-04** — wire the missing sim→render aether bridge (the never-landed A1d 2/2);
    makes the three landed done-items (rank 31) actually visible. S effort, pillar-scale payoff.
80. **AETHER-10** — RenderContext-driven glow color/intensity setters.
81. **INSTINCT-04** — activate the feeding loop (GrazeableComponent participants).
82. **INSTINCT-07** — stamp the deep-ecology components on the ambient spawn (alarm, pack,
    migration, territory currently have ZERO live participants).
83. **INSTINCT-06** — needs consequences: starvation/exhaustion degrade → death → decay.
    *(Batch 81–83 with one PopulatedWorldReplay re-pin.)*
84. **INSTINCT-08** — live vertebrate scent tracking (predators hunt upwind in play).
85. **INSTINCT-05** — complete the IAUS arbiter (Drink/Forage into DecideCreatureAction).
86. **WATER-07 + ATMO-11** *(joint — solver side + weather side)* — drive spec-010 hydrology
    rain/evap from `WeatherSystem::PrecipitationAt` (default-OFF, deterministic quantization).
87. **AUDIO-05** — the SFX bus (persisted slider is currently dead).
88. **AUDIO-07** — night soundscape (birdsong gated by sun elevation + night bed; pairs the
    landed moon channel with audible night).
89. **AUDIO-06** — waterfall roar (sites render silently; ComputeWaterfallRoar has zero call
    sites).
90. **AUDIO-08** — distance-delayed thunder cues off storm cells (replaces the 22s timer;
    pairs with 77).
91. **UI-06** — world-selection wired to real saves + thumbnails (SetLoadWorldCallback is
    never called outside tests).
92. **UI-09** — settings completeness (expose hidden resolution/sfx/music rows, live-apply).
93. **UI-08** — extend the UI fidelity baseline to all screens at native 3840x1600.
94. **UI-10** — codex v2: per-species best-capture thumbnails + detail pane.
95. **UI-11** — sequenced first-session tutorial (find → photograph → codex → objective).
96. **INSTINCT-10 then INSTINCT-11** — ecology sub-hash v2 (make AI state visible to the
    oracle) + the real world seed into mating (currently seed=0). One batched re-pin.
97. **INSTINCT-15** — bless the EcologyTickPerf budgets (measured but unenforced).
98. **ATMO-10** — unify time authority on the sim tick (retire the wall-clock 60s day).
99. **ATMO-12** — wire the consumerless WeatherEventSystem into the GameSession tick.

### Ranks 100–112 — TIER E2: water/audio/aether follow-through

100. **WATER-12** — the untested dam half of spec 009 AC-5 (cheap oracle strengthening).
101. **WATER-13** — prove + document the save/load flow-momentum settle contract.
102. **WATER-09** — close the water sub-hash localization blind spot (integer state is
     currently attributed to no section).
103. **WATER-10** — water-scoped debug-vs-release cross-build hash gate (spec 009 AC-4).
104. **WATER-08** — sever the two float→sim feedback edges, then exclude the float mirrors
     from world_hash (ONE deliberate re-blessed bump; the last unlanded water-perf step).
105. **WATER-11** — waterfalls respond to live water + terraform (needs 104).
106. **AUDIO-09** — EnvironmentalAudioSystem into the IN_GAME loop with real DSP (reverb is
     a log stub; fix the `wind_loop.ogg` landmine).
107. **AUDIO-10** — mix buses + ducking.
108. **AUDIO-11** — physics-raycast audio occlusion (SetPhysicsSystem is never called).
109. **AETHER-05** — author the Aetheric completion spec (research-first, per standing rule).
110. **AETHER-06** — deterministic emitter/sink API + stateful field layer (default-OFF;
     deliberate bump when ON).
111. **AETHER-07 then AETHER-11, AETHER-12** — ECS emitter component + Lua sampling seam;
     aether-modulated emissive materials; first sim/photo-scorer consumers.
112. **AETHER-08** — Lumin/Umbra dual-polarity channel (after 109/110).

### Ranks 113–121 — TIER F: the GPU deep track (post-pilot; the DLSS/RT payoff)

Order per `gpu-modernization-plan.md`; every step FLIP-gated in-process per pass.

113. **GPU-P09 + GPU-07** *(joint)* — the internal-render-scale seam (registry-owned scaled
     G-Buffer chain, mip bias, TAAU actually upsampling; scale=1.0 byte-identical). THE DLSS
     precondition.
114. **GPU-P06 + GPU-08** *(joint)* — Group-F single-source HLSL port (~53 shaders,
     risk-ordered batches, ported-HLSL-on-GL FLIP-validated before any backend port).
115. **FOLIAGE-09** — drop the `GL_ARB_gpu_shader_int64` hard requirement from the scatter
     compute (bit-exact 32-bit-pair splitmix64) so it ports under the RHI track.
116. **GPU-P07** — pass-by-pass native-Vulkan port in increasing-coupling order (TAAU last,
     as the DLSS fallback path) → the full-Vulkan milestone.
117. **GPU-P10 + GPU-11** *(joint)* — the IUpscaler provider contract; TAAU first provider;
     FSR2/3 + XeSS named behind the same seam (FR-E-004 — DLSS is never the sole path).
118. **GPU-P11 + GPU-13** *(joint)* — DLSS via NVIDIA Streamline at the extracted TAAU point
     (runtime RTX detection, TAAU fallback, dev/ship DLL split). Requires Vulkan/DX12 (116).
119. **GPU-P08** — DX12 backend bring-up behind the same seam (DXIL reuse of 114).
120. **GPU-P12** — BLAS/TLAS streaming infrastructure (budgeted refit-vs-rebuild; SDF
     far-field stays analytic — no far-field BLAS ever).
121. **GPU-P13 + GPU-10** *(joint)*, then **GPU-P14** — RT-GI/AO pass alongside the untouched
     software ShieldRtFarFieldPass (augments, never replaces — 014 OQ-3), validated on the
     caverns scene where it closes the cave-lighting half of the coarse-LOD gap; then RT
     water reflections.

### Ranks 122–137 — TIER G: scale, structural, and the long tail

122. **NET-06** — delta-vs-acked ON for the scale paths (zero non-test callers today).
123. **NET-07** — the over-the-wire soak at full N=32 + freeze the bandwidth baseline +
     NetSoak gate mode.
124. **NET-08** — GNS over-the-wire matrix (build GNS, loss/jitter/reorder gates over real UDP).
125. **NET-11** — true single-port multi-connection accept (the real dedicated-server shape;
     depends on the GNS build at 124).
126. **NET-09** — the FR-D-002/003 backpressure policy, decided from 123's soak data.
127. **NET-13** — POSIX TcpTransport (a Linux dedicated server currently cannot network).
128. **NET-12** — Steam SDR second-machine validation — explicitly deferred to hardware
     (TDD-LOCK scenario; GNS is the local proxy).
129. **SHIELD-08** — far-field fidelity: coarse LOD + far tiles reduce the SAME SDF (caves/
     edits currently vanish at distance). XL; the geometry half of the gap whose lighting
     half RT-GI (121) covers.
130. **OPS-05** — retire the root `build/` tree (strict preflight; fix the dormant ci.yml).
131. **OPS-09** — charter the scheduled local full-gate run (nightly Build → UnitTests →
     All → matrix-Quick → RenderBudget with a dated artifact).
132. **OPS-07 then OPS-08** — config owning-constant drift check; generated shared constant
     header + config-side hot-reload rollback.
133. **FOLIAGE-05** — re-task the forest budget to a GREEN posture (model the landed
     impostor path; invert the intentionally-RED gate).
134. **FOLIAGE-07** — spec 006 Phase 6: per-(genome,stage) mesh cache + instanced draws for
     10k-plant fields (retire the rebake-everything path).
135. **FOLIAGE-10** — richer tree morphology under the same deterministic visual-only contract.
136. **INSTINCT-09** — unify the two AI stacks onto one PerceptionSystem substrate (L/high;
     after the participant-activation batch proves the IAUS side live).
137. **INSTINCT-12, INSTINCT-14** — per-species behaviour data; server-authoritative nests
     (needs 019 online — after 122–126).

### Ranks 138–146 — TIER H: docs/consistency debt (cheap, autonomous-lane filler)

138. **WATER-15** — river-channel fidelity beyond the 8×8 grid (XL/high — parked here
     deliberately: cost/risk dominates until the GPU track settles; render-side channel
     detail may obsolete it).
139. **ATMO-13** — region-follow weather/wind grids (HIGH world_hash-semantics risk; needs
     its own spec-level determinism design before scheduling).
140. **ATMO-14** — snow-cover ground response (render-only; after 76/99).
141. **SHIELD-10** — refresh `docs/shield/sdf-contract.md` (two-tier producer contract) +
     stale line refs.
142. **INSTINCT-13** — fix stale determinism-critical comments in CreatureBrainSystem.h.
143. **WATER-14** — water docs debt (spec 009 status, missing spec-010 directory,
     stale resolution comment).
144. **NET-10** — AC-B-001 demotion language in headers + arch-doc line-anchor drift.
145. **AUDIO-12, AUDIO-13** — prune 7 unloaded ogg banks + 5 orphan header-only systems;
     refresh the audio architecture doc.
146. **AUDIO-14, OPS-06, OPS-15, ATMO-15** — photography sound set (pre-iter-7); 'build both
     trees' doc correction in 016/018/019; the harness operator doc (18 flags); delete the
     dead `skybox.frag` trap.

---

## Cross-pillar duplicate register (kept as separate items, ranked jointly)

| Joint rank | Items | One work item |
| --- | --- | --- |
| 7 | RENDER-02 / GPU-01 / FOLIAGE-02 | 017-A ring (done) |
| 9 | RENDER-04 / GPU-02 | FR-D reflection (done) |
| 12 | ATMO-01+02 / RENDER-05 | 015 Pillar A core (done) |
| 53 | SHIELD-07 / OPS-13 | FR-D-002 job-throttle matrix axis |
| 57 | RENDER-12 / GPU-12 | registry resource ownership |
| 59 | GPU-05 / RENDER-13 | ExpectedLayout 13/13 |
| 66 | GPU-03 / GPU-P05 | the 014 pilot |
| 67 | RENDER-09 / ATMO-04 | photo manual EV |
| 68 | RENDER-08 / ATMO-03 | moon calibration re-bless |
| 69 | RENDER-07 / ATMO-05 | A-T06 auto-exposure |
| 86 | WATER-07 / ATMO-11 | weather-driven hydrology |
| 113 | GPU-P09 / GPU-07 | internal-render-scale seam |
| 114 | GPU-P06 / GPU-08 | Group-F HLSL port |
| 117 | GPU-P10 / GPU-11 | IUpscaler + FSR/XeSS |
| 118 | GPU-P11 / GPU-13 | DLSS via Streamline |
| 121 | GPU-P13 / GPU-10 | RT-GI/AO |

## Wave A execution record (2026-07-02)

Landed this wave (commits d96d6cb0 → fee7d624): **RENDER-01** (the capture hang was the
spec-013 crystal-scan CPU stall — 6m25s main-thread in debug; headless automation now skips
the one-time locator flourishes; NEW HeadlessInGameCapture gate green on both legs),
**FOLIAGE-01** (root cause: the 2026-06-26 session shipped an INVERTED SDF density
convention — negative is SOLID, not air — which rejected every open-sky column and
defoliated the world's grass AND trees while its sibling bug blinded every capture;
convention fixed in four sites, world restored, FoliageInstancing GREEN with a re-blessed
saturation normalizer + a 100k decimation floor), **FOLIAGE-11** (refusal diagnostics — a
gate run can no longer end silent or vacuous-green), **WATER-06**, **FOLIAGE-08** (stranded
grass-overhaul tuning preserved under `stranded/` — it was fighting the defoliation's
symptoms), **AETHER-09** (EngineGameSplitLint green: comments reworded, code debt
enumerated), **RENDER-10** (FR-G-001 `.ipp` hole closed). `--smoke == 6f008a9f637c40b7`
re-verified after each code commit.

Two items discovered and filed during the wave (already in `backlog.json`):

- **RENDER-19** (rank 49, batched) — the interactive world-entry locator stall (6m25s
  debug / est. 10–25s release on frame 2); background-job fix with a teardown drain.
- **OPS-16** (rank 132, batched) — retire the EngineGameSplitLint allowlist by
  data-driving the enumerated hardcoded game content (species spawn is hash-visible).

The Wave-A tail landed in the same session (commits 378a7436 → 8d8a56d5): **OPS-10**
(all 25 tracked test-artifact run-outputs untracked — `git status build/` clean for the
first time; the 9 read-on-disk render/audio artifacts now fail closed), **OPS-11** (the
named-phase JobWatchdog helper wraps all three EnsureSurfaceReadyNear waits; unit-tested),
**SHIELD-04** (wrong-sized-SDF OOB closed on the streaming promotion path + a
PolygoniseTerrain entry guard; regeneration proven byte-identical), **SHIELD-05**
(persistence quarantine at save ADOPTION in GameSession — the persistence library stays
byte-faithful for its corruption-corpus contracts), **AUDIO-04** (bank-integrity + ogg-guard
+ event-literal-resolution + species-call-family ctest), **FOLIAGE-06** (octa_impostor_test
registered), **UI-07** (`--ui-fixtures` real: gallery fixture source + the root-mismatch fix;
the gallery e2e is hermetic and green), **UI-12** (gate honesty: all 20 pinned UI names
cross-checked against `ctest --show-only` reality; ghost-name negative test proven),
**OPS-12** (manifest roster + floors derived from LUMINUMBRA_GTEST_TARGETS, 22 targets),
**OPS-04** (validate-build-tree preflight wired into -Mode Build — first live run flagged
the real concurrent root `build/` tree — + provenance binding on RenderBudget and
WorldVisualSweep).

**Wave A close (2026-07-02): every ranked hygiene item (34–48) is done.** Carried open:
RENDER-19 (rank 49, wave-discovered — the interactive world-entry locator stall; M-effort
background-job refactor, first in queue at resume). Wave gate held: `--smoke ==
6f008a9f637c40b7` run==replay after every commit; FoliageInstancing + EngineGameSplitLint +
HeadlessInGameCapture + UiTestBaseline + Build green; `git status` clean.

**Wave-close discovery (the OPS-09 finding validating itself):** the first full default-lane
ctest run in weeks surfaced three PRE-EXISTING failures (proven identical with the
pre-Wave-A mesher): the MeshingDeterminism archipelago/cave mesh-hash pins (locked
2026-06-10) are stale — mesh bytes drifted since and `--smoke` cannot see it (mesh bytes are
hash-excluded) — and WorldAndWaterTest.DryHighAltitudeCellsStayDryAfterSimulation fails its
SEA_LEVEL settle contract. Filed as **SHIELD-17** and **WATER-16** (both rank 50, ahead of
the 017-B chain — a red determinism-adjacent oracle gets fixed before HIGH-hash-risk work
relies on the suite). Minor observation also carried: the foliage `windy_max_sway` measure
reports 8.46 m tip displacement (implausible for ≤0.44 m blades) — audit the measure's scale
when next touching FoliagePass.

## Pre-Wave-B close (2026-07-03)

The three items between Wave A and Wave B landed (commits b9276ad2 + 1901c5a7):
**SHIELD-17** (mesh-hash pins root-caused by two-point check to `d53c99c5` analytic MC
normals — deliberate render-only; re-pinned WITH the evidence chain, never blind),
**WATER-16** (the test pinned the pre-spec-009 dry==SEA_LEVEL convention; refreshed to
assert the authoritative contract: depth==0 + surface≤bed), **RENDER-19** (spec-013
world-entry scans backgrounded — the 6m25s frame-2 stall became 33 s of parallel
background jobs with byte-identical crystal placement; teardown drains at all three
world-transition sites). Full default ctest lane green except the intentionally-RED
manual ForestPerfBudget (= FOLIAGE-05). Filed during verification: **RENDER-20**
(rank 76) — the pre-existing ~30 s wildlife/procgen-tree bring-up frame, visible again
now that the defoliation fix restored real tree building.

Final gates: `--smoke == 6f008a9f637c40b7` run==replay; HeadlessInGameCapture PASS
(both legs). **Wave B (the 017-B chain, ranks 50–56) is next** — see
`docs/HANDOFF-2026-07-03-wave-b-fable.md` for the next orchestrator.

## Wave B execution record (2026-07-03 → 07-04)

Landed this wave (commits `35a7cf71 → 67fae941`, 22 commits, strictly serialized, every
increment individually gated): **SHIELD-02** (017-B step 1 — sim-truth publish decoupled
from render meshing via the two-stage promotion lane, sequenced INSIDE the barrier so
settlement stayed same-tick; ZERO hash movement, red→green decoupling gtest), **SHIELD-03**
(017-B step 2, landed as SIX gated increments: activation-latency shadow → scheduler
de-timing to publication-keyed reads → sim_available_lod0 centralization → non-publishing
save quiesce → main-thread generation publication + per-lane batch FIFOs + activate_due
proven pre-swap by the ActivationQueueSemantics gtest → **THE BARRIER SWAP**:
`activate_due(tick)` with fixed K=8 replaces the per-tick `wait_for_streaming_jobs`;
measured **moving main-thread wait p50 28 ms / p99 239–262 ms / ~4.2 s total → 0.001 ms /
0.001 ms / ≤0.1 ms over 90 ticks**; the moving hash became per-tick-trace run==replay
MATCH **and worker-count-invariant** — a determinism property the engine never had; the
wave's ONE sanctioned re-bless: moving debug `cf501b6676d67249 → 0431682a3f8a8a24`,
release `→ d79fdbbdbfe6580f`; static debug/release BYTE-IDENTICAL throughout, with the
debug per-tick trace identical to the pre-wave baseline artifact), **SHIELD-01** (the
world-load hang ROOT FIX — bounded 64-job sub-batch surface builds with named per-batch
watchdog progress; NEW WorldLoadBounded gate: 20 cold interactive loads at radius 12/4,
all < 60 s, ~17 s average debug, zero wedges), **SHIELD-07 + OPS-13** (the FR-D-002
fast/slow-job axis — LUMINUMBRA_JOB_THROTTLE seeded shuffled-pop at the JobSystem pop
site; the matrix SKIP is gone and throttled runs reproduce the unthrottled baselines),
**RENDER-06** (the FR-G-001 allowlist is EMPTY — GPU-SDF and sky-LUT blocking readbacks
retired onto the 017-A ring as bounded zero-timeout polls with CPU fallbacks; the fully
async GPU worldgen pipeline remains chartered in the GPU track), **SHIELD-06**
(FR-A-003 enforced, not remembered — the world_hash exclusion scope DERIVES from the
new kChunkFieldResidency table in core/ResidencyContract.h, now a production-consumed
contract, with a first-hash completeness check and the HashScopeDerivesFromPartition
projection pin; byte-neutral), **SHIELD-09** (the worldgen-epoch gate — reinit quiesces
off-main-thread samplers via a per-job shared_mutex scope, replacing the point guard;
WorldgenPreviewReinitRace soak with a guaranteed 500 ms contention window).

Discovered and filed during the wave: **WATER-17** (rank 77) — the heavy-oracle water
roundtrip is pre-existing-red, proven byte-identically at the pre-wave commit and at
ticks=0; root-caused with new permanent settle-exit telemetry: the boot water settle
cap-exits with 2577/5433 chunks never water-inited and ALL inited chunks awake, so the
settle is reproducible but not idempotent. Also fixed en route: FOUR stale/broken gate
pins — ReplayRoundtrip/LockstepLoopback/LockstepFaultInjection pinned the 2026-06-22-era
static canonical `ab0869af…` (re-synced to `6f008a9f…` per their own comments), and
**PopulatedWorldReplay was already red at the pre-wave commit** (two-point check: it
died on a run-vs-replay MESH sub-hash desync before its golden check could run, so the
old golden `114ff66c…` had been unreachable for an unknown period; under the queue the
populated scenario is run==replay through EVERY sub-hash including mesh — an
improvement — and the golden is re-pinned to the now-deterministic `9f0dd5b9b27ecb9d`,
ecology sub-hash run==replay, non-vacuous 8→10 entities). The ctest-lane serial-only
constraint is recorded (a -j4 run produces false failures incl. a WaterDeterminism
SEGFAULT from test-isolation contention).

**Wave B close (2026-07-04): every ranked 017-B-chain item (50–56) is done.** Wave gate
held: static `--smoke == 6f008a9f637c40b7` / release `ea9a0121d13bc3bd` byte-identical
after every commit with the static per-tick availability trace identical to the
pre-wave baseline; the moving oracle run==replay at every increment (byte-identical
until the swap, re-blessed once at it); full determinism matrix PASS — 26 cells
including the new FR-D-002 axes in both builds; HeadlessServerTick + ReplayRoundtrip +
LockstepLoopback + LockstepFaultInjection + MovingResidency + PopulatedWorldReplay +
WorldLoadBounded + RenderReadbackAllowlist green; full serial ctest lane green except
the intentional ForestPerfBudget. **Wave C (014 pilot-gate closure → RHI pilot, ranks
57–66) is next**, pending owner review of this wave.

## Wave C execution record (2026-07-04)

Landed this wave (commits `15ce5cd2 → 31fabfcc`, 25 commits, strictly serialized, every
increment individually gated; the wave is **render/RHI/infra work — hash-NEUTRAL end to
end**, Diligent linked into ctest targets ONLY so `--smoke == 6f008a9f637c40b7` held
byte-identical throughout): **RENDER-12 + GPU-12** (rank 57 — the render-target ownership
migration, the highest-blast-radius gate leg: the G-buffer family, shadow atlas, SSAO chain,
lighting-accum FBO, water-caustics target and TAAU history all moved off RenderPipeline onto
an owning `RenderResourceRegistry` allocate/own/lifetime path, seven commits A–G, each
`flip_diff` byte-identical same-pose), **GPU-04** (rank 58 — the pilot-pass seam gap closed:
DebugViewPass, GroundDecalPass, and the inline aerial/god-rays + TAAU resolve post-passes
migrated onto the RenderContext contract; DebugView given explicit coverage so the signal is
non-vacuous), **GPU-05 + RENDER-13** (rank 59 — shader-reflection `ExpectedLayout` coverage
1/13 → 14/14 across every pass + the NEW ReflectionCoverage gate), **GPU-09** (rank 60 — the
in-process dual-backend FLIP parity harness required by 014 FR-C.3, the twice-in-one-process
zero-variance metric the whole pilot is measured on), **GPU-06** (rank 62 — the RenderDoc
in-app capture API linked as a real trigger, MarkerOnly fallback still headless-green;
GPU-14 filed), **GPU-P02** (rank 63, the one sanctioned spine inversion — Diligent vendored
via FetchContent + `rhi/` device/swapchain bring-up + the `LUMIN_RHI=gl|vulkan|dx12` flag +
headless GL/Vk device creation on this box, ZERO passes ported), **GPU-P01** (rank 61 — the
PilotReadiness frontier gate over the four 014 hard-gate legs), **GPU-P03** (rank 64 — the
`Rhi*` backend type set beneath the 016 handles + the RhiNoReexport grep-gate), **GPU-P04**
(rank 65 — the pilot-pair single-source HLSL port with three-way SPIRV-Cross==GL-introspected
==ExpectedLayout reflection parity for ssao + debug_view), and **GPU-03 + GPU-P05** (rank 66,
strictly last — **THE PILOT GO/NO-GO = GO**: the `RhiPilotParityGpu` ctest renders the GPU-09
calibration cube on GL-via-Diligent and on native Vulkan in one process and FLIP-compares
each against the raw-GL golden with thresholds pre-registered from GPU-09's calibration.
**Leg B — GL-via-Diligent is BIT-identical to raw-GL** (FLIP 0.0 < 0.0027451, `4fc41c27`);
**Leg C — native Vulkan is BYTE-identical to raw-GL** (FLIP 0.0 < 0.0439216, zero
`VK_LAYER_KHRONOS_validation` errors, `31fabfcc`) — the first render-through-Vulkan in the
project. Non-vacuous: the flipped-orientation signature (0.0453 / max 0.53) is identical
across both legs, so raw-GL, GL-via-Diligent and native-Vulkan are all the same bytes on the
RTX 5070 Ti. OQ-6 recorded GO in `014/spec.md:366`; the 015 C-1/B/C-2 downstream unblock
declared in `015/spec.md:283`).

Owner-approved infra sweep, folded in before the pilot's shader work: **Slang** replaces the
dxc+spirv-cross two-tool pilot chain with a single `slangc` (`b73e8953`), and **Tracy**
landed behind `LUMINUMBRA_ENABLE_TRACY` (off by default, `50c049db`). A grounding research
brief — the 2026 engine-library landscape vs the Luminumbra stack — was written en route
(`04b845b3`).

Discovered and fixed during the wave: a bare `cmake --build --preset debug` (the `all`
target — the documented per-increment gate command) was newly broken by the Diligent
integration. Diligent's *shared* GL/Vk/Archiver DLLs fail to link under Ninja+mingw (a
relative `-Wl,--version-script=export.map` that does not resolve from the build root), and
the two Diligent test-framework libs (`GPU/TestFramework`) build even under
`DILIGENT_BUILD_TESTS=OFF` and drag the DLLs back via order-only deps. We link only the
`-static` engine libs, so all five are now `EXCLUDE_FROM_ALL` in `cmake/diligent.cmake` —
the gate command builds clean again. Also filed: **GPU-14** (during GPU-06).

**Wave C close (2026-07-04): every ranked pilot-gate item (57–66) is done, and the RHI
pilot go/no-go landed GO.** Wave gate held: after a full clean rebuild, BOTH determinism
smokes run==replay at their established Wave-B baselines — static `--smoke ==
6f008a9f637c40b7` and moving `--smoke-moving == 0431682a3f8a8a24` (hash-neutral by
construction — no server/sim code touched, Diligent is ctest-only); the rank-66 in-process FLIP verdict artifacts
(`rhi_pilot_flip.json` leg B GO, `rhi_pilot_flip_vk.json` leg C GO) recorded; the full
SERIAL ctest lane green — **1608 of 1609 tests pass, the sole failure the chartered
`ForestPerfBudget`** (two expected non-runs: the disabled throughput benchmark and the
`SecondBackendParityLandedInRhiPilotFlip` seam stub that now Skips, pointing at the landed
`RhiPilotParityGpu`); the four GPU tests — `RhiDeviceBringupGpu` 4/4 and `RhiPilotParityGpu`
legs B/C — all green. The strategic result: the GL → Diligent-RHI → native-Vulkan migration
path is proven pixel-lossless on this hardware, so 014 Phase A is de-risked and the 015
volumetrics/OIT pillars are unblocked on the seam. **Paused at the wave boundary for owner
review**; the next ranked band (67+, led by the now-unblocked 015 atmospheric pillars)
follows.

## Wave D execution record (2026-07-04)

Landed this wave (commits `f54e53a6`, `24e73efa`, `f95023da` — spec 015 **Pillar A**
atmospheric-lighting completion; the wave is **render-only / hash-NEUTRAL end to end** —
both determinism smokes held byte-identical throughout, static `--smoke ==
6f008a9f637c40b7` and moving `--smoke-moving == 0431682a3f8a8a24`, run==replay, since no
server/sim code was touched and the headless server smoke loads no shaders):
**ATMO-04 + RENDER-09** (rank 67, A-T07 — photo-mode manual EV now drives the RENDER
exposure: the player's lens aperture/shutter/ISO → EV overrides the A-T05 analytic
time-of-day exposure curve, so the photographer exposes for the light — stopping down
darkens, opening up brightens; a shared `rendering/ExposureModel.h` seam owns the mapping +
precedence so the GPU-pixel gate exercises the same code the frame uses),
**ATMO-06** (rank 70, FR-A-001 — the direct-sun MAGNITUDE is now the atmosphere's
transmittance toward the sun × a top-of-atmosphere solar constant, replacing the authored
`smoothstep` intensity ramp + horizon hue mix; the golden hour now reddens AND dims from one
physical model, AC-A-002. **Decision: BUILD, not amend** — ATMO-06 sanctioned either, but a
doc/grep pre-check found NO owner-signed FR-A-001→hue-only relaxation, so the coupling goal
was live/locked. A `rendering/SunLightModel.h` seam + a GPU-free contract gate pin the
property; the solar constant is calibrated 1/t_ref so noon + all high sun (sun_up ≥ 0.25)
stays byte-identical and only the low-sun arc changes — the answer to spec-015 OQ-2), and
**ATMO-03 + RENDER-08** (rank 68, FR-A-003 — de-garished the moon Purkinje tint: the night
desaturation target `vec3(0.55,0.75,1.35)` was a strongly-saturated blue mixed on top of the
already-cool `u_moonRadiance`, double-pulling tree bark/foliage to a garish electric
blue-purple; retargeted to a desaturated moon-grey `vec3(0.70,0.80,1.05)` so night foliage
reads as natural cool tones. Both night modes verified navigable — full-moon ground luma ~84,
new-moon 0.08 starlight-floor ~24. The moon_radiance seam was already fully wired).

Two decisions of record:
- **Rank 69 (ATMO-05 + RENDER-07, A-T06 GPU auto-exposure metering) is SPLIT OUT of Wave D
  and deferred.** It is a refinement, not an AC bar — no AC requires it (FR-A-004 permits the
  "fixed day/night stops" branch, which the landed A-T05 analytic exposure curve already
  provides), and building it now would force a second visual re-bless. It stays `todo` in the
  ranked band as a standalone item.
- **AC-A-001 amended — pending owner ratification.** The AC asked for a navigable midnight
  "with `kMoonKeyScale` / `nightAmbient` / wrap-floor removed." An evidence-first honesty
  check (a floor-zeroed capture) split that clause into two findings. *(1) Sound:* at night the
  sun is below the horizon so atmospheric in-scatter ≈ 0 — unlike the daytime sun (FR-A-001,
  transmittance-coupled) there is no physical model to replace the authored night fill, so night
  lighting is inherently authored and the moon key / ambient stay. *(2) A taste lever:* the
  wrap-floor VALUE is the knob — floorless is moodier (luma 46, 16% near-black) yet **still
  navigable by the AC's own ≳8 bar**, while the current floored value is brighter (luma 84, 6%)
  per the owner's "moon very dark" (brighter-nights) intent. Both pass. The current build keeps
  the floor; the genuine ratifiable choice for the owner is **brighter-floored [current] vs
  moodier-floorless**. Consistent wave principle: **build when the physics delivers a real
  signal (FR-A-001 sun), amend only when the capture shows the choice is authored taste (this
  floor).**

**Wave D close (2026-07-04): the three ranked Pillar-A items in the band (67, 68, 70) are
done; rank 69 split out.** Wave gate held: both determinism smokes run==replay at their
Wave-B baselines (static `6f008a9f637c40b7`, moving `0431682a3f8a8a24` — hash-neutral by
construction, all changes render-only); the FR-A-001 + exposure contract gates green (the GPU
pixel pairs non-vacuous); the full SERIAL ctest lane green — **1611 of 1612 tests pass, the
sole failure the chartered `ForestPerfBudget`**; and the visual intent verified on looked-at
captures — noon byte-identical (mean-luma 0.414, unchanged), the golden hour reddens AND dims
physically, night foliage de-garished, both moon modes navigable. **AC-A-005 (intentional
re-bless) disposition:** the noon lighting inputs are byte-identical, so no blessed visual
baseline moved — no re-bless was required; the TOD-specific golden/midnight changes were
verified on looked-at ad-hoc captures, not on pinned baselines. **Paused at the wave
boundary for owner review** (the AC-A-001 amend surfaced above for ratification); the next
ranked band (71+) follows.

## Wave E execution record (2026-07-04)

Wave E is the **016 render-framework** band (ranks 71–73) — structural, **render-only /
hash-NEUTRAL end to end**: both determinism smokes held byte-identical throughout, static
`--smoke == 6f008a9f637c40b7` and moving `--smoke-moving == 0431682a3f8a8a24`, run==replay,
because no server/sim code was touched and the headless server renders nothing.

Landed this wave:
- **RENDER-14** (rank 72, 016 FR-F, commits `9762ddac`, `1c505952`, `9b751ef4`, `5ce975dd`) —
  **DONE.** `update_time_of_day` decomposed 270 → 201 lines (AC-008 line-drop): ~69 lines of
  pure render-derived math (season / sun geometry / day-factors / season palette tint / moon /
  lunar phase / auto time-of-day exposure) extracted VERBATIM into header-only pure functions in
  `rendering/TimeOfDayModel.h` + `rendering/ExposureModel.h`. The pipeline keeps only the
  side-effecting assembly (clock advance, LUT refresh, LUT-coupled sun.color/ambient, member
  writes). Each facet is a mechanical byte-identical cut, pinned bit-exact against the *canonical
  library primitives* (DM::Sin, glm::normalize/smoothstep, std::sin/cos) by 6 gtests — not a
  re-typed copy, so a wrong-primitive swap or reassociation diverges. The load-bearing byte-fragile
  trig asymmetry is preserved verbatim: sun direction uses UNQUALIFIED `sin`/`cos` (→ global
  `::sin`, no `using namespace std` in the TU), moon uses `std::sin`/`std::cos` (float overload).
- **RENDER-11** (rank 71, 016 FR-C, commits `8c3deb03` + `453c8e97`) — **DECLARATION HALF LANDED;
  status `in-progress`.** `rendering/RenderGraph.h` promotes render_frame's hand-scripted 23-stage
  dispatch sequence to DATA: each stage is a node declaring the named render resources it
  reads/writes, plus the one ordering fact resource-flow can't express — the god-rays "latest
  opaque snapshot" (snapshot #2 if the weather overlay ran, else #1) as an explicit latest-writer
  edge. A topo-scheduler derives the order; a validator proves internal consistency
  (no read-before-write; every latest-writer resolves). render_frame now emits its REAL stage
  trace (`record_frame_stage` at each of the 23 slots), and `validate_render_health` (the
  RenderHealth gate) asserts `schedule() == that trace` — so the declaration can never silently
  drift from the shipping order. Byte-identical by construction: not one GL call moved; the only
  render_frame change is ~23 pure-CPU string appends. Gated by `render_capture_test RenderGraph.*`
  (4 tests: canonical schedule/validate, god-rays both branches, validator teeth, WAW/RAW) +
  `-Mode RenderHealth` GREEN on a real 5070 Ti frame.
- **RENDER-16** (rank 73, commits captured in the RenderBudget artifact) — **DONE.**
  `-Mode RenderBudget` (release, 5070 Ti, forest_dense, native 3840×1600): the default-ON half-res
  GTAO holds post-Pillar-A — **ssao 0.407 ms ≤ 0.70 budget GREEN**, skybox 1.118 ≤ 1.50 GREEN. The
  aggregate `total` 4.043 ms > 3.33 is the PRE-EXISTING aspirational 300 fps whole-frame budget
  (never green — git `9dab1807`/`e57fdcf0` "budget not yet met" / "RenderBudget RED gate"), the
  same by-design-RED class as `ForestPerfBudget`, NOT a Pillar-A regression (wall 4.487 ms =
  223 fps on the worst-case dense pose, present-bound).

**The constraint that shaped RENDER-11 (recorded because it governs Wave F):** there is **no
byte-exact whole-frame gate** on this engine. The headless server `--smoke` does not render
(RenderPipeline is client-only), and whole-frame FLIP floors at ~0.057 run-to-run noise. So a
blind execution rewrite of the GL-state-dense render_frame hot path — on a Frostbite fidelity
floor — would be *unverifiable*. RENDER-14's verbatim cuts were safe because they were bit-exact
gateable (the primitive gtests); RENDER-11's GL-state node bodies have no equivalent gate. The
responsible v1 is therefore **declaration + validation only** (zero execution risk), with the
execution migration charted and gate-blocked.

Two items of record:
- **RENDER-11 execution migration → Wave F.** Making the scheduler DRIVE the passes (replacing
  the hand-scripted sequence) is deferred until an **in-process old-path-vs-graph-path whole-frame
  A/B** exists — render the frame via both paths in one process, same frame state, `flip_diff == 0`
  (deterministic; the `--render-parity-*` modes + GPU-09's dual-render harness already prototype
  the same-process zero-variance FLIP). Only then can the clean pass-object stages
  (shadow/gbuffer/ssao/lighting/skybox/water/foliage/particles/final_blit) migrate byte-safely;
  the GL-state-dense inline stages (waterfall/cloud-composite/decals/far-field) come last.
- **RENDER-16 WorldVisualSweep rerun DEFERRED to AC-A-001.** The visual re-bless is owner-gated
  (the night-floor ratification) and capture-hang-blocked (RENDER-01); the AO-budget
  re-validation that RENDER-16 exists for is done and green.

**Wave E close (2026-07-04): ranks 72 + 73 done; rank 71 declaration done (execution → Wave F).**
Wave gate held: both determinism smokes run==replay at their Wave-B baselines (hash-neutral by
construction); `render_capture_test` model + graph suite green (11/11: RenderGraph 4 +
TimeOfDayModel 3 + ExposureModel 3 + SunLightModel 1); `-Mode RenderHealth` green (drift guard
exercised on a real frame); the full SERIAL ctest lane green except the chartered
`ForestPerfBudget`. No visual re-bless required — Wave E is byte-identical framework work with no
pixel delta. **Paused at the wave boundary for owner review.** Next: Wave F (RENDER-15 colored
shadows + RENDER-11 execution migration + the 015 Pillar B/C render band) and the ranked band
beyond, per the handoff.

## Spine-inversion register (AC-003)

Exactly one deliberate inversion, justified inline at its rank:

- **Rank 63 (GPU-P02, Diligent vendoring/bring-up) ahead of full pilot-gate closure** — the
  Codex-#8 gate guards the pilot *pass ports*; vendoring + device bring-up ports zero passes,
  is hash-neutral by its own proving signal, and de-risks the pilot's longest lead item. The
  pilot itself (rank 66) remains strictly behind every gate leg (ranks 57–61).

All other orderings honor the spine as verified in the reconciliation table above.
