# Installed asset build service

`luminumbra-author` runs an explicitly configured native `asset_processor` in a
separate process. It snapshots source files, validates compiled assets,
and publishes a generation only after the build succeeds. This optional Python
3.11+ tool has no Blender or game dependency and uses only the standard library.

The `glb-geometry-v1` profile produces flattened LMSH geometry or one-skin LMS2
geometry with LANM clips, according to the installed compiler's qualified profile.
The optional [static prefab profile](PREFABS.md) preserves hierarchy and instances
with material bindings and compiled textures. The optional
[compiled prefab runtime](../../../prefab_runtime/README.md) provides ECS
instantiation and an installed headless inspector for retained generations.
The service itself does not instantiate prefabs. Components, graphs, rendering
and engine preview frames remain unavailable; `capabilities` describes the
service's operations separately from the optional runtime consumer.
Compiler fidelity must be qualified with the exact installed executable: pinning
a hash does not establish that an older compiler supports a new asset feature.

## Package and configure

Build an executable Python archive into an isolated installation:

```sh
python tools/blender/authoring/service/package.py /your/install/luminumbra-author
```

Use `python /your/install/luminumbra-author` on Windows. The archive runs without
the source checkout. Install the native asset compiler and its required engine
libraries separately; this package never rebuilds them or downloads tools.

Place a toolchain manifest beside the installed compiler. Paths are relative to
the manifest, and every declared file needs its actual lowercase SHA-256 digest:

```json
{
  "schema": "luminumbra.authoring.toolchain.v1",
  "id": "project.qualified_compiler",
  "processor": "asset_processor",
  "files": {"asset_processor": "<actual SHA-256>"}
}
```

Use `asset_processor.exe` on Windows and include the required installed libraries
in `files`. The caller chooses this trusted configuration at service launch;
build requests cannot supply a command, script or arbitrary compiler arguments.
The service hashes these members before and after compilation. Qualify the host's
system runtime separately. Different host/compiler profiles need separate evidence.

## Author and build a snapshot

An asset sidecar selects the geometry profile and records its exporter identity:

```json
{
  "schema": "luminumbra.authoring.asset.v1",
  "asset_id": "project.prop",
  "revision": 1,
  "profile": "glb-geometry-v1",
  "source": "exports/prop.glb",
  "dependencies": ["source/prop.blend"],
  "exporter": {"id": "blender.gltf.qualified", "sha256": "<actual exporter tree SHA-256>"},
  "settings": {"emit_lods": false}
}
```

All files must already exist inside the project, without symlinks or path aliases.
The GLB must embed its buffers and images. `.blend` dependencies contribute bytes
and hashes only; the service never opens them in Blender or executes their scripts.
The exporter identity is a declaration from the snapshot producer, recorded in
build identity. Qualification of that exporter remains a separate fixture lane.

Geometry sidecars may add optional `settings.max_tris`: an integer from 64 through
1,000,000, excluding booleans. The service passes `--max-tris N` to the compiler
and independently validates the actual primary mesh triangle count before
publication. Simplification can stop above its target; such a build fails with
`output.budget_exceeded`. When `max_tris` is supplied, every emitted LOD must have
no more triangles than the primary mesh; excess fails with `output.lod_exceeded`.
These failures remove unpublished compiler output staging while retaining the
failure receipt, input evidence, last valid generation and its current pointer.
`emit_lods` remains independent.
Static prefab sidecars refuse `max_tris` with `prefab.max_tris` until an
aggregate/per-part budget contract is defined.

Clients must first request `capabilities` and require
`features.max_tris_setting` to be `true` before submitting a budgeted build.
An absent field means unsupported: refuse the build instead of omitting its cap.
This feature describes service enforcement; qualify the installed compiler's
option support separately. Budgeted job receipts record the supplied `settings`,
including `max_tris`, for successful and postcompile failed builds. The hashed
sidecar already binds the cap to build identity. Documents without `max_tris`
keep the existing compiler arguments, output bytes and receipt shape; no default
cap is inserted.

Snapshot producers may supply `expected_dependency_hashes`, an object mapping
declared dependency paths to their captured SHA-256 digests. Extraction refuses
a mismatch before running the compiler. The Blender adapter uses this guard to
prevent an edit made before service extraction from publishing an older export.
Dependencies are checked again before publication. These producer guards are
optional for ordinary immutable CLI inputs.

`required_schemas` may contain only `luminumbra.authoring.asset.v1`. Unknown fields,
required extensions and engine metadata are refused. The only engine extras
accepted in GLB are string correlation IDs named `luminumbra.object_id`,
`luminumbra.asset_id` and `luminumbra.material_id`. Optional sidecar `annotations`
are inert JSON objects. This profile does not interpret components or behaviors.

```sh
python /your/install/luminumbra-author --project /your/project --toolchain /your/install/toolchain.json validate asset.json
python /your/install/luminumbra-author --project /your/project --toolchain /your/install/toolchain.json build asset.json --revision 1
python /your/install/luminumbra-author --project /your/project --toolchain /your/install/toolchain.json generation.inspect
```

`validate` checks the sidecar and GLB containment. `build` waits for native geometry
validation, compilation, output readback and publication; it exits nonzero on
failure, cancellation or a stale revision. Diagnostics contain rule IDs and field
locations. Native compiler diagnostics appear in a bounded receipt log. Ctrl-C
cancels a standalone build. Resource counters report compiled bytes, vertices,
triangles, joints and clip tracks, with no GPU-memory or rendering claims.

## Session RPC and lifecycle

Start `serve` with `LUMINUMBRA_AUTHOR_TOKEN` set to an unpredictable token of at
least 32 characters. Its inherited stdin/stdout pipes carry newline-delimited
JSON. There is no socket or network listener. Every request includes the token:

```json
{"id":1,"token":"<session token>","op":"build","params":{"source":"asset.json","revision":1}}
```

Responses echo the integer `id` and contain either `ok: true, result: ...` or
`ok: false, findings: [...]`. Supported operations are `capabilities`, `registry`,
`validate`, `build`, `job.status`, `job.cancel` and `generation.inspect`.
Build submission returns a job ID; compilation advances independently of polling.
Status/cancel take `job_id`; generation inspection takes an optional job ID and
otherwise returns the current generation. Cancellation becomes terminal after
the owned worker stops. A request arriving after publication observes success.
Use these RPC operations within the same session to inspect or cancel live jobs;
independent CLI invocations cannot take over another session's project lock.

