# Engine Iteration 5a Critique — adversarial pass + resolutions

Devil's-advocate review of the 5a spec + research, in the spirit of the
iteration-3/4 critiques. Each finding (Fn) argues *against* the plan; the
resolution states what the design-decisions doc and task prompts must bind.
Findings that change a task contract are marked **[binds task]**.

---

## F1 — The `world_hash` mega-bump can silently break replay/lockstep **[binds task]**

Objection: adding `wind` + `weather` sub-hashes changes `world_hash`
(`2fa007951a21e140` today). If a task lands the sim system and the hash change in
one commit but does NOT re-run the heavy-mode save/load/resim oracle and the
LREC1 replay + lockstep loopback gates, a determinism leak (e.g. a stray libm
`expf` in the storm advection) ships green-looking but desyncs in the field — the
exact Factorio-class failure the iteration-4 contract was built to prevent.

Resolution: the mega-bump is an **orchestrator-gated single commit** (design-doc
rule, repeated in B1/D prompts). The commit that first changes `world_hash` MUST,
in the same commit: add the sub-hash(es); re-run and re-bless the heavy-mode
oracle, LREC1 ReplayRoundtrip/ReplayDivergence, and LockstepLoopback/
LockstepFaultInjection; log the old→new `world_hash`. Each sim system (A2 wind,
B1 weather, B3 strike schedule) proves its own update is DeterministicMath-clean
and SimDeterminismLint-passing BEFORE it is folded into the hash. If a system is
unexpectedly not hash-load-bearing or breaks replay, STOP and report. Sequence so
A2's wind sub-hash and B1's weather sub-hash bumps are deliberate and adjacent,
not scattered.

## F2 — Particle/cloud "determinism" is a trap: too much in the hash, or too little **[binds task]**

Objection: the plan says particle *motion* is render-only but emitter *schedule*
is hashed — a fuzzy line an implementer will get wrong. Hash the particle arrays
and you've made cosmetics lockstep-critical (and non-deterministic GPU FP will
desync you). Hash nothing and the ParticleEmitterDeterminism gate is vacuous.
Same risk for clouds (a "pure function of weather state" that accidentally feeds
a value back into a sim decision).

Resolution: the design doc pins the snapshot surface **explicitly**: the
ParticleEmitterDeterminism gate snapshots the *emitter descriptor set* only
(id, type, origin region, spawn-rate, RNG seed, enable flag) at a fixed tick;
particle arrays are never snapshotted and never hashed. Clouds and bolt geometry
are pure render functions of already-replicated state with a one-way data flow —
a SimDeterminismLint-adjacent review item: **no render subsystem (particles,
clouds, sky, bolts) may write into any sim/`world_hash` input.** A1/C3/B3 prompts
state the one-way rule. The emitter RNG seed itself is derived deterministically
from world state so the *descriptor* is reproducible without the *motion* being.

## F3 — Perf cost stack is under-budgeted; five GPU features land in one iteration

Objection: particles + analytic aerial fog + Hillaire sky precompute + a per-lit-
fragment cloud-shadow sample + bolt light-pulse all add GPU cost. The cloud-
shadow sample in the lighting pass is per-fragment over the whole scene every
frame — potentially the most expensive item, hidden as "one texture lookup." The
release lane (`initial_world_loading_perf_test`) measures worldgen/streaming, NOT
render submission, so — exactly as the iteration-4 MDI win could not appear in it
— these render costs won't show up in the existing perf gate at all.

Resolution **[binds task]**: each render addition gets a **GPU-timer-backed**
budget, not the worldgen perf lane. Reuse the per-pass GPU timers (landed
iteration 2) — add timer scopes for ParticlePass, the aerial term, the cloud-
shadow sample, and the sky precompute (startup, one-shot). The design doc pins
per-feature ms thresholds at the baseline view; RenderHealth telemetry records
them; PerfRegression's render-submission assertions (not the worldgen lane) gate
them. Cloud-shadow specifically: pin a ceiling and prefer a low-res shadow
coverage texture + cheap sample; if it exceeds budget, fall back to a coarser
projection rather than shipping a regression.

## F4 — Gate-premise conflicts (the iteration-3 MaterialVisual lesson) **[binds task]**

Objection: the new sky/weather gates may conflict with existing gate premises.
A dark overcast/storm sky will tank SkyboxVisual's monotonic-brightening-toward-
horizon and sun-disc-present assertions (no sun disc under cloud). A dimmed
stormy scene could violate PlayerView's `min_renderable_ratio`/sky-ratio bands or
FarLodHorizon's `max_sky_ratio`. TimeOfDaySweep's noon-brightest ordering could
break if scattering changes absolute luminance. This is precisely the
"irreducibly-in-conflict on one preset" trap that deferred MaterialVisual.

Resolution: weather/storm captures run in a **dedicated weather scenario with
clear-sky control phases** (WeatherVisual already uses a baseline-then-weather
structure — extend it, do not enable storms inside SkyboxVisual/PlayerView/
TimeOfDaySweep). SkyboxVisual + TimeOfDaySweep keep a **clear-sky** atmosphere so
their sun-disc/monotonic/ordering premises hold; the scattering change is
validated there against re-derived bands (deliberate logged re-bless if noon
luminance shifts). New atmospheric assertions get their OWN scenario/ROIs rather
than overloading existing ones. CloudShadow uses a fixed partly-cloudy fixture,
not full overcast. The closeout explicitly checks PlayerView + FarLodHorizon stay
green on a weather-enabled-but-clear world.

