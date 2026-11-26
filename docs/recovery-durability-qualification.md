# Recovery durability qualification

Native recovery qualification for issue #508 remains **PENDING**. This recipe
defines diagnostic evidence; it changes no save format, recovery protocol,
authority revision or enforced engine limits. Passing the synthetic
`RecoveryDurabilityEvidenceContract` checks the validator only.

## Scope and collection

Collect on the final repaired candidate, using three independent lanes:
`windows-msvc`, `windows-ucrt64` and `posix`. Record the full lowercase
40-character candidate commit and independently retain each lane's executable
SHA-256. Record compiler version, build configuration, OS version, filesystem
and mount options, virtual disk/controller and backing storage, guest and host
cache/writeback settings, and hypervisor version. Unknown configuration is
incomplete evidence; a storage or cache change requires a new bundle.

The approved interruption method is **abrupt VM reset**. Qualification applies
only to the recorded virtual storage stacks. It establishes no physical-host
power-loss claim. Injected failures and process termination are separate evidence
classes; neither substitutes for reset. Reset receipts need an external
controller's retained raw log, distinct guest boot identities, and confirmation
that no graceful guest shutdown occurred. Process termination retains the same
guest boot identity. The validator checks the declared bindings and content
digests, not producer authenticity or the truth of hardware observations;
maintainers must review controller logs and configuration independently.

Keep the bundle, receipts, raw logs, discovery, JUnit, worlds and generated
verdicts in a new directory outside **all** source checkouts. Keep the collector
and test writers stopped while validating. The validator reads bounded snapshots
through confined file handles and never launches an engine, modifies a world or
writes a report file. Supply additional `--source-root` arguments for other
checkouts. Contained path aliases use the shared Windows/POSIX fixture reader;
drive-relative references, escapes and special files are refused.

For each lane retain unedited CTest discovery from
`ctest --test-dir BUILD --show-only=json-v1` and JUnit from the corresponding
qualification execution with `--output-junit`. Retain the exact discovered test
commands and use a distinct registered testcase for each matrix cell. All
executed tests in that JUnit must pass, and must occur in discovery. Failed,
disabled, skipped, duplicate, missing or partially written results remain
incomplete, even if another attempt passed. Keep separate attempts in separate
bundles. Never manufacture traces from an expected sequence or turn a synthetic
receipt into native evidence by changing its label.

## Required matrix

Each row is required in all three lanes, for each of `injected_fault`,
`process_termination` and `abrupt_vm_reset`: 54 distinct cells.

| Case | Required observation |
| --- | --- |
| `matching_restore` | Matching index and payload bytes still complete durability on retry; index completion precedes payload work. |
| `absent_create` | Already-absent index and payload creates still complete absence durability on retry, index first. |
| `abort_previous_commit` | An interrupted abort retains owned recovery evidence and restores the prior committed member tree. |
| `abort_initial_creation` | An interrupted creation abort retains its artifact-registry evidence and completes removal of created members. |
| `cleanup_committed_journal` | Interrupted stale committed-journal cleanup retains sufficient evidence until completion, preserving committed members. |
| `cleanup_directory_retirement` | Interrupted recovery-directory retirement remains retryable and preserves committed members. |

Observe failures at the index completion boundary, or the cleanup completion
boundary for cleanup cases. Include namespace publication before unsuccessful
completion, matching-byte retry and already-absent retry in the corresponding
native exercises. The Windows observer must record completion of the approved
identity-bound recoverable namespace protocol with journal v4; flushing a
published file alone does not establish durable namespace removal. POSIX
completion includes all relevant destination, restoration and staging parents.
Completion events below describe the full platform contract, not just a return
from one flush call. Missing platform instrumentation or a refused native
operation leaves its cell unevaluated. This recipe does not implement those
native observers or claim their execution.

## Bundle contract

The new diagnostic schema is `luminumbra.recovery_durability_evidence.v1`.
The manifest filename is `recovery-evidence.json`. Its fields are:

| Field | Content |
| --- | --- |
| `schema`, `candidate` | Schema above; full candidate commit. |
| `provenance` | `synthetic` or `native`; applies to the whole bundle. |
| `lanes` | Unique lane objects with `lane`, `candidate`, `binary_sha256`, `compiler`, `configuration`, `environment`, `discovery`, `junit`, `runs`. |
| `environment` | Nonempty strings `os`, `filesystem`, `storage`, `cache`, `virtualization`, with the configuration details listed above. A VM scope is required. |
| `discovery`, `junit` | References to raw lane discovery JSON and JUnit XML. |
| `runs` | Unique cells with `id`, `case`, `mechanism`, `testcase`, integer `exit_code` zero, `before`, `interrupted`, `after`, `expected`, `trace`; external interruptions also have `interruption`. |

