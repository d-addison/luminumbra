# Known limitations

An honest inventory of the engine's current boundaries. Each item is a real,
verifiable property of the source tree today, not a roadmap promise. Interfaces
and content formats may still change.

## Simulation and world

> This world predates the v0.3.0 format and cannot be opened. Create a new world; migration is not supported.

- **v0.3.0 breaks saved-world compatibility.** Create a new world. Current saves
  use LMR1 v2, the `luminumbra.persistence.world_manifest.v1` schema declaring
  container version 2, and FSD2 v3. Explicit preset revisions must be 6; in-memory
  callers may omit the revision. Shaping is unconditional and enabled caves use
  the noise router; content, hydro and biome controls remain. Retired selectors
  are rejected regardless of value. Future-format and corruption errors remain
  distinct from this obsolete-world refusal. See the
  [format contract](engine-guide.md#world-format-compatibility).
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
- **Physics acoustic materials use a limited classifier.** Raycasts return real
  Jolt body surface normals oriented against the ray. With a world attached,
  terrain hits use its voxel/biome-derived surface materials and dynamic bodies
  use Stone. Height-band fallback applies only to unattached physics instances.
  These normals and material absorption feed audio occlusion; they do not imply
  detailed acoustic materials for every dynamic object.

Plants, server avatars/creature rosters, perception/awareness, scent and instinct
systems are implemented. Their component/config opt-ins and empty default
rosters should not be confused with missing simulation wiring.

## Networking

- **The Steamworks transport is not compiled or exercised in CI.** The
  GameNetworkingSockets (GNS) CI lane builds `luminumbra_common`, including the
  shared `NetSocketsTransport`, against pinned redistributable upstream source.
  That implementation honours `FrameDelivery::Unreliable` for both GNS and
  Steam sockets. Compile coverage is distinct from a live multi-client transport
  test. Steamworks is different: its SDK is
  owner-provided, is not redistributable, and `vendor/steamworks/` is
  gitignored, so a hosted runner cannot fetch the required Steam headers and
  libraries. A project-owned stub would have to reproduce the SDK declarations
  used by `SteamNetworkingTransport.cpp`; besides creating a redistribution
  concern, such a duplicate could drift from the real SDK and would not prove
  compatibility with it.

  Before trusting a Steam-enabled build, a maintainer with a licensed SDK must
  compile the real transport and run the non-manual suite locally (replace the
  path with the SDK root containing `public/steam/steam_api.h`):

  ```sh
  cmake --preset debug -DLUMINUMBRA_ENABLE_STEAM=ON \
    -DLUMINUMBRA_STEAM_SDK_DIR=/absolute/path/to/steamworks/sdk
  cmake --build --preset debug
  ctest --preset debug --output-on-failure -LE manual
  ```

  This catches compile and link drift against that installed SDK, but the
  standard test suite still does not exercise Steam's live service or an
  over-the-wire Steam multi-client session.

## Client and audio

- **Audio recordings are external.** Bank metadata is public; the referenced
  recordings are optional. Synthetic silent-WAV fixtures already cover bank
  paths, event literals and species-call references. Tests also cover mixer and
  environmental math, volume readback math and physics-backed occlusion. This
  does not validate external recording quality, audio-device output or a complete
  reverb pipeline. Headless operation and `--no-audio` use the null backend.
- **Diligent is not a shipping render backend.** Its opt-in test targets run in
  the required software GL/Vulkan CI bring-up and calibration-cube parity lane.
  OpenGL remains the maintained shipping renderer; this coverage is not full
  scene/render-graph equivalence across backends.
- **Motion history is incomplete for independently moving objects.** Camera
  reprojection and instanced foliage wind sway already produce motion vectors
  consumed by TAAU. Previous rigid-object transforms and previous skinned bone
  poses are the remaining history gap.
- **Tree impostors require a successful atlas bake.** They default on at startup;
  `LUMIN_TREE_IMPOSTORS=0` disables them. See the
  [environment controls](engine-guide.md#environment-controls) for exact parsing.

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
