# Iteration 6 Completion Plan

Sources:
- `.forge/specs/iter6/wave-b-gpu-grass-residual.md`
- `.forge/specs/iter6/wave-b-volumetric-clouds-tier-2.md`
- `.forge/specs/iter6/wave-b-aurora-curtains.md`
- `.forge/specs/iter6/wave-b-ocean-water-waves.md`
- `.forge/specs/iter6/wave-c-erosion-gaps-and-far-lod-meshing.md`
- `.forge/specs/iter6/wave-c-multi-anchor-streaming-and-server-scale.md`
- `.forge/specs/iter6/multiplayer-multi-client-accept-over-tcp-udp.md`
- `.forge/specs/iter6/multiplayer-runtime-join-leave-server-mode.md`
- `.forge/specs/iter6/multiplayer-client-render-remote-avatars.md`
- `.forge/specs/iter6/multiplayer-steam-p2p-lobby-and-sdr.md`
- `.forge/specs/iter6/wave-d-closeout.md`

The current closeout baseline from `remaining.md` is `world_hash=f17726d44054d133`.
This plan intentionally serializes the two ordered hash-affecting commits called
out by the determinism discipline: Aetheric first, erosion/worldgen second. Each
bump is its own local commit with its own old -> new hash record and re-bless
evidence. The two bumps must never be combined, squashed together, or hidden
inside render, networking, Steam, closeout, or gate-maintenance work.

## Non-Negotiable Execution Rules

1. Every task is test-first:
   - write or lock the failing BDD/acceptance tests first with `forge tdd lock`;
   - implement only enough to go green;
   - run the relevant engine-frontier gate plus `ctest`;
   - commit locally only, never push.
2. Every build, validator, and `ctest` run prepends MSYS2 UCRT to PATH:
   - PowerShell: `$env:PATH='C:\msys64\ucrt64\bin;' + $env:PATH`
3. One landed task equals one local commit.
4. Render-only work lands before network/server and sim/worldgen work.
5. Any sim or worldgen work that changes `world_hash` lands alone:
   - one deliberate bump per commit;
   - old hash and new hash recorded;
   - exactly one intended sub-hash term changes;
   - heavy oracle, LREC1 replay, lockstep, `HeadlessServerTick`, and relevant
     BDD acceptance evidence are re-blessed in that commit.
6. The ordered hash sequence for this plan is exactly:
   - Bump 1: Aetheric scalar field and Aetheric sub-hash re-bless;
   - Bump 2: erosion/worldgen terrain sub-hash re-bless;
   - Aetheric and erosion are never in the same commit.
7. Render-only and transport-only tasks must preserve `world_hash=f17726d44054d133`
   unless their task explicitly stops and is reclassified as a hash-bump task.
8. Visual-debt closure requires passing objective gates. Do not discharge visual
   debt by renaming, reclassifying, or weakening thresholds without a separately
   reviewed fixture-backed gate change.

## Stage 0 - Acceptance And Baseline Lock

Purpose: make the acceptance surface executable before implementation.

Tasks:
- Lock BDD/acceptance tests for every iter6 spec with `forge tdd lock`.
- Add or lock acceptance mappings for `WorldVisualSweep`, `RenderHealth`,
  `FoliageInstancing`, `FarLodHorizon`, `HeadlessServerTick`, `ReplicationSmoke`,
  `NetworkedReplication`, `NetworkStateHash`, `NetworkLoopbackAuthorityGate`,
  `ReplicationScale`, `NetworkedSession` successor/retirement, Steam/GNS optional
  lanes, and full non-placeholder `ctest`.
- Run baseline ctest/gates with the required PATH prefix and record which tests are
  expected to fail red before the implementation stage.

Commit envelope:
- Tests, BDD locks, and acceptance artifacts only.
- No product behavior changes.

## Stage 1 - Render-Only Work First

Purpose: finish visual work that must not affect simulation or `world_hash`.

Tasks:
- GPU grass residual:
  - curved blade/LOD/far texture-grass acceptance coverage;
  - `FoliageInstancing`, `WorldVisualSweep`, `FarLodHorizon` if horizon-visible,
    `RenderHealth`, release perf, and targeted `ctest`.
