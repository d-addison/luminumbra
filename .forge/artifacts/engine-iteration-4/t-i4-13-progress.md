# T-I4-13 lockstep-transport progress

Recovery path for the lockstep transport dispatch. Append a dated entry per
milestone (what landed, files, verification state, next).

## 2026-06-14 -- Milestone 0: context absorbed, design fixed

Read: design-decisions.md (section 8 lockstep), worldgen-lockstep-sdfrt.md
(Area 2: delay-based, rollback REJECTED, adaptive horizon 3->10), ReplayStream.h/.cpp
(LREC1 wire discipline -- PutU*/Cursor, length-framed records, trailer), ServerWorldRunner.h
(RunFixedTicks(1) per-tick stepping, ComputeWorldHashAndSubHashes), main_server.cpp
(--smoke/--heavy/--record/--replay modes; CaptureCheckpoint pattern), the validator
(HeadlessServerTick/ReplayRoundtrip/ReplayDivergence/SimDeterminismLint gate shapes;
the slow modes are OFF the default All lane), ReplayStream_test.cpp (hermetic temp-file
test style), sources.cmake (common manifest), test/CMakeLists.txt (common_tests target).

Key facts pinned:
- Canonical world_hash for 90-tick/seed-424242/default = 2fa007951a21e140 (must NOT move).
- Checkpoint cadence = 30 ticks (reuse for hash exchange).
- SimDeterminismLint scans src/luminumbra_server but NOT src/luminumbra_common/net;
  regardless, ALL adaptive-horizon/RTT/wall-clock machinery stays OUT of the tick-affecting
  path (it never feeds the hash).
- luminumbra_common links no winsock today; TcpTransport must link ws2_32 + guard _WIN32.
  Gates/tests use LoopbackTransport only (no real sockets/ports).

Design fixed:
- ILockstepTransport seam (send/recv framed messages, non-blocking recv, disconnect).
  Impls: LoopbackTransport (in-proc queue pair), TcpTransport (_WIN32 winsock).
- Protocol (LE fixed-width, mirror ReplayStream): Hello, Input(tick,client,blob),
  Hash(tick,world_hash+sub), Bye. Length-prefixed frames.
- Delay-based horizon: input for tick T applied at T+horizon; tick does not advance until
  all peers' inputs for it are present. Adaptive in TICKS (not wall-clock): grow on late
  arrival, shrink on slack, clamp [3,10]; >max => visible pause not desync.
- Desync oracle: exchange hash+sub every 30 ticks; mismatch => HALT + LREC1 dump (checkpoints
  carry local hashes, divergent tick+section recorded).
- Clean disconnect (F3): Bye/socket close ENDS session cleanly, surviving side reports
  disconnect tick, exits 0 (not a desync).

Next: write LockstepSession.h/.cpp, register in common sources.cmake, confirm it compiles
into libluminumbra_common.a.

## 2026-06-14 -- Milestone 1: LockstepSession core (Part 1) LANDED + compiles

Files:
- src/luminumbra_common/net/LockstepSession.h (new): protocol msgs (Hello/Input/Hash/Bye),
  ILockstepTransport seam, LoopbackTransport + MakeLoopbackPair, TcpTransport (_WIN32 winsock,
  stub elsewhere), LockstepConfig/TickResult/TickOutcome/LockstepHooks/LockstepStatus, LockstepSession.
- src/luminumbra_common/net/LockstepSession.cpp (new): LE encoders/decoders (ReplayStream
  discipline), loopback + tcp transports, Handshake/PumpTick/Disconnect/EmitDesyncDump.
- src/luminumbra_common/sources.cmake: registered net/LockstepSession.cpp.
- src/luminumbra_common/CMakeLists.txt: link ws2_32 under WIN32 for TcpTransport.

CONFIRMED: net/LockstepSession.cpp compiles into lib/libluminumbra_common.a (built clean).

Gotcha resolved: <windows.h> (via spdlog/Log.h) #defines SendMessage->SendMessageA, which
renamed the virtual. Transport methods are SendFrame/TryReceiveFrame (not SendMessage). Also
winsock2.h is included FIRST in the TU (guarded _WIN32) before windows.h ordering requirement.

Protocol wire layout (all LE, length-prefixed strings/blobs, frame=[u8 type][u32 len][payload]):
- Hello payload: magic"LSTP1"(5) | u16 protocol_version | u64 seed | u16 tick_rate | u32 client_id | str preset
- Input payload: u64 tick | u32 client_id | blob inputs(u32 len+bytes)
- Hash  payload: u64 tick | str world_hash | str terrain | str water | str entities
- Bye   payload: u64 tick

Adaptive horizon rule (TICKS, hash-neutral): start=3, [min=3,max=10]. On the wanted tick,
if the peer's input is NOT yet buffered -> grow horizon by 1 (<=max), send the next-horizon
local input ahead, count a late_input_event; at max -> WaitingForPeer (visible pause, never
desync/guess). When all inputs present and horizon>min, count a slack tick; after
slack_ticks_to_shrink(30) consecutive slack ticks -> shrink by 1. Inputs merged in ascending
client-id order (std::map ordered => deterministic). Hash exchange every 30 ticks; mismatch =>
localize via sub-hashes (terrain/water/entities then world_hash) => HALT + LREC1 dump.

Next: gtest over LockstepSession (LoopbackTransport), then wire server (--lockstep-loopback),
then validator modes (LockstepLoopback, LockstepFaultInjection).

## 2026-06-14 -- Milestone 2: gtest (Part 4) LANDED + GREEN

