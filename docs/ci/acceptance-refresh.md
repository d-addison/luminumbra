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

CLI enrollment first exclusively creates a bounded candidate beside the canonical
journal, obtains its native identity, and writes and syncs its authenticated
enrollment record. It then exclusively writes, syncs and closes a separate caller
receipt before atomically renaming the closed candidate to the preparation marker.
The receipt binds that identity, canonical journal path, nonce and authentication
key. The key is absent from recovery files. A matching filename or self-described
record alone never authorizes reacquisition.

Every new CLI enrollment requires `--preparation-receipt-file` naming a new file
in an existing caller-owned directory outside both the checkout and Git metadata.
An existing file is never overwritten. Retain this file in the caller's run
metadata. Ordinary writes also print the receipt to stderr before payload staging;
explicit enrollment prints it to stdout. Neither diagnostic is commit authority.
Successful automatic writes reset and retire their marker; the caller receipt
file remains available until the caller removes it.

For reuse across several invocations, enroll explicitly and retain the returned
JSON. For example, choose a new receipt pathname in an existing directory:

```bash
custody="$(python3 -I -B tools/ci/check_acceptance_matrix.py --enroll-preparation --preparation-receipt-file "$callerMetadata/enrollment.json")" || exit
python3 -I -B tools/ci/check_acceptance_matrix.py --preparation-custody "$custody" --refresh-inventory
python3 -I -B tools/ci/check_acceptance_matrix.py --preparation-custody "$custody" --check-doc docs/acceptance-matrix.md
python3 -I -B tools/ci/check_acceptance_matrix.py --preparation-custody "$custody" --retire-preparation
```

After interrupted delivery, read the durable caller file and pass its JSON with
`--preparation-custody`; `--retire-preparation` recovers and removes its marker.
Readers without the independent receipt still refuse the marker. A crash before
marker publication leaves no fixed preparation marker and cannot block another
enrollment. Retry the original command with a new caller receipt pathname.
An uncommitted `.acceptance-enrollment-*` candidate may remain in Git metadata;
it is inert, is never adopted or scanned for authority, and does not prevent the
bounded retry. Unreadable or partial receipt persistence never publishes a marker.

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

Every recovery copy, backup, staged candidate and metadata record must have
exactly one link before any recovery mutation. Cleanup and rollback validate
the complete retained set first, then recheck identity, content and sole-link
status immediately before removal, replacement or permission changes. An
extra link refuses the transaction with its files and alias permissions intact.

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
The invocation retains that automatic enrollment through validation and any
subsequent publication, creating the caller receipt file only once. Recovery-only
invocations and failures before publication retire the reset marker on exit;
successful publication does likewise. The caller receipt remains unchanged.
An interruption that retains recovery state uses the emitted receipt on restart.
After a validation failure retires the marker, retry with a fresh receipt pathname.

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

Sixty-four selections without child processes and 161 selections with child
processes include all matrix, recovery and custody cases. Each selection reserves
at most 24 slow-storage work units: one second for each child launch or outer
CLI/transaction operation, and at least one second per case. Actual outer calls,
including nested fault grids and setup helpers, are checked against fixed case
allowances. Child deadlines independently reserve at most 24 seconds per
selection. Six seconds remain inside the unchanged 30-second timeout for imports
and cleanup. Publication-boundary and native-creation sweeps are separate cases;
partition placement balances the complete reservation rather than case counts.
Four further selections retain the public-tree sweeps. Every CTest registration
keeps its `hygiene;tools` labels and 30-second timeout. Direct Python invocation
and unittest discovery still run the complete suite. Registration and workflow
policy checks pin exact coverage; hosted sharding consumes the registered names
and labels. Local timings support partition balance; Windows qualification
requires hosted execution.

## Integration notes

The inventory refresh now includes receipt-backed restart recovery, bounded
reset and reuse, and explicit retirement for issue #540. Unknown occupants and
uncommitted artifact enrollment remain protected. Recovery cases share the expanded
contract partitions, with unchanged deadlines and labels. Source checks cover
the implementation; repaired-head hosted Windows execution remains pending.
