# Viewport wire v1 (frozen transport contract)

Experimental portable transport; no real renderer or Blender qualification yet.
All multibyte integers are little endian. One private inherited duplex pipe pair
per host. Key is 32 random bytes, hex encoded in LUMINUMBRA_VIEWPORT_KEY, removed
from the child environment before any further children. No key in messages/logs.
Trust boundary is other sessions, not a hostile process running as the same user.

Record: magic `LVP1` (4 bytes), JSON header length u32, raw payload length u64,
HMAC-SHA256 (32 bytes), UTF-8 canonical JSON header, raw payload. HMAC covers
prefix (first16 bytes) + header bytes + raw payload bytes. Verify before parsing
or publishing. Python emits sorted keys, ASCII escaping and compact separators; C++ may use
its own valid number formatting. Authenticate exact received bytes, never
re-serialize to verify a MAC. Reject duplicate keys/nonfinite numbers; compare
parsed state values exactly across the producer/host join. Header <=1MiB, payload <=8,294,400 bytes.
Read exactly the declared lengths; EOF midway is a failure, not a partial frame.

Header exact common members: `kind`, `session` (32 lowercase hex), `sequence`
(positive integer), `state`. State exact members: `generation_id` (32 hex),
`manifest_sha256` (64 hex), `scene_revision` and `camera_revision` (nonnegative
integers), `width`,`height` (1..1280 and1..720), `near_plane`,`far_plane` (finite 0.001<=near<far<=1e6),
`view`,`projection` (16 finite
column-major numbers), `locals` (complete list, <=1024 unique nodes, each exact
`instance_id` native stable string matching [A-Za-z][A-Za-z0-9_.:-]{0,127}, `node_id` nonempty <=128 ASCII chars,
`matrix`16 finite numbers). This transport checks representation, not renderer
projection/material semantics. Host must independently validate its capabilities.

`kind=state`: common header only, empty payload. Every state contains the complete
local transform set, never deltas. Strictly increasing sequence within session;
scene and camera revisions cannot decrease anywhere in the session, including
generation changes. Every change to view, projection, extent or clip planes
requires a camera revision greater than the previous accepted camera revision.
Changing generation is a barrier and requires a scene revision greater than the
previous accepted scene revision. These rules allow an intermediate generation
to be coalesced safely, including A→B→A: the final A still has revisions valid
relative to the last A actually delivered to the host.

Both broker and host remember every admitted generation/manifest pair for the
session. Reusing a generation ID with a different manifest is refused even after
switching to another generation. At most 64 distinct generations are admitted;
revisiting an existing pair does not consume another entry. A refused state
changes neither the last accepted header nor this immutable-generation map.
The broker retains
one in-flight state plus one replaceable latest pending state. A returned frame
must exactly echo the entire in-flight state and sequence. If a newer state is
pending, discard old output; never relabel its revision or publish stale depth.

`kind=frame`: common header plus exact `planes_sha256` (64 hex). Payload is
contiguous RGBA8 (4wh bytes), reversed-Z f32le depth (4wh), coverage u8 (wh).
Origin bottom-left; depth finite0..1, coverage0/1 equals depth>0; alpha255 for
coverage1 and0 otherwise. Color encoding `engine-display-power2.2` is implicit
v1 and is NOT scene linear/IEC sRGB. Hash covers all9wh raw bytes. This is only a
transport validation profile, not overlay/color/visual approval.

`desired.bin` is a single atomic replace mailbox containing an authenticated
state record. Separate broker process polls it; Blender never writes a pipe in
a draw callback. Producer must serialize writes (one Blender main thread).
`stop.bin` is a signed state-free `kind=stop` record with session and sequence;
sequence must exceed latest accepted state. It ends the owned host process.

Two fixed-size mmap files `frame-0.bin`, `frame-1.bin`, each capacity
48+1MiB+8,294,400 bytes, hold one full authenticated frame record. A small atomic
`frame-N.json` descriptor contains record length/sequence, not trusted provenance.
Both reader and writer acquire the SAME `frame-N.lease` file with O_CREAT|O_EXCL.
Reader holds it until copy/consume is complete; writer cannot overwrite leased
bytes. Descriptor read happens after acquiring lease. No expiry/forced stealing:
crashed consumer requires whole-session teardown/new directory. If both slots
are leased, retain at most one completed latest frame and retry publication.
A newer desired state or stop discards it; the delivery deadline still starts
at the first outstanding request and is never extended by coalescing or retries.
Consumer validates record MAC, session, length and descriptor
sequence; chooses highest sequence and must compare current desired state before
GPU upload. Slots are bounded and never append. Caller creates a fresh private
session directory (POSIX0700; Windows caller must apply user-only ACL), never
reuses directories from another session. Reparse/symlink inputs are refused.

Host startup is an externally supplied absolute executable + argument vector,
with executable SHA256 pin, shell=False. This initial broker runs only a child
that does not spawn descendants; process-tree/job-object supervision is a required
integration prerequisite for any host that can spawn children. Render deadline
60s; shutdown terminates then kills the owned child, never unrelated processes.
Fixture child is explicitly test-only. No network endpoints.

Startup configuration pins one trusted absolute project_root outside the wire.
A session admits one generation/manifest at a time from this project. State
replacement must load and validate the new generation transactionally before
publishing; no request can change project root. Pass near/far unchanged to
RenderView; the host validates them against actual projection entries.

Each instance ID in locals denotes one instance of the admitted prefab. Its local
set MUST contain every descriptor node exactly once; host verifies membership and
parent topology against the pinned descriptor. The full set of instance IDs is
authoritative: absent IDs are removed transactionally. At most64 instances and
1024 total node locals. Instance placement is identity in v1; an authored world
placement is composed into every root node's local matrix by the adapter. This
avoids an implicit integer/string ID conversion and a second placement channel.
Host acknowledges by echoing the complete state after successful transactional
publication/render; a failure closes the child session without a false frame.
