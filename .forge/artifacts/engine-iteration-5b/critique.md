# Engine Iteration 5b Critique — adversarial pass + resolutions

Concise devil's-advocate pass on the 5b plan (life & water). Each finding argues
against; the resolution binds the design-decisions / task prompts.

## F1 — Ecology "canonical-neutral" is fragile **[binds T-I5b-2]**
Objection: adding a stimulus-channel registry that the planner samples every tick
can shift the default entities sub-hash even if NO creature subscribes — a new
tick step, a new RNG draw, or reordered planner iteration perturbs `world_hash`
(bump #4) silently. The whole no-bump design rests on this.
Resolution: the registry must be INERT for non-subscribing creatures — channels
are computed lazily/only when a creature's game-data declares a subscription;
the planner's existing tick path is byte-unchanged for the default roster.
T-I5b-2 PROVES it: HeadlessServerTick must stay `d950a6afc12a5cdc` in the same
commit. If it can't, STOP and report — a bump is an orchestrator decision.

## F2 — Live-ring sea coverage re-breaks the FarLodHorizon classifier **[binds T-I5b-5]**
Objection: this is the EXACT trap that reverted the iter-4 attempt — rendering a
pale sheet behind live transparent water shifted the boundary-band blue-dominance
ratio to 0 and broke the gate. Naively filling the 0–512 m annulus will do it
again.
Resolution: T-I5b-5 OWNS both the coverage AND the re-derivation of the
FarLodHorizon water-band classifier (now that 5a aerial fog tints far water) in
the same task. The gate is re-blessed deliberately against the new look, not
fought. If sea coverage and the band premise are irreconcilable on the shipped
preset, report the tension with evidence (the iter-3 MaterialVisual lesson).

## F3 — Foliage perf is unbudgeted against a noisy baseline **[binds T-I5b-1 / closeout]**
Objection: per-chunk instanced scatter across the view is the heaviest render
addition of the iteration, and the release perf baseline is still the retained
T-I3-20 one (the post-5a quiet bless never happened). A foliage budget gated
against a stale/noisy baseline is meaningless.
Resolution: T-I5b-1 budgets foliage via a per-pass GPU-timer (like the 5a
features), independent of the worldgen perf lane. The quiet-machine release
re-bless is a closeout prerequisite; until then the GPU-timer ceiling is the
load-bearing number. Instance counts are capped + distance-faded.

## F4 — Foliage/water re-bless could mask a coverage regression **[binds T-I5b-1/5]**
Objection: foliage adds pixels everywhere there's ground; a blanket RenderHealth
re-bless could hide a real PlayerView terrain-coverage or FarLodHorizon sky-ratio
regression.
Resolution: re-bless RenderHealth deliberately + logged, but PlayerView
(min_renderable_ratio / max_sky_ratio) and FarLodHorizon sky-ratio premises must
stay GREEN unchanged — foliage adds detail, it must not reduce renderable terrain
or leak sky. Closeout verifies both.

## F5 — Waterfall detection must be world-deterministic, not frame-deterministic **[binds T-I5b-4]**
Objection: "render-side detection" invites a per-frame or camera-dependent scan
that yields different sites for different players → not "same falls for every
replay".
Resolution: detection is a PURE function of the generated world data (waterline +
heightfield), computed once per region (cached), independent of camera/frame.
The gate asserts same-seed → same-sites. Render-only (not hashed) but
world-deterministic.

## F6 — Shared-file merge conflicts across the parallel wave
Objection: foliage, audio, waterfalls, ecology, water-backlog all touch
`RuntimeScenarioHarness.{h,cpp}` + `validate-engine-frontier.ps1` (gate wiring) →
parallel agents conflict.
Resolution: additive (each adds its own gate/scenario) — the git ort strategy
auto-resolved these for the 5a Wave-C agents; the orchestrator resolves the
ValidateSet/dispatch/scenario hunks as union (proven in the 5a B2 merge). Not a
blocker; sequence merges, rebase each agent on the prior.

## Disposition
No re-scoping needed. Load-bearing binds: F1 (canonical-neutral proven in-commit),
F2 (band re-derivation owned with the coverage), F5 (world-deterministic
detection). 5b keeps `world_hash` at `d950a6afc12a5cdc` by design.
