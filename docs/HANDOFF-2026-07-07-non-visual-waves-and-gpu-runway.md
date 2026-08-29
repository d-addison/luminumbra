# HANDOFF 2026-07-07 — non-visual feature waves landed; next waves + GPU runway

Branch `feat/polyglot-audit-roadmap` (HEAD `84eb4c1a`, **never push**). Backlog **156/190**
(`docs/audit/021/backlog.json`). This session shipped the render-scale seam bugfix, a "white filter"
render pass, 12 non-visual backlog items across 3 parallel-workflow batches, two hash-sensitive
completions (INSTINCT-09 retirement, NET-11 full form) verified byte-identical, and the low-risk half
of OPS-16. Details in auto-memory `non-visual-feature-loop-2026-07-07.md` + `white-filter-investigation.md`.

---

## 0. STANDING CONSTRAINTS (load-bearing — violate none)
- **Never push.** Commit per green gate (`git commit -F <file>` — PowerShell has no heredoc; write msg to a temp file).
- **Toolchain:** prepend `C:\msys64\ucrt64\bin` to PATH on EVERY build/ctest/validator. Build the tree you test (`build/debug`). Full rebuild after header changes.
- **ctest SERIAL only** (`-j` gives false failures). PS `2>&1 |` MANGLES a native exe's stderr — use `*>`.
- **Render/sim-hash code is INLINE-only** — never fan out `RenderContext`/`RenderPipeline`/pass seams/`main_client`/`GameSession`/world_hash paths to agents.
- **Determinism:** the canonical `world_hash` is `d950a6afc12a5cdc` (all systems OFF). Any sim change must keep it (verify via `ctest -R Ecology` for AI, or the relevant hash gate) OR be a deliberate, documented re-pin.

## 1. THE PARALLEL-WORKFLOW PATTERN (reuse it — it landed 12 items cleanly)
Agents **cannot build** (Bash sandbox has no compiler). So: `Workflow` fans out N **file-disjoint** agents
that WRITE code + a test to their scoped files (and flag `ORCHESTRATOR-REGISTER` for test/CMake + `ORCHESTRATOR-WIRE`
for main_client). Then the ORCHESTRATOR (you) registers tests in `test/CMakeLists.txt`, wires main_client,
builds, runs the new + regression tests, commits. Rules that made it work: airtight per-agent file scopes;
agents never touch shared build files, main_client, GameSession, or hashed sim output; a `git status` sweep
+ full build after every batch. Backlog status flips via minimal-diff regex
(`re.sub(r'("id":"X".*?"status":")todo(")','\\1done\\2',DOTALL)`), then `python docs/audit/021/validate_backlog.py`.

## 2. KNOWN ISSUES carried in (not this session's regressions)
- **`-Mode NetworkedSession` is RED** on a stale terrain-hash pin (`end_hash 354be8d… != 46f89d27449011a0`).
  It asserts host==client (passes) → internally deterministic; only the PIN differs. PROVEN not from this
  session: no commit touches worldgen; that scenario runs creatures/ecology OFF so INSTINCT-09 is a no-op.
  FIX: re-run + re-pin `46f89d…` at line ~5752 of `tools/gates/validate-engine-frontier.ps1` if the new
  value is stable across runs, OR chase flaky worldgen determinism if it varies run-to-run.
- **GPU-P09 is PARTIAL.** `c6493044` fixed the scale<1.0 pass-internal viewport SEAM (byte-identical at 1.0,
  0.67 renders correctly). The rest of the item is unstarted — see Wave 1.
- **The headless "giant white shards"** are a material-255 CAPTURE-POSE artifact (billboards edge-on from the
  elevated frame-scan pose), NOT eye-level gameplay — do NOT chase them as terrain. Use `--debug-view material`
  FIRST for any geometry-ID question. The frame-scan pose is ELEVATED and misleading; owner reports the
  terrain/clouds still look "messed up" at eye level — a real render-fidelity pass is still owed (Wave 5).

---

## 3. NEXT WAVES (ordered by value ÷ risk; each is a session's worth or less)

### WAVE 1 — close M1 render-scale (GPU-P09 remainder + GPU-07) — INLINE, render code
The seam bug is fixed; finish the item so DLSS (Wave 4) has its precondition. Per the prior plan
(`C:\Users\David\.claude\plans\handoff-2026-07-06-polished-planet.md`, Phases 2–5):
1. **LOD mip bias** `u_lodBias = log2(m_render_scale)` on scaled samplers (guard default 0.0 → no-op at 1.0).
2. **Wire `user.render_scale`** — `RenderPipeline::set_render_scale(clamp[0.5,1.0])` recompute `m_internal_*`
   + realloc; call from `main_client.cpp:3223` before `startup()`; env `LUMIN_RENDER_SCALE` must still WIN for A/B.
3. **`UpscaleSeamParity` gate** (leg1 EXACT 0.0 at 1.0; leg2 soft ≤ pre-registered threshold; NOT in `-Mode All`).
4. Flip **GPU-P09 + GPU-07** → done; `validate_backlog.py`.
ACCEPT: `-Mode RenderParityFrame` EXACT 0.0 at 1.0 after every step; looked-at 0.67 capture terrain+blue-sky
(`tools/framediff.py` band-parity); interactive run at 1.0 and 0.67.

