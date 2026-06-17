# Iteration 6 Wave C: Multi-Anchor Streaming And Server Scale

## Task

`T-I6-043-multi-anchor-server-scale`

Make headless server world streaming scale beyond the single spawn anchor while
preserving the historical single-anchor path.

## Requirements

- Server runtime streams around the active player-avatar positions when avatars
  exist; avatar count zero keeps the spawn-anchor behavior.
- World streaming treats the wanted set as the union of all active anchors and
  dedupes shared chunks deterministically.
- Multi-anchor pressure is bounded by the global active-chunk budget so 20+
  spread-out players receive fair local area-of-interest coverage instead of an
  unbounded full render disc per player.
- Under pressure, unloading uses the active target radius rather than the maximum
  render radius so stale far-rim chunks can leave residency.
- Server boot keeps collision-ready terrain under spawned avatars before the
  first authoritative physics tick.
- Single-anchor streaming remains byte-identical unless the existing pressure
  heuristics already reduce radius.

## Implementation Notes

- `SHIELD_WorldSystem::update` already forwards the single-anchor overload into
  the multi-anchor path.
- `update_chunk_activation` computes one deterministic target radius per frame.
  For multiple anchors, the radius is additionally capped by an estimate of the
  global active-chunk budget divided across anchors and the expected surface
  stack depth.
- Generation batch size scales up to 2x for multi-anchor sessions so large union
  backlogs drain without letting one activation monopolize the server tick.
- Eviction uses `target_radius + hysteresis` in XZ, keeping full historical
  behavior when the target radius equals `RENDER_DISTANCE`.
- `ServerWorldRunner::Boot` warms compact, deduped collision horizons for avatar
  anchors not covered by the spawn collision horizon.

## Acceptance

- `--avatars 0` uses the spawn-anchor stream path.
- `--avatars N` uses player avatar positions as streaming anchors.
- Multi-anchor generation and eviction stay bounded by
  `STREAMING_MAX_ACTIVE_CHUNKS_BUDGET`.
- Spawn-clustered avatars avoid duplicate boot horizon work.
- Avatar collision horizons are ready before the first `RunFixedTicks` physics
  update.
