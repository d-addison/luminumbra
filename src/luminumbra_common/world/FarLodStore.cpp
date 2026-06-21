#include "FarLodStore.h"

#include "MarchingCubes.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Luminumbra::World {
namespace {

constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

void FnvMix(u64& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(bytes[i]);
        hash *= kFnvPrime;
    }
}

template <typename T>
void FnvMixValue(u64& hash, const T& value) {
    FnvMix(hash, &value, sizeof(T));
}

void AppendBytes(std::string& buffer, const void* data, std::size_t size) {
    buffer.append(static_cast<const char*>(data), size);
}

template <typename T>
void AppendValue(std::string& buffer, const T& value) {
    AppendBytes(buffer, &value, sizeof(T));
}

template <typename T>
bool ReadValue(const std::string& buffer, std::size_t& offset, T& out) {
    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }
    std::memcpy(&out, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

constexpr u8 kTileRecordFlagEdited = 0x01;

int FloorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

} // namespace

u16 QuantizeFarLodHeight(float world_height) {
    const float scaled = (world_height - kFarLodHeightQuantMin) * kFarLodHeightQuantScale;
    const float clamped = std::clamp(scaled, 0.0f, 65535.0f);
    return static_cast<u16>(std::lround(clamped));
}

float DequantizeFarLodHeight(u16 height_q) {
    return kFarLodHeightQuantMin + static_cast<float>(height_q) / kFarLodHeightQuantScale;
}

