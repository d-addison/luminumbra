# T-I4-12 Session Replay (LREC1) — Progress Log

Recovery file (prior agents lost final reports to session limits). Append a dated
entry after EACH numbered part. Raw verification values live here.

Branch: feat/polyglot-audit-roadmap. Build: build/debug (ninja, MinGW ucrt64).
Baseline HeadlessServerTick world_hash = 2fa007951a21e140 (90 ticks, must stay
unchanged with recording active).

## Part 1 — LREC1 stream format + ReplayStream — STATUS: DONE (2026-06-12)

Files CREATED:
- `src/luminumbra_common/replay/ReplayStream.h` — engine-generic (no client/game
  deps). Types: ReplayHeader, InputRecord, CheckpointRecord, ReplayWriter,
  ReplayContents, ReadReplay(), FindInput/FindCheckpoint, Fnv1a64Hex().
- `src/luminumbra_common/replay/ReplayStream.cpp` — implementation.
- `src/luminumbra_common/sources.cmake` — registered the new .cpp in
  COMMON_SOURCES (verified it compiles into libluminumbra_common.a; the i2
  WorldStreamingState manifest-omission incident is the reason this is checked).

LREC1 FORMAT (little-endian, fixed-width, no struct padding / no native-int
memcpy on the wire; strings = u32 LE length + raw bytes):
- HEADER: magic "LREC1" (5B) | reserved u8 | version u16 (=1) | tick_rate_hz u16
  | tick_count u64 (placeholder, PATCHED by Finalize at fixed offset 10) |
  seed u64 | preset_hash u64 | surface_radius u32 | collision_radius u32 |
  str seed_string | str preset | str start_world_hash | str engine_version.
