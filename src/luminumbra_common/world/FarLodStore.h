#pragma once

// Far-LOD region tile store (, the deterministic runtime contract sections 3/4).
//
// Far tiers extend the visible horizon past the live chunk ring with packed
// per-region heightfield tiles:
//   F1: 4 m samples, intended coverage 512-768 m
//   F2: 8 m samples, intended coverage 768-1536 m
// Region = 32x32 chunks = 512 m, addressed like the persistence container
// (rx = floor(chunk_x/32), rz = floor(chunk_z/32)). A tile spans its full
// region INCLUDING a shared border row/column (samples_per_side =
// 512/step + 1), so adjacent region meshes share edge vertex positions and
// stitch without cracks.
//
// Pristine tiles are built analytically from GetTerrainHeightAt + the same
// surface material classification the coarse chunk mesher uses - a pure
// function of (seed, params) with a deterministic fnv1a64 tile hash. They are
// a regenerable cache keyed (seed, params_hash, tier, region). Authoritative
// far data is stored as aligned, decimated full-SDF bricks.
//
// Persistence: tiles ride the LMR1 region container alongside chunk records
// (<save_dir>/chunks/region/r.<rx>.<rz>.lmr) as lod_level 1/2 records; the
// chunk writer preserves them verbatim (merge keyed (lod_level, id)).

#include "Chunk.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
struct TerrainGenParams;
} // namespace Luminumbra::Systems

namespace Luminumbra::World {

enum class FarLodTier : u8 {
    F1 = 1, // 4 m samples, 512-768 m
    F2 = 2, // 8 m samples, 768-1536 m
};

// Region edge length in meters (32 chunks x 16 m).
constexpr i32 kFarLodRegionSizeMeters = 512;

constexpr i32 FarLodSampleStepMeters(FarLodTier tier) {
    return tier == FarLodTier::F1 ? 4 : 8;
}

// Includes the shared border row/column: 512/step + 1.
constexpr u32 FarLodSamplesPerSide(FarLodTier tier) {
    return static_cast<u32>(kFarLodRegionSizeMeters / FarLodSampleStepMeters(tier)) + 1u;
}

// Height quantization: u16 height_q in 1/16 m steps biased by -2048 m, i.e.
// representable world heights are [-2048, +2047.9375] m.
constexpr float kFarLodHeightQuantMin = -2048.0f;
constexpr float kFarLodHeightQuantScale = 16.0f;

// Far SDF payload quantization. -32768 is deliberately reserved as an
// on-disk corruption sentinel; valid source density always preserves its sign.
constexpr float kFarLodSdfQuantScale = 256.0f;
constexpr i16 kFarLodSdfInvalid = static_cast<i16>(-32768);

i16 QuantizeFarLodSdf(float density);
float DequantizeFarLodSdf(i16 density_q);

u16 QuantizeFarLodHeight(float world_height);
float DequantizeFarLodHeight(u16 height_q);

// Per-sample flag bits.
constexpr u8 kFarLodSampleFlagWater = 0x01;  // water at or above the sample
constexpr u8 kFarLodSampleFlagEdited = 0x02; // sample rebuilt from edited chunk data

enum class FarLodBrickSourceKind : u8 {
    RegenerableCache = 0,
    Authoritative = 1,
};

// Wire fields are intentionally represented separately so persistence never
// depends on native struct padding. Descriptors are sorted by (z, x, y).
struct FarLodSdfBrickDescriptor {
    u8 local_chunk_x = 0;
    u8 local_chunk_z = 0;
    FarLodBrickSourceKind source_kind = FarLodBrickSourceKind::Authoritative;
    u8 reserved = 0;
    i32 chunk_y = 0;
    u32 revision = 0;
    u32 payload_crc32 = 0;
};

// Worker-only descriptor for an assembled mesh input.  Unlike the persisted
// FSD2 descriptor it is expressed in absolute chunk coordinates and therefore
// can describe the one-chunk halo that crosses a home-region boundary.
struct FarLodWorldSdfBrickDescriptor {
    i32 chunk_x = 0;
    i32 chunk_z = 0;
    i32 chunk_y = 0;
    FarLodBrickSourceKind source_kind = FarLodBrickSourceKind::Authoritative;
    u32 revision = 0;
    u32 payload_crc32 = 0;
};

// An owned, transient world-coordinate SDF view passed from a far worker to
// the mesher.  It is deliberately separate from FarLodTile: neighbours and
// generated halo support are mesh inputs only and can never be persisted in
// the requested region record.
struct FarLodRegionSdfAssembly {
    FarLodTier tier = FarLodTier::F1;
    i32 rx = 0;
    i32 rz = 0;
    u64 params_hash = 0;
    // Canonically sorted (chunk_z, chunk_x) absolute columns.
    std::vector<std::pair<i32, i32>> authority_columns;
    std::vector<std::pair<i32, i32>> owned_columns;
    // Canonically sorted by (chunk_z, chunk_x, chunk_y).  Each descriptor owns
    // FarLodSdfBrickSampleCount(tier) consecutive density/material samples.
    std::vector<FarLodWorldSdfBrickDescriptor> bricks;
    std::vector<i16> density_q;
    std::vector<u8> material;
};

// An owned full-lattice snapshot. Far workers consume this value instead of a
// mutable Chunk, so reduction cannot race an edit or eviction.
struct FarLodSdfSnapshot {
    IVec3 coords{};
    u32 revision = 0;
    FarLodBrickSourceKind source_kind = FarLodBrickSourceKind::Authoritative;
    std::vector<f32> sdf_data;
    std::vector<u8> material_data;
};

enum class FarLodSdfReduceResult : u8 {
    Inserted,
    Replaced,
    Unchanged,
    Error,
};

// One far-LOD region tile. Sample arrays are row-major
// (index = x + z * samples_per_side), x/z in sample-grid units; world
// position of sample (x, z) is (rx*512 + x*step, rz*512 + z*step).
struct FarLodTile {
    FarLodTier tier = FarLodTier::F1;
    i32 rx = 0;
    i32 rz = 0;
    u32 samples_per_side = 0;
    // fnv1a64 of (seed, TerrainGenParams) the tile was generated against;
    // a mismatch invalidates PRISTINE tiles only (edited tiles are
    // authoritative and never regenerated).
    u64 params_hash = 0;
    bool edited = false;
    std::vector<u16> height_q;
    std::vector<u8> material;
    std::vector<u8> flags;

