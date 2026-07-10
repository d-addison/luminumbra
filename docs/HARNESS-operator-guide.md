# Harness Operator Guide

The gate-facing one-page index is [`docs/operability/harness.md`](operability/harness.md).
This document is the expanded catalogue for operators who need the complete flag surface.

Operator reference for the headless/client capture and server harness CLI. Verified against
`src/luminumbra_client/main_client.cpp`, `src/luminumbra_client/core/RuntimeScenarioHarness.cpp`,
and `src/luminumbra_server/main_server.cpp`.

## Client harness flags

| Flag | Purpose | Feeds | Gotcha |
| --- | --- | --- | --- |
| `--frame-scan <out.json>` | Boots an auto-world, pins the fixed forest-dense pose, writes a material/water/foliage/luminance scan plus adjacent PPM. | RenderHealth / frame inspection gates. | Implies `--auto-create-world` and `--auto-enter-world`; uses a 90-frame settle and a watchdog. |
| `--render-parity-finalblit <dir>` | Captures the FinalBlit in-process A/B pair under `<dir>`. | Render pass parity / 016 pass-contract checks. | Reuses the frame-scan boot/settle and writes `parity_scan.json`. |
| `--render-parity-frame <dir>` | Captures whole-frame same-frame A/B parity. | `RenderParityFrame` / RENDER-11 migration safety. | In-process FLIP must be exactly `0.0`; cross-run full-frame FLIP is not the contract. |
| `--render-parity-ssao <dir>` | Captures SSAO ctx-mapping/seam parity. | SSAO pass migration parity. | Also reuses frame-scan boot/settle and `parity_scan.json`. |
| `--render-benchmark <path>` | Writes averaged per-pass GPU timer JSON for the fixed forest-dense pose. | `RenderBudget`. | Pair with `--auto-create-world --auto-enter-world`; the gate forces cloud/SSAO env tiers. |
| `--render-benchmark-frames <n>` | Sets measured frames after warm-up. | `RenderBudget`. | Default is 120 in the parser; gate currently passes 150. |
| `--render-benchmark-warmup <n>` | Sets settle/warm-up frames before measuring. | `RenderBudget`. | Default is 60. |
| `--render-benchmark-screenshot <path>` | Dumps a PPM on the final measured benchmark frame. | Benchmark pose audit. | Empty means no screenshot. |
| `--debug-view <albedo|normal|depth|material|position>` | Selects a render-only G-buffer overlay. | Visual/debug captures. | Invalid names silently leave overlay off. |
| `--debug-goto <cave|doline|spawn>` | Finds a deterministic feature after world load and frames it. | Debug captures / timelapse setup. | Intended with auto-world and capture paths. |
| `--play-paths` | Runs scripted scenario camera motion through normal-play render/streaming paths. | Moving-profile diagnostics. | Diagnostic only; avoids gate-mode readback/cache behavior. |
| `--profile-fly <seconds>` | Drives the player forward in normal-play mode and exits after the duration. | Moving cost / SLOWFRAME profiling. | Pair with `--auto-create-world --auto-enter-world --no-audio`; `0` disables it. |
| `--cam-pos x,y,z` | Pins the camera position every frame. | Reproducible screenshots, benchmarks, scenes. | Keep the pose near streamed chunks. |
| `--cam-yaw <deg>` | Pins camera yaw with `--cam-pos`. | Reproducible captures. | Parse errors are ignored. |
| `--cam-pitch <deg>` | Pins camera pitch with `--cam-pos`. | Reproducible captures. | Parse errors are ignored. |
| `--scene-config <json>` | Loads declarative scene JSON: camera/fov, time of day, moon, weather, clouds, screenshot. | Reference-driven scene capture. | Writes PPM; drives a single-frame capture path and creates the screenshot directory. |
| `--survey <dir>` | Tours generated-world POIs and writes screenshot/frame-scan artifacts. | Scene survey / visual audit. | Operator comment says pair with `--auto-create-world --auto-enter-world --no-audio`. |
| `--bake-tree-impostor <out.ppm>` | Bakes the far-field tree impostor atlas. | Far-field/impostor asset workflow. | No world needed; runs on the first GL frame then exits. |
| `--ui-screenshot <screen[,screen...]>` | Loads `data/ui/<screen>.rml`, settles layout, writes `ui-<screen>.ppm`. | UI fidelity gate. | Comma-separated screens run in one window session; default output dir is `references/compare`. |
| `--ui-screenshot-dir <dir>` | Output directory for UI screenshots. | UI fidelity gate. | This is the parsed output flag. The source comment mentions `--ui-screenshot-out`, but no parser read was found. |
| `--ui-fixtures` | Points capture-backed UI screens at deterministic fixture data. | UI screenshot reproducibility. | Currently used for gallery/settings-style fixture data. |
| `--timelapse-frames <n>` | Enables timelapse capture for `n` frames. | Timelapse/video tooling. | Pair with `--auto-create-world --auto-enter-world`; use `--no-ui` for clean frames. |
| `--timelapse-ticks <n>` | Advances fixed sim ticks between captured frames. | Timelapse speed. | Default is 60 ticks per frame. |
| `--timelapse-dir <dir>` | Output directory for `frame_####.ppm`. | Timelapse artifacts. | Defaults to `<runtime-artifact-dir>/timelapse` when a runtime artifact dir exists, else `timelapse`. |
| `--timelapse-daystep <f>` | Advances time-of-day per captured frame. | Lighting/sky drift captures. | `0` leaves time of day fixed. |
| `--timelapse-tod <f>` | Sets starting time of day. | Timelapse/scene captures. | `0` is the brightest/noon convention in this path. |
| `--timelapse-grow` | Grows procgen plants from seed to tree over capture. | Foliage timelapse. | Re-bakes foliage as the stage changes. |
| `--timelapse-simgrow` | Seeds sim plants and grows them via the real growth tick. | Sim-plant bridge capture. | Rebuilds live sim plants each captured frame. |
| `--timelapse-season` | Drifts summer to autumn leaf color over capture. | Seasonal foliage capture. | Re-bakes on season change. |
| `--timelapse-creatures` | Spawns predator/prey markers for ecology capture. | Ecology timelapse. | Also enables debug creature overlay when UI is active. |
| `--timelapse-living` | Captures the living world with ambient creatures and forager colony. | Ecology/scent/rest-pose capture. | Uses live world state during timelapse. |
| `--timelapse-calm` | Runs a calm grazing/reproduction creature scenario. | Ecology evolution demo. | Implies `--timelapse-creatures`. |
| `--timelapse-fire` | Spawns/ignites combustible foliage for fire spread. | Sim.fire visual demo. | Render/demo path only. |
| `--timelapse-dig` | Progressively carves a trench mid-capture. | Terraform timelapse. | Starts after an establishing hold. |
| `--timelapse-drain` | Breaches a water bank and captures finite drainage. | Hydrology timelapse. | Anchors on a real river/lake. |
| `--timelapse-rain` | Rains, then dries, while the finite water state is captured. | Hydrology/rain demo. | Combine with `--timelapse-rain-mm` for rate. |
| `--timelapse-rain-mm <n>` | Rain rate for the rain timelapse. | Hydrology/rain demo. | Default is 18. |
| `--auto-create-world` | Creates an automated test world. | Most client capture/scenario workflows. | Some flags imply it (`--frame-scan`, render parity, scenario smoke modes, boot metrics). |
| `--auto-enter-world` | Bypasses loading UI into the created/loaded world. | Most headless captures. | Needed when a capture must reach `IN_GAME` unattended. |
| `--no-audio` | Uses `NullAudioManager`. | Scenario/capture gates. | Audio telemetry still computes where configured; playback is suppressed. |
| `--no-ui` | Suppresses UI manager/ImGui paths. | Clean captures and scenarios. | Useful for timelapse and render captures; UI screenshot obviously needs UI enabled. |
| `--ui-hot-reload` | Enables the 1s UI filesystem poller. | UI authoring. | Opt-in only; off during normal/gate runs. |
| `--runtime-boot-metrics` | Records runtime boot metrics and implies automated boot. | Runtime stability/perf telemetry. | Also participates in hidden-window selection. |
| `--runtime-boot-frames <n>` | Number of boot frames to record. | Runtime boot metrics. | Default is 300. |
| `--runtime-boot-output <dir>` | Output directory for boot metrics. | Runtime boot metrics. | Defaults under `build/debug/test-artifacts/performance`. |
| `--scenario <name>` | Selects a runtime scenario such as visual, persistence, networked session, or slice smoke. | `validate-engine-frontier.ps1` scenario lanes. | Scenario smoke names imply auto-create/auto-enter for the known smoke modes. |
| `--timed-run <seconds>` | Limits scenario play duration. | Runtime scenario lanes. | Defaults depend on scenario; `0` means no timed-run completion gate. |
| `--horizon-radius <chunks>` | Streaming/readiness horizon radius. | Runtime scenario readiness and networked-session smoke. | Parsed in the runtime harness, not the main-client local block. |
| `--collision-radius <chunks>` | Collision readiness radius. | Runtime scenario readiness and networked-session smoke. | Also exists on the server with a similar meaning. |
| `--runtime-artifact-dir <dir>` | Scenario artifact root. | Runtime scenario JSON/screenshots/analysis. | Defaults under `build/debug/test-artifacts/runtime`. |