## F5 — Wind grid "generalized field container" is speculative generality

Objection: building the wind grid as a generalized `Field<T>` for a hypothetical
iteration-6 Aetheric reuse risks over-engineering a vector field to fit an
unbuilt scalar use-case, gold-plating the waypoint (the iteration-4 SHIELD-RT
spike lesson: don't gold-plate the destination).

Resolution: the mandate is real (roadmap pins it) but bounded — the design doc
specifies the **minimum shared surface**: storage layout (cell grid + stride),
the tick-budget slot in TickSimulation, and the snapshot/sub-hash plumbing. It
does NOT require a finished generic template now; A2 implements a concrete wind
field whose storage/iteration/snapshot interface is factored so a scalar field
can reuse it, validated only by "the interface is documented + the snapshot/hash
path is shared." No second consumer is built in 5a; the iteration-6 reuse is a
noted enabler, not a 5a deliverable.

## F6 — "Hillaire 2020 sky" is a large render rewrite hiding behind one bullet

Objection: replacing the authored gradient with precomputed scattering LUTs +
coherent sun/ambient/fog is a multi-week renderer change (LUT passes, exposure
recalibration, ambient integration, fog term) dropped as one task (C1). It can
balloon and destabilize TimeOfDaySweep/SkyboxVisual/RenderHealth at once.

Resolution **[binds task]**: C1 is scoped to the **three LUTs + analytic aerial
term only**, explicitly NOT froxel volumetrics (deferred), and reuses the
existing `u_skyDayFactor` envelope + star/aurora layers rather than rewriting
them. Calibrate atmosphere params so noon lands inside the existing TimeOfDaySweep
band where achievable to avoid a gratuitous exposure re-bless; any band move is a
single deliberate logged re-bless. If C1's scope is larger than one deep task at
review, split LUT-precompute from the lighting-pass aerial term as a follow-up —
but the spec keeps it one task with a clear stop line at "no froxel."

## F7 — Season/celestial time risks a wall-clock determinism hole **[binds task]**

Objection: "long-period time scale driving sun path" invites someone to derive
season from real elapsed time or a float accumulator that drifts — a wall-clock
determinism hole, and if season state is then read by any sim system it poisons
`world_hash`.

Resolution: C2 derives the season phase as a pure function of **tick count**
(integer epoch math, DeterministicMath if any trig is needed for sun path),
never wall-clock, never a free-running float accumulator. Prefer season as a
**render-derived** quantity (computed from the authoritative tick on the client)
so it adds nothing to `world_hash`; if any sim system must read season (e.g. for
a future ecology hook — but that's 5b), it is hashed and DeterministicMath-clean.
SimDeterminismLint already bans wall-clock in sim paths; C2 stays render-side.

## F8 — Lightning audio "free realism" assumes propagation supports a delayed one-shot

Objection: thunder-after-flash assumes `AudioPropagationSystem` can schedule a
distance-delayed one-shot at a world position. If it can't, B3 quietly grows an
audio-engine task that wasn't budgeted.

Resolution: B3's prompt requires a **read of `AudioPropagationSystem.h` first**
to confirm the delayed-positional-one-shot capability; if present, reuse; if a
thin hook is needed, it is a small additive method, NOT new propagation
machinery, and the null-audio gates must stay green (thunder is optional dressing,
gated only on the visual pulse, not on audio). The strike *visual* gate does not
depend on audio.

## F9 — Endurance300Storm may mask the real risk: unbounded weather/particle state

Objection: a 300-tick storm endurance run checks for a perf cliff but the subtler
failure is unbounded growth — storm cells that never retire, an emitter pool that
leaks, a precip field that accretes. A pass/fail on frame time can be green while
memory climbs.

Resolution **[binds task]**: Endurance300Storm asserts **bounded state**, not
just frame time — storm-cell count stays within a cap, emitter pool is
fixed-capacity (recycled, asserted), precip-field memory is flat, and the
`weather` sub-hash space is bounded. Record peak memory + storm-cell count in the
endurance artifact. This is a closeout (Wave D) assertion.

## F10 — Splitting 5a/5b leaves dangling consumers

Objection: 5b's foliage wind-displacement, ecology stimulus channels, atmosphere
audio, and waterfall spray all consume 5a outputs (wind grid, weather state,
particle framework). If 5a's interfaces aren't designed with those consumers in
mind, 5b forces 5a rework.

Resolution: the design doc lists 5b's consumers as **named downstream readers**
of A1 (particle emitter API), A2 (wind sampling API), B1 (weather-state query
API) so those interfaces are public and stable at 5a close. No 5b code lands in
5a, but the read-side APIs are designed for them. The closeout writes the 5b
planning inputs (incl. the re-derived FarLodHorizon water-band premise now that
aerial perspective exists).

---

## Disposition

All ten findings are resolvable within the planned scope; none require a
re-scoping of 5a. The load-bearing binds are F1 (single-commit mega-bump +
re-validation), F2 (explicit snapshot surface + one-way render→sim rule), F3
(GPU-timer budgets, not the worldgen lane), F4 (dedicated clear-sky scenarios so
existing gate premises hold), F6/F7 (scoped sky rewrite, tick-derived season),
and F9 (bounded-state endurance). These are folded into design-decisions.md and
the per-task prompts in dispatch.json.
