# TDD Acceptance Lock

Feature: Iteration 6 acceptance stays test-first

  The iteration-6 workflow requires each implementation task to lock its BDD or
  acceptance proof before production code changes, then implement to green and
  report the named verification gate.

```gherkin
Scenario: Test-first lock before implementation
  Given an approved iteration-6 task has a named acceptance criterion
  When the task starts execution
  Then the task first adds or updates a failing BDD, unit, integration, or gate assertion
  And the task links that proof to the acceptance criterion before production code changes

Scenario: SDD trace entry for every acceptance criterion
  Given an iteration-6 spec defines acceptance criteria
  When the SDD trace is reviewed
  Then every acceptance criterion links to one concrete proving signal
  And each proving signal is a ctest, an engine-frontier gate, or a BDD scenario

Scenario: Visual debt requires WorldVisualSweep evidence
  Given a visual-debt task is ready to close
  When completion evidence is recorded
  Then the evidence includes a passing WorldVisualSweep rerun
  And visual debt is not closed by reclassification alone

Scenario: World hash changes are deliberate and re-blessed
  Given a task changes simulation, world generation, or replay-visible behavior
  When the task is committed
  Then exactly one intentional world_hash transition is documented for that commit
  And the transition is re-blessed with heavy oracle, LREC1 replay, and lockstep evidence

Scenario: Steam validation split is explicit on one local PC
  Given Steam transport work is implemented in a single-PC environment
  When local validation is reported
  Then standalone GNS UDP is used for local two-process validation
  And Steam over-the-wire validation is marked deferred until a second machine is available
```

## Locked Gates

| Work type | Minimum proving signal |
| --- | --- |
| Visual debt | `WorldVisualSweep` |
| Determinism or world generation | Heavy oracle, `LREC1` replay, and lockstep evidence |
| Replication and transport | `ReplicationSmoke`, `NetworkedReplication`, or `ReplicationScale` |
| Steam P2P over the wire | Second-machine validation; local substitute is GNS UDP |
| General acceptance criteria | Linked BDD scenario, `ctest`, or engine-frontier gate |
