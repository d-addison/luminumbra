# Known limitations

An honest inventory of the engine's current boundaries. Each item is a real,
verifiable property of the source tree today, not a roadmap promise. Interfaces
and content formats may still change.

## Simulation and world

> This world predates the v0.3.0 format and cannot be opened. Create a new world; migration is not supported.

- **v0.3.0 breaks saved-world compatibility.** Create a new world. Current saves
  use LMR1 v2, the `luminumbra.persistence.world_manifest.v1` schema declaring
  container version 2, and FSD2 v3. New packaged presets use revision 7; revision 6
  and omitted revisions retain historical defaults. Saves without an embedded
  packaged preset resolve to frozen revision-6 bytes, including `cave_dense`.
  Shaping is unconditional and enabled caves use
  the noise router; content, hydro and biome controls remain. Retired selectors
  are rejected regardless of value. Future-format and corruption errors remain
  distinct from this obsolete-world refusal. See the
  [format contract](engine-guide.md).
- **Far-LOD terrain omits caves and edits.** Coarse distant tiers render
  surface authority only; caves and player voxel edits are not carried at
  distance. The [SHIELD SDF contract](shield/sdf-contract.md) defines the
  exact tier semantics.
- **Water simulates at 1 m cells by default.** Chunks are 16 m across and the
  live water solver runs a 16×16 grid per chunk, so each cell is 1 m. Broad
  bodies, rain, and terraform coupling resolve well; channels narrower than a
  few cells (a small river's inner bank) are below the solver's spatial
  resolution. Explicit `sim.water_high_res=false` selects Medium's 8×8 grid
  (2 m cells). High's per-edge flow constants are resolution-scaled (derivation in
  `WaterSystem.cpp`), so per simulated step High reproduces Medium's physics — a
  dug pit gains the same physical volume per unit of simulated-chunk-time, within
  a tested band.

  The live solver budget is 4096 cells per tick rather than a fixed chunk count, so
  High shrinks the chunk window (16 instead of 64) to bound solver cell steps
  at uniform resolution. This does not bound the entire update, including scans
  over resident chunks and activity scoring.
  Chunks carrying active edge flux are scheduled ahead of the rotating
  cursor, so active flow fronts advance every tick at either resolution while the
  cursor's share keeps every awake chunk starvation-free; Medium's rotating window
  is unchanged byte-for-byte. What High still costs: first-time grid seeding is
  ~4× more expensive per chunk, and steady-state solver time is a few percent
  higher. First-time seeding is amortized at six chunks per live tick, and grid
  resizes at one chunk per live tick, so mid-run resolution changes converge over
  many ticks by design. Boot settle bypasses the init and solver caps, seeding
  every pending chunk and simulating every awake chunk per update. World load
  migrates all mismatched grids in one pass before the first live tick. Neither
  loading path is bounded by the live solver's cell budget.

  The resolution is baked into the hashed water grids, so every peer in a session
  must match and enabling the flag deliberately changes the world hash.
- **Determinism is pinned for x86 hosts only.** Terrain generation and the
  ambient (wind, weather, aether) fields cap FastNoise at its AVX2 implementation
  on x86, so AVX2 and AVX-512 hosts produce the same world hashes. Non-x86
  hosts (for example ARM with NEON) use FastNoise's other backends, whose
  reciprocal and reciprocal-square-root math can differ from the x86 AVX2 path.
  Bit-identical worlds across architectures are therefore not claimed, and the
  world-hash fixtures are exercised on x86 hosts only. See
  [active regions](active-regions.md) for the investigation of the x86 case.
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