One service owns a project. Linux uses `flock`; Windows uses a nonblocking
exclusive byte-range lock on `.luminumbra-author/session.lock`. The Windows lock
coordinates logon sessions through the same file and uses the project's file
access controls. Any thread can close it; release failures are reported and can
be retried. Filesystems that cannot provide byte-range locks refuse acquisition.
Windows also retains the legacy session-local mutex name while holding the file
lock, excluding older services during an upgrade. This mutex is an unowned
existence marker, so closing it does not depend on the acquiring thread.
Simultaneous Windows/Linux access is not qualified. Use a canonical project path
and trusted local collaborators.
Source writers should publish sidecars atomically and advance revisions; arbitrary
concurrent hostile filesystem mutation is outside this profile.

The compiler supervisor holds an inherited ownership pipe. Closing the session,
cancelling a job or losing the service process closes that pipe and stops the
compiler. The default compiler timeout is 120 seconds, configurable up to one
hour. The registered compiler is a single native process; arbitrary process trees
are not a supported toolchain on Windows. Logs are drained and retain at most
64 KiB; requests are at most 64 KiB, snapshots and output sets at most 256 MiB,
and the project retains at most 256 jobs. Size limits are checked during
extraction and output validation, not an operating-system disk quota.

## Publication and recovery

Private inputs, outputs and receipts live under `.luminumbra-author/jobs/`.
Output readback checks exact layouts/counts, finite attributes, mesh indices,
joint order, influence budgets and clip target/interpolation contracts. A completed
generation moves into `.luminumbra-author/generations/`. The service rechecks the
source hashes before atomically replacing `current.json`; failed, cancelled and
stale work leaves the previous pointer intact.

The service and optional viewport package share one standard-library file I/O
implementation. JSON publication flushes and fsyncs a complete temporary file
in the destination directory before replacement. Shared readers keep the old
complete file; subsequent opens see the replacement. Windows uses explicit
delete-sharing readers and `FileRenameInfoEx` POSIX replacement semantics.
Unsupported filesystems and non-sharing readers fail without truncating or
removing the current pointer. The shared helper contributes to service build
identity and is copied byte-for-byte into the viewport extension archive.
The generation reader compares native Windows file identifiers, sizes, write
times and metadata change times consistently across path and held-file checks.
Creation timestamps cannot substitute for change timestamps. Replacement and
metadata changes during a read are refused even when file size and bytes match.

Consumers retain a generation ID and read its manifest instead of following the
current pointer on every access. Published generations are never modified or
automatically deleted. Garbage collection and power-loss durability are future
work; an atomic pointer does not imply a durable directory fsync on every host.
Startup marks interrupted jobs failed, except when their generation pointer
already committed. That case recovers as success after hash verification.
Recovery runs only after acquiring the project lock. Receiptless directories and
interrupted jobs move intact to timestamped directories under
`.luminumbra-author/quarantine/`; interrupted jobs retain a failed receipt under
`jobs/` for status queries. Quarantine preserves all files and is never deleted
automatically. Before moving an interrupted job, recovery records an intent and
replays it on startup until the failed status receipt is saved. Process exits and
write failures during quarantine therefore retain the job for status queries.
On Windows, recovery retries directory access/sharing violations while the prior
compiler supervisor releases its working directory. All startup moves share a
ten-second retry deadline; expiry preserves the source and pending intent for
the next startup.
Completed jobs and committed generations remain in place.

Receipts record the host/Python profile, source/dependency hashes, exporter metadata through the hashed
sidecar, service-code hashes, compiler/library identities, settings, outputs and
timing. This timing ends at publication, not at a visible Blender viewport frame.
The receipt and generation schemas remain tooling contracts; no saved-world or
network state format changes are introduced by this service.

## Meshgen configuration and game script pins

The source invocation of the read-only configuration check uses the trusted
launcher with isolated, no-site, no-bytecode Python startup. Supply an absolute
path to the reviewed service launcher, outside the game/project:

```sh
python3 -I -S -B /your/engine/tools/blender/authoring/service/launch.py --project /your/game --toolchain /your/install/toolchain.json meshgen check-config --profile rock --print-effective
```

In PowerShell, use `python -I -S -B C:\your\engine\tools\blender\authoring\service\launch.py`
with the same arguments and absolute Windows paths. `-I` ignores `PYTHONPATH`
and `PYTHONDONTWRITEBYTECODE`, so `-B` is required even if that environment
variable is set. `-S` prevents startup `.pth`, `sitecustomize` and `usercustomize`
processing. Before importing the service, the launcher explicitly sets
`sys.path` to the service package root and interpreter stdlib locations only;
cwd and the game/project root are excluded. For the installed zip application,
use `python -I -S -B /your/install/luminumbra-author.pyz` with the same check-config
arguments; the archive is its package root. The supported launch creates no bytecode, state,
session lock, backend connection, secret-value read or Blender process. The
required toolchain argument retains the existing CLI shape; this planning check
does not verify executables. Live backend validation is reported as unavailable.

Toolchain manifests may include `scripts`, a role mapping. Each role declares a
project-relative `.py` entry `path`, its `sha256`, and a `modules` mapping from
companion `.py` paths (including regular package initializers) to SHA-256 values.
Requests cannot choose scripts. Old compiler-only toolchains remain valid;
MG2 script use requires pins, and clients must negotiate `meshgen_script_pins`.

`Toolchain.verify_scripts()` rechecks all pins and provides conservative AST
diagnostics. Its import path is the entry directory followed by companion roots
derived from the pinned files, above each outermost pinned regular package.
Optional `import_roots` lists additional canonical project-relative directories
containing pinned companions (`"."` means the project root); namespace packages
may need this explicit root. Every transitive import uses that same path.
Symlinks, junctions and project shadows of stdlib or Blender module names are
refused, even when the shadow has a pin. Assignment aliases of `__import__`,
importlib spec/loader APIs and `exec`/`eval`/`compile` are diagnosed as unsupported.
AST diagnostics alone do not establish a complete inventory.

Execution must use the trusted launcher in a fresh interpreter:

```sh
python3 -I -S -B /your/engine/tools/blender/authoring/service/launch.py --scripts --project /your/game --toolchain /your/install/toolchain.json --role finisher
```

