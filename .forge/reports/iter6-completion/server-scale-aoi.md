# Server Scale AOI Completion Report

## Task

`T-I6-043-multi-anchor-server-scale`

## Completed

- Added budget-aware radius capping for multi-anchor world streaming.
- Scaled generation scheduling for multi-anchor backlogs up to a bounded 2x
  dispatch size.
- Changed pressure unloading to use the active target radius with hysteresis,
  allowing stale far-rim chunks to leave residency under server load.
- Added compact boot-time avatar collision horizon warm-up in
  `ServerWorldRunner`.
- Preserved the zero-avatar spawn-anchor path.

## Behavioral Contract

- Single-anchor sessions retain the existing radius decisions.
- Multi-anchor sessions keep deterministic chunk ordering through sorted,
  deduped candidate generation.
- Chunk content generation remains unchanged; only residency, priority, and boot
  warm-up scope changed.
- Avatar entity hashes remain driven by `BuildAvatarEntitySnapshot`; world hash
  changes only when streamed residency changes for multi-anchor sessions.

## Verification

- Source files compile against existing APIs: no public header changes required.
- Reported AOI behavior is implemented in the assigned files only.
