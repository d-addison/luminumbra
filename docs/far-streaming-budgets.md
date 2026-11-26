# Far streaming budget measurements

Version v0.3.2 is in preparation. The issue #133 measurement harness is implemented;
representative native captures, measured proposals, approved numbers, enforcement,
qualification, merge and release remain pending. This document describes measurement
evidence, not the versioned acceptance receipt or production budget selection.

## Matrix and dependencies

`tools/perf/qualification/far-budgets.json` defines default, mountains, cave-dense
and edited fixtures, each with Quality and Performance profiles on 3840x1600 and
3440x1440 displays. These 16 groups expand into 32 settled/traversal cells. Every
capture includes its cold/settling observations and measured window. The measured
window is either a settled view or the complete pinned traversal; cold observations
are never silently removed from the memory, work or first-fill report.

Fixture identities include the preset, referenced biome document, native preset
revision/identity, native world content identity, seed, pose, traversal digest and
dependency declarations. JSON input pins use sorted compact ASCII JSON plus one
newline; traversal pins use UTF-8 text with universal newline translation. The
runner snapshots traversal text before launch or inspection. Actual benchmark workload identities
must match the fixture, and profile, dimensions, warmup and frame counts are checked.
The complete matrix and its digest accompany the evidence index.

Only the existing default surface traversal has native preset/content identity
pins. Settled poses and the mountains/cave-dense native identities and traversals
remain unavailable. The existing edit traversal records dig/fill actions; it does
not prove a committed authority snapshot and all-tier bake. Until issue #132
supplies that evidence contract, every edited cell remains incomplete. Bake-generation
changes alone cannot qualify it. Missing inputs or dependencies produce incomplete
cells; malformed, escaping or identity-mismatched inputs are refused. No missing
measurement is represented by a measured zero.

## Running the harness

Run the planner/report mode without launching any client:

```sh
python3 -I -B tools/perf/run_far_budget.py --out-dir /evidence/far-plan
```

Use a new directory outside the source checkout. Existing directories are refused.
The command writes `review.md`, then `index.json` as the final publication marker.
An incomplete evidence index, including invalid source cells, returns exit 3.
Invalid invocation, matrix or fixture input and publication failures return exit 2;
a completely measured passing matrix returns exit 0. Because dependencies
remain unavailable, the committed matrix currently produces incomplete results.

On the qualification host, the same tool can capture ready cells:

```sh
python3 -I -B tools/perf/run_far_budget.py \
  --client /runtime/luminumbra_client --runtime-root /runtime \
  --build-manifest /evidence/performance-build.json --out-dir /evidence/far-capture
```

The runner reuses render process custody, isolated settings, sanitized environment,
explicit configuration override declarations, executable/build/configuration hashes,
retained capture hashes, process logs and deadline handling. The selected runtime's
pinned fixture documents must match before any capture starts. Blocked cells never
launch. Capture files, retained copies and receipts use the `cells/<cell-id>.json`,
`cells/<cell-id>.artifact.json` and `cells/<cell-id>.process.json` names.

Inspect that directory later, without launching anything:

```sh
python3 -I -B tools/perf/run_far_budget.py \
  --evidence-root /evidence/far-capture --out-dir /evidence/far-review
```

Source paths in the index are relative to its recorded evidence root. Keep original
capture paths available: custody binds the process command's output path as well
as capture and retained bytes. The reader uses bounded regular-file descriptors
with portable containment checks. Source captures, retained copies and receipts
are hashed from the bytes read for that inspection. Script clients are usable as
synthetic controls but cannot provide native build evidence.

## Review evidence

The review table lists every cell, its state, capture digest, measured p99, combined
resident peak and first-fill elapsed time. The machine-readable index retains
source identities, dependency reasons, render floor verdicts and phase-separated
V1–V5 CPU/GPU/combined resident peaks, sampled memory-accounting peaks, admission
refusals, work and draw distributions. Peaks are sampled observations; they do not
claim an independent allocator high-water measurement. Attempted/successful upload
bytes, attempts, dispatches, completions, rejections and evictions remain distinct.
Combined peaks sum tier residency on the same frame, not independent tier maxima.

Fill events retain cold, epoch, cohort, edit/bake and refill associations, per-tier
completion and elapsed time from both arming and explicit cohort acceptance.
Hitches use the existing 16.67 ms floor as a reporting boundary and join work/draw
observations using `timing_frame`, never the delayed result position or render-frame
index. The complete source capture remains the authority for these joins.

The reporter reuses existing render measurement validity and p99 floor checks.
Every measured far-mode row must request and use Ladder without submission refusal,
suppression or unavailable authority identity. Settled captures additionally retain
existing Ladder baseline, geometry, camera and queue-settling checks. Incomplete or
timed-out fills and Legacy fallback cannot produce complete measurements. Cold
render-wall distributions remain separate from the existing measured timing floor;
this harness does not issue a full performance qualification or acceptance receipt.

Candidate per-tier resident/build/upload/byte ceilings, global streaming caps and
separately measured reserve are explicit unavailable fields. No cap or reserve
number is selected or enforced here. Native baseline capture and owner review must
supply those values. Source implementation, native verification, merge and release
status must continue to be reported separately.

## Regression inventory

`FarBudgetPython` covers the complete matrix, identity and input refusals, sampled
statistics, originating-frame hitch joins, missing edit/bake evidence and Legacy
fallback. `FarBudgetRunnerPython` covers process custody, source completeness,
native-build refusal for synthetic clients, isolated invocation, deadline/spawn/exit
failures and final-index publication ordering. Both suites run without an engine.
The acceptance evidence inventory is
`tools/ci/acceptance/rows/render.far-budget-harness.json`.
