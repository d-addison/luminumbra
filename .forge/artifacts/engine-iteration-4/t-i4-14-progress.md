# T-I4-14 client-over-transport — progress

## 2026-06-14 — Milestone 1: scenario + driver landed and passing

### What landed
- New engine-generic `NetworkedSessionDriver` (RuntimeScenarioHarness.h/.cpp):
  owns a HOST authority GameSession (headless, in-process) + a LoopbackTransport
  pair + both LockstepSession ends. Per agreed tick it steps BOTH worlds one
  fixed sim tick from the SAME spawn anchor and exchanges world_hash + sub-hashes
  (desync oracle). The client world is the caller's render GameSession, stepped
  via the driver's `apply_and_step` hook. Hash capture mirrors
  ServerWorldRunner::ComputeWorldHashAndSubHashes (snapshot_streamed_chunks ->
  WorldStreamingState -> WorldSaveService::world_hash + sub-hashes).
- Input round-trip: `collect_local_input` returns the EMPTY world-affecting input
  blob (no gameplay inputs yet); it still travels LockstepSession::*Input and is
  handed to apply_and_step. Camera LOOK is NOT collected here.
- main_client.cpp: new `networked_session_smoke` scenario branch. On first ready
  frame it Begins the driver, drains the lockstep to completion (budget 90),
  writes the artifact, and closes. Camera look is applied RENDER-SIDE each frame
  (fixed eye-level framing at the spawn anchor) and never sent through the
  session. The default per-frame TickSimulation + camera-anchored streaming are
  SKIPPED for this scenario (the driver owns the client tick; double-ticking or
  camera-anchored streaming would desync the hash). Research citation in comments.
- Artifact schema `luminumbra.networked_session.v1` written to
  networked-session-analysis.json (ticks, in-sync hash equality, end_hash,
  clean disconnect, per-peer status, gl_debug).

### Files
- src/luminumbra_client/core/RuntimeScenarioHarness.h (driver decl + accessor)
- src/luminumbra_client/core/RuntimeScenarioHarness.cpp (driver impl + hooks)
- src/luminumbra_client/main_client.cpp (scenario branch + default-tick guard + state)

### Verification state
- Build: luminumbra_client_app builds clean.
- Full-radius (12) run: passed=true, host==client agreed_tick=90,
  end_hash=a8ddeccff8ad9a52 (host==client), hash_exchanges=3 (30-tick cadence),
  clean_disconnect=true, gl errors 0. Wall ~282s (too slow for a gate at radius 12).
- IN PROGRESS: re-run at small radius (4/2) for gate-practical wall time. Hash
  equality is radius-invariant (both peers same anchor+radius), so this only
  changes speed, not the in-sync contract.

## 2026-06-14 — Milestone 2: validator mode + full verification COMPLETE

### What landed
- NetworkedSession validator mode in .forge/scripts/validate-engine-frontier.ps1
  (added to the param ValidateSet + the switch; OFF the default All lane, like
  LockstepLoopback/HeadlessServerTick). Runs the scenario at --horizon-radius 4
  --collision-radius 2 (matches the headless server streaming profile => the
  in-sync end hash IS the canonical 2fa007951a21e140), asserts: both peers reach
  budget tick 90, in_sync_every_cadence, host==client end_hash, >= floor(90/30)=3
  cadence hash exchanges, input_round_tripped, camera_look_render_side, clean
  disconnect, schema, AND end_hash == canonical 2fa007951a21e140.

### Verification (all GREEN)
- NetworkedSession gate: PASS. 90 ticks in sync (host==client), end_hash
  2fa007951a21e140 (canonical), 3 cadence exchanges, clean disconnect, gl errors
  0. Wall ~209s (heavy two-world lockstep, comparable to server LockstepLoopback
  ~172s; both are off-lane heavy gates).
- Full ctest: 196/196 passed (0 failed).
- EngineGameSplitLint: PASS (172 engine files, 0 game-noun violations).
- SimDeterminismLint: PASS (39 files, 0 new violations).
- HeadlessServerTick: PASS, world_hash 2fa007951a21e140 (UNCHANGED — transport/
  scenario wiring did not perturb the sim).
- LockstepLoopback: PASS, canonical hash 2fa007951a21e140 (unchanged).
- Visual set unaffected: PlayerView (default/mountains/archipelago, max_sky 0.0,
  void_clusters 0), FarLodHorizon (default/mountains/archipelago, sliver 0px),
  TimeOfDaySweep, CreatureSlice — all PASS with identical metrics. Additive: every
  main_client.cpp edit is gated behind networked_session_smoke().
- No golden/baseline re-blessed.

### Notes / deviations
- The scenario drains the lockstep to completion in one ready frame (each agreed
  tick quiesces both worlds' streaming, expensive in debug; spreading across
  frames blew the run window). One agreed tick is stepped before the first render
  so the render frame draws the settled server-owned world (render-capable, unlike
  the headless server). Camera look applied render-side each frame regardless.
- Default-radius (12) run also passes but at ~282s wall; gate uses radius 4 for
  practicality + canonical-hash alignment.
