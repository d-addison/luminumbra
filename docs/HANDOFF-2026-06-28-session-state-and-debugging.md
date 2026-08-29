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
- A1 residency contract; D1 the determinism matrix (`tools/gates/validate-determinism-matrix.ps1`,
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
3. **Restore worldgen-preview loading** (see Part 4): the crash is fixed by disabling far-LOD for
   the preview, but that left the **far field absent** + there are **near-field holes**. Proper fix:
   `swap_pending_into_live()` drains in-flight far-LOD jobs before freeing the world, then re-enable
   far-LOD for the preview; separately audit the near-field preview chunk streaming for the gaps.
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
seconds. `tools/gates/symbolize-crash.ps1` exists but prefer the nm-range method for release.

### 3.3 Catch races BEFORE they ship — the missing tool
This crash was a **data race** (worker-thread sampling vs main-thread reinit). A symptom-level
fix (guarding readers) is whack-a-mole — we hit two different generators. **Build a
ThreadSanitizer (or ASan) preset and run the preview/far-LOD path under it** — it names the
racing read+write with both stacks instantly. There is already a `debug-asan` preset; add a
`debug-tsan` (gcc `-fsanitize=thread`) and a headless "orbit the worldgen preview" driver
(there is none today — single `--ui-screenshot` does NOT pan, which is why the static render
never reproduced it). A scripted orbit + TSAN would have found this in one run.

### 3.4 The fix discipline
- **CONFIRM the live call path before you fix.** This crash took ~5 partial fixes because each
  one fixed *near* the symbolized frame on an assumption (guard the generator; gate the offscreen
  render path) instead of verifying the actual path. The symbolized stack tells you WHERE; a 30-
  second `grep` for the callers (e.g. `grep "WorldgenPreview::render"` → it uses
  `render_to_backbuffer`, not `render()`) tells you WHICH path is live. Symbolize → confirm path →
  then fix. Don't fix the path you assumed.
- A reader-side null-guard stops one crash but not the underlying defect. Fix at the **source of
  the invariant / the object's lifetime**: a null call (`rip=0`) deep in code that references an
  object another thread can FREE = suspect a USE-AFTER-FREE of that object, not just a null member.
  Here the real fix was the object lifetime (don't dispatch far-LOD jobs against a world the
  preview frees), not the generators.
- Worldgen is hashed → any fix there MUST keep `--smoke == 6f008a9f637c40b7`. The never-null
  reorder + reader guard + the far-LOD gate are all no-ops for real worlds / render-only, so they
  are determinism-safe; always re-run `--smoke` to confirm.

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

**TRUE root cause (the generator-null theory was a red herring):** a destroyed-world
**use-after-free**. `WorldgenPreview::swap_pending_into_live()` replaces + FREES the candidate
world between frames, while far-LOD tile-build jobs dispatched the previous frame are still running
on worker threads holding a reference to that now-freed world. Reading freed memory (a SmartNode /
vtable) manifested as a null function-pointer call. That is why guarding individual generators was
whack-a-mole — EVERY sampler (`ComputeShapedHeightSampleImpl`, `RiverInfluenceFromNoise`,
`GetTerrainHeightAtCoarse`) reads freed memory. **Not the render-seam / Pillar A work.**

**RESOLVED — fix sequence (6 crash traces, ~5 partial fixes; lesson below):**
- `fc360183` reader guard + `da5eee10` `reinitialize_noise` never-null reorder — defense-in-depth,
  but only addressed the *null-generator* sub-symptom (kept moving across samplers).
- `6632157c` disabled far-LOD when `m_offscreen_target_active` — WRONG path: the create-world
  screen renders via `WorldgenPreview::render_to_backbuffer` (NO offscreen target), so it never fired.
- **`09a3e6b2` (the real fix):** a `RenderPipeline::set_far_lod_enabled` flag that the preview
  toggles OFF around `render_frame` in BOTH `render()` and `render_to_backbuffer()`. No far-LOD
  build job is ever dispatched for the transient/freed preview world → no UAF. Game path untouched,
  render-only (no `world_hash`). **No longer crashes.**

**THE LESSON (added to §3.4):** the diagnostics were instant every time; the *fixes* were slow
because I fixed *near* the crash (guard generators → gate the offscreen path I assumed) instead of
first CONFIRMING the live call path. One `grep` for who calls `WorldgenPreview::render*` would have
shown `render_to_backbuffer` immediately. **Symbolize → confirm the exact path → then fix.**

**KNOWN ISSUE introduced by the fix — preview areas don't load (near + far):**
- **FAR** not loading is the *direct, expected* trade-off: far-LOD is now OFF for the preview, so
  the streaming far-field (>256m, the SDF/far-LOD tiles) is absent in the create-screen diorama.
- **NEAR** gaps are a SEPARATE preview streaming issue (near-field <256m Marching-Cubes chunks not
  all arriving) — investigate whether the preview's chunk-build radius / swap timing leaves holes
  (it may predate this session; the world swaps as you pan).
- **Proper follow-up that restores BOTH:** make `swap_pending_into_live()` DRAIN the pipeline's
  in-flight far-LOD build jobs BEFORE freeing the old world (add a `RenderPipeline::drain_far_lod()`
  that `m_job_system->wait()`s the FarLodSystem build handles), then RE-ENABLE far-LOD for the
  preview. That removes the UAF at the source so the far-field can render again. Validate with the
  TSAN + scripted-orbit harness (§3.3) — it would have caught the original UAF in one run. Separately
  audit the near-field preview streaming for the holes.

---

## Part 5 — Create-world UI + preview polish backlog (owner punch-list, 2026-06-28)

The owner exercised the create-world screen and surfaced a set of UI/preview issues. Files:
`data/ui/world_creation.rml`, `data/ui/game_theme.rcss` (RmlUi, hot-reloadable — no C++ build
needed for RML/RCSS), and the host wiring in `main_client.cpp` (~:9048-9170) +
`ui/Rml_UIManager.cpp` (knob collection :165-200, customize toggle :530-537). Best worked with
the headless RmlUi e2e harness `test/ui/ui_smoke_test.cpp` (drives real RmlUi via `Element::Click()`).

1. **Slider knob not vertically centred — FIXED** (`game_theme.rcss` `input.settings-slider
   sliderbar` margin-top -6 → -5; 14px knob on a 4px track @ margin-top 10 centres at -5).
2. **"world feel" knob doesn't change the preview.** The wiring EXISTS — the rebuild signature
   (`main_client.cpp:9088-9089`) concatenates every `pv.params` incl. the knobs (`knob.<id>`),
   and a sig change calls `BuildKnobResolvedPreset` + `set_candidate` (:9106-9111). So debug:
   (a) is `pv.params` re-collected from the live form each frame (vs cached/stale)? (b) do the
   KnobLayer response curves actually move terrain enough to SEE? (c) is the change only in the
   far field (now hidden because far-LOD is OFF for the preview, item 6)? Add a one-line log of
   the sig in the rebuild branch to confirm it fires on a knob drag.
3. **Advanced params can't be interacted with.** The toggle handler exists
   (`Rml_UIManager.cpp:530-537`, flips `.collapsed` on `#customize_body`). Check: does the click
   actually fire (log it)? When expanded, `.customize-body { max-height:360px; overflow-y:auto }`
   — verify RmlUi scroll + slider drag work inside a scroll container (a known RmlUi friction);
   confirm the sliders aren't covered by a transparent element / the panel's stacking.
4. **Layout — reduce scrolling, use space.** Owner wants TABS or SECTIONS instead of one long
   scroll, and the value NEAR the slider, not floated far-right. Today every row is
   `label (block, value floated right)` then the slider full-width BELOW (`.settings-row`,
   `.knob-row`, `.param-row`). Proposal: a horizontal row `[label][slider flex][value]` (flex,
   `align-items:center`) so the value sits beside its slider; group `world feel` / `terrain` /
   `water` / `biomes` / `features` into tab panes (chips toggle `.active` on one pane). RmlUi
   supports flexbox + class toggles; no engine change.
5. **Areas don't load (near + far)** — see Part 4. FAR is the far-LOD-off trade-off; NEAR holes
   need the preview chunk-streaming audit. The proper fix (drain far-LOD before the preview's
   world swap, then re-enable far-LOD) restores the far field.
6. **Weather (rain/snow/storm) looks flat/slow, not like real particles.** Likely the particle
   pass isn't visible/active in the preview path, OR the preview world's weather drives only the
   shader overlay, not the `ParticlePass`. Check whether `render_to_backbuffer` runs the particle
   pass + whether precipitation particles are spawned for the preview world (the game path may
   gate them on sim state the preview lacks). The `--frame-scan` weather scenes can validate the
   particle look headlessly.

This is a cohesive focused effort (RML/RCSS + a little host wiring) — a good candidate for a
dedicated session or a small subagent fan-out (one per item) against the committed RML/RCSS.

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
