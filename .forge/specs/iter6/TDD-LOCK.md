# Iteration 6 TDD Acceptance Lock

## Purpose

This lock makes the iteration-6 execution rule explicit before any feature task is
implemented: every acceptance criterion must be captured by a failing BDD,
unit, integration, or gate-backed test first, then implemented to green, then
verified with the named gate.

## Scope

Applies to every spec and task produced under `.forge/specs/iter6` and the
iteration-6 completion workflow. It covers the remaining visual wave work,
worldgen and scale work, closeout work, and Steam or standalone multiplayer work.

## Locked Invariants

1. Each task starts by adding or updating the failing acceptance test that proves
   the target behavior.
2. Each acceptance criterion has an SDD trace link to one proving signal:
   `ctest`, an engine-frontier gate, or a BDD scenario.
3. Visual-debt items can close only on a passing `WorldVisualSweep` rerun.
4. Determinism-affecting simulation changes must be isolated to one intentional
   `world_hash` transition per commit and re-blessed with the heavy oracle,
   `LREC1` replay, and lockstep evidence.
5. Steam over-the-wire validation remains a second-machine requirement; local
   proof uses the standalone GNS UDP path.

## Acceptance Criteria

### AC-I6-TDD-LOCK-001 - Test-first execution

Given an iteration-6 task is selected for execution
When implementation work begins
Then a failing acceptance test or gate assertion is locked before production code
changes are made.

Proving signal: `test/features/TDD-LOCK.md`.

### AC-I6-TDD-LOCK-002 - SDD traceability

Given an iteration-6 acceptance criterion exists
When the task is planned or closed
Then it links to a concrete proving test or gate in the SDD trace.

Proving signal: `.forge/reports/iter6-completion/tdd-lock.md`.

### AC-I6-TDD-LOCK-003 - Visual debt gate discipline

Given a visual-debt item is marked complete
When the completion is reviewed
Then the completion evidence includes a passing `WorldVisualSweep` rerun.

Proving signal: `.forge/reports/iter6-completion/tdd-lock.md`.

### AC-I6-TDD-LOCK-004 - Determinism discipline

Given a task changes simulation or world generation behavior
When it is committed
Then any `world_hash` movement is deliberate, single-step, and re-blessed with
the required replay and lockstep evidence.

Proving signal: `.forge/reports/iter6-completion/tdd-lock.md`.

### AC-I6-TDD-LOCK-005 - Multiplayer validation split

Given Steam transport work is implemented on a single local PC
When validation is reported
Then local validation uses GNS UDP and Steam over-the-wire validation is marked
deferred until a second machine is available.

Proving signal: `test/features/TDD-LOCK.md`.
