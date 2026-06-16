# forge-critique — Isolation/Layer render mode spec (2026-06-16)

4-lens devil's-advocate pass (correctness / architecture / testing-CI / scope-value)
on `.forge/artifacts/engine-iteration-6/ISOLATION-LAYER-SPEC.md`. Verdict: the v1
design (render-side per-pass masking + backdrop fill + transparent alpha) is BOTH
over-engineered (risky deferred surgery) AND under-scoped (no sim/studio context).
Pivot v1 to the low-risk scenario-driven approach; defer the render-side machinery.

## BLOCK / MAJOR objections + disposition

### CORRECTNESS (deferred pipeline) — the naive "skip passes + clear backdrop" breaks
- **C1** Lighting pass overwrites the backdrop at empty pixels (ambient/aether glow;
  always writes alpha=1) — `LightingPass.cpp` + `lighting_pass.frag`. Clearing the
  FBO to a backdrop BEFORE lighting is overwritten by the fullscreen lighting quad.
- **C2** Aerial fog blends over the backdrop / depends on a consistent depth buffer.
- **C3** Foliage/particles depth-test against the scene depth (blitted from gbuffer);
  with terrain suppressed the depth is uninitialized → they get culled or float wrong.
- **C4** Transparent alpha does NOT survive: default framebuffer is RGB, the final
  blit + `glClearColor(...,1.0)` clobber alpha, capture is `glReadPixels(GL_RGB)`.
  Transparent backdrop = a much bigger change than the spec admitted.
- **C5** The SHIELD-RT far-field pass writes the gbuffer unconditionally — no mask.
- **Disposition:** these are exactly why render-side masking is the WRONG v1. **DEFER**
  render-side pass masking + transparent alpha to v2. v1 keeps the full deferred
  pipeline intact and isolates by NOT spawning content (below).

### SCOPE / VALUE (decisive) — pivot v1 to scenario spawn-suppression
- **S1 (adopt):** 80% of the review value comes from **not spawning** the non-isolated
  content into the scenario world (terrain/foliage/particles/water/creatures) — a
  data-driven harness change, ORTHOGONAL to the render pipeline → near-zero risk,
  full deferred pipeline + existing RGB capture intact, default runs byte-stable.
- **S2 (adopt):** backdrop = a **SkyboxPass shader override to a flat colour**
  (void/greenscreen/checker) — a few shader LOC, no deferred surgery. Transparent → v2.
- **S3 (defer, document):** "floating lit grass on green" may not be reviewable
  without a **studio rig** (neutral ground plane + stable lighting + turntable) — that
  is a SEPARATE, bigger feature (v2). v1 ships void/greenscreen capture; note the limit.
- **S4 (defer):** SIM isolation (aether/wind field, weather, GOAP debug viz) is its own
  debug-viz layer system — v2.
- **S5 (defer):** an interactive in-client layer toggle (hotkeys) is likely higher
  long-term ROI than capture-only — v2, owner to confirm.
- **S6 (acknowledge):** this is a convenience tool, not a blocker — the visual-critique
  already gives the fidelity signal. v1 must be CHEAP + safe so it doesn't derail the
  terrain/far-field push. The scenario approach satisfies that.

### ARCHITECTURE / MAINTAINABILITY (apply to whichever path)
- **A1:** a single dependency-free `core/IsolationConfig.h` (enum + parse), like
  `CaptureScale.h` — pure, unit-testable without GL. CLI `--isolate <csv>` +
  `--backdrop <mode>` (hyphen style matches `--window-mode`); parse in the render/
  config domain, not scattered.
- **A2:** the default config (no `--isolate`, backdrop=Scene) is provably a no-op →
  existing 247 ctest + RenderHealth + WorldVisualSweep stay byte-stable.
- **A3 (for v2 render-masking):** a single `should_render_pass(pass)` helper + a
  centralized pass↔layer table keyed off the existing GpuTimerPass enum (no parallel
  drift); single-source backdrop colour (the 3 clear/capture sites must agree).

### TESTING / CI
- **T1:** a synthetic-quad headless test would NOT exercise the real isolation path —
  test the PURE gating/parse LOGIC as a unit test (no GL), and validate the real path
  via a scenario gate. Don't ship a fake.
- **T2:** objective backdrop checks are antialiasing/quantization-fragile → fixtures +
  tolerances (>=95% of background within 1 LSB of the backdrop colour), not exact-match.
- **T3:** byte-stability must be ACTUALLY tested (default config), not just claimed.
- **T4:** minimise CI cost — ONE isolation scenario in the gate, reuse the existing
  capture + critique infra, seed for determinism, avoid another flaky manual;gpu test.

## Revised v1 (executing now, TDD + CI)
1. `core/IsolationConfig.h` (pure): `BackdropMode` enum, `ParseIsolationLayers(csv)` →
   layer set, `ParseBackdropMode(str)`. Dependency-free. **Unit test first** (ctest,
   default lane), incl. default-is-noop + unknown-token handling.
2. Backdrop: SkyboxPass flat-colour override (void/greenscreen/checker) gated by config.
3. Spawn-suppression: a scenario `--isolate <csv>` that spawns only the selected
   content; the rest never created (harness/world driver), full pipeline intact.
4. CI: pure unit test in default ctest; `-Mode IsolationLayer` gate = one seeded
   capture (e.g. foliage on greenscreen) + objective backdrop-fill check (tolerant,
   fixture-pinned); default render byte-stable.
5. Defer to v2 (documented): render-side per-pass masking, transparent RGBA, studio
   rig (ground plane + lighting + turntable), sim debug-viz layers, interactive toggle.