    // Flat, sorted streams. Every descriptor owns exactly
    // FarLodSdfBrickSampleCount(tier) consecutive density/material samples.
    std::vector<FarLodSdfBrickDescriptor> sdf_bricks;
    std::vector<i16> sdf_density_q;
    std::vector<u8> sdf_material;

    std::size_t sample_count() const {
        return static_cast<std::size_t>(samples_per_side) * samples_per_side;
    }
};

constexpr u32 FarLodSdfBrickSamplesPerSide(FarLodTier tier) {
    return static_cast<u32>(CHUNK_SIZE_X / FarLodSampleStepMeters(tier)) + 1u;
}

constexpr std::size_t FarLodSdfBrickSampleCount(FarLodTier tier) {
    const std::size_t side = FarLodSdfBrickSamplesPerSide(tier);
    return side * side * side;
}

// Decimates a full 17^3 lattice at the aligned far-tier stride and upserts the
// corresponding brick. Material data is optional; an absent full material
// lattice is encoded as 0xff. Heightmap-only input is never accepted.
FarLodSdfReduceResult ReduceChunkSdfIntoFarTile(FarLodTile& tile,
                                                const FarLodSdfSnapshot& snapshot,
                                                std::string* error = nullptr);

// Region mesh output (consumed by MarchingCubes::GenerateFarLodRegionMesh and
// the far render path). Vertex positions are region-local in X/Z (relative to
// the region origin rx*512, rz*512) and absolute in Y, VoxelVertex layout
// untouched (28 bytes).
struct FarLodRegionMesh {
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
};

// Deterministic fnv1a64 over (seed, params) - the pristine-tile cache key
// component shared with worldgen.
u64 ComputeTerrainParamsHash(const Systems::TerrainGenParams& params, int seed);

// Deterministic fnv1a64 over all current tile metadata and payload streams.
u64 ComputeFarLodTileHash(const FarLodTile& tile);

// Builds a pristine tile analytically (batch-friendly row-major loops over
// GetTerrainHeightAt + the chunk mesher's surface material classification).
// Pure function of (seed, params, tier, region). params_hash is the cache key
// component recorded into the tile (ComputeTerrainParamsHash of the world's
// params and seed - passed in because the world system does not expose its
// seed).
FarLodTile BuildPristineFarLodTile(const Systems::SHIELD_WorldSystem& world_system,
                                   FarLodTier tier,
                                   i32 rx,
                                   i32 rz,
                                   u64 params_hash);

// Persists far-LOD tiles through the LMR1 container beside the chunk records.
class FarLodStore {
public:
    explicit FarLodStore(std::filesystem::path save_dir);

    const std::filesystem::path& save_dir() const {
        return m_save_dir;
    }

    // Record id for a tile inside its region file (the (lod_level, id) pair
    // keys the record; lod_level carries the tier).
    static u64 tile_record_id(FarLodTier tier, i32 rx, i32 rz);

    // Strict current payload decoder shared with whole-world validation.
    static bool decode_payload(const std::string& payload,
                               FarLodTier tier,
                               i32 rx,
                               i32 rz,
                               u8 record_flags,
                               FarLodTile& out_tile,
                               std::vector<std::string>* errors = nullptr);

    bool save_tile(const FarLodTile& tile, std::vector<std::string>* errors = nullptr) const;

    // Loads the (tier, region) tile. Pristine tiles are regenerable cache: a
    // params_hash mismatch is a clean miss (returns false, no error) so the
    // caller rebuilds. Edited tiles are authoritative and load regardless of
    // the params hash. A missing record/file is a clean miss.
    bool load_tile(FarLodTier tier,
                   i32 rx,
                   i32 rz,
                   u64 expected_params_hash,
                   FarLodTile& out_tile,
                   std::vector<std::string>* errors = nullptr) const;

private:
    std::filesystem::path m_save_dir;
};

} // namespace Luminumbra::World