File: test/common/LockstepSession_test.cpp (new), registered in test/CMakeLists.txt.
7 tests, all GREEN (LoopbackTransport only, hermetic temp dumps):
- HandshakeAccepts, HandshakeRejectsSeedMismatch, HandshakeRejectsPresetMismatch
- InSyncTickExchange (2 peers, 90 ticks, both Finished at tick 90, no desync)
- HorizonAbsorbsLateInput (stall b 15 pumps -> horizon grows >3, late_input_events>0,
  then both Finished at 90, no false desync)
- DesyncDetectedAndDumpEmitted (b world corrupts from tick 30 -> oracle HALTS at tick 30
  section=terrain, LREC1 dump re-reads clean)
- CleanDisconnect (b Bye -> a reports PeerDisconnected, not desync, no hang)

Verified run: [PASSED] 7 tests. (uses a deterministic FakeWorld hash, not the real server.)

Next: wire server (--lockstep-loopback + fault-injection knobs), then validator modes.

## 2026-06-14 -- Milestone 3: server wiring (Part 2) LANDED + GREEN

File: src/luminumbra_server/main_server.cpp -- added RunLockstepLoopback + CLI:
  --lockstep-loopback [--lockstep-delay-input N] [--lockstep-corrupt-tick T] [--lockstep-dump P]
Two ServerWorldRunners (host=client0, peer=client1), same seed/preset, each stepping the
real world via RunFixedTicks(1); hooks: collect=empty, apply=RunFixedTicks(1),
capture=ComputeWorldHashAndSubHashes. Emits luminumbra.lockstep_loopback.v1 artifact.

VERIFIED (build/debug, default workers):
- Scenario clean (--ticks 90): PASS, end_hash=2fa007951a21e140 (CANONICAL, host==peer),
  max_horizon=3 late_inputs=0 -> lockstep does NOT perturb the sim.
- Scenario delay (--lockstep-delay-input 8): PASS, stays in sync, end_hash canonical,
  max_horizon GREW to 9, late_inputs=6 -> adaptive horizon ABSORBED the jitter, no desync.
- Scenario corrupt (--lockstep-corrupt-tick 30): PASS, oracle HALTED at tick 30
  section=terrain, LREC1 dump emitted (740 bytes). Dump re-reads via existing --replay
  path and is detected as a divergence (exit 1, replay_divergence.v1) -> valid repro artifact.

Next: validator modes LockstepLoopback + LockstepFaultInjection (OFF the default All lane,
like HeadlessServerTick/ReplayRoundtrip), register in ValidateSet + switch + help. Then
full ctest + the existing HeadlessServerTick/ReplayRoundtrip/ReplayDivergence/SimDeterminismLint
re-run (hash must stay 2fa007951a21e140).

## 2026-06-14 -- Milestone 4: validator gates (Part 3) LANDED + GREEN

File: .forge/scripts/validate-engine-frontier.ps1 -- added Test-LockstepLoopback +
Test-LockstepFaultInjection, registered in ValidateSet + -Mode switch (OFF the default
All lane, like the other headless-server modes). Note: this env is Windows PowerShell 5.1
(no pwsh); invoke the script directly with `& .forge/scripts/validate-engine-frontier.ps1`.

VERIFIED:
- -Mode LockstepLoopback: PASS (90 ticks, 2 peers in sync, end_hash=2fa007951a21e140
  canonical host==peer, max_horizon=3 late_inputs=0).
- -Mode LockstepFaultInjection: PASS -- (1) horizon ABSORBED delayed input (max_horizon=9,
  late_inputs=6, end_hash canonical, no desync); (2) real STATE divergence HALTED at tick 30
  (section=terrain) + LREC1 dump that REPLAYS to a divergence (oracle not vacuous).

Next: full ctest (189+) + re-run HeadlessServerTick/ReplayRoundtrip/ReplayDivergence/
SimDeterminismLint to confirm hash 2fa007951a21e140 unchanged + no perturbation.

## 2026-06-14 -- Milestone 5: FULL VERIFICATION GREEN -- task COMPLETE

Full ctest: 100% of 196 tests passed, 0 failed (was 188; +7 LockstepSession tests +1).
Existing regression gates re-run, all GREEN, hash UNCHANGED:
- HeadlessServerTick: world_hash=2fa007951a21e140 (sub: terrain=9e1b9316d5eeca32
  mesh=812c3bb1c19b127a water=ed4265f8b090adf7 entities=5735a5094c1e92a8).
- ReplayRoundtrip: end_hash=2fa007951a21e140 (recording hash-neutral), 3 checkpoints.
- ReplayDivergence: caught corrupt checkpoint at tick 30 section=terrain (exit 1, clean).
- SimDeterminismLint: 39 files, 0 new violations (4 allowlist sites; net code lint-clean).
- EngineGameSplitLint: 172 files, 0 game-noun violations (net files engine-generic).
New gates GREEN:
- LockstepLoopback: 90 ticks 2 peers in sync, end_hash=2fa007951a21e140 host==peer.
- LockstepFaultInjection: horizon absorbed delay (max_horizon=9), real divergence HALTED
  at tick 30 + LREC1 dump replays-to-divergence.

ALL FOUR PARTS COMPLETE. No golden/baseline re-blessed. Do NOT commit (orchestrator commits).

Files touched (final):
- NEW src/luminumbra_common/net/LockstepSession.h
- NEW src/luminumbra_common/net/LockstepSession.cpp
- NEW test/common/LockstepSession_test.cpp
- src/luminumbra_common/sources.cmake (register LockstepSession.cpp)
- src/luminumbra_common/CMakeLists.txt (link ws2_32 under WIN32)
- test/CMakeLists.txt (register LockstepSession_test.cpp in common_tests)
- src/luminumbra_server/main_server.cpp (RunLockstepLoopback + --lockstep-* CLI)
- .forge/scripts/validate-engine-frontier.ps1 (LockstepLoopback + LockstepFaultInjection modes)