u64 ComputeTerrainParamsHash(const Systems::TerrainGenParams& params, int seed) {
    u64 hash = kFnvOffsetBasis;
    FnvMixValue(hash, seed);
    FnvMixValue(hash, params.base_frequency);
    FnvMixValue(hash, params.base_amplitude);
    FnvMixValue(hash, params.octaves);
    FnvMixValue(hash, params.persistence);
    FnvMixValue(hash, params.lacunarity);
    FnvMixValue(hash, params.height_offset);
    FnvMixValue(hash, static_cast<u8>(params.caves_enabled ? 1 : 0));
    FnvMixValue(hash, params.cave_frequency);
    FnvMixValue(hash, params.cave_threshold);
    FnvMixValue(hash, params.cave_carve_value);
    FnvMixValue(hash, static_cast<u8>(params.island_mask_enabled ? 1 : 0));
    FnvMixValue(hash, params.island_mask_frequency);
    // T-I4-2: mix the biome-table content hash ONLY when biomes are enabled, so
    // pristine far-LOD tiles self-invalidate on a table content change
    // (design-decisions section 2). Worlds without biomes contribute nothing
    // here, keeping every pre-biome far-tile cache key byte-identical (the
    // disabled path stays byte-zero; FarLodStore fixtures pass unchanged).
    if (params.biomes_enabled && params.biome_table_content_hash != 0) {
        FnvMixValue(hash, static_cast<u8>(1));
        FnvMixValue(hash, params.biome_table_content_hash);
    }
    // T-I4-3: mix river params ONLY when rivers are enabled, so river presets'
    // pristine far tiles invalidate on a river-tuning change while non-river
    // worlds keep byte-identical far-tile cache keys.
    if (params.rivers_enabled) {
        FnvMixValue(hash, static_cast<u8>(2));
        FnvMixValue(hash, params.river_frequency);
        FnvMixValue(hash, params.river_pv_min);
        FnvMixValue(hash, params.river_pv_max);
        FnvMixValue(hash, params.river_depth);
        FnvMixValue(hash, params.river_max_carve);
    }
    // Slice 2: mix lake params ONLY when lakes are enabled (byte-zero drift off).
    if (params.lakes_enabled) {
        FnvMixValue(hash, static_cast<u8>(4));
        FnvMixValue(hash, params.lake_frequency);
        FnvMixValue(hash, params.lake_threshold);
        FnvMixValue(hash, params.lake_depth);
        FnvMixValue(hash, params.lake_max_carve);
        FnvMixValue(hash, params.lake_bank_offset);
    }
    // Slice 3: per-biome relief modulation (only when enabled).
    if (params.biome_relief_enabled) {
        FnvMixValue(hash, static_cast<u8>(5));
        FnvMixValue(hash, params.biome_relief_strength);
    }
    // Slice 4: cliff terracing (only when enabled).
    if (params.cliffs_enabled) {
        FnvMixValue(hash, static_cast<u8>(6));
        FnvMixValue(hash, params.cliff_frequency);
        FnvMixValue(hash, params.cliff_threshold);
        FnvMixValue(hash, params.cliff_step);
    }
    // T-I4-4: mix the structure template content hash ONLY when structures are
    // enabled, so structure presets' pristine far tiles invalidate on a template
    // change while non-structure worlds keep byte-identical far-tile cache keys.
    if (params.structures_enabled && params.structures_content_hash != 0) {
        FnvMixValue(hash, static_cast<u8>(3));
        FnvMixValue(hash, params.structures_content_hash);
    }
    // T-I6-A1.5: mix the SHAPING params ONLY when shaping is enabled, so shaped
    // presets' pristine far-LOD tiles self-invalidate on a shaping-spline / freq
    // change (closing the latent gap where continentalness/erosion/peaks were
    // NOT in the cache key). Non-shaped worlds skip this block entirely, so their
    // far-tile cache key is byte-identical to before (FarLodStore fixtures + the
    // shaping-off legacy preset hash stay green). Canonical spline encoding:
    // count (size_t) then each [input,output] control point in stored order via
    // raw IEEE bits -- pinned by FarLodStore_test::TerrainParamsHashShapingFold.
    if (params.shaping_enabled) {
        FnvMixValue(hash, static_cast<u8>(4)); // marker 0x05 (4th conditional block)
        FnvMixValue(hash, params.continentalness_frequency);
        FnvMixValue(hash, params.erosion_frequency);
        FnvMixValue(hash, params.peaks_frequency);
        FnvMixValue(hash, params.peaks_amplitude);
        FnvMixValue(hash, params.domain_warp_amplitude);
        FnvMixValue(hash, params.domain_warp_frequency);
        const auto mix_spline = [&hash](const std::vector<std::array<float, 2>>& spline) {
            FnvMixValue(hash, static_cast<std::uint64_t>(spline.size()));
            for (const std::array<float, 2>& cp : spline) {
                FnvMixValue(hash, cp[0]);
                FnvMixValue(hash, cp[1]);
            }
        };
        mix_spline(params.continental_spline);
        mix_spline(params.erosion_spline);
        mix_spline(params.peaks_spline);
    }
    // T-I6-A2: mix the hydraulic-relief params ONLY when hydro is enabled, so a
    // relief-tuning change invalidates shaped presets' pristine far-LOD tiles.
    // Disabled worlds skip the block (byte-stable cache key). marker 0x06.
    if (params.hydro_enabled) {
        FnvMixValue(hash, static_cast<u8>(5)); // marker 0x06 (5th conditional block)
        FnvMixValue(hash, params.hydro_iterations);
        FnvMixValue(hash, params.hydro_cell_size_m);
        FnvMixValue(hash, params.hydro_talus_height);
        FnvMixValue(hash, params.hydro_thermal_rate);
        FnvMixValue(hash, params.hydro_rain_per_sweep);
        FnvMixValue(hash, params.hydro_solubility);
        FnvMixValue(hash, params.hydro_deposition);
        FnvMixValue(hash, params.hydro_evaporation);
        FnvMixValue(hash, params.hydro_sediment_capacity);
        FnvMixValue(hash, params.hydro_max_offset);
    }
    // FR-A3: mix surface-break params ONLY when enabled, so dolines/cave-mouths
    // tuning invalidates pristine far-LOD tiles. Disabled worlds skip the block
    // (byte-stable cache key -> byte-zero drift). marker 0x07.
    if (params.surface_breaks_enabled) {
        FnvMixValue(hash, static_cast<u8>(6)); // marker 0x07 (6th conditional block)
        FnvMixValue(hash, params.surface_break_density);
        FnvMixValue(hash, params.feature_cell_size);
        FnvMixValue(hash, params.max_feature_radius);
        FnvMixValue(hash, params.carve_smoothness);
        FnvMixValue(hash, params.entrance_min_cap);
    }
    return hash;
}

u64 ComputeFarLodTileHash(const FarLodTile& tile) {
    u64 hash = kFnvOffsetBasis;
    FnvMixValue(hash, static_cast<u8>(tile.tier));
    FnvMixValue(hash, tile.rx);
    FnvMixValue(hash, tile.rz);
    FnvMixValue(hash, tile.samples_per_side);
    const std::size_t count = tile.sample_count();
    for (std::size_t i = 0; i < count; ++i) {
        FnvMixValue(hash, tile.height_q[i]);
        FnvMixValue(hash, tile.material[i]);
        FnvMixValue(hash, tile.flags[i]);
    }
    return hash;
}

