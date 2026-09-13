# Experimental viewport transport

This portable library and external broker implement WIRE.md. They do not yet
connect Blender to the installed renderer. The child under test/viewport is a
synthetic protocol fixture, not a renderer, performance benchmark or visual test.

Run portable tests from repository root:

```sh
python3 -B -m unittest discover -s test/viewport -v
```

An integration caller creates a fresh private session directory and a 32-byte key,
then supplies broker.py --config FILE. Trusted config has exact Broker constructor
arguments: root, session, command (absolute executable plus argv),
executable_sha256, project_root, optional deadline<=60. Supply the key hex only in
LUMINUMBRA_VIEWPORT_KEY. The broker verifies the executable bytes and starts it
with shell=False; its command arguments and project root are trusted startup
configuration, not accepted from mailbox messages. Pin scripts/modules/resources
in the external installation manifest before launch: an executable hash alone
does not authenticate those dependencies.

Use protocol.encode + atomic_write to publish desired.bin and stop.bin. Use
Slots.read(0/1) to copy authenticated completed frames under leases; choose the
newest frame and call match_frame with the current desired state before uploading.
The returned bytes own their storage, so leases end before any GPU operation.
StateGate admission is strict; do not reuse a sequence for changed content.

The directory owner is the security boundary. On Windows the caller must apply a
user-only ACL before launch; reparse checks do not replace permissions. The broker
never steals leases, cleans unrelated files, executes shell strings, or opens a
network socket. Crashed leases require a new session. It currently supervises one
child that must not create descendants. A production child needing descendants
requires a reviewed process-tree/job-object supervisor before integration.

Receipt broker-result.json distinguishes cooperative stop from failure and records
owned-child reaping and publication/drop counts. Child stdout is protocol-only;
the broker drains stderr continuously and retains its final MiB in host-stderr.log.
Stop completes the active request, discards pending work, forwards authenticated
shutdown, and waits at most five seconds (or the shorter configured deadline).
It reaps uncooperative children and records failure. Production integration must
pin the complete installed host before claiming qualification.