- RECORD FRAME: type u8 (Input=0x01 / Checkpoint=0x02) | tick u64 |
  payload_len u32 | payload. Explicit length => truncation detectable + unknown
  record skippable.
  - Input payload: blob(inputs) = u32 len + bytes. EMPTY input set = 4 bytes
    (zero length) — compact, as required. Inputs are OPAQUE (transport's concern).
  - Checkpoint payload: str world_hash | str terrain | str water | str entities.
    MESH EXCLUDED per T-I4-11 caveat (derived render artifact; authoritative =
    terrain/water/entities). Hashes travel as hex strings (no float/endian risk).
- TRAILER: magic "LRECEND1" (8B) | tick_count u64 | input_count u64 |
  checkpoint_count u64. Trailer magic 'L'=0x4C cannot collide with a record type
  tag (0x01/0x02), so the reader disambiguates trailer-vs-record by peeking.

TRUNCATION ROBUSTNESS: ReadReplay parses the whole buffer; any short read mid-
frame, a frame claiming more payload than present, or a missing trailer sets
contents.truncated=true / trailer_present=false. Trailer record counts are
cross-checked against parsed counts. A bad HEADER (magic/short) returns nullopt
(not a valid LREC1 stream at all).

VERSIONING: header carries engine_version (GetEngineVersionString) + version u16;
playback (Part 3) refuses a version mismatch loudly.

Verification: libluminumbra_common.a links clean with ReplayStream.cpp.obj
present (no manifest/link gap). Unit tests + gates land in later parts.

## Part 2 + 3 — server --record / --replay / --mutate-replay-fixture — DONE (2026-06-12)

Files MODIFIED:
- `src/luminumbra_server/main_server.cpp`: new CLI flags --record/--replay/
  --mutate-replay-fixture; functions RunRecord, RunReplay, RunMutateReplayFixture,
  CaptureCheckpoint; include core/EngineVersion.h + luminumbra_common/replay/
  ReplayStream.h. Checkpoint interval = 30 ticks. main() dispatches the new modes.

DETERMINISM (recording is hash-neutral, PROVEN):
- The recorder steps RunFixedTicks(1) per tick (so checkpoint hashing aligns to
  the same settled per-tick state). ReplayWriter buffers in memory; flush is at
  Finalize() only -> no IO on the tick path. Checkpoint hashing reuses
  ComputeWorldHash/ComputeWorldSubHashes (read-only quiesce+snapshot).
- 90-tick record reached end_hash = 2fa007951a21e140 (THE canonical hash,
  UNCHANGED by recording). 30 ticks=d9622e2c4cd6b833 (1 cp), 60=6bd6ce854a5fe0d4
  (2 cp), 90=2fa007951a21e140 (3 cp at 30/60/90).

VERIFICATION (manual, build/debug):
- RECORD 90 -> replay round-trips: 3 checkpoints verified, end_hash
  2fa007951a21e140 matches recording, start_world_hash 9d9d500dd22504f4 matched.
  Artifact luminumbra.replay_roundtrip.v1 passed=true.
- DIVERGENCE: --mutate-replay-fixture corrupts the FIRST checkpoint (tick 30)
  in-process (parses + re-emits with world_hash+terrain prefixed "dead"); replay
  of the mutated stream DIVERGED at tick 30, section=terrain, exit 1, wrote
  luminumbra.replay_divergence.v1 {divergence_tick:30, divergence_section:
  terrain, expected/actual per section}. Oracle is NOT vacuous.
- TRUNCATION: half-file replay -> "stream is truncated (no valid trailer)",
  exit 1, refused before boot.
- VERSIONING: header carries engine_version (0.1.0+...) + LREC version u16;
  RunReplay refuses a version mismatch loudly.

Mutation choice: in-process --mutate-replay-fixture (parse the real stream, flip
one checkpoint's hashes, re-emit) -- LEAST hacky vs byte-offset surgery: no
fragile offset math, survives format tweaks, self-documents intent.

Next: Part 4 (validator gates ReplayRoundtrip + ReplayDivergence), Part 5 (gtest).

## Part 4 — validator gates ReplayRoundtrip + ReplayDivergence — DONE (2026-06-12)

Files MODIFIED:
- `.forge/scripts/validate-engine-frontier.ps1`: added Test-ReplayRoundtrip and
  Test-ReplayDivergence (+ helper Invoke-ServerWithCrashRetry). Registered both
  in the param ValidateSet and the -Mode switch. Kept OFF the default "All" lane
  (slow: 90-tick server passes), mirroring the HeadlessServerTickHeavy precedent.
- `src/luminumbra_server/ServerWorldRunner.{h,cpp}`: added
  ComputeWorldHashAndSubHashes (single quiesce+snapshot feeding both hashes;
  byte-identical to the two separate calls) + an env-gated worker-count override
  (LUMINUMBRA_JOB_WORKERS) -- see the flakiness note below.
- `src/luminumbra_server/main_server.cpp`: CaptureCheckpoint now uses the
  combined single-snapshot path.

GATE RESULTS (both EXIT 0):
- ReplayRoundtrip: record 90 -> replay; end_hash=2fa007951a21e140 (CANONICAL,
  recording is hash-neutral), start_world_hash matched, 3 checkpoints verified.
  The gate asserts the end hash == the canonical 2fa007951a21e140 (the
  determinism proof that recording does not perturb the sim).
- ReplayDivergence: record 90 -> mutate first checkpoint (tick 30) ->
  replay FAILS at tick 30, section=terrain, 0 checkpoints verified before
  divergence; gate confirms the oracle is not vacuous. The deliberately-nonzero
  inner replay exit is cleared so the gate process exits 0.

KNOWN ENGINE FLAKINESS (NOT a replay defect; PRE-EXISTING, flagged in T-I4-11):
- The headless server's 90-tick streaming/shutdown path has an INTERMITTENT
  Windows 0xC0000005 access violation. Measured ~40-50% per 90-tick run with the
  default 16 workers; the crash fires DURING the run (before the stream is
  finalized), at any checkpoint, NOT concentrated in late ticks. --smoke (single
  RunFixedTicks(90), one end-of-run hash) is stable 3/3; the replay recorder's
  per-tick stepping + mid-run checkpoint hashing exposes the race. EVERY
  completing run yields the EXACT canonical hash 2fa007951a21e140 -- the replay
  logic is correct; the crash is an engine concurrency bug.
- ROOT-CAUSE PROBE: forcing 1 job worker dropped the crash rate to ~20% with the
  hash UNCHANGED (confirming the race is in parallel streaming AND that the hash
  is worker-count-invariant). gdb's serialized scheduling masks the race (ran to
  completion under gdb), so no clean backtrace was obtainable.
- MITIGATION (gate reliability): the two replay gates run the server with
  LUMINUMBRA_JOB_WORKERS=1 (hash-neutral; ~20% crash) AND retry the 0xC0000005
  crash class up to 8x with a 2s settle between attempts. P(all 8 fail) is far
  below 1e-5. Both gates pass cleanly (observed: occasional single retry, then
  green). FOLLOW-UP for the orchestrator: the underlying engine streaming race is
  worth a dedicated fix (out of scope for T-I4-12, the replay-format task; same
  defect class the T-I4-11 heavy gate documented).

## Part 5 — gtest (ReplayStream unit behavior) — DONE (2026-06-12)

Files:
- NEW `test/common/ReplayStream_test.cpp`: 6 hermetic cases (temp files under the
  test artifact dir / temp dir, auto-removed): HeaderRoundtrip,
  RecordAppendReadIdentity (incl. EMPTY input set preserved + FindInput/
  FindCheckpoint lookups), TruncationDetected (lop tail -> truncated=true /
  trailer_present=false; whole-file sanity first), BadMagicRejected (nullopt),
  CheckpointEncodeDecodeMultiple (90 inputs + 3 checkpoints at 30/60/90),
  Fnv1a64HexStable.
- `test/CMakeLists.txt`: added the file to the common_tests executable
  (gtest_discover_tests already wired). Tests #88-93 in ctest.

Verification: common_tests --gtest_filter=ReplayStreamTest.* = 6/6 PASS.

---

## FINAL VERIFICATION SUMMARY (2026-06-12) — ALL GREEN

- Full clean debug build: exit 0 (common lib w/ ReplayStream.cpp, server w/
  record/replay/mutate modes, client app).
- Full ctest (-LE manual): 188/188 PASSED, 0 failed (includes the 6 new
  ReplayStreamTest cases #88-93).
- validate-engine-frontier.ps1:
  - ReplayRoundtrip: GREEN (exit 0). record 90 -> replay; end_hash
    2fa007951a21e140 (CANONICAL, recording hash-neutral), start hash matched,
    3 checkpoints verified.
  - ReplayDivergence: GREEN (exit 0). mutate first checkpoint (tick 30) ->
    replay refused at tick 30 section=terrain, 0 cps before divergence; oracle
    proven non-vacuous; inner divergence exit cleared so the gate exits 0.
  - HeadlessServerTick: GREEN, world_hash=2fa007951a21e140 UNCHANGED, sub-hashes
    [terrain=9e1b9316d5eeca32 mesh=812c3bb1c19b127a water=ed4265f8b090adf7
    entities=5735a5094c1e92a8] match (= T-I4-11 values).
  - SimDeterminismLint: GREEN, 39 files, 0 violations (new std::getenv worker
    knob is not a banned construct; replay/ is not in the sim-scan roots).
  - EngineGameSplitLint: GREEN, replay/ is engine-clean (0 game nouns).
- No golden/baseline re-blessed (build/debug/test-artifacts/replay/ is not
  tracked; server-tick golden hash unchanged).

### Files touched (final list)
Created: src/luminumbra_common/replay/ReplayStream.h;
  src/luminumbra_common/replay/ReplayStream.cpp;
  test/common/ReplayStream_test.cpp; this progress file.
Modified: src/luminumbra_common/sources.cmake;
  src/luminumbra_server/ServerWorldRunner.{h,cpp};
  src/luminumbra_server/main_server.cpp; test/CMakeLists.txt;
  .forge/scripts/validate-engine-frontier.ps1.