FarLodTile BuildPristineFarLodTile(
    const Systems::SHIELD_WorldSystem& world_system,
    FarLodTier tier,
    i32 rx,
    i32 rz,
    u64 params_hash) {
    FarLodTile tile;
    tile.tier = tier;
    tile.rx = rx;
    tile.rz = rz;
    tile.samples_per_side = FarLodSamplesPerSide(tier);
    tile.params_hash = params_hash;
    tile.edited = false;

    const std::size_t count = tile.sample_count();
    tile.height_q.resize(count);
    tile.material.resize(count);
    tile.flags.resize(count);

    const int step = FarLodSampleStepMeters(tier);
    const float origin_x = static_cast<float>(rx) * static_cast<float>(kFarLodRegionSizeMeters);
    const float origin_z = static_cast<float>(rz) * static_cast<float>(kFarLodRegionSizeMeters);
    const u32 n = tile.samples_per_side;

    // Batch-friendly row-major sweep (z rows, x fastest), matching the
    // sample-array layout exactly.
    std::size_t index = 0;
    for (u32 z = 0; z < n; ++z) {
        const float world_z = origin_z + static_cast<float>(z * step);
        for (u32 x = 0; x < n; ++x, ++index) {
            const float world_x = origin_x + static_cast<float>(x * step);
            // T-I4-DR-river-seam-sliver: coarse sampler anti-aliases the river
            // carve over the tile's sample step so a sub-step-width channel no
            // longer aliases into an isolated deep notch (the FarLodHorizon
            // sliver). Non-river worlds are byte-identical (the coarse path
            // falls through to GetTerrainHeightAt).
            const float height = world_system.GetTerrainHeightAtCoarse(world_x, world_z, step);
            tile.height_q[index] = QuantizeFarLodHeight(height);
            // The same surface classification the coarse chunk mesher uses,
            // so the far field matches the live field at the seam.
            tile.material[index] = static_cast<u8>(
                MarchingCubes::TerrainSurfaceMaterialAt(world_system, world_x, world_z, height));
            tile.flags[index] = height < SEA_LEVEL ? kFarLodSampleFlagWater : 0u;
        }
    }
    return tile;
}

std::size_t ApplyChunkHeightmapToFarLodTile(
    FarLodTile& tile,
    const Chunk& chunk,
    bool mark_edited) {
    const IVec3 coords = chunk.get_coords();
    const int chunks_per_region = kFarLodRegionSizeMeters / CHUNK_SIZE_X;
    if (FloorDiv(coords.x, chunks_per_region) != tile.rx ||
        FloorDiv(coords.z, chunks_per_region) != tile.rz) {
        return 0;
    }
    constexpr std::size_t kHeightmapSide = static_cast<std::size_t>(CHUNK_SIZE_X) + 1u;
    if (chunk.heightmap_data.size() < kHeightmapSide * kHeightmapSide) {
        return 0;
    }

    const int step = FarLodSampleStepMeters(tile.tier);
    // Chunk footprint offset inside the region, in meters. CHUNK_SIZE (16)
    // is an exact multiple of both sample steps (4/8), so chunk borders land
    // exactly on tile sample columns.
    const int local_x0 = coords.x * CHUNK_SIZE_X - tile.rx * kFarLodRegionSizeMeters;
    const int local_z0 = coords.z * CHUNK_SIZE_Z - tile.rz * kFarLodRegionSizeMeters;
    const int sx0 = local_x0 / step;
    const int sz0 = local_z0 / step;
    const int samples_per_chunk = CHUNK_SIZE_X / step + 1;

    std::size_t written = 0;
    for (int dz = 0; dz < samples_per_chunk; ++dz) {
        const int sz = sz0 + dz;
        const int chunk_z = dz * step;
        for (int dx = 0; dx < samples_per_chunk; ++dx) {
            const int sx = sx0 + dx;
            const int chunk_x = dx * step;
            if (sx < 0 || sz < 0 ||
                sx >= static_cast<int>(tile.samples_per_side) ||
                sz >= static_cast<int>(tile.samples_per_side)) {
                continue;
            }
            const float height = chunk.heightmap_data[
                static_cast<std::size_t>(chunk_x) + static_cast<std::size_t>(chunk_z) * kHeightmapSide];
            const std::size_t tile_index =
                static_cast<std::size_t>(sx) + static_cast<std::size_t>(sz) * tile.samples_per_side;
            tile.height_q[tile_index] = QuantizeFarLodHeight(height);
            u8 flags = tile.flags[tile_index] & static_cast<u8>(~kFarLodSampleFlagWater);
            if (height < SEA_LEVEL) {
                flags |= kFarLodSampleFlagWater;
            }
            if (mark_edited) {
                flags |= kFarLodSampleFlagEdited;
            }
            tile.flags[tile_index] = flags;
            ++written;
        }
    }
    if (written > 0 && mark_edited) {
        tile.edited = true;
    }
    return written;
}

FarLodStore::FarLodStore(std::filesystem::path save_dir)
    : m_save_dir(std::move(save_dir)) {}

