# Asynchronous installed viewport client

`extension/viewport_client.py` exposes a portable client with no Blender or GPU
imports. Package `viewport_client.py`, `viewport_installation.py`, and
`viewport_process.py` alongside an extension-local `viewport/` containing the
transport's `protocol.py` and `broker.py`. The Blender adapter owns draw callbacks
and texture upload; this module owns its background worker and broker process tree.

```python
client = ViewportClient(python, broker_script, host, project, host_manifest)
sequence = client.submit(state)
frame = client.poll()  # None, or (authenticated header, immutable plane bytes)
status = client.status  # Snapshot; contains state, error, session and sequence.
client.restart()  # Explicit fresh session; installation identities stay pinned.
client.close()  # Requests asynchronous shutdown.
closed = client.close(wait=True, timeout=10)  # Bounded wait, outside draw callbacks.
receipt = client.session_receipt()  # Deep copy of final host evidence, after close.
```

Construction starts a worker; resolution, hashing, subprocess creation, filesystem
mailboxes, slot leases, plane validation and shutdown run there. `submit` validates
and snapshots one bounded state. A newer submission replaces pending work and
invalidates an older completed frame. `poll` only consumes a completed frame that
joins the latest state, sequence and session; it never reads a pipe or frame file.
There are two fixed transport slots and one completed immutable CPU frame, with
no frame queue. The caller owns bytes it has consumed and must bound its own
texture/frame retention.

Mailbox and frame-descriptor readers permit Windows atomic replacement using
[`FILE_SHARE_DELETE`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).
Writers use `FileRenameInfoEx` with replace-existing and
[`POSIX_SEMANTICS`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information)
on Windows 10 version 1607 or later. Ordinary `os.replace` still refused a held
shared reader in the native control; reader sharing alone was insufficient.
An open reader retains the complete previous file while the next file becomes
visible at the path. Handles are non-inherited, reject reparse/directory objects,
and transfer to one owned Python stream. Record-size checks apply to the opened
file. Writers still publish through one atomic replacement; this adds no retry,
in-place mutation, weakened error handling or deadline extension. A Windows-only
negative control holds a real ordinary CRT reader to reproduce sharing denial,
then verifies successful replacement and complete old/new records using the new
reader. Both command mailboxes and frame descriptors are covered.

Timeouts are configurable independently: SDK/startup validation defaults to ten
seconds, outstanding frames to sixty, and cooperative shutdown to five. Each is
bounded to sixty seconds; at most one automatic fresh-session restart is attempted
by default (configurable zero through three). Flooding newer desired states does
not extend an outstanding frame deadline. Final identity validation runs even
after failure, and is independently bounded by the startup timeout. Forced child
termination waits at most another two seconds. A timed `close` reports whether
the worker finished; it never claims a successful shutdown merely because its wait
expired. Local regular files are required; these are operational timeout bounds,
not a guarantee against a stuck operating-system filesystem call.

Each start/restart uses a new private directory, key and UUID. The key exists only
in memory and child environment, never command arguments or config. POSIX creates
a mode-0700 directory and owns a fresh process group. Windows 10+ atomically creates
the broker in a kill-on-close Job Object through `PROC_THREAD_ATTRIBUTE_JOB_LIST`;
there is no create/assign orphan interval or inherited job handle. A protected
current-user-only inheritable DACL is supplied when creating the directory.
Closing the job kills descendants even after an abrupt owner death. POSIX group
cleanup covers normal close and broker failure; abrupt parent death is not
qualified as automatic group cleanup.

The explicit installation manifest is `SDK/installation.json`, directly in the
SDK root, with this exact shape:

```json
{
  "schema": "luminumbra.static_preview.installation.v1",
  "source_commit": "<40 lowercase hexadecimal digits>",
  "source_dirty": false,
  "source_input_sha256": "<64 lowercase hexadecimal digits>",
  "executable": "bin/luminumbra_preview_capture.exe",
  "files": {"<relative SDK path>": "<sha256>"},
  "identity_receipt_sha256": "<qualified installed-host receipt sha256>"
}
```

The complete file roster must contain the actual host, renderer module,
`share/luminumbra-static/source-inputs.txt`, and all five installed shaders.
Missing/extra files, aliases, traversal, symlinks/reparse points, non-regular files,
changed content, unknown fields and duplicate keys are refused. The only excluded
file is the manifest itself. File count, aggregate bytes and hashing time are
bounded. The client pins the manifest, whole SDK, interpreter, broker and protocol
before starting; it checks them again after every session and never silently
repins changes during recovery.

Before removing an owned session directory, the worker retains at most one MiB
each of the host's `session.json` and `failure.json`, their exact UTF-8 text and
raw-byte SHA-256 hashes. `session_receipt()` returns an independent copy, separate
from the small per-draw status snapshot. Missing output is `unavailable`; failure
output, malformed/oversized output or failed final identity checks remain
`failed`. A complete receipt must join the installation's source, executable,
module and shader identities and report clean shutdown. These statuses are technical evidence and
never visual approval. Restart begins a new receipt scope with a fresh session ID.

Seal an SDK after `test/static_preview/installed_viewport.py` succeeds:

```sh
python3 -B tools/blender/authoring/seal_viewport_installation.py \
  --host /sdk/bin/luminumbra_preview_capture \
  --qualification /evidence/installed-qualification.json \
  --host-receipt /evidence/host-evidence/session.json \
  --manifest /sdk/installation.json
```

Receipts stay outside the SDK. The emitter joins the successful qualification to
the native session hash, clean source identity, executable/module/shader hashes,
and identical complete SDK inventories before and after the run. It validates
the current installation against that inventory and removes its own new manifest
on refusal. It accepts no replacement source identity and will not overwrite an
existing manifest. The receipt is an integrity chain from local qualification;
it is not a third-party signature or visual approval.

Run supporting portable tests with
`python3 -B -m unittest discover -s test/viewport -v`. They use actual subprocesses
and authenticated transport with synthetic frames; dummy SDK files exercise only
identity validation. The Windows-only process tests remain separately qualified:
`python -B test/viewport/probe_windows_client.py --receipt client-windows.json`
records executable/source hashes, exact test count, explicit skips and results.
Its protected-DACL, abrupt-owner-death and real file-sharing tests cannot pass by being skipped.
These tests do not qualify Blender presentation, installed rendering, native GPU
performance or visual acceptance.
