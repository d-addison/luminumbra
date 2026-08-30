# Luminumbra

[![License: MIT](docs/assets/badges/license-mit.svg)](LICENSE)
![Language: C++](docs/assets/badges/language-cpp.svg)
![Standard: C++20](docs/assets/badges/standard-cpp20.svg)
![Platforms: Windows and Linux](docs/assets/badges/platforms.svg)

Luminumbra is a C++20 voxel engine for a living simulated world. It combines an
EnTT-based ECS, GPU-driven rendering, and a deterministic fixed-tick simulation
so world generation, ecology, weather, and field systems can evolve together.

## Build

### Requirements

| Requirement | Repository source of truth |
|---|---|
| CMake 3.20 or newer | `CMakeLists.txt` and `CMakePresets.json` |
| Ninja | The base configure preset selects the Ninja generator |
| A C++20-capable compiler | The root CMake project requires C++20 without extensions |
| Git and network access during configuration | The GoogleTest submodule and pinned `FetchContent` dependencies are populated from upstream repositories |

On Ubuntu, the CI build installs the following system packages:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  libgl1-mesa-dev \
  libglu1-mesa-dev \
  pkg-config \
  xorg-dev
```

### Clean checkout

Clone with submodules, or initialize them before configuring. From the
repository root, the same release sequence used by CI is:

```sh
git submodule update --init --recursive
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

The presets keep each configuration in its own directory under `build/`.

| Purpose | Configure preset | Build preset | Test preset |
|---|---|---|---|
| Debug | `debug` | `debug` | `debug` |
| Debug with AddressSanitizer | `debug-asan` | `debug-asan` | `debug-asan` |
| Release | `release` | `release` | `release` |
| Coverage | `coverage` | `coverage` | `coverage` |
| Simulation optimization-parity build | `debug-simo0` | `debug-simo0` | — |

For example, a normal development build uses:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

## Architecture

Luminumbra separates deterministic world state from presentation. The shared
engine owns ECS data, simulation, world generation, networking, replay, and
persistence; the client consumes that state through the rendering and UI
layers; the server advances the authoritative fixed-tick world.

### Four world pillars

| Pillar | Responsibility | Grounded implementation |
|---|---|---|
| **SHIELD** | Procedural voxel/SDF world generation, chunk streaming, meshing, and persistence | `SHIELD_WorldSystem`, Marching Cubes, far-LOD storage, and world persistence under `src/luminumbra_common/` |
| **Instinct** | Creature perception, planning, locomotion, needs, and ecology | EnTT components plus the planners and tick systems under `src/luminumbra_common/ai/` |
| **Atmospheric** | Deterministic weather and wind state that can feed simulation and rendering | `WeatherSystem`, `WeatherEventSystem`, and `WindFieldSystem` |
| **Aetheric** | A persistent scalar energy field with emitters, diffusion, and deterministic hashing | `AetherFieldSystem`, `FieldEmitterSystem`, and the field-grid implementation under `src/luminumbra_common/fields/` |

### Engine foundations

| Area | Design |
|---|---|
| **ECS and simulation** | EnTT stores entity state; systems advance it on the canonical 30 Hz `SimulationClock`. Simulation paths use stable ordering and deterministic state hashes for replay and desynchronization checks. |
| **Rendering** | A client-side render graph coordinates deferred passes. Terrain uses indirect multi-draw submission, while compute paths support SDF generation, foliage scattering, and other GPU work. |
| **Concurrency** | A first-party job system handles asynchronous work while activation and residency contracts keep timing-dependent render work out of deterministic simulation state. |
| **Persistence and replay** | Common-engine modules serialize world state, record replay streams, and expose sub-hashes for deterministic verification. |
| **Testing** | CTest covers common, client, server, rendering, simulation, persistence, networking, and tooling behavior. AddressSanitizer and coverage have dedicated presets. |

The testing philosophy and pillar-level test strategy are documented in
[`docs/TDD.md`](docs/TDD.md). SHIELD's voxel density and sampling rules live in
[`docs/shield/sdf-contract.md`](docs/shield/sdf-contract.md).

## Repository layout

| Path | Contents |
|---|---|
| `.github/` | Windows and Linux CI workflows |
| `assets/` | Authored source assets consumed by the asset pipeline |
| `cmake/` | Build helpers and dependency integration |
| `config/` | Repository and runtime policy configuration |
| `data/` | Authored runtime data copied into each build tree |
| `docs/` | Architecture, contracts, specifications, and historical notes |
| `include/` | Public engine headers |
| `references/` | Visual reference material and UI concept frames |
| `res/` | Runtime shader sources |
| `scripts/` | Lint and developer workflow scripts |
| `src/` | Common engine, client, and server source code |
| `test/` | CTest and GoogleTest suites grouped by subsystem |
| `tools/` | Asset, validation, capture, and analysis utilities |
| `updates/` | Project update notes |
| `vendor/` | Vendored sources and pinned dependency integration |
| `worlds/` | Authored world definitions |

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for source-list ownership, linting, and
the expected development workflow. Changes should configure, build, and pass
CTest through the preset that matches the work being performed.

## License

Luminumbra is released under the [MIT License](LICENSE). Third-party components
and assets retain their own terms; see
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) for attribution and license
details.