u64 FarLodStore::tile_record_id(FarLodTier tier, i32 rx, i32 rz) {
    // Region-unique and collision-free against chunk record ids by
    // construction: chunk records and tile records never share a lod_level,
    // and the (lod_level, id) pair keys the container record.
    return Chunk::calculate_id(IVec3(rx, static_cast<int>(tier), rz));
}

bool FarLodStore::save_tile(const FarLodTile& tile, std::vector<std::string>* errors) const {
    const std::size_t count = tile.sample_count();
    if (tile.samples_per_side != FarLodSamplesPerSide(tile.tier) ||
        tile.height_q.size() != count ||
        tile.material.size() != count ||
        tile.flags.size() != count) {
        if (errors) {
            errors->push_back("far-LOD tile has inconsistent sample arrays");
        }
        return false;
    }

    Persistence::WorldSaveService::ContainerRecord record;
    record.id = tile_record_id(tile.tier, tile.rx, tile.rz);
    record.lod_level = static_cast<u8>(tile.tier);
    record.flags = tile.edited ? kTileRecordFlagEdited : 0u;

    std::string& payload = record.payload;
    payload.reserve(32 + count * 4);
    AppendValue(payload, static_cast<u8>(tile.tier));
    AppendValue(payload, tile.rx);
    AppendValue(payload, tile.rz);
    AppendValue(payload, tile.samples_per_side);
    AppendValue(payload, tile.params_hash);
    AppendValue(payload, static_cast<u8>(tile.edited ? 1 : 0));
    AppendBytes(payload, tile.height_q.data(), count * sizeof(u16));
    AppendBytes(payload, tile.material.data(), count);
    AppendBytes(payload, tile.flags.data(), count);

    const std::filesystem::path region_file =
        Persistence::WorldSaveService::region_file_path(m_save_dir, tile.rx, tile.rz);
    return Persistence::WorldSaveService::upsert_container_records(region_file, {record}, errors);
}

bool FarLodStore::load_tile(
    FarLodTier tier,
    i32 rx,
    i32 rz,
    u64 expected_params_hash,
    FarLodTile& out_tile,
    std::vector<std::string>* errors) const {
    const std::filesystem::path region_file =
        Persistence::WorldSaveService::region_file_path(m_save_dir, rx, rz);

    std::vector<Persistence::WorldSaveService::ContainerRecord> records;
    if (!Persistence::WorldSaveService::read_container_records(region_file, records, errors)) {
        return false;
    }

    const u64 wanted_id = tile_record_id(tier, rx, rz);
    for (const auto& record : records) {
        if (record.lod_level != static_cast<u8>(tier) || record.id != wanted_id) {
            continue;
        }

        FarLodTile tile;
        std::size_t offset = 0;
        u8 tier_byte = 0;
        u8 edited_byte = 0;
        if (!ReadValue(record.payload, offset, tier_byte) ||
            !ReadValue(record.payload, offset, tile.rx) ||
            !ReadValue(record.payload, offset, tile.rz) ||
            !ReadValue(record.payload, offset, tile.samples_per_side) ||
            !ReadValue(record.payload, offset, tile.params_hash) ||
            !ReadValue(record.payload, offset, edited_byte)) {
            if (errors) {
                errors->push_back("far-LOD tile payload header is truncated: " + region_file.string());
            }
            return false;
        }
        tile.tier = static_cast<FarLodTier>(tier_byte);
        tile.edited = edited_byte != 0;
        if (tile.tier != tier || tile.rx != rx || tile.rz != rz ||
            tile.samples_per_side != FarLodSamplesPerSide(tier)) {
            if (errors) {
                errors->push_back("far-LOD tile payload header mismatch: " + region_file.string());
            }
            return false;
        }

        const std::size_t count = tile.sample_count();
        if (record.payload.size() - offset != count * 4u) {
            if (errors) {
                errors->push_back("far-LOD tile payload sample stream is truncated: " + region_file.string());
            }
            return false;
        }
        tile.height_q.resize(count);
        tile.material.resize(count);
        tile.flags.resize(count);
        std::memcpy(tile.height_q.data(), record.payload.data() + offset, count * sizeof(u16));
        offset += count * sizeof(u16);
        std::memcpy(tile.material.data(), record.payload.data() + offset, count);
        offset += count;
        std::memcpy(tile.flags.data(), record.payload.data() + offset, count);

        // Pristine tiles are regenerable cache: a params mismatch is a clean
        // miss so the caller rebuilds from (seed, params). Edited tiles are
        // authoritative and must never be regenerated.
        if (!tile.edited && tile.params_hash != expected_params_hash) {
            return false;
        }

        out_tile = std::move(tile);
        return true;
    }
    return false; // clean miss
}

} // namespace Luminumbra::World