Every artifact reference has exactly `path` and lowercase `sha256`. Paths are
portable root-relative names, at most eight components and 256 characters.
Every run artifact has `schema` and an exact `identity` object containing
`candidate`, `lane`, `binary_sha256`, `run_id`, `case`, `mechanism`. The execution
identity also includes `context_sha256`: SHA-256 over UTF-8 JSON containing
`lane`, `candidate`, `binary_sha256`, `compiler`, `configuration`, `environment`,
with sorted keys, ASCII escaping and comma/colon separators without whitespace
(`json.dumps(context, sort_keys=True, ensure_ascii=True, separators=(",", ":"))`).
This binds every run receipt to its recorded build and storage/cache settings;
relabeling the manifest alone cannot reuse an earlier run. The execution
collector must bind the raw discovery and JUnit digests to that same build and
candidate when constructing the lane. SHA-256 binds retained bytes; it is not a
signature. The validator requires independent candidate/build arguments and
refuses mismatched identities.

The four member-tree artifacts use `luminumbra.recovery_member_tree.v1`,
`phase` (`before`, `interrupted`, `after` or `expected`), and `tree`. The latter
uses the existing `luminumbra.save_receipt.v1` encoding from
`tools/persistence/save_receipt.py`: strictly sorted unique `files` entries with
`path`, unsigned 64-bit `size`, lowercase `sha256`, and checked `file_count`,
`total_bytes`, `tree_digest`. The digest is SHA-256 of the concatenated UTF-8
strings `path + NUL + decimal size + NUL + sha256 + newline` in path order.
Capture all committed members and owned recovery artifacts, including creation
registry evidence. Do not exclude unexpected occupants or changed members.
Prepare the independent expected receipt before faulting the candidate.

The interrupted receipt is collected after the failure/interruption and before
retry. Every `recovery_paths` entry must be present there and absent after
completion. Matching restores require index/payload bytes equal to the committed
before receipt in the interrupted receipt. Absent-create retries require both
paths already absent there. Restores/aborts preserve the before tree exactly;
cleanup removes precisely the named recovery paths and preserves every other
member. The after receipt must match the independent expected receipt.

Trace artifacts use `luminumbra.recovery_trace.v1` with `events`, `index_path`,
`payload_path` and unique nonempty `recovery_paths`. Events are a minimal
normalized observer sequence; retain detailed native operation logs separately
for review. The exact first event is respectively
`matching_index_observed`, `absent_index_observed`,
`previous_commit_abort_started`, `initial_creation_abort_started`,
`committed_journal_cleanup_started` or `directory_retirement_started`.

For injected faults, follow it with `fault_injected`,
`index_completion_failed` (cleanup: `cleanup_completion_failed`),
`recovery_retained`, `retry_started`. For external interruptions, follow it with
`index_completion_pending` (cleanup: `cleanup_completion_pending`),
`process_terminated` or `vm_reset_observed`, `recovery_retained`, `retry_started`.
Then record `index_complete`, `payload_started`, `payload_complete` for
restore/abort cases, or `cleanup_complete` for cleanup. End with
`recovery_retired`. Unknown, missing, repeated or reordered events refuse the
cell; detailed syscall traces are not substituted for this normalized sequence.

External interruption artifacts use `luminumbra.recovery_interruption.v1`,
the run identity, `method` matching its mechanism, boolean `observed` true,
`controller`, `controller_log` (a raw artifact reference), `guest_boot_before`
and `guest_boot_after`. Reset additionally requires boolean `graceful_shutdown`
false and `virtual_storage_stack` exactly equal to the lane's `storage` string.
Injected faults have no external interruption artifact.

Parsing budgets are diagnostic only: 1 MiB per artifact, 64 MiB total, 512
artifacts, 4096 member records per tree, 256 trace events, 10000 tests per raw
discovery/JUnit and 80000 JUnit nodes. Duplicate JSON keys, nonfinite values,
DTD/entity declarations and unsupported schemas refuse evidence. Exceeding a
budget leaves qualification incomplete; it does not raise an engine limit.
Failed reads conservatively consume their full byte allowance; each bounded
read permits one lookahead byte solely to detect growth past that allowance.

## Evaluation

Run with independently retained executable hashes and a final candidate SHA;
replace capitalized values and `/path/to` locations with collected inputs:

```sh
python3 -I -B tools/persistence/recovery_durability_evidence.py /path/to/evidence \
  --expected-candidate FULL_CANDIDATE_SHA \
  --build windows-msvc=MSVC_EXECUTABLE_SHA256 \
  --build windows-ucrt64=UCRT64_EXECUTABLE_SHA256 \
  --build posix=POSIX_EXECUTABLE_SHA256 \
  --source-root /path/to/another/checkout
```

Stdout is deterministic JSON; redirect it only outside source checkouts, after
the evidence inputs are finalized. Exit zero means the diagnostic matrix is
complete; exit one means incomplete/refused. `native_declared` is true only
for a complete bundle declared native. The validator cannot authenticate the
producer, so `native_qualified` and `physical_power_loss_qualified` are always
false; native qualification needs owner review of the retained bundle.
`checks` lists every required cell; `artifacts` hashes every read input,
including the manifest. Input order does not affect evaluation order,
but changing raw bytes changes their recorded hashes. Retain failed bundles,
do not claim qualification from incomplete output, and do not publish actual
native results until the final repaired candidate has been exercised.