## Server harness flags

| Flag | Purpose | Feeds | Gotcha |
| --- | --- | --- | --- |
| `--smoke` | Runs the in-process double-run determinism smoke and writes the server tick artifact. | `HeadlessServerTick`, determinism lanes. | Asserts run/replay hash equality. |
| `--smoke-moving` | Drifts the streaming anchor each tick. | Moving-residency determinism. | Implies `--smoke`. |
| `--heavy` | Runs save/load/resim heavy oracle. | `HeadlessServerTickHeavy`, populated/heavy checks. | Can be combined with smoke/populated rosters. |
| `--heavy-resim <n>` | Resim ticks for heavy mode. | Heavy determinism. | Default is 30. |
| `--wind-bench` | Runs wind-field determinism/perf bench. | WindFieldDeterminism. | Parser flag is server-only. |
| `--weather-bench` | Runs weather determinism/perf bench. | Weather/atmosphere determinism. | Server-side only. |
| `--aether-bench` | Runs aether-field determinism/perf bench. | AetherFieldDeterminism. | Server-side only. |
| `--avatars <n>` | Spawns deterministic player avatars. | Smoke, replicate, visual avatar scenarios. | Server parser default is 0; client runtime parser clamps 0..32 for visual scenarios. |
| `--ecology-roster` | Spawns the fixed deterministic creature roster. | PopulatedWorldReplay. | Smoke asserts the ecology sub-hash too. |
| `--planted-roster` | Spawns deterministic plants. | Populated replay / plant determinism. | Usually paired with ecology roster. |
| `--avail-trace` | Adds per-tick availability-set trace to smoke artifact. | MovingResidency / determinism audit. | Implies `--smoke`. |
| `--water-hash-trace` | Adds per-tick water-state hash trace. | WaterCrossBuild determinism matrix. | Implies `--smoke`. |
| `--replicate` | Runs authoritative server plus in-process loopback replication client. | ReplicationSmoke. | Uses loopback, not real sockets. |
| `--npcs <n>` | Adds replicated NPCs in `--replicate`. | Replication heterogeneity. | Only meaningful with replication paths. |
| `--arrow` | Fires one server-authoritative replicated arrow. | Replication typed/despawn checks. | Only meaningful with `--replicate`. |
| `--net-host` | Starts the real TCP host/listener path. | NetworkedReplication. | The parser has `--net-host`; no `--net-listen` parser was found. |
| `--net-join` | Starts the real TCP join/client path. | NetworkedReplication. | Use with `--host` and `--port`. |
| `--host <addr>` | Hostname/IP for `--net-join`. | NetworkedReplication. | Default is `127.0.0.1`. |
| `--port <n>` | Base TCP/GNS port. | NetworkedReplication / soak. | Default is 27015; multi-client joiners use offsets by `--player-id`. |
| `--clients <n>` | Number of clients expected/accepted. | Networked host/server-mode/soak. | Clamped to at least 1. |
| `--player-id <id>` | Client id for join/soak client. | NetworkedReplication / soak. | Clamped to at least 1; also selects per-client port offset. |
| `--server-mode` | Dedicated host mode that ticks immediately and accepts late clients. | Dedicated server runtime. | Used with `--net-host`. |
| `--net-soak` | Authoritative multi-process TCP soak server. | Networking scale-out soak. | Off the default frontier lane. |
| `--net-soak-client` | Driving avatar soak client. | Networking scale-out soak. | Reconnects according to `--soak-cycles`. |
| `--soak-cycles <n>` | Reconnect cycles for soak client. | Networking reconnect soak. | Clamped to at least 1; default is 2. |
| `--steam` | Uses Steam networking sockets transport. | Optional real UDP transport. | Requires `LUMINUMBRA_ENABLE_STEAM=ON` and Steam runtime support. |
| `--udp` | Uses standalone GameNetworkingSockets transport. | Optional real UDP transport. | Requires `LUMINUMBRA_ENABLE_GNS=ON`. |
| `--lockstep-loopback` | Runs a pair of lockstep peers over in-process loopback. | LockstepLoopback. | No sockets/ports. |
| `--lockstep-delay-input <n>` | Fault injection: peer 1 withholds inputs for first `n` agreed ticks. | LockstepFaultInjection. | Tests adaptive horizon tolerance. |
| `--lockstep-corrupt-tick <t>` | Fault injection: corrupt peer 1 hashes from tick `t`. | LockstepFaultInjection. | Must halt and dump at the divergence tick. |
| `--lockstep-dump <path>` | Output path for lockstep dump/record. | Lockstep fault artifacts. | Useful with corrupt-tick. |
| `--record <path>` | Records an LREC1 replay stream. | ReplayRoundtrip. | Captures inputs and hash checkpoints. |
| `--replay <path>` | Replays an LREC1 stream. | ReplayRoundtrip. | Verifies live hashes against checkpoints. |
| `--mutate-replay-fixture <path>` | Rewrites an LREC1 stream with one corrupted checkpoint. | ReplayDivergence. | In-process mutation helper for the negative oracle. |
| `--ticks <n>` | Number of fixed 30 Hz server ticks. | Most server gates. | Default is 90. |
| `--radius <chunks>` | Surface streaming radius. | Server world runner. | Default is 4. |
| `--collision-radius <chunks>` | Collision streaming radius. | Server world runner. | Default is 2. |
| `--autosave-ticks <n>` | Autosave cadence. | Default server run / persistence. | `0` disables autosave cadence. |
| `--seed <seed>` | World seed. | Determinism and default boot. | Default is `424242`. |
| `--world-id <id>` | Existing world/save id. | Default server boot. | Empty means fresh/preset path. |
| `--preset <name>` | World preset. | Default server boot and smoke setup. | Default is `default`. |
| `--root <path>` | Runtime root override. | Asset/world preset resolution. | Otherwise the server walks ancestors for world presets. |
| `--artifact <path>` | Writes the gate artifact JSON. | Most server validation lanes. | Many gates assert this file after the process exits. |

## FLIP noise rule

Per-run full-frame FLIP is noisy on this tree: same-tree captures have observed floors around
`0.057`, even with TAAU off. Treat only same-frame, in-process A/B parity as the zero-noise
contract; those scores are expected to be exactly `0.0`.
