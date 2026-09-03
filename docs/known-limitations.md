# Known limitations

An honest inventory of the engine's current boundaries. Each item is a real,
verifiable property of the source tree today, not a roadmap promise. Interfaces
and content formats may still change (see the README's project status).

## Simulation and world

- **Far-LOD terrain omits caves and edits.** Coarse distant tiers render
  surface authority only; caves and player voxel edits are not carried at
  distance. The [SHIELD SDF contract](shield/sdf-contract.md) defines the
  exact tier semantics.
- **Water simulates at 2 m cells by default.** Chunks are 16 m across and the
  live water solver runs an 8×8 grid per chunk, so each cell is 2 m. Broad
  bodies, rain, and terraform coupling resolve well; channels narrower than a
  few cells (a small river's inner bank) are below the solver's spatial
  resolution. The experimental `sim.water_high_res` flag switches a session to
  a 16×16 grid (1 m cells) at the cost of a proportionally smaller simulation
  window; its flow constants are untuned for the finer grid, so surface flow
  propagates more slowly. Grid resizes are amortized one chunk per tick during live
  play (world load migrates all chunks at once), so mid-run resolution changes
  converge over many ticks by design.
- **Physics surface queries are placeholders.** Raycasts currently report a
  fixed upward surface normal, and surface material classification derives
  from world height bands rather than actual voxel material. Audio
  occlusion/absorption consumes both, so acoustic detail inherits these
  approximations.

## Networking

- **TCP lockstep loopback is the exercised transport.** The GameNetworkingSockets
  and Steam transport implementations require external SDKs and build flags
  that are not yet wired as first-class CMake options, and no over-the-wire
  multi-client test runs in CI (a manual soak test exists).

## Client and audio

- **Audio binaries are external.** Sound banks are intentionally not part of
  the public tree; headless and CI runs use the null audio backend, and test
  coverage of audio is limited to the pure math models (mixer, environmental
  propagation).
- **The Diligent/RHI second render backend is opt-in** and not exercised by
  any CI lane; the OpenGL path is the maintained backend.

## Testing and CI

- **Windows CI lanes run without a GL context.** Rendering, UI, and
  GPU-dependent suites skip on the hosted Windows runners under an explicit,
  audited allow-list (`tools/ci/check_ctest_junit.py` fails the lane on any
  skip reason outside it). Software-GL rendering coverage runs on Linux.
- **Engine performance measurement blocks merges on Linux only.** Windows and
  macOS currently exercise the portable runner contract; see
  [performance measurement](performance.md) for the gate's design.

## Packaging

- **No stable SDK.** The build does not yet install a versioned public SDK;
  the supported extension paths are documented in the
  [engine guide](engine-guide.md). Binary releases are dormant by policy —
  releases are source-only.
