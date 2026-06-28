# Handoff — 2026-06-28: session state, remaining pillars, and a debugging playbook

Branch: `feat/polyglot-audit-roadmap`. Determinism law held all session:
`luminumbra_server_app --smoke == 6f008a9f637c40b7` (run==replay).

---

## Part 1 — What landed this session (≈30 commits)

### Spec 016 — render framework (THE keystone): **DONE to AC-001**
- `RenderPipeline.h` friend list is **EMPTY**. All 13 render passes decoupled behind the
  `RenderContext` seam + `RenderResourceRegistry` (adopt-by-name typed handles). Every
  conversion byte-identical (full-image frame-scan == baseline mean_luma 0.3608).
- Remaining 016 (NOT blocking AC-001): declarative frame graph (FR-C), shader reflection
  (FR-D), async-readback migration (FR-E, needs the 017 ring).

### Spec 019 — networking: A/B/D/E done
- D1 killed the `WSAEWOULDBLOCK` busy-spin (bounded no-drop backpressure, 5 tests).
- E1 per-client outbound queue-depth + snapshot-age metrics (3 tests).
- Remaining: **019-C1** 32-client multiprocess GNS soak harness.

### Spec 018 — determinism: A1 + D1 done
- A1 residency contract; D1 the determinism matrix (`.forge/scripts/validate-determinism-matrix.ps1`,
  proves determinism across worker counts {1,2} + multiprocess).
- Remaining: 018-B/C/E/F (boot-settle, moving-residency, readback discipline, audit gate).

### Spec 020 — build/config: A1 + B1 done
- A1 build-tree validator; B1 config codegen (`--check` green). Remaining: 020-B wiring.

### Spec 015 Pillar A — atmosphere-coupled lighting: **the visible payoff, mostly landed**
- A-T01 LUT magnitude getters + A-T05b RenderContext.exposure seam slot (byte-identical).
- A-T05 deterministic time-of-day **exposure curve** (eye adaptation): noon 1.12 (preserved),
  night ~1.75 (navigable), golden-hour dip.
- A-T03 **ambient ↔ sky-view LUT** coupling (hue + calibrated magnitude, kSkyAmbientRenderScale
  8.49 from a measured-noon probe; noon preserved at mean_luma 0.351).
- A-T04 **moonlight + lunar phase** ("two night modes"): a render-only lunar cycle drives
  moon key + crisp cast-shadow floor + night-skylight scale; full moon = navigable, new moon
  = dark. Controllable via scene-config `moon` field / `set_moon_illumination()` / `LUMIN_MOON`.
  Full-moon verified (.forge/_flip/show_moon_full.png).
- **Remaining Pillar A:** A-T04 *dedicated moon radiance channel* (Codex C5 — a shader refactor
  + NIGHT re-bless, distinct from the phase scalar already landed); A-T07 wire photo manual EV
  (currently METADATA-ONLY at main_client.cpp:7121 — shutter/ISO/aperture stored but never drive
  `u_exposure`; needs EV→exposure math + calibration); A-T06 GPU-metered auto-exposure (BLOCKED
  on the 017 async-readback ring); a true-midnight (TOD 0.5) re-bless to tune the cool moonlight.

### Crash diagnostics (NEW infrastructure — see Part 3)
- Persistent file log + symbolized crash stack trace + a worldgen-preview race fix.

### Specs not started: 017 (concurrency ring + activation queue — also the real world-load-hang
fix), 014 (RHI/Diligent pilot).

---

## Part 2 — The pillars / remaining work, prioritized

1. **Pillar A finish** (visible, unblocked): dedicated moon radiance channel + photo manual EV
   + midnight re-bless. Fresh, calibration-light. `spec-015-pillar-a-plan` memory has the v2 plan.
2. **Spec 017 concurrency ring + activation queue** (determinism-sensitive): replaces the
   unbounded `EnsureSurfaceReadyNear` waits (the **world-load hang** root cause — diagnostic
   breadcrumbs already landed) AND unblocks 015 A-T06 async exposure + 016 FR-E. High value.
3. **The worldgen-preview far-LOD race — proper sync fix** (see Part 3; a guard is in, the race
   is the real fix).
4. **016 framework remainder**: declarative graph (FR-C), shader reflection (FR-D).
5. **019-C1** 32-client soak; **018-B/C/E/F**; **020-B**.
6. **Spec 014** RHI pilot (large, later).

Routing (per the workflow memory): opus inline = architecture + ALL shaders + determinism-
sensitive (017 activation queue); codex = heavy mechanical C++ + spec→plan; sonnet = atomic
fan-out. Forge stays codex-only for planning/gates; harness drives code.

---

## Part 3 — A DEBUGGING PLAYBOOK (the better way to diagnose + fix)

This session burned ~4 round trips on one crash because the game recorded nothing. That is now
fixed. **Use this pipeline; do not guess from a description.**

### 3.1 The runtime diagnostics (committed: caf35aa7, 51eb660e, 7c397d83)
- **`logs/luminumbra.log`** — every run, persistent (survives double-click launch + hard crash),
  timestamped + leveled. `flush_on(warn)` + `flush_every(1s)`. The breadcrumbs (e.g.
  `EnsureSurfaceReadyNear` phases) are here.
