# Luminumbra harness operator quick reference

This is the gate-facing one-page index. The expanded flag catalogue and operational gotchas
remain in [`docs/HARNESS-operator-guide.md`](../HARNESS-operator-guide.md).

Always prepend `C:\msys64\ucrt64\bin` to `PATH`, build `build/debug`, and run CTest serially.
Capture and runtime artifacts belong under `build/debug/test-artifacts/` unless a gate specifies
another output directory.

## Server surface

| Flag | Purpose / proving lane |
|---|---|
| `--smoke` | Fixed-tick run/replay determinism; feeds `HeadlessServerTick`. |
| `--smoke-moving` | Moving streaming anchor; feeds moving-residency determinism. |
| `--record <path>` | Write an LREC1 stream for `ReplayRoundtrip`. |
| `--replay <path>` | Replay LREC1 checkpoints and hashes. |
| `--ticks <n>` | Set the fixed 30 Hz tick budget. |
| `--avatars <n>` | Add deterministic player avatars to smoke/replication fixtures. |
| `--avail-trace` | Emit the per-tick availability-set trace used by residency gates. |
| `--replicate` | Run authoritative server plus an in-process replication client. |
| `--artifact <path>` | Write the machine-readable server gate artifact. |

## Client surface

| Flag | Purpose / proving lane |
|---|---|
| `--frame-scan <out.json>` | Fixed-pose material, coverage, luminance, and frame-health capture. |
| `--scene-config <json>` | Declarative camera/weather/time capture used by visual gates. |
| `--ui-screenshot <screen>` | Deterministic RmlUi fidelity capture. |
| `--render-benchmark <out.json>` | Settled per-pass GPU timing lane for `RenderBudget`. |
| `--play-paths` | Exercise scripted cameras through normal play/streaming paths. |
| `--survey <dir>` | Capture the generated-world POI survey. |
| `--worldgen-graph` | Enable constrained layer-graph authoring/inspection. |
| `--timelapse-frames <n>` | Capture the live simulation sequence (`--timelapse-*` options refine it). |
| `--no-audio` | Use the null audio backend for unattended gates. |
| `--crash-dir <dir>` | Select the crash/report artifact directory. |

## FLIP rule

Cross-run full-frame FLIP on this tree has an observed noise floor near `0.057`, above the
offline tool's `0.05` default. Zero-difference gates must compare two renders from the same
process, frame, and GL context. `RenderParityFrame` and `UpscaleSeamParity` use that in-process
contract; their scale-1 leg must be exactly `0.0`.