### WAVE 2 — hygiene / ops / docs (small, safe, mostly parallelizable) — WORKFLOW-friendly
- **OPS-16 hash half** (INLINE — hashed + main_client): data-drive the `ServerWorldRunner` grovestrider spawn
  (HASH-VISIBLE → deliberate `world_hash` re-pin) + `main_client` audio event ids / ambient fauna list /
  `MaterialType::LuminCrystal`. Remove the last 2 `EngineGameSplitLint` allowlist entries. The low-risk half
  (SpeciesCodex + harness) is already done (`84eb4c1a`).
- **OPS-09** (M/low): charter a scheduled local full-gate run (CI is dormant/wrong-tree; `-Mode All` excludes Build).
- **OPS-15** (S/low, doc): harness operator doc — the 18 headless flags (8 server + 10 client).
- **AUDIO-13** (S/low, doc): refresh `docs/AUDIO-everything-maps-to-sound.md` (sleep/feed/drink/colony SFX, day/night).
- Close the **NetworkedSession stale pin** (§2).

### WAVE 3 — dedicated-server networking finish (needs Linux/GNS/2nd machine — CANNOT fully verify on this Win box)
Implement + compile-guard, flag verification-blocked:
- **NET-13** (M/low): POSIX `TcpTransport` (`#else` branch is a no-connection stub) — Docker/Linux-validated.
- **NET-08** (L/med): GNS over-the-wire matrix (`LUMINUMBRA_ENABLE_GNS` OFF; needs GNS vendored).
- **NET-12** (M/high): Steam SDR 2-machine (single-PC can't validate; see memory `single-pc-testing-constraint`).

### WAVE 4 — GPU ENGINE TRACK (the big multi-milestone runway, M2–M6) — INLINE, XL
Depends on Wave 1. In order:
- **M2 GPU-P06/GPU-08** (XL): single-source HLSL port of the ~53 GLSL shaders (pass-risk order); INLINE, per
  memory `dispatch-shaders-inline` (codex under-delivers on intricate shaders).
- **M3 GPU-P07** (XL): pass-by-pass native-Vulkan port (increasing-coupling order), per-pass FLIP-byte-identical.
- **M4 GPU-P10/GPU-11 + GPU-P11/GPU-13** (M–L): `IUpscaler` provider contract → DLSS via NVIDIA Streamline
  (FetchContent the SDK; default-OFF). This is why Wave 1's render-scale seam must land first.
- **M5 GPU-P08** (L): DX12 backend behind the same seam.
- **M6 GPU-P12/GPU-10/GPU-P13/GPU-P14** (L–XL): BLAS/TLAS from the near-field marching-cubes mesh → RT-GI/AO
  + RT reflections (`LUMIN_RT*`, default-OFF). RTX 5070 Ti target.

### WAVE 5 — world/render fidelity (owner-visible; the "still messed up" complaint) — INLINE, XL
- **SHIELD-08** (XL): coarse LOD (step>1) + far tiles must reduce the SAME SDF (currently re-derive from
  heightmap → edits/caves vanish at distance). See memory `coarse-lod-ignores-sdf`. Likely the biggest lever
  on the owner's far-field "messed up terrain/clouds".
- **ATMO-13** (L/high): region-follow the weather/wind grids (both anchor on the fixed spawnPoint).
- **WATER-15** (XL/high): river-channel sim beyond the uniform 8×8 4m grid.
- **FOLIAGE-10** (L/low): richer tree/plant morphology (space colonization); FOLIAGE-07 (sim-grown fields),
  FOLIAGE-09 (drop the `GL_ARB_gpu_shader_int64` hard requirement from `grass_scatter.comp`).
- Re-examine the eye-level render at a NON-elevated pose (add a pose-height override or run interactive) —
  the grade de-wash (`b59a65ea`) may need a broader visual re-bless (SkyboxVisual/PlayerView/WorldVisualSweep).

### WAVE 6 — UI / photography game loop (the iteration-7 payoff) — Sonnet-fan-out-friendly (RmlUi)
- **UI-06** (saves list), **UI-09** (expose resolution/sfx/music controls), **UI-08** (fidelity baseline all
  screens), **UI-10** (codex v2 photo thumbnails), **UI-11** (first-session tutorial).
- **AUDIO-14** (photography SFX — needs ElevenLabs key), **INSTINCT-14** (server-auth nests/homes + return-home).

---

## 4. RECOMMENDED NEXT-SESSION START
Wave 1 (finish GPU-P09 — small, unblocks DLSS) + Wave 2 (hygiene batch via Workflow) is the highest
value-per-risk opener. Then commit to ONE of the XL tracks: Wave 5 (SHIELD-08) directly addresses the owner's
still-open visual complaint; Wave 4 (GPU engine) is the strategic DLSS/RT runway. Ask the owner which to prioritize.