- **`build/<tree>/crashes/crash-<ts>.txt`** — on any unhandled exception: exception code,
  faulting address, registers, and a per-frame `module+0xRVA` backtrace. Three hard-won fixes
  baked in: (a) when `rip==0` (null function-pointer call) seed the PC from the return address
  at `[rsp]` — else `StackWalk64` returns nothing; (b) a parallel fault hits EVERY job worker
  at once → the first handler thread writes, **siblings PARK** (a `Sleep` loop) instead of
  returning (returning terminates the process before the writer flushes → empty file); (c)
  flush the file per line.

### 3.2 Symbolizing a release crash (mingw)
DbgHelp can't read mingw DWARF line info, and `addr2line` on the optimized release binary gives
garbage. The reliable method is **`nm` address-range lookup**:
```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
nm -C --numeric-sort --defined-only build/release/bin/luminumbra_client_app.exe > nm.txt
# For each frame RVA: VMA = 0x140000000 (ImageBase) + RVA; find the symbol whose
# address is the largest <= VMA. (See the inline python used in this session's transcript.)
```
This turned `client_app.exe+0x48E11A` into `SHIELD_WorldSystem::RiverInfluenceFromNoise` in
seconds. `.forge/scripts/symbolize-crash.ps1` exists but prefer the nm-range method for release.

### 3.3 Catch races BEFORE they ship — the missing tool
This crash was a **data race** (worker-thread sampling vs main-thread reinit). A symptom-level
fix (guarding readers) is whack-a-mole — we hit two different generators. **Build a
ThreadSanitizer (or ASan) preset and run the preview/far-LOD path under it** — it names the
racing read+write with both stacks instantly. There is already a `debug-asan` preset; add a
`debug-tsan` (gcc `-fsanitize=thread`) and a headless "orbit the worldgen preview" driver
(there is none today — single `--ui-screenshot` does NOT pan, which is why the static render
never reproduced it). A scripted orbit + TSAN would have found this in one run.

### 3.4 The fix discipline
- A reader-side null-guard stops one crash but not the race. Fix at the **source of the
  invariant**: here, `reinitialize_noise()` must never leave a generator transiently null
  (assign in the enabled branch, null only in the `else`) — and ultimately must not run
  concurrently with far-LOD sampling at all.
- Worldgen is hashed → any fix there MUST keep `--smoke == 6f008a9f637c40b7`. The never-null
  reorder and the reader guard are both no-ops for real worlds (generators always present when
  enabled), so they are determinism-safe; always re-run `--smoke` to confirm.

---

## Part 4 — The worldgen-preview crash (root cause + state)

**Symptom:** rotating the create-world preview (default preset) crashes release-only,
`0xC0000005 @ 0x0` (null function-pointer call) on every job worker.

**Captured stack (the pipeline above):**
```
JobSystem::worker_loop → run_slot → FarLodSystem::update → BuildPristineFarLodTile
  → MarchingCubes::TerrainSurfaceMaterialAt → SHIELD_WorldSystem::SampleWorldGenLayers
    → ComputeShapedHeightSampleImpl → {m_warp_generator | RiverInfluenceFromNoise}->GenSingle2D
```

**Root cause:** `SHIELD_WorldSystem::reinitialize_noise()` (SHIELD_WorldSystem.cpp:363) nulls the
shaping/river/biome generators then recreates them. As you pan, the preview re-seeds/rebuilds its
candidate world while `FarLodSystem` samples it on worker threads → a build job reads a generator
in its null window → null call. The real game world never hits this (its worldgen isn't sampled
during reinit — `EnsureSurfaceReadyNear` waits). **Not the render-seam / Pillar A work.**

**Fixes applied this session:**
- `fc360183` — reader guard in `ComputeShapedHeightSampleImpl` (capture the 4 shaping generators
  to locals + require non-null; unshaped fallback). Stopped the shaping crash; the river path
  then surfaced (whack-a-mole, as expected).
- (this commit) — `reinitialize_noise` never-null reorder for the shaping + river blocks (assign
  in the enabled branch, null in the `else`). `--smoke` re-verified byte-identical.

**STILL OPEN (the proper fix):** a concurrent read+assign of the same `FastNoise::SmartNode` is
still a data race (UB; no longer a *null* crash, but the refcount race remains). The correct fix:
**quiesce far-LOD sampling during the preview's world rebuild** (or guard the generator swap with
the same lock the sampler takes). Also apply the never-null pattern to the remaining generators in
`reinitialize_noise` (temperature/humidity/biome — temperature is already reader-guarded). Verify
with the TSAN + scripted-orbit harness from §3.3.

---

## Constraints carried forward (don't relearn these)
- Prepend `C:\msys64\ucrt64\bin` to PATH for every build/gate. Build the tree you test
  (`build/debug` for the engine-frontier gate; `--preset release` for the dist client).
- The night `--scene-config` client **hangs on exit** (holds the exe → next link
  "Permission denied"). Kill `Get-Process luminumbra_client_app | Stop-Process -Force` + the
  parent task before rebuilds; render via Start-Process + poll-for-PPM + kill.
- `LUMIN_MOON` env does NOT propagate through PowerShell `Start-Process`; use the scene-config
  `moon` field. `--frame-scan` pins TOD 0.04 (noon) — can't night-render via frame-scan.
- Render gate = in-process A/B parity / frame-scan frame-health (per-run FLIP is noisy). Pillar
  changes deliberately move the image → re-bless, not byte-parity.
