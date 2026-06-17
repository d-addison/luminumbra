# Iteration 6 TDD Lock Report

Task: `T-I6-000-bdd-acceptance-lock`

## Status

The iteration-6 BDD acceptance lock is established.

Owned lock artifacts:

- `.forge/specs/iter6/TDD-LOCK.md`
- `test/features/TDD-LOCK.md`
- `.forge/reports/iter6-completion/tdd-lock.md`

## Inputs

- `.forge/workflows/iteration-6-completion.yaml`: present and used as the
  controlling workflow source.
- `.forge/specs/iter6`: absent at task start; created for the lock artifact.
- `test/features/TDD-LOCK.md`: absent at task start; created.
- `.forge/reports/iter6-completion/tdd-lock.md`: absent at task start; created.
- `remaining.md`: absent at task start.
- `.forge/reports/iter6-completion/critique-dispositions.md`: absent at task
  start.

## Locked Acceptance Rules

| Rule | Required evidence |
| --- | --- |
| Test-first implementation | A failing BDD, unit, integration, or gate assertion is created before production implementation for each task. |
| SDD traceability | Every acceptance criterion links to a concrete `ctest`, engine-frontier gate, or BDD scenario. |
| Visual-debt closeout | A visual-debt item closes only with a passing `WorldVisualSweep` rerun. |
| Determinism changes | Any simulation or worldgen `world_hash` movement is intentional, isolated to one commit, and re-blessed with heavy oracle, `LREC1`, and lockstep evidence. |
| Steam validation | Single-PC validation uses GNS UDP; Steam over-the-wire validation is deferred until a second machine is available. |

## Coverage Map

| AC ID | BDD lock scenario | Proving signal |
| --- | --- | --- |
| AC-I6-TDD-LOCK-001 | Test-first lock before implementation | `test/features/TDD-LOCK.md` |
| AC-I6-TDD-LOCK-002 | SDD trace entry for every acceptance criterion | `.forge/reports/iter6-completion/tdd-lock.md` |
| AC-I6-TDD-LOCK-003 | Visual debt requires WorldVisualSweep evidence | `.forge/reports/iter6-completion/tdd-lock.md` |
| AC-I6-TDD-LOCK-004 | World hash changes are deliberate and re-blessed | `.forge/reports/iter6-completion/tdd-lock.md` |
| AC-I6-TDD-LOCK-005 | Steam validation split is explicit | `test/features/TDD-LOCK.md` |

## Execution Rule For Follow-on Tasks

Follow-on iteration-6 tasks must update their BDD or acceptance proof first,
observe the locked rule while implementing, and report the passing gate before
closing. A task that cannot name its proving signal remains incomplete.