- Volumetric clouds, aurora curtains, and ocean/water waves:
  - these are currently done per `remaining.md`, so lock regression coverage and
    only repair if the locked acceptance tests fail;
  - rerun `WorldVisualSweep`, `RenderHealth`, release perf, and relevant shader/math
    `ctest`.
- Far-LOD horizon render repair:
  - close the live `mountains` sky-sliver failure without touching authoritative
    terrain/worldgen;
  - keep `HeadlessServerTick` at `f17726d44054d133`.

Commit envelope:
- Separate local commits for each scoped render feature or gate repair.
- No sim, terrain params, entity, network authority, or hash literal changes.

Exit gates:
- `WorldVisualSweep` green with zero objective/fidelity flags.
- `RenderHealth` green.
- `FoliageInstancing` green.
- `FarLodHorizon` green for all presets, including `mountains`.
- Relevant targeted `ctest` lanes green.
- `HeadlessServerTick` still reports `world_hash=f17726d44054d133`.

## Stage 2 - Aetheric Hash Bump / Re-Bless Before Erosion

Purpose: land Bump 1 as the first and only Aetheric hash-affecting commit before
any erosion/worldgen bump is allowed.

Tasks:
- Lock failing tests for the Aetheric scalar field, Aetheric sub-hash, origin-scroll
  and long-horizon replay, render coupling if touched, `HeadlessServerTick`, LREC1
  replay, lockstep, and heavy oracle.
- Implement or isolate only the Aetheric scalar-field surface needed for Bump 1:
  the sim-owned Aetheric state, deterministic update order, Aetheric sub-hash fold,
  and one-way render tap if required by the acceptance tests.
- Record pre-bump hash, post-bump hash, the Aetheric sub-hash delta, re-bless
  commands, and artifacts in the same local commit.

Commit envelope:
- One local commit containing only Aetheric code, tests, expected-hash updates,
  and re-bless evidence.
- No erosion, terrain/worldgen, SHIELD-RT parity, render cleanup, networking, Steam,
  or closeout-warning cleanup in this commit.
- If execution discovers Aetheric was already landed before this plan can own the
  bump, stop and revise the plan/dispatch; do not fold Aetheric evidence into the
  erosion or closeout commit.

Exit gates:
- Aetheric Bump 1 old -> new hash and sub-hash evidence is present.
- Erosion work is explicitly blocked until this checkpoint is green and committed.

## Stage 3 - Erosion / Worldgen Hash Work Alone

Purpose: land Bump 2 as the erosion/worldgen terrain hash-affecting commit, after
the Aetheric bump/re-bless and before SHIELD-RT/far-field parity baselines are
blessed.

Tasks:
- Implement only the hydraulic/thermal relief, erosion, terrain params, baked hydro
  offset, or authoritative terrain-source work needed for Bump 2 from
  `wave-c-erosion-gaps-and-far-lod-meshing.md`.
- Lock failing tests for:
  - hydro/erosion fixture determinism;
  - halo independence;
  - collision/spawn/water/waterfall/material/near mesh/far tile consistency;
  - old hash -> new hash with exactly one intended sub-hash movement;
  - LREC1 replay, lockstep, heavy oracle, and `HeadlessServerTick`;
  - `FarLodHorizon` and `WorldVisualSweep` after the shape change.
- If execution proves no erosion/worldgen bump is required, stop and revise the
  graph instead of fabricating a hash bump or silently converting Bump 2 into
  render-only work.

Commit envelope:
- Erosion/worldgen code, tests, hash update, and re-bless artifacts only.
- No Aetheric changes in this commit.
- No unrelated render, network, or Steam changes in this commit.

Exit gates:
- `HeadlessServerTick` is blessed at the new erosion-only post-bump hash.
- Old/new hash and intended terrain/worldgen sub-hash evidence is in the commit.
- `ctest -R "Hydraulic|Erosion|FarLod|Terrain|Headless|Replay|Lockstep"` is green.
- `FarLodHorizon` and `WorldVisualSweep` are green after the authoritative shape.

