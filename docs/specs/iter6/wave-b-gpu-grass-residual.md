# T-I6-010 GPU Grass Residual Render

## Scope

Finish the residual render-side work for the T-I6 GPU grass scatter path.

## Requirements

- Keep foliage render-only: no sim state writes and no world-hash inputs.
- Keep CPU fallback behavior intact when compute shader compilation or GPU scatter setup fails.
- Let the GPU scatter pass produce the command needed to draw the generated grass blades directly.
- Preserve the CPU-side instance mirror used by foliage gates and diagnostics.
- Keep shader and renderer changes limited to the foliage scatter path.

## Implementation Notes

- `grass_scatter.comp` now writes a bounded `DrawArraysIndirectCommand` beside the append counter.
- `FoliagePass` initializes and binds the combined count/draw buffer for both compute and indirect draw use.
- The GPU path still reads back the bounded instance count and generated records after rebuilds so existing gate hooks remain valid.

