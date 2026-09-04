# Known limitations

An honest inventory of the engine's current boundaries. Each item is a real,
verifiable property of the source tree today, not a roadmap promise. Interfaces
and content formats may still change.

## Simulation and world

- **Far-LOD terrain omits caves and edits.** Coarse distant tiers render
  surface authority only; caves and player voxel edits are not carried at
  distance. The [SHIELD SDF contract](shield/sdf-contract.md) defines the
  exact tier semantics.
- **Water simulates at 2 m cells by default.** Chunks are 16 m across and the
  live water solver runs an 8×8 grid per chunk, so each cell is 2 m. Broad
  bodies, rain, and terraform coupling resolve well; channels narrower than a
  few cells (a small river's inner bank) are below the solver's spatial
  resolution. The `sim.water_high_res` flag switches a session to a 16×16 grid
  (1 m cells). Its per-edge flow constants are resolution-scaled (derivation in
  `WaterSystem.cpp`), so per simulated step High reproduces Medium's physics — a
  dug pit gains the same physical volume per unit of simulated-chunk-time, within
  a tested band.

  The per-tick budget is expressed in cells rather than chunks, so a higher
  resolution shrinks the chunk window (16 instead of 64) to hold per-tick work
  constant. Chunks carrying active edge flux are scheduled ahead of the rotating
  cursor, so active flow fronts advance every tick at either resolution while the
  cursor's share keeps every awake chunk starvation-free; Medium's rotating window
  is unchanged byte-for-byte. What High still costs: first-time grid seeding is
  ~4× more expensive per chunk, and steady-state solver time is a few percent
  higher. Grid resizes are amortized one chunk per tick during live play (world
  load migrates all chunks at once), so mid-run resolution changes converge over
  many ticks by design.

  The resolution is baked into the hashed water grids, so every peer in a session
  must match and enabling the flag deliberately changes the world hash.
- **Physics surface queries are placeholders.** Raycasts currently report a
  fixed upward surface normal, and surface material classification derives
  from world height bands rather than actual voxel material. Audio
  occlusion/absorption consumes both, so acoustic detail inherits these
  approximations.

## Networking

- **TCP lockstep loopback is the exercised transport.** The GameNetworkingSockets
  and Steam transport implementations are available through the
  `LUMINUMBRA_ENABLE_GNS` and `LUMINUMBRA_ENABLE_STEAM` CMake options. Their SDK
  inputs are owner-provided and not redistributed; no CI job compiles either
  transport, and no over-the-wire multi-client test runs in CI (a manual soak
  test exists).

## Client and audio

- **Audio binaries are external.** Sound banks are intentionally not part of
  the public tree; headless and CI runs use the null audio backend, and test
  coverage of audio is limited to the pure math models (mixer, environmental
  propagation).
- **The Diligent/RHI second render backend is opt-in** and not exercised by
  any CI lane; the OpenGL path is the maintained backend.

## Testing and CI

- **Windows CI uses a software GL context.** Both hosted Windows lanes stage
  Mesa3D llvmpipe before running the rendering, UI, and GPU-dependent suites.
  `tools/ci/check_ctest_junit.py` makes any GL-related skip a hard failure, so
  software-GL rendering coverage runs on Windows as well as Linux.
- **Engine performance measurement blocks merges on Linux only.** Windows and
  macOS currently exercise the portable runner contract; see
  [performance measurement](performance.md) for the gate's design.

## Packaging

- **No stable SDK.** The build does not yet install a versioned public SDK;
  the supported extension paths are documented in the
  [engine guide](engine-guide.md). Binary releases are dormant by policy —
  releases are source-only.
