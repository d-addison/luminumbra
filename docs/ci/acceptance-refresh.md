# Acceptance inventory refresh

`tools/ci/check_acceptance_matrix.py --refresh-inventory` relocates uniquely
matching complete contract statements. It preserves stable IDs, requirement
mappings, evidence, statuses and audited exclusions. Ambiguous, new, reworded
or removed statements require an explicit inventory decision before publication.

## Preparation custody

Issue #540 adds persistent bounded preparation records to journaled CLI writes.
The directory belongs to the developer or CI job. The refresh lock serializes
cooperating invocations; non-cooperating concurrent writers remain outside the
threat model. Verification before a named mutation is not identity-conditioned
unlink or replacement, on either Windows or POSIX.

Enrollment exclusively creates a preparation marker, obtains its native file
identity, and writes and syncs an authenticated enrollment record. A separate
caller receipt binds that identity, the canonical journal path, a fresh nonce
and the authentication key. The key is absent from recovery files. The receipt
is independent authority: a matching filename or self-described record alone
never authorizes reacquisition. Ordinary CLI writes emit the receipt to stderr
and flush it before staging payloads. Retain the run log outside the recovery
files. Successful automatic writes reset and retire their enrollment.

For reuse across several invocations, enroll explicitly and retain the returned
JSON in the caller's run metadata. For example, in one shell session:

```bash
custody="$(python3 -I -B tools/ci/check_acceptance_matrix.py --enroll-preparation)" || exit
python3 -I -B tools/ci/check_acceptance_matrix.py --preparation-custody "$custody" --refresh-inventory
python3 -I -B tools/ci/check_acceptance_matrix.py --preparation-custody "$custody" --check-doc docs/acceptance-matrix.md
python3 -I -B tools/ci/check_acceptance_matrix.py --preparation-custody "$custody" --retire-preparation
```

After an interrupted automatic write, pass the JSON from its `preparation
custody receipt:` diagnostic with `--preparation-custody`. The same operation
recovers before reading inputs; retirement can then remove the persistent
marker. Readers without the receipt refuse an existing preparation marker.
Receipt delivery is enrollment commit authority. A failed or lost delivery,
including termination before the receipt is emitted, leaves UNKNOWN enrollment;
there is no automatic adoption or filename-based reset of that state.

Before creating each staging file, the marker records its intention. After
exclusive creation, it records and syncs the native identity through the open
handle before writing payload bytes. Completion records bind the full digest.
The same protocol covers backup, publication, restoration and pending journal
files. A complete authenticated event ending in LF commits artifact enrollment; a torn
suffix cannot enroll a file. Termination between creation and committed identity
enrollment is uncertain: any occupant at that intention remains UNKNOWN and is
refused untouched. Missing intention files can be discarded during reset.

Restart verifies the receipt, marker identity, sole-link status, event chain and
the entire bounded set before cleanup. Enrolled staging identities grant content
reset authority over incomplete payloads, including empty files. Changed native
identities, extra links, unlisted pending occupants, unknown complete metadata,
authentication failures, unreadable files and exceeded bounds refuse untouched.
A torn final marker suffix can be discarded only after this validation. No
directory scan, age check or PID inference supplies ownership.

Only a closed pending file renamed to the journal commits transaction metadata.
Unpublished pending bytes are reset using enrolled identity. Published v1/v2
journals retain their existing formats, validation, rollback and cleanup rules.
After validating the complete journal and its destinations, recovery transfers
custody of its referenced artifacts to that journal and compacts the preparation
record before creating restoration candidates. Repeated recovery interruptions
therefore do not accumulate records. Failed cleanup retains discovery until a
later restart completes it. Reset retains the enrollment line and native marker
identity for reuse; retirement removes it only after recovery and reset.
When a legacy journal needs restoration without an existing receipt, recovery
automatically enrolls and emits a receipt before creating any new staging file.
Successful recovery retires that automatic enrollment; an interruption uses the
emitted receipt on restart.

The limits remain five outputs, 16 MiB per payload file and 64 KiB per metadata
file. At most fifteen payload artifacts, one pending pathname and 64 preparation
events including enrollment are accepted. JSON encodings are measured before
allocation, with at most 64 nesting levels. Reads allocate from verified size plus
one byte; event parsing and replay stay within these counts and byte limits.
Handles close before rename or unlink on Windows. Native Windows identities and
link counts come from handles, independently of directory-entry metadata and
short path spellings. Legacy direct helper calls and legacy markers without a
receipt retain their existing UNKNOWN refusals; completed v1/v2 journals remain
readable without a caller-supplied receipt.

## Contract partitions

Twenty-four disjoint selections include all matrix, recovery and custody cases.
Four further selections retain the public-tree sweeps. Each CTest registration
keeps its `hygiene;tools` labels and 30-second timeout. Direct Python invocation
and unittest discovery still run the complete suite. Registration and workflow
policy checks pin exact coverage; hosted sharding consumes the registered names
and labels. Local timings support partition balance; Windows qualification
requires hosted execution.

## Integration notes

The inventory refresh now includes receipt-backed restart recovery, bounded
reset and reuse, and explicit retirement for issue #540. Unknown occupants and
uncommitted enrollment remain protected. Recovery cases share the expanded
contract partitions, with unchanged deadlines and labels. Source checks cover
the implementation; repaired-head hosted Windows execution remains pending.