Hosts calling `meshgen_scripts.run_pinned_script()` through another entry point
must establish the same startup contract before importing the service. The
guarded path returns a structured `toolchain.bootstrap` refusal unless Python
has isolated/no-site/no-bytecode flags and only service/stdlib paths on `sys.path`,
with no project path. Removing unsafe paths after ambient startup is too late.

The launcher installs an import finder and a permanent audit hook before adding
the controlled companion import roots. Each project module
is read through containment checks, hashed, and compiled from those exact bytes;
cached bytecode is never consumed. Project module origins and search locations
must use a source loader and a pinned source file with a matching digest.
Sourceless, native and other non-source project loaders are refused regardless
of suffix, suffix case or embedded code filename. Directly obtained source
loaders use the same verified snapshots. Unpinned or changed files are refused
at load time. Project `.pth` reads and `site.addsitedir`/`site.addpackage` calls
targeting the project or its descendants are refused before processing.
Execution provenance walks the whole frame stack, so stdlib helpers cannot
launder unpinned source or synthetic-filename `exec` from project callers.
Actual stdlib module imports and their initialization remain supported. Code
already imported from the project is refused before launch. Interpreter and
engine installations remain trusted; native Blender integration is E4 work.

This guard enforces source integrity for trusted, reviewed scripts. It is not an
OS sandbox against malicious pinned Python that changes interpreter internals,
uses native code or deliberately delegates arbitrary execution to trusted
external libraries. The E4 supervisor must use this launcher contract together
with its process/resource envelope; E1 does not wire up or qualify Blender
execution. Provisional profile budgets remain uncalibrated.

## Verification

```sh
python -B -m unittest discover -s tools/blender/authoring/service/tests -v
python -B -m unittest discover -s test/viewport -p test_shared_file_io.py -v
python tools/blender/authoring/service/tests/native_acceptance.py --service /your/install/luminumbra-author --toolchain /your/install/toolchain.json --output /fresh/evidence/directory --exporter-id blender.gltf.qualified --exporter-sha256 ACTUAL_EXPORTER_SHA256 prop.glb plant.glb character.glb
```

Unit tests inject compiler failures and publication races, cover triangle-cap
bounds and negotiation, read actual fixture mesh counts, and verify that budget
and LOD refusals leave the previous publication intact. The separate native
runner requires a real installed compiler and caller-provided GLBs, builds each
twice, compares output hashes, retains old generations, exercises refusals and
checks unchanged toolchain hashes. Neither lane closes renderer, physical
animation or newly authored production-content acceptance.

Windows sharing qualification uses
`python -B test/viewport/probe_windows_file_io.py --receipt /fresh/receipt.json`.
Its nineteen contracts include an independently packaged service subprocess
publishing `current.json` while the packaged generation reader holds the old
file, a real non-sharing CRT-reader negative control, and unchanged input pins.
The MG2 ledger regression holds the same reader used by `status()` across
`update()` and terminal publication, including owner removal. It runs without
mocks on POSIX and Windows in the `Authoring mock / windows-2025` CI lane;
Windows publication requests a handle flush while POSIX fsyncs the directory.
It does not qualify the native renderer or Blender adapter.

## MG2 E3: licence admission and the synthetic backend

The installed service includes the stdlib-only `meshgen_licence`,
`meshgen_backend`, and `meshgen_jobs` APIs. Existing build commands and the
side-effect-free `meshgen check-config` CLI retain their behavior. The existing
capability response advertises `meshgen_backend_broker=true` and
`meshgen_real_backends=false`. These describe reusable engine APIs; the full
`meshgen.*` Banso RPC contract is still a P3 deliverable.

`meshgen_licence.gate(card, policy, notes)` is pure. `notes` contains the exact
committed bytes of `authoring/meshgen/notes/<id>.md`; the parent reads them with
containment checks and the gate verifies their hashes. Severity is
`blocked > internal_only > counsel > internal_ok > shippable`, with every
applicable reason retained. Restricted component licences need acknowledgements
bound to their ID and licence-text hash. Prerequisites for revenue,
no-improvement, and registration remain independent; neither acknowledgements
nor allowed statuses substitute for verified notes. Revenue-cap licences also
require registration. Counsel notes authorize only their recorded internal
outcome. NC lineage is never runnable, including NC dataset licence records marked
`executed=false`; dataset lineage is evaluated independently of runtime
component execution. Dataset audit
waivers are backend-specific committed records.

The synthetic mock card is pinned in engine code. It has no weights or training
dataset and can pass the default licence policy without acknowledgements or
attestations. The trusted parent separately sets candidate eligibility to
`shippable=false, origin=backend:mock`. A changed card or another backend claiming
to be the mock is refused. Mock GPU fields are unavailable (`null`). No model
adapter is imported or spawned, and enabled real backends are refused before
any backend contact.

`BackendClient` uses direct stdlib HTTP to literal `127.0.0.1` with a client bind
to `127.0.0.1:0` and an actual peer check. It ignores proxy environment settings,
refuses redirects, and requires exact Content-Length framing without transfer
encoding. Header bytes, JSON bytes/depth/collections, result streaming, and
absolute deadlines are bounded. Every header/body send and receive reapplies the
remaining absolute deadline; upload chunks are at most 64 KiB. The five endpoints are capabilities, submit,
status, cancel and binary result. A successful status carries the validated
result metadata (`sha256`, `bytes`, bounded `backend_reported`, `logs_tail`), and
`GET /v1/jobs/<id>/result` returns `model/gltf-binary` bytes. Job IDs are validated
before URL construction. Tokens occur only in Authorization headers.

The planning request stays path-and-digest based. Upload JSON adds
`declared_source` and `image_b64`; its allowance is calculated from the decoded
16 MiB cap. The parent snapshots and hashes the same bytes before upload and
retains them for E4. PNG/JPEG grammar, dimensions, pixel expansion and metadata
are bounded and checked through the final end marker. PNG intake supports
noninterlaced 8-bit grayscale/RGB/grayscale-alpha/RGBA, a complete zlib pixel
stream, and the explicitly supported chunk subset (IHDR, optional truecolor
PLTE, contiguous IDAT, tEXt, IEND). JPEG intake supports 8-bit sequential Huffman
frames with complete coefficient scans, valid restart sequences, fixed JFIF
metadata and plain comments. Progressive/arithmetic JPEGs, EXIF/vendor metadata,
indexed/interlaced PNGs and unrecognized chunks are refused. Every JPEG metadata
segment is inspected, including between later scans. Uncompressed PNG tEXt CAWG
JSON assertions can be inspected; reserved/constrained assertions are refused.
C2PA/JUMBF, compressed text and XMP credential containers are unsupported and
refused. Inspected assertions are not authenticated
signatures. Missing credentials are recorded as absent, and `refuse_unknown`
refuses that absence. P1 has no image-waiver escape hatch.

