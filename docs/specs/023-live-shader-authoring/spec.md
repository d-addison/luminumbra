# Spec 023 — Live Shader Authoring (crawl + walk)

**Status:** Landed (crawl + walk, 2026-07-05, Wave F F3). Run tier: charted, not built.
**Owner ask (2026-07-04):** "a way (like Minecraft) to tweak things, drop things in,
experiment, ideally even a panel for dev changing shaders on the fly and seeing its
effects (look at Blender)."

## Problem

Shaders are runtime-loaded from `res/shaders/` (no rebuild for a `.frag` edit) and
`Shader::Reload()` (spec 016 FR-D-003) is rollback-safe — but it had ZERO callers. The
edit→see loop was: edit, restart the client, re-stream the world (~minutes). For a
Frostbite-floor fidelity target tuned by iteration, that loop is the bottleneck.

## Requirements

- **FR-023-1 (crawl — reload-all):** one hotkey (F5) hot-reloads every live shader from
  disk. Per-shader result honored: a broken edit KEEPS the previous good program
  (Reload's rollback contract) and reports the diagnostic; the session never breaks.
- **FR-023-2 (roster):** ONE enumeration of the pipeline's live shaders
  (`RenderPipeline::enumerate_shaders`) feeds shader health, reload-all, the dev panel,
  and the watcher — the roster cannot drift between consumers by construction.
- **FR-023-3 (walk — auto-reload):** an opt-in file watcher (once/sec mtime poll on the
  enumerated shaders' source paths, main-thread, zero threads) auto-triggers per-shader
  Reload when a source file changes. Edit in any editor → see it next second.
- **FR-023-4 (walk — dev panel):** an ImGui "Shaders (F10)" panel lists every roster
  shader (valid state + last diagnostic), offers per-shader Reload + Reload All + the
  auto-reload toggle, and exposes LIVE uniform editing — active non-sampler uniforms
  (bool/int/float/vec2/3/4) introspected from the linked program and written via
  `glProgramUniform*` (GL 4.5 DSA).
- **FR-023-5 (uniform-edit honesty):** passes re-set most uniforms every draw; a live
  edit persists only for uniforms NOT re-set per frame. The panel is therefore a probe +
  a home for the `u_dev_*` convention: an authored-tweakable uniform a pass deliberately
  never sets gets its value from the panel alone. Documented in the panel header.

## Determinism firewall (LOAD-BEARING)

Render-side hot-reload + uniform tweaks are determinism-FREE: RenderPipeline is
client-only and never feeds `world_hash` (018 FR-E-003). Headless server paths never
construct the panel/watcher. Any future drop-in that affects SIM (a material with sim
properties) MUST route through SystemConfig's gated/hashed path (additive sub-hash,
default-OFF, re-pin discipline) — the two worlds must not blur.

## Proving signals

- Both determinism smokes byte-identical (no sim surface).
- `-Mode RenderHealth` green (the health list is now roster-derived and grew to cover
  god_rays/cloud_composite/waterfall — previously unmonitored).
- `-Mode RenderParityFrame` EXACT 0.0 (the watcher/panel are frame-loop features outside
  dispatch; parity capture unaffected).
- The Reload rollback contract is pinned by the FR-D-003 gates from spec 016.

## Run tier (charted, NOT built — rank alongside the render band)

Data-driven pass/material drop-in: `res/materials/*.json` (or similar) loading pass
definitions as RenderGraph nodes, so a new visual effect needs no C++. Now that the
graph DRIVES execution (RENDER-11 complete, Wave F F2), a dropped-in pass is a new
node + a generic executor — charge this when the froxel/OIT stages (RENDER-17/18) have
proven the add-a-node workflow end to end.