## Stage 4 - Multiplayer And Server Scale

Purpose: finish runtime networking/server work without perturbing the sim hash.

Tasks:
- Multi-client accept over TCP and optional GNS UDP:
  - N clients, stable client ids, bounded inbound pump, disconnect handling,
    per-client snapshots, and red-team admission rejection.
- Runtime join/leave wired into server mode:
  - late join baseline delivery, leave/despawn propagation, idempotent duplicate
    leave, survivor continuity, and server stays alive.
- Client render integration of remote avatars:
  - real server snapshots drive remote render entities, interpolation, local
    prediction/reconciliation, despawn cleanup, stale snapshot rejection, and
    render capture analysis.
- Multi-anchor streaming and server scale:
  - anchors follow all clients, per-anchor near floors, per-client AOI snapshots,
    bounded bandwidth/tick cost, churn without stale anchors, and visual gates
    remain green.
- Steam P2P/lobby/SDR:
  - optional Steam build, lobby create/list/join/invite/leave, metadata/admission
    validation, shared server seam, local GNS substitute proof, and a concrete
    deferred two-machine Steam validation artifact.

Commit envelope:
- Separate local commits for transport manager, join/leave, remote render,
  server-scale/AOI, and Steam/lobby work.
- No worldgen, Aetheric, or hash-literal movement.
- If avatar admission changes canonical entity state and requires a hash bump, stop
  and split that into a later explicit hash-bump task after erosion.

Exit gates:
- `ReplicationSmoke` green.
- `NetworkedReplication` N-client TCP green.
- Optional `NetworkedReplication --udp` green in a GNS-enabled build, or a clear
  build-disabled artifact.
- `ReplicationScale`, `ReplicationLifecycle`, `PlayerAvatar`, `MultiAnchorStreaming`,
  `NetworkStateHash`, and `NetworkLoopbackAuthorityGate` green.
- Remote-avatar render capture gate green.
- Steam optional build green; two-machine validation clearly deferred if not
  executable on this machine.
- `HeadlessServerTick` hash remains the latest blessed value.

## Stage 5 - Closeout Sweep

Purpose: prove iteration 6 is complete without hiding any debt.

Tasks:
- Run full non-placeholder `ctest`.
- Run `RenderHealth`, `WorldVisualSweep`, `FarLodHorizon`, `HeadlessServerTick`,
  `ReplicationSmoke`, `NetworkedReplication`, `NetworkStateHash`,
  `NetworkLoopbackAuthorityGate`, endurance/storm variants, release perf, and
  `forge verify --new-only`.
- Repair, replace, or retire stale `NetworkedSession` truthfully through a locked
  acceptance test and documented project decision.
- Update closeout artifacts and handoff with:
  - local commits;
  - gate evidence;
  - world_hash chain: baseline -> Aetheric bump -> erosion/worldgen bump;
  - deferred Steam two-machine validation dependency;
  - no new visual or verification blockers.

Commit envelope:
- Closeout evidence, gate fixes, stale-gate retirement/replacement, and handoff only.
- No new feature work unless a failing closeout acceptance test demands a narrowly
  scoped fix with its own tests and local commit.

Exit gates:
- Full non-placeholder `ctest` green.
- All named engine-frontier gates green or explicitly deferred by spec-approved
  hardware/account constraint.
- `forge verify --new-only --validation-policy warn --testing-policy warn` has no
  new blockers.
- Handoff records all evidence and any remaining deferred lanes precisely.

## Dispatch Graph

The executable task graph for this plan is:

` .forge/tasks/iter6-completion/dispatch.json `

It serializes stages as:

`BDD lock -> render-only tasks -> Aetheric bump/re-bless -> erosion/worldgen bump task -> multiplayer/server tasks -> closeout`

The graph intentionally prevents the Aetheric and erosion hash surfaces from landing
in one commit and repeats the required test-first, PATH, gate, `ctest`, local-commit,
and never-push instructions in every task prompt.