Use `BackendBroker.plan(request)`, `start(plan_id, plan_digest)`, and the ledger's
`status(job_id)`/`result(job_id)` APIs. Requests still use the E1 backend grammar
and explicit target-height overrides. The broker runs one owned background
task, with 1–4 sequential raw candidates and recorded seeds, manual selection,
and bounded polling. Plans bind request, resolved config, complete capability,
recipe and gate digests. At start the parent rechecks config, image bytes,
capabilities and licence-note evidence. Scene revision changes do not cancel
inference; document-session or project changes prevent staging. Automatic
scoring is refused without measured evidence.

The project lock is `.luminumbra-author-recipes/.meshgen.lock`, atomically
published with complete PID, process creation identity, nonce, job ID and wall
start time bytes under the coordination guard.
A short OS coordination guard serializes acquisition/recovery; a second owner
receives `busy` without queueing or reattachment. When both MG2 and build locks
are needed, acquire MG2 first, then the existing build lock. Shared Windows/WSL
directory coordination remains unqualified. Read-only construction and terminal
queries create no state and do not instantiate BuildService.

Mutable records live at `<state>/<document>/jobs/<job>/running.json` and
`terminal.json`, with `terminal.sha256` published as a separate commit. Durable
stage-specific request reservations live in the sibling `requests/` directory;
identical replay returns the original terminal bytes, conflicts are refused,
and active duplicates do not submit twice. The project-wide ledger counts records across all documents, including orphan
job directories and incomplete reservations. It admits at most 256 distinct
records (or the tightened `terminal_requests` limit) and 4 GiB of state bytes;
new jobs reserve their worst-case retained input allowance before upload.
Atomic publication bytes are included in the byte check. New work is refused
when full; replay evidence is never evicted. Startup recovery validates markers,
reconstructs missing reservations, reconciles malformed/empty owner files, and
detects dead/reused PIDs. An orphan becomes terminal only after backend stop is
confirmed. Failed confirmation keeps a recoverable `blocked` marker and the
project lock. Recovery can retry a drained blocked job even while its original
PID remains alive. Interrupted failure/cancel publication
preserves its already hashed terminal result. The deterministic mock job ID,
backend ID, endpoint and resolved config digest are persisted before submission.
Cancellation and recovery verify that endpoint and digest; configuration drift
keeps ownership blocked with `backend.pin_mismatch` until the original
configuration is restored. Request overrides are retained for digest comparison.

