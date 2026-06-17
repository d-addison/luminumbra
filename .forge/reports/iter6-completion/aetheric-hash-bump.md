# T-I6-020 Aetheric Hash Bump

## Status

Complete. The server runner folds the Aetheric scalar field into the
runner-level composite world hash with an append-only `|aether:` term.

## Hash Outcome

- Pre-A1 baseline: `d950a6afc12a5cdc`
- Post-A1 aether composite: `f17726d44054d133`
- Added sub-hash source: `AetherFieldSystem::ComputeAetherSubHash()`
- Composite order: `chunk`, `wind`, `weather`, `aether`

## Completion Notes

- `ServerWorldRunner.cpp` already contained the aether include, defensive
  `AetherSubHash()` helper, four-argument `ComposeWorldHash()`, both runner call
  sites, and `WorldStreamingStateSubHashes::aether` population at task start.
- `WAVE-A-SPEC.md` now records T-I6-020 as the landed A1 aether bump and carries
  the post-A1 hash.
- `_synthesis.md` now reflects the revised three-step hash sequence:
  Aetheric, shaping-spline params fold, then hydro baked-grid bump.

## Verification

- Static review confirmed both hash paths use the same aether sub-hash:
  `ComputeWorldHash()` and `ComputeWorldHashAndSubHashes()`.
- Static review confirmed `ComputeWorldSubHashes()` and
  `ComputeWorldHashAndSubHashes()` both populate `sub.aether`.
- `git diff --check` passed.
