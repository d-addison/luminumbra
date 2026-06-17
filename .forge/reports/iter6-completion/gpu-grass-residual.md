# GPU Grass Residual Render

## Completed

- Added a compute-written indirect draw command to the GPU grass scatter count buffer.
- Switched the GPU-active foliage render path to `glDrawArraysIndirect` while leaving the CPU fallback on `glDrawArraysInstanced`.
- Kept the CPU mirror read-back path for foliage gate hooks and diagnostics.
- Added command-buffer memory visibility for the compute-to-draw transition.

## Verification

- `git diff --check -- .forge/specs/iter6/wave-b-gpu-grass-residual.md src/luminumbra_client/rendering res/shaders .forge/reports/iter6-completion/gpu-grass-residual.md` passed.
- Full build not run in this scoped worktree.
