#pragma once

// Far-LOD region tile store (T-I3-8, design-decisions.md sections 3/4).
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
// a regenerable cache keyed (seed, params_hash, tier, region). Edited tiles
// are authoritative: built by downsampling edited chunks' 17x17
// heightmap_data and never regenerated from noise.
//
// Persistence: tiles ride the LMR1 region container alongside chunk records
// (<save_dir>/chunks/region/r.<rx>.<rz>.lmr) as lod_level 1/2 records; the
// chunk writer preserves them verbatim (merge keyed (lod_level, id)).

#include "Chunk.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
struct TerrainGenParams;
}

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

u16 QuantizeFarLodHeight(float world_height);
float DequantizeFarLodHeight(u16 height_q);

// Per-sample flag bits.
constexpr u8 kFarLodSampleFlagWater = 0x01;  // water at or above the sample
constexpr u8 kFarLodSampleFlagEdited = 0x02; // sample rebuilt from edited chunk data

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

    std::size_t sample_count() const {
        return static_cast<std::size_t>(samples_per_side) * samples_per_side;
    }
};

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

// Deterministic fnv1a64 over the tile header (tier, rx, rz, samples) and the
// packed sample stream {height_q, material, flags} in row-major order.
u64 ComputeFarLodTileHash(const FarLodTile& tile);

// Builds a pristine tile analytically (batch-friendly row-major loops over
// GetTerrainHeightAt + the chunk mesher's surface material classification).
// Pure function of (seed, params, tier, region). params_hash is the cache key
// component recorded into the tile (ComputeTerrainParamsHash of the world's
// params and seed - passed in because the world system does not expose its
// seed).
FarLodTile BuildPristineFarLodTile(
    const Systems::SHIELD_WorldSystem& world_system,
    FarLodTier tier,
    i32 rx,
    i32 rz,
    u64 params_hash);

// Downsamples a chunk's 17x17 heightmap_data into the covering tile samples
// (the chunk's 16 m footprint aligns exactly with the 4 m / 8 m sample
// lattices). When mark_edited is set the touched samples and the tile itself
// are flagged edited (authoritative). Returns the number of samples written;
// 0 when the chunk lies outside the tile's region or carries no heightmap.
std::size_t ApplyChunkHeightmapToFarLodTile(
    FarLodTile& tile,
    const Chunk& chunk,
    bool mark_edited);

// Persists far-LOD tiles through the LMR1 container beside the chunk records.
class FarLodStore {
public:
    explicit FarLodStore(std::filesystem::path save_dir);

    const std::filesystem::path& save_dir() const { return m_save_dir; }

    // Record id for a tile inside its region file (the (lod_level, id) pair
    // keys the record; lod_level carries the tier).
    static u64 tile_record_id(FarLodTier tier, i32 rx, i32 rz);

    bool save_tile(const FarLodTile& tile, std::vector<std::string>* errors = nullptr) const;

    // Loads the (tier, region) tile. Pristine tiles are regenerable cache: a
    // params_hash mismatch is a clean miss (returns false, no error) so the
    // caller rebuilds. Edited tiles are authoritative and load regardless of
    // the params hash. A missing record/file is a clean miss.
    bool load_tile(
        FarLodTier tier,
        i32 rx,
        i32 rz,
        u64 expected_params_hash,
        FarLodTile& out_tile,
        std::vector<std::string>* errors = nullptr) const;

private:
    std::filesystem::path m_save_dir;
};

} // namespace Luminumbra::World
