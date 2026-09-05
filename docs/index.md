# Luminumbra engine documentation

Luminumbra is a C++20 voxel-world engine built around a deterministic common
simulation, a GPU client, and a headless authoritative server. This site starts
with the engine model and development contracts; the generated namespace,
class, and file indexes are the lower-level API reference.

## v0.3.0 world format break

> This world predates the v0.3.0 format and cannot be opened. Create a new world; migration is not supported.

Create a new world using a current preset. The format is LMR1 container v2,
`luminumbra.persistence.world_manifest.v1` with `container_version: 2`, and FSD2
far-LOD payload v3. Explicit preset revisions must be `schema_rev: 6`; omission
remains accepted for in-memory callers. Shaping is unconditional and enabled
caves use the noise router, while content, hydrology, and biome controls remain.
Future-format and corruption failures are separate from obsolete-world refusal.
See [world format compatibility](engine-guide.md#world-format-compatibility).

## Start here

- [Engine/framework guide](engine-guide.md) explains the runtime lifecycle,
  fixed-tick simulation, world streaming, rendering, server host, and supported
  extension paths.
- [Architecture](architecture.md) defines layer ownership and dependency
  boundaries.
- [Development and testing](development.md) covers toolchains, CMake presets,
  test tiers, and local validation.
- [Performance measurement](performance.md) documents the relative regression
  gate and its statistical interpretation.
- [Visual regression](visual-regression.md) explains maintained captures and
  visual evidence.
- [SHIELD signed-distance-field contract](shield/sdf-contract.md) defines the
  terrain sampling and meshing invariants.
- [Known limitations](known-limitations.md) is an honest inventory of the
  engine's current boundaries.

## Engine at a glance

| Runtime | Responsibility | Primary entry point |
|---|---|---|
| Common engine | ECS, fixed-tick simulation, terrain, streaming, physics, persistence, replay, Lua, and networking | `Luminumbra::world::GameSession` |
| Client | Window/input loop, render graph, UI, audio, and presentation resources | `src/luminumbra_client/main_client.cpp` |
| Server | Headless boot, fixed-tick execution, state hashing, and save/replay orchestration | `Luminumbra::Server::ServerWorldRunner` |

The authoritative simulation never depends on client rendering or audio. A
client frame may interpolate and present state, but wall-clock timing and GPU
results do not feed simulation hashes. Work can run concurrently only when its
publication order remains a deterministic function of simulation inputs.

## Build a runnable engine

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug -LE manual --no-tests=error --output-on-failure
```

The resulting programs are under `build/debug/bin/`:

- `luminumbra_client_app` runs the graphical client;
- `luminumbra_client_qa_app` runs the same client with the QA scenario harness
  compiled in — the gate scripts' `--scenario <name>` capture/validation runs
  target this binary (the shipping client refuses `--scenario` with exit
  code 6);
- `luminumbra_server_app` runs a headless authoritative world; and
- `asset_processor` converts authored model inputs to runtime data.

Run the client from the repository root, or exercise the same simulation
without a window through the deterministic server smoke path:

```sh
build/debug/bin/luminumbra_client_app --no-audio
build/debug/bin/luminumbra_server_app --smoke --root . --preset default --seed 1337 --ticks 90
```

Luminumbra 0.3.0 is an early source release. The repository builds an engine and
its applications together; it does not yet promise a stable installed SDK or
ABI for third-party applications.
