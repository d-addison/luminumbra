# SHIELD SDF Contract

This contract defines the Signed Distance Field (SDF) data shape consumed by
SHIELD terrain generation, meshing, collision generation, and GPU generation
callbacks.

## Density Semantics

- `0.0f` is the terrain isolevel.
- Negative density is solid terrain.
- Positive density is empty air.
- A surface exists where density crosses `0.0f`.
- Density values must be finite `float` values. Producers must not write NaN or
  infinity.

The CPU terrain generator currently computes base terrain density as:

```cpp
density = world_y - terrain_height
```

That means points below the terrain height are solid and points above it are
air. Marching Cubes must polygonise terrain with an isolevel of `0.0f`.

## Chunk Sample Grid

Each chunk stores a padded SDF grid with one extra sample on each axis edge so
Marching Cubes can read all cube corners inside the chunk:

```text
size_x = CHUNK_SIZE_X + 1
size_y = CHUNK_SIZE_Y + 1
size_z = CHUNK_SIZE_Z + 1
sample_count = size_x * size_y * size_z
```

Chunk-local sample coordinates are integer grid points:

```text
x = 0..CHUNK_SIZE_X
y = 0..CHUNK_SIZE_Y
z = 0..CHUNK_SIZE_Z
```

World-space sample coordinates are derived from chunk coordinates:

```text
base = chunk_coords * (CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z)
world = base + (x, y, z)
```

## Storage Layout

`Chunk::sdf_data` is stored with `x` as the fastest-varying coordinate, then
`y`, then `z`:

```cpp
index = x + y * size_x + z * size_x * size_y;
```

`Chunk::heightmap_data` stores one terrain height per `(x, z)` column with `x`
as the fastest-varying coordinate:

```cpp
heightmap_index = x + z * size_x;
heightmap_count = size_x * size_z;
```

## Terrain Height

`GetTerrainHeightAt(world_x, world_z)` returns:

```cpp
height_offset + terrain_noise(world_x, world_z, seed) * base_amplitude
```

The generated heightmap stores this terrain height for each chunk column. The
heightmap is not a density field; it is a cached surface-height value for
systems that need a column lookup.

## Optional Island Mask

When `TerrainGenParams::island_mask_enabled` is true, terrain height used for
SDF generation is blended toward `height_offset` before density is calculated:

```cpp
island_mask = smoothstep(0.1f, 0.25f, island_noise);
terrain_h = mix(height_offset, terrain_h, island_mask);
```

This affects generated density values. Producers that replace CPU generation
must apply the same mask semantics when the flag is enabled.

## Optional Cave Field

When `TerrainGenParams::caves_enabled` is true, caves modify the base terrain
density after terrain height is applied:

```cpp
cave_val = clamp((raw_cave_noise + 1.0f) * 0.5f, 0.0f, 1.0f);
cave_density = (cave_val - cave_threshold) * cave_carve_value;
density = max(terrain_density, cave_density);
```

Because negative density is solid and positive density is air, taking the
maximum allows cave noise to raise solid terrain samples into air.

## Meshing Contract

Marching Cubes reads `Chunk::sdf_data` using the padded grid and extracts the
surface at `0.0f`. LOD is represented by a sample step (`1`, `2`, `4`, etc.).
All SDF producers must fill every padded-grid sample so any supported LOD step
can safely read cube corners up to the chunk boundary.

Material selection is separate from the density contract. Mesh generation may
classify material from world position and terrain height after the surface has
been extracted.

## Two Producer Tiers

A chunk's `Chunk::sdf_data` is produced at one of two tiers, selected by the
meshing **sample step** the chunk needs (). The meshing promotion lane
and every SDF producer must agree on which tier a chunk is in from its
`sdf_data` size alone:

- **Tier 1 — full unit-step lattice ("sim truth").**
  `sdf_data.size() == kFullSdfLattice` (`(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) *
  (CHUNK_SIZE_Z + 1)`, the padded grid defined above). This is the full-resolution
  field a unit-step (`step == 1`) Marching Cubes polygonise reads, and the only
  tier that carries interior detail (caves, player edits). CPU generation, the GPU
  callback, and a well-formed save all produce this shape.
- **Tier 2 — coarse heightmap-only (surface-band).** For a coarse meshing step
  (`step > 1`) a chunk is meshed directly from the terrain heightmap by
  `SHIELD_WorldSystem::GenerateCoarseHeightfieldTerrain` (sampling
  `GetTerrainHeightAtCoarse`), and its `sdf_data` is left **empty** — no interior
  SDF lattice is generated. A coarse chunk therefore re-derives its surface from
  the heightmap and does not carry caves or edits at distance; promoting it to a
  unit-step mesh requires (re)generating the full Tier-1 field first.

### Malformed SDF — regeneration rule

A `sdf_data` that is **non-empty but not exactly `kFullSdfLattice`** is treated as
**malformed** (e.g. a wrong-version or truncated save). Before a unit-step
polygonise — which assumes the full padded lattice and would otherwise read out of
bounds — the promotion lane **clears the malformed field and regenerates** the full
Tier-1 SDF. The three states are exhaustive and distinguishable by size alone:

| `sdf_data` state | tier | meshing |
| --- | --- | --- |
| `size() == kFullSdfLattice` | Tier 1 (full lattice) | unit-step Marching Cubes |
| `empty()` | Tier 2 (coarse) | heightmap-only coarse mesher |
| non-empty, `size() != kFullSdfLattice` | malformed | cleared + regenerated to Tier 1 |

## GPU SDF Callback Contract

`SHIELD_WorldSystem::SetGPUSDFCallback` may install a producer with this shape:

```cpp
bool callback(
    const IVec3& chunk_coords,
    const TerrainGenParams& params,
    int seed,
    std::vector<float>& sdf_data);
```

When the callback returns `true`, it owns the generated SDF result and the CPU
fallback is skipped. A successful callback must:

- Resize `sdf_data` to `(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) *
  (CHUNK_SIZE_Z + 1)`.
- Fill every sample using the storage layout defined above.
- Preserve the density sign convention: negative solid, positive air,
  `0.0f` surface.
- Apply the same seed, terrain parameters, island mask, and cave semantics as
  CPU generation for deterministic worlds.
- Leave no partial data on success.

When the callback returns `false`, SHIELD uses CPU generation and ignores any
partial data the callback may have written.

After successful GPU generation, SHIELD derives `heightmap_data` from the SDF by
scanning each `(x, z)` column from the top of the padded chunk toward the bottom
and recording the first sample with density `<= 0.0f`.

## Determinism Requirements

For a fixed chunk coordinate, seed, and `TerrainGenParams`, SDF generation must
produce repeatable output. CPU generation is expected to be exactly repeatable
for the same inputs. Alternate producers should avoid time-varying state,
thread-order-dependent writes, uninitialized data, and platform-specific
randomness.