Ledger publication flushes file bytes before publishing names. POSIX operations
fsync the containing directory after replacement, directory creation and lock
removal. Windows uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING |
MOVEFILE_WRITE_THROUGH` for publication and directory creation; lock removal
first durably renames the authoritative name to an ignored tombstone.

EOF or shutdown calls `BackendBroker.on_eof()`/`close()`, which requests
cancellation, drains the owned executor task, confirms backend cancellation,
then discards scratch staging and publishes the terminal result. Failure uses
the same stopped/drained transition. Unconfirmed cancellation preserves
ownership in `blocked` for retry or recovery. Admission and executor submission
share the shutdown mutex. Admission owns its marker before publication and rolls
back marker, reservation and owner writes on failure. If rollback fails, a
drained blocked marker permits recovery while the original PID remains alive.
A rejected executor submission publishes a terminal failure and removes its lock.
The inherited-pipe `serve()` wrapper invokes an owner's EOF
hook on every exit. Cancellation arriving before submission returns also
cancels the late backend ID. `RequestLedger.commit_success(job_id, trusted_commit)`
serializes the future candidate commit with cancellation; a committed terminal
result wins subsequent cancellation.

E3 stops at `finishing` after validating raw static triangle accessor bounds,
indices, finite coordinates, aggregate accessor/scene expansion, pre-finish
count and single-file containment. Only POSITION attributes are supported;
NORMAL, TEXCOORD, skinning/custom attributes, morphs and extensions are refused
before an importer could consume unchecked accessors.
GLB header, declared length and JSON/BIN chunk headers are checked incrementally
before payload collection or the shared format validator. JPEG restart storage
is bounded by both the expected MCU extent and collection limits before allocation;
truncated marker-fill sequences return `request.invalid`. PNG inflate uses
compressed slices and output pieces bounded to 64 KiB, bounding tail copies.
Exact uploaded bytes, raw GLB, capability/result metadata and bounded logs are
in private job scratch `inputs/` (later candidates use `raw-<index>/inputs/`).
Decoded capability/status/result observations, decoded GLB JSON, raw retained
bytes and refusal findings are inspected for the bearer token before
persistence, including tokens containing JSON quote/backslash escapes. Ledger,
owner, marker and reservation writes check both decoded values and serialized
bytes at the persistence boundary. Credentials are resolved again before
admission and every write; token rotation does not permit a previously planned
request ID or document ID to leak the current token.
They are not sealed candidates. E4 owns finisher supervision, authoritative
provenance, prepared candidate receipts, seal verification and committed-success
recovery. E3 creates no candidate marker and grants no import/promotion rights.

The mock launcher is `tools/blender/authoring/probes/mock_backend.py`; set
`LUMINUMBRA_GEN_TOKEN` and run it with `--bind 127.0.0.1 --port 0`. It also permits
an explicit `::1` test bind; the production client remains IPv4-only as required
by Q20. Fault modes are launcher-controlled, never request-selected: oversize,
nonfinite, external_uri, redirect, nonloopback, slow, cancel, crash, malformed
job IDs/JSON/framing, card changes and result-hash mismatch. Cube and subdivision
zero icosphere GLBs are generated deterministically from source. The mock is
excluded from the installed runtime package. Its own retention is bounded to
256 replay records and 64 MiB; it refuses new submissions when full while
preserving identical replay and retained results.

Both Linux and Windows workflow lanes already discover `service/tests/test_*.py`,
including the E3 test modules. Default tests use only owned port-0
loopback listeners. For explicitly network-disabled developer environments,
`LUMINUMBRA_TEST_NO_SOCKETS=1` selects an in-memory HTTP wire harness that runs the
same handler and stdlib response parser. It cannot qualify real socket binding,
peer behavior or timeout enforcement; CI retains the default socket lane.
Portable evidence does not qualify Blender operations or the Windows 5.1.0
native acceptance lane.

Binding Q&A coverage (test method names are exact; all files are under
`tools/blender/authoring/service/tests/`):

| Q | Tests / E3 scope |
|---|---|
| 1, 3, 13, 27 | `test_real_backend_and_later_phase_requests_are_disabled`; `test_package_ships_broker_modules_and_excludes_mock_and_adapters` |
| 15 | `test_nc_dataset_executed_false_always_blocks`; `test_all_precedence_combinations_retain_reasons`; `test_audited_unaudited_and_backend_specific_waiver`; `test_missing_conflicting_and_unexecuted_inventory`; `test_nc_component_or_dataset_cannot_be_waived` |
| 16 | `test_acknowledgements_bind_id_text_hash_and_scope`; `test_verified_and_false_attestations_never_bypass_prerequisites`; `test_revenue_clause_requires_registration_independently`; `test_counsel_notes_verify_bytes_and_never_upgrade_shipping`; `test_note_bytes_are_read_without_fabricating_evidence` |
| 17 | `test_complete_card_changes_and_model_pins`; `test_capability_change_refuses_before_submission` |
| 18 | `test_mock_permission_and_forced_nonshipping_are_distinct`; `test_mock_raw_ready_replay_retained_bytes_and_eof` |
| 19 | `test_image_snapshot_digest_dimensions_metadata_and_rights`; `test_complete_jpeg_scans_and_metadata_after_later_scan`; `test_png_requires_complete_bounded_pixel_stream_and_grammar` |
| 20 | `test_five_endpoints_and_deterministic_shapes`; `test_authentication_and_host_check`; `test_literal_loopback_bind_and_no_proxy_routing`; `test_redirect_nonloopback_and_strict_framing_modes`; `test_runtime_peer_and_absolute_deadline_use_bounded_reads`; `test_absolute_deadline_also_bounds_drip_fed_headers`; `test_every_send_uses_remaining_deadline_and_bounded_chunks`; `test_http_headers_and_upload_share_the_absolute_deadline`; `test_every_receive_reapplies_remaining_timeout_and_rejects_late_bytes`; `test_all_mock_modes_have_explicit_portable_evidence` |
| 21 | `test_exact_wire_grammar_and_base64_allowance`; `test_oversize_malformed_job_ids_and_geometry_refusals`; `test_mock_raw_ready_replay_retained_bytes_and_eof` |
| 22, 23 (raw intake) | `test_bounded_json_duplicate_nonfinite_depth_and_collections`; `test_image_snapshot_digest_dimensions_metadata_and_rights`; `test_oversize_malformed_job_ids_and_geometry_refusals`; `test_raw_accessor_bounds_encodings_indices_and_scene_expansion`; `test_unconsumed_glb_attributes_refused_and_aggregate_expansion_bounded`; `test_five_endpoints_and_deterministic_shapes`. Exported artifact inspection remains E4/G2. |
| 24 | `test_failure_and_cancellation_block_until_confirmed_backend_stop`; `test_real_mock_slow_mode_reaches_deadline_and_confirms_cancellation`; `test_executor_submission_failure_is_terminal_and_close_serializes_admission`; `test_terminal_cannot_release_undrained_backend`; `test_injectable_clock_deadline_cancels_slow_backend`; `test_eof_during_submission_cancels_late_backend_id`; `test_mock_raw_ready_replay_retained_bytes_and_eof`. Child process supervision belongs to E4. |
| 25 | `test_read_only_access_has_no_state_or_build_lock`; `test_project_wide_lock_contention_across_documents`; `test_dead_or_reused_pid_recovery_cancels_backend_and_fails_marker`; `test_orphan_marker_without_lock_is_reconciled`; `test_active_duplicate_from_another_owner_cannot_reattach`; `test_atomic_owner_publication_and_malformed_owner_reconciliation`; `test_invalid_markers_cannot_reconcile_or_release_a_lock`; `test_unconfirmed_orphan_keeps_ownership_until_recovery_confirms_stop`; `test_separate_process_lock_contention`; `test_separate_process_crash_at_each_reservation_publication` |
| 26 | `test_reservation_reconstruction_conflicts_orphans_and_project_quotas`; `test_orphan_directory_and_retained_bytes_count_toward_admission`; `test_mock_record_and_byte_quotas_preserve_replay`; `test_stage_specific_replay_conflict_active_duplicate_and_quota`; `test_scene_revision_does_not_cancel_but_session_change_prevents_staging`; `test_cancel_before_commit_and_cancel_after_committed_success`; `test_cancellation_and_candidate_commit_are_one_serialized_transition` |
| 38 (backend sequence) | `test_sequential_four_candidates_retain_each_seed_without_overflow`. Native previews and import selection remain G2/G3. |
| 40 (mutable ledger groundwork) | `test_terminal_hash_tampering_is_refused`; `test_interrupted_terminal_publication_preserves_original_result`; `test_durable_publication_flushes_file_before_directory`; `test_file_replace_uses_shared_publication_with_durability`; `test_windows_directory_replace_uses_replace_existing_and_write_through`; `test_actual_held_ledger_reader_allows_update_and_terminal_transition`. Prepared candidate receipt/seal transaction remains E4. |
| 41, 42 (retained backend evidence / parent gate) | `test_escaped_tokens_in_decoded_logs_glb_json_and_raw_bytes_are_never_retained`; `test_mock_raw_ready_replay_retained_bytes_and_eof`; `test_tokens_are_references_only_and_not_in_wire`; `test_mock_permission_and_forced_nonshipping_are_distinct`. Provenance v2 assembly remains E4. |
| 52 | `test_existing_ci_discovers_new_service_tests`; `test_new_runtime_modules_use_only_stdlib_and_service_imports`; `test_package_ships_broker_modules_and_excludes_mock_and_adapters`. The import check scans E1 and all three E3 runtime modules, including nested platform imports. The new review regressions are in `test_meshgen_e3_review.py`; refusal assertions check exact `rule_id` values. Native acceptance remains pending. |

Round-two regression additions in `test_meshgen_e3_review.py`:

| Q | Tests |
|---|---|
| 24, 25 | `test_recovery_and_cancel_require_recorded_endpoint_and_config_digest`; `test_backend_identity_is_durable_before_submit`; `test_subprocess_readiness_deadline_and_early_exit_always_reap` |
| 41 | `test_each_ledger_payload_is_checked_decoded_and_serialized`; `test_document_token_and_rotated_request_token_refuse_before_admission` |
| 19, 22 | `test_restart_flood_refused_before_allocating_beyond_mcu_or_collection_bound`; `test_png_large_idat_has_linear_bounded_inflate_work_and_exact_refusal`; `test_truncated_jpeg_fill_returns_request_invalid` |
| 22, 23 | `test_raw_glb_container_refuses_before_shared_parser_or_payload_allocation` |
| 24, 26, 40 | `test_admission_rolls_back_every_write_failure_with_live_pid`; `test_failed_admission_rollback_is_drained_and_recoverable_while_pid_live`; `test_post_admission_marker_failure_has_terminal_owned_cleanup` |

Subprocess readiness uses a pipe reader thread and a queue deadline, including
on Windows. Every interactive child is killed when needed, reaped, and has its
streams closed before its temporary directory is removed. Parser adversarial
fixtures assert exact refusal rules, bounded allocations/input work, and finite
execution time without large memory inputs.

## MG2 E4: candidate evidence and worker ownership

`meshgen_provenance.build_evidence()` accepts the broker's exact validated
image/raw-GLB snapshots, resolved config, pinned card/result metadata and bounded
finisher observations. The trusted parent recomputes licence permission and
candidate eligibility, validates the exported GLB and preview, and assembles
provenance v2 and the registered sealed members. Workers cannot submit policy
fields, even inside a step's statistics. Mock GPU fields remain unavailable;
synthetic finisher/preview observations explicitly say `simulated`. Licence
permission does not make a mock candidate shippable. Prompt bytes are retained
only with `logging.keep_prompts=text`; model weights and credentials are never
retained. Config identity and the hash of the stored config bytes are distinct.

`CandidateStore(project, document)` publishes at
`.luminumbra-author-recipes/<document>/candidates/<32-hex-job>/`. Use
`CandidateLedger.commit_success(job, trusted_callback)` to serialize this
publication with cancellation. `CandidateLedger` is an opt-in E4 extension of
the E3 ledger; existing E3 callers retain their behavior. A ready owner's
request, plan and backend/config identity must match the candidate before
publication, and its backend work must already be stopped and drained. The
storage guard lives beside `jobs/`, so E3 job enumeration stays compatible.
Lock order is owner transition, storage guard, then ledger publication guard;
MG2 ownership still precedes the independent build-session lock.

The transaction prepares `jobs/<job>/receipt.prepared.json` first, publishes all
registered members and `manifest.json`, validates their actual bytes, then
publishes `candidate.sha256`. This marker hashes the manifest; it is distinct
from the GLB's artifact hash. `receipt.json` and `receipt.sha256` are published
last outside the seal. Publication retains every verified ancestor until
replacement and durability finish. POSIX uses `O_NOFOLLOW` directory descriptors
for creation, staging, replacement and directory fsync; retained identities are
checked before and after mutation. Windows retains non-delete-sharing directory
handles and refuses reparse points. Regular files use shared
`file_io.replace_file`: temporary file fsync, Windows publication-handle flush
and POSIX fsync of both source and destination directories. File and directory
temporaries live outside seals in document-owned `.candidate-staging/` (initial
state-directory temporaries also remain outside seals). Ordinary failures remove
the current temporary; abrupt process death can retain staging evidence there.
Recovery does not delete that evidence, and retained-state quotas count it.
Interrupted pre-seal work remains uncommitted evidence;
it is refused by reads and recovery. After seal commitment, recovery verifies
the entire bundle and prepared receipt before completing receipt publication.
A valid committed candidate wins subsequent cancellation and E4 orphan
recovery, including a crash before E3's terminal result publication.

Readers reject missing/extra/changed members, escaped paths, symlinks, Windows
reparse points, hard links, malformed JSON and unsupported geometry encodings.
Containment and content checks use the same retained regular-file handle.
`stage_for_import()` copies already-verified GLB bytes into new job-private
staging; it does not reopen the candidate path after validation. G3 still owns
the scene/session/scope plan and main-thread mutation. Existing publications
are preserved on refusal. Project entry/byte quotas refuse new evidence;
`delete()` refuses deletion, protecting retained and future promoted evidence.
Durable game-owned promotion packages and scene lineage remain G3 work.

`append_attestation(job, kind, utc, observation)` writes
`attestations/<kind>.<YYYYMMDDTHHMMSSffffffZ>.json` and its separate hash. The
original seal/provenance never changes. Each payload hashes its predecessor,
current candidate seal, candidate job ID, kind and timestamp, and observation.
Entries and hashes must be unique. The anchor in `jobs/<job>/attestations.json`
and numbered records in `jobs/<job>/attestation-history/` are retained evidence;
they are ordinary mutable filesystem files, not an immutable authority.

The authoritative project-state pointer at
`<state-root>/<document>/attestation-heads/<job>.json`, beside the E3 job ledger
and outside both candidate and job evidence, retains the committed count and
head hash, candidate ID, seal and a prepared append. It uses the existing
ancestor-retaining, durable publication primitives and the document storage
guard. Every read validates all remaining payloads, hashes and numbered records
against the chain and this independent committed pointer. Coordinated suffix
or whole-history deletion and coordinated replacement of candidate attestations,
anchor and numbered history are refused with `attestation.changed` across fresh
service instances. A prepared payload is durably retained in the pointer before
candidate mutation; recovery finishes only that append, including interruptions
between evidence publication and head advancement in either order. The pointer
keeps its prepared payload until anchor completion, then clears it. Missing
committed evidence outside a prepared transaction is refused, not reconstructed.

The threat model is tamper evidence for the candidate directory (and its job
anchor/history copies) relative to the service's independently committed project
state. An actor who rewrites the authoritative state store itself is out of
scope. These files are append-only through the service API; there is no claim
that ordinary filesystem records are physically immutable. Crashes during file
creation, fsync, replacement or Windows directory publication leave temporaries
outside the seal and do not obstruct recovery. Evidence is advisory and grants
no policy permission.

`meshgen_metrics` provides bounded exported accessor inspection, connectivity
across split UV seams, finite nonzero normals, exported height/pivot and texture stripping checks, positive-area UV intersection
with an X sweep broad phase and exact rational predicates/clipping on accepted
binary floating-point coordinates, deterministic area-weighted sampling in a common
metres/Y-up frame, matched-component/open-sheet decisions, and baseline-restored
decimation/whole-chain retry helpers. Fixed metric work limits are versioned
measurement ceilings: one million expanded elements, 200,000 UV broad-phase
pairs and 20,000 area-weighted samples. Exhausted UV evidence is unavailable and
fails `uv_overlap_zero`; it never becomes a raster-based clearance. The raw
reference is retained before any repair. Thickness requires both hit rates;
open-sheet fallback matches distinct raw components and uses each raw
component's largest fixed X/Y/Z projected view. No independent geometry refit
is performed. Native BVH distance/ray queries and actual Blender repair/unwrap
operations are G2 responsibilities. Watertightness and other advisory statistics
are not added as universal gates. Waivers still require committed internal
policy plus a new human-reviewed plan; sealed evidence is never edited to add
acceptance.

`meshgen_worker.run_simulated()` uses a fresh isolated, no-site, no-bytecode
interpreter, bounded request and output pipes, configuration byte/identity
rechecks, scrubbed environment, and an ownership supervisor. Pipe reads/writes
run in threads with bounded buffers, so Windows anonymous pipes need no
`select`. Readiness has its own deadline; execution deadlines and cancellation
use injectable clocks/events. A successful result requires process exit and
completion of stdout, stderr and stdin processing under the same absolute
deadline; late I/O failures are validated before result construction. Execution
reserves up to 0.5 seconds (at most half the total budget) for cleanup inside
that deadline. Root waits, retry waits, Job accounting and thread joins all use
its remaining time and the injected clock, without new cleanup windows. An
unconditional finalizer attempts every tree/Job/handle close, stream close and
thread join even when stop, kill or wait raises. Successful cleanup kills
descendants and waits for every owned PID to disappear, including zombies,
before closing streams. Cleanup failure prevents a successful result. EOF
preserves the supervisor as a reaper through shutdown. Only dedicated Linux ownership supervisor children enable
`PR_SET_CHILD_SUBREAPER`; the caller/service/test runner remains unchanged.
Ordinary `run_owned()` commands also run under an isolated supervisor with an
EOF ownership pipe. A separate start pipe holds a trusted launcher until the
supervisor has established tree ownership and retained the launcher's validated
pidfd. Startup cleanup closes both supervisor-held start-pipe ends before any
wait, so a failed termination cannot leave the launcher waiting for release.
All cleanup waits use the remaining absolute deadline. Direct-child termination
uses its retained pidfd; only setup failure before pidfd ownership permits
`Popen.kill()` on the unreleased launcher. Cleanup attempts every finalizer and
preserves the original setup error. The supervisor repeatedly enumerates its entire
descendant tree using `/proc` parent IDs and creation ticks. It opens and retains pidfds,
then revalidates each child's identity and its chain to the supervisor before
accepting the handle. SIGKILL uses `signal.pidfd_send_signal`, so later numeric
PID reuse cannot redirect a signal. Changed parentage is retried on the next
scan, including subreaper adoption. Cleanup checks the absolute deadline before
and after proc iteration, identity reads, validation and signalling, and inside
the nonblocking `waitpid(-1)` drain. Each scan builds parent adjacency lists
once; frontier expansion and each reap pass take linear work. Scans allow at
most 65,536 proc entries, trees and retained handles at most 4,096 processes,
and each reap pass at most 4,096 wait calls. Deadline or work-limit exhaustion
reports cleanup failure while final resource closure is still attempted. These
checks bound traversal work between clock checks; OS scheduling and individual
system calls can still finish after a deadline. Adoption and enumeration cover
changed sessions/process groups and fork races, including successful root exits. Supervisors also apply the
inherited address-space rlimit. Windows assigns kill-on-close Job objects with
an aggregate memory cap before the stdin handshake; neither
`JOB_OBJECT_LIMIT_BREAKAWAY_OK` nor `JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK` is set.
It waits for their active process count to reach zero. Other POSIX platforms refuse the owned-worker lane
until an equivalent adoption/reaping mechanism is qualified. Unavailable
enforcement refuses the lane. Linux checks both pidfd APIs and probes kernel
support before spawning a worker; missing or denied pidfd enforcement returns
`worker.unavailable`. Windows continues to use Job objects without Linux/POSIX
calls. Compiler supervision is unchanged.

`finisher_envelope()` rechecks the E1 game entry/module pins and returns the
factory-startup, disable-autoexec, two-thread, isolated-resource-directory,
bounded-stdin, deadline and memory contract. It executes no game code or
Blender. G1 currently pins only the companion skeleton; its finisher is planned
for G2. `run_native()` deliberately returns `worker.unavailable`, and
capabilities advertise `meshgen_native_finisher=false` and real backends false.
No native qualification is inferred from portable simulations. Status:
**portable Linux verified; Windows and native acceptance pending** on Windows Blender 5.1.0.

E4 tests are discovered by both existing `blender-authoring.yml` OS lanes and
the installed service ships all four E4 modules. No workflow change is needed.
The E4 fixtures are small GLB/PNG byte strings generated from source. Local
verification includes loopback socket tests for transport coverage.

#### E4 platform mechanism tests

Four tests retain Linux capability guards because their kernel mechanisms do
not exist on the Windows Job path. Their Windows counterparts below run on
every platform with mocked Job APIs and have no platform guards:

| Linux mechanism test | Portable Windows counterpart |
|---|---|
| `test_real_setup_failures_prevent_execution_and_leave_no_worker_or_zombie` (pidfd held launcher) | `test_windows_job_setup_failure_kills_reaps_and_closes_without_handshake` |
| `test_supervision_preserves_caller_subreaper_and_unrelated_child_lifecycle` (`prctl` subreaper state) | `test_windows_job_ownership_terminates_only_assigned_child_and_drains_before_close` |
| `test_linux_reaper_rescans_fork_races_and_waits_for_all_children` (pidfd adoption and `waitpid(-1)`) | `test_windows_job_ownership_terminates_only_assigned_child_and_drains_before_close`; `test_windows_job_accounting_uses_injected_absolute_deadline_and_disallows_breakaway` |
| `test_real_supervisor_owner_eof_reaps_detached_orphan_descendants` (held-launcher/control pipe and subreaper adoption) | `test_windows_job_ownership_terminates_only_assigned_child_and_drains_before_close`; `test_cleanup_closes_job_streams_handle_and_joins_after_wait_and_kill_failures` |

The held-launcher test is in `test_meshgen_process.py`; the other tests and all
Windows counterparts are in `test_meshgen_worker.py`. Deterministic startup
tests also inject simultaneous setup and kill failures, require pipe closure
before a finite wait, and verify that the original error survives deadline
expiry and finalizer failures.

Binding Q coverage below names exact E4 tests (the native and scene portions
remain in their assigned G2/G3 slices):

| Q | Tests |
|---|---|
| 1, 3, 13, 27, 52 | `test_worker_modules_ship_without_ml_bpy_and_work_in_installed_package`; `test_pinned_game_script_envelope_recheck_and_native_lane_unavailable` |
| 5, 28, 29 | `test_pinned_game_script_envelope_recheck_and_native_lane_unavailable`; `test_unavailable_tree_enforcement_refuses_and_reaps_without_handshake` |
| 9, 11, 17, 21 | `test_actual_final_budget_refused_and_grounded_pivot_verified`; `test_config_image_raw_result_exporter_and_capability_identity_mismatch`; `test_configuration_bytes_and_resolved_identity_rechecked_before_spawn`; `test_v2_superset_parent_mock_policy_gpu_unavailable_exact_retained_bytes` |
| 18, 42 | `test_policy_forgery_in_worker_observations_and_nested_fields`; `test_parent_policy_forgery_rejected_even_with_rehashed_members`; `test_v2_superset_parent_mock_policy_gpu_unavailable_exact_retained_bytes`; `test_forged_gate_stats_and_prepared_owner_plan_refused` |
| 22, 23, 34 | `test_exported_accessor_counts_normals_uv_bounds_and_split_seam_bodies`; `test_malformed_bounds_indices_attributes_expansion_and_encodings`; `test_preview_parser_bounded_chunks_and_dimensions` |
| 24, 29 | `test_windows_compatible_readiness_timeout_cancellation_and_reaping`; `test_crash_missing_readiness_bounded_stdout_stderr_and_stdin`; `test_blocked_stdin_writer_is_bounded_and_reaped`; `test_eof_stops_simulated_worker_tree_with_bounded_readiness`; `test_every_owned_pid_disappears_after_timeout_cancellation_and_exception`; `test_result_waits_for_delayed_stderr_and_writer_and_validates_late_failures`; `test_real_supervisor_owner_eof_reaps_detached_orphan_descendants`; `test_linux_reaper_rescans_fork_races_and_waits_for_all_children`; `test_supervision_preserves_caller_subreaper_and_unrelated_child_lifecycle`; `test_windows_job_accounting_uses_injected_absolute_deadline_and_disallows_breakaway`; `test_cleanup_closes_job_streams_handle_and_joins_after_wait_and_kill_failures` |
| 25, 26, 40, 46 | `test_cancellation_and_commit_share_e3_owner_transition`; `test_committed_seal_wins_cancel_and_recovers_original_e3_terminal_replay`; `test_orphan_recovery_uses_seal_prepared_receipt_and_refuses_policy_tampering`; `test_subprocess_crash_at_candidate_marker_recovers_seal_and_terminal_success` |
| 31, 32, 33 | `test_area_weighted_sampling_deterministic_in_common_frame_and_whole_chain_retry`; `test_closed_shell_and_matched_open_sheet_guard_modes`; `test_under_budget_guard_runs_retry_starts_same_baseline_restores_last_accepted` |
| 35 | `test_exact_uv_positive_area_overlap_shared_edges_and_bounded_adversarial_pairs`; `test_uv_overlap_translated_thin_and_nearly_touching` |
| 36, 37 | `test_missing_guard_evidence_fails_under_budget_and_accept_flagged_needs_human_plan`; `test_v2_superset_parent_mock_policy_gpu_unavailable_exact_retained_bytes` |
| 38 | `test_worker_step_inventory_bound_and_preview_no_fake_claim`; `test_simulated_worker_verifies_config_and_records_no_native_measurement` (sequential seeds retain E3 coverage) |
| 39 | `test_member_tamper_missing_extra_and_manifest_rewrite`; `test_escapes_symlinks_and_hardlinks_refused`; `test_append_only_attestations_preserve_original_seal_detect_rewrite_truncation`; `test_attestation_interrupted_append_recovery`; `test_attestation_reorder_duplicate_replay_and_anchor_truncation_refused`; `test_publication_ancestor_swaps_refuse_and_preserve_outside`; `test_new_directory_ancestor_swap_refused_during_creation`; `test_authoritative_head_refuses_coordinated_suffix_deletion_and_replacement` |
| 40 | `test_seal_and_receipt_order_recovery_after_each_publication_interruption`; `test_recovery_refuses_changed_prepared_and_terminal_receipt`; `test_regular_publication_fsyncs_before_shared_replace_and_posix_directory_after`; `test_held_shared_readers_and_durability_use_shared_publication`; `test_attestation_publication_internal_crashes_recover_outside_seal_staging`; `test_attestation_pointer_and_candidate_crash_recovery_in_both_orders` |
| 41 | `test_complete_seal_retains_replay_distinct_identities_and_receipt`; `test_prompt_retention_opt_in_and_no_secret_payloads`; `test_secret_exclusion_decoded_json_glb_logs_and_rotation`; `test_dynamic_token_rotation_refuses_document_before_state_and_attestation_before_anchor` |
| 43 | `test_held_shared_readers_and_durability_use_shared_publication`; `test_project_byte_quota_refuses_publication_preserving_last_receipt` (GC refuses deletion; promotion remains G3) |
| 44 | `test_import_private_snapshot_uses_verified_bytes_despite_source_replacement`; `test_import_private_traversal_refused_before_any_destination_publication` |

Run the stdlib service suite with temporary files inside the clone:

```sh
mkdir -p .test-tmp
TMPDIR="$PWD/.test-tmp" TMP="$PWD/.test-tmp" TEMP="$PWD/.test-tmp" \
  PYTHONDONTWRITEBYTECODE=1 \
  python3 -B -m unittest discover -s tools/blender/authoring/service/tests -v
rmdir .test-tmp
```

The suite includes deterministic proc-reader, enumeration and pidfd-signal
fixtures in `test_meshgen_process.py` for exit/reuse, parent reuse, adoption,
unavailable enforcement, traversal limits and deadline expiry inside cleanup
loops. Socket tests require local loopback permission. Native Blender and
Windows execution require their own acceptance environments.