- **Networking backends are mutually exclusive.** Both options default to OFF.
  Use `LUMINUMBRA_ENABLE_GNS=ON` with `LUMINUMBRA_ENABLE_STEAM=OFF` for local/LAN
  play, and `LUMINUMBRA_ENABLE_STEAM=ON` with `LUMINUMBRA_ENABLE_GNS=OFF` for Steam
  qualification. Enabling both is a configuration error; dual-backend support
  is future work. Keep separate build directories for the two configurations.

  GNS uses CONFIG packages for `Protobuf` and `absl`, preserving protobuf's
  transitive dependencies and `PROTOBUF_USE_DLLS`. Linux distribution packages
  without a protobuf CONFIG file use CMake's module fallback for the compile
  probe. Both `USE_CRYPTO` and `USE_CRYPTO25519` default to `OpenSSL` as cache
  values that can be overridden; the OpenSSL Crypto component is required.
  The pinned GNS build requests a static library and disables shared libraries,
  tests and examples through function-local options, preserving caller cache
  settings. Its installed dependencies may still be shared libraries.

  A single `luminumbra_network_runtime` prerequisite stages shared networking
  DLLs beside the server, client and QA client executables, including when only
  one application target is built. It runs once per requested build and
  configuration, scans dependency DLLs with `file(GET_RUNTIME_DEPENDENCIES)` and
  uses `copy_if_different`. Each application's post-build step scans its linked
  executable and verifies the staged DLLs without copying shared files. These
  scans require CMake 3.21 or newer for networked Windows builds; older versions
  fail configuration. The union of all three targets' runtime DLL lists supplies
  additional shared inputs; all inputs and resolved DLLs are deduplicated.
  Visual Studio and Ninja
  Multi-Config use separate input files, stamps and runtime directories per
  configuration. Supplemental package DLLs and OpenSSL search roots select only
  that configuration's imported files, respecting `MAP_IMPORTED_CONFIG_<CONFIG>`
  (including configuration-independent mappings), then the exact configuration,
  generic imported location and first available imported configuration when no
  mapping is set. Linux Steam builds also stage their shared library once.
  MSYS2 imported targets can omit DLL locations and transitive runtime imports,
  so the GNS UCRT64 build also explicitly copies `libprotobuf.dll`,
  `libutf8_validity.dll`, `zlib1.dll`, `libcrypto-3-x64.dll`, every
  Abseil runtime DLL. Compiler runtime candidates follow the MinGW toolchain:
  GCC seeds available `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and
  `libwinpthread-1.dll`; Clang seeds available `libc++.dll`, `libunwind.dll` and
  `libwinpthread-1.dll`. Missing compiler runtimes fail the PE scan when imported,
  rather than requiring GCC DLLs for Clang or an unused winpthreads DLL.
  Package bin directories are derived from protobuf/absl
  CONFIG locations and selected imported library locations, with the configured
  compiler bin directory as fallback. Abseil discovery also searches package bin
  directories for `libabsl_*.dll` without pinning a version, preferring imported DLL
  locations when available. Missing Abseil DLLs or multiple distinct DLL
  versions across that configuration's candidate directories fail configuration.
  Search paths are derived from CONFIG and imported-target locations, including
  OpenSSL targets without DLL metadata. Windows GNS builds without MinGW also seed OpenSSL
  DLLs from those directories and package targets' imported DLL locations.
  MSVC builds seed the compiler redistributables
  (including available debug DLLs) without adding install rules.
  Both backends search the configured compiler's bin directory, so Steam's
  UCRT64 closure also includes `libstdc++-6.dll`,
  `libgcc_s_seh-1.dll` and `libwinpthread-1.dll` when imported. Windows system
  directories and `api-ms-*`/`ext-ms-*` imports are excluded. Unresolved non-system
  imports fail staging or verification with their names and search directories.
  DLLs with the same basename (case-insensitive on Windows) but different contents
  fail with the configuration and conflicting paths before files are copied;
  identical copies are accepted during staging and verification.
  A resolved executable import missing from the shared staging inputs, or with
  different staged contents, also fails verification and must be added to the
  shared inputs before the build can succeed. Both MinGW backends explicitly
  seed available compiler runtime DLLs, including when Steam itself does not
  import them. Empty target DLL lists are valid; missing explicitly required
  GNS DLLs fail configuration. Steam requires both
  `steam_api64.lib` and `steam_api64.dll` in the SDK's `redistributable_bin/win64`
  directory, which is also searched by the scan; its DLL is an additional input.
  These staging steps cover build-tree previews; release packaging still needs
  runtime deployment.

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
  cmake --preset debug -DLUMINUMBRA_ENABLE_STEAM=ON -DLUMINUMBRA_ENABLE_GNS=OFF \
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
  [environment controls](engine-guide.md) for exact parsing.

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
