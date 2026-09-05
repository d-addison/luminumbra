#include "FarLodStore.h"

#include "HydraulicErosion.h"
#include "MarchingCubes.h"
#include "core/Crc32.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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

template<typename T>
void FnvMixValue(u64& hash, const T& value) {
    FnvMix(hash, &value, sizeof(T));
}

void AppendBytes(std::string& buffer, const void* data, std::size_t size) {
    buffer.append(static_cast<const char*>(data), size);
}

template<typename T>
void AppendValue(std::string& buffer, const T& value) {
    AppendBytes(buffer, &value, sizeof(T));
}

template<typename T>
bool ReadValue(const std::string& buffer, std::size_t& offset, T& out) {
    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }
    std::memcpy(&out, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

constexpr u8 kTileRecordFlagEdited = 0x01;
constexpr char kFarLodPayloadMagic[] = {'F', 'S', 'D', '2'};
constexpr u16 kFarLodPayloadVersion = 2;
constexpr u8 kFarLodMaterialAnalytic = 0xff;

int FloorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

bool ReadBytes(const std::string& buffer, std::size_t& offset, void* out, std::size_t size) {
    if (offset > buffer.size() || size > buffer.size() - offset) {
        return false;
    }
    if (size != 0) {
        std::memcpy(out, buffer.data() + offset, size);
    }
    offset += size;
    return true;
}

bool IsValidSourceKind(FarLodBrickSourceKind source_kind) {
    return source_kind == FarLodBrickSourceKind::RegenerableCache ||
           source_kind == FarLodBrickSourceKind::Authoritative;
}

bool IsValidFarLodTier(FarLodTier tier) {
    return tier == FarLodTier::F1 || tier == FarLodTier::F2;
}

bool BrickLess(const FarLodSdfBrickDescriptor& left, const FarLodSdfBrickDescriptor& right) {
    if (left.local_chunk_z != right.local_chunk_z) {
        return left.local_chunk_z < right.local_chunk_z;
    }
    if (left.local_chunk_x != right.local_chunk_x) {
        return left.local_chunk_x < right.local_chunk_x;
    }
    return left.chunk_y < right.chunk_y;
}

bool SameBrickKey(const FarLodSdfBrickDescriptor& left, const FarLodSdfBrickDescriptor& right) {
    return left.local_chunk_x == right.local_chunk_x && left.local_chunk_z == right.local_chunk_z &&
           left.chunk_y == right.chunk_y;
}

u32 Crc32(const i16* density_q, const u8* material, std::size_t count) {
    Core::Crc32Accumulator crc;
    crc.Update(density_q, count * sizeof(i16));
    crc.Update(material, count);
    return crc.Value();
}

bool ValidateTileStreams(const FarLodTile& tile, std::string* error) {
    if (!IsValidFarLodTier(tile.tier)) {
        if (error)
            *error = "far-LOD tile has an invalid tier";
        return false;
    }
    const std::size_t background_count = tile.sample_count();
    if (tile.samples_per_side != FarLodSamplesPerSide(tile.tier) ||
        tile.height_q.size() != background_count || tile.material.size() != background_count ||
        tile.flags.size() != background_count) {
        if (error)
            *error = "far-LOD tile has inconsistent background arrays";
        return false;
    }

    const std::size_t samples_per_brick = FarLodSdfBrickSampleCount(tile.tier);
    if (tile.sdf_bricks.size() > std::numeric_limits<std::size_t>::max() / samples_per_brick ||
        tile.sdf_density_q.size() != tile.sdf_bricks.size() * samples_per_brick ||
        tile.sdf_material.size() != tile.sdf_bricks.size() * samples_per_brick) {
        if (error)
            *error = "far-LOD tile has inconsistent SDF brick streams";
        return false;
    }
    for (std::size_t index = 0; index < tile.sdf_bricks.size(); ++index) {
        const FarLodSdfBrickDescriptor& descriptor = tile.sdf_bricks[index];
        if (descriptor.local_chunk_x >= 32u || descriptor.local_chunk_z >= 32u ||
            descriptor.reserved != 0 || !IsValidSourceKind(descriptor.source_kind) ||
            (index != 0 && !BrickLess(tile.sdf_bricks[index - 1], descriptor))) {
            if (error)
                *error = "far-LOD tile has invalid or unsorted SDF brick descriptors";
            return false;
        }
        const std::size_t payload_offset = index * samples_per_brick;
        for (std::size_t sample = 0; sample < samples_per_brick; ++sample) {
            if (tile.sdf_density_q[payload_offset + sample] == kFarLodSdfInvalid) {
                if (error)
                    *error = "far-LOD tile contains an invalid SDF density sentinel";
                return false;
            }
        }
        if (descriptor.payload_crc32 != Crc32(tile.sdf_density_q.data() + payload_offset,
                                              tile.sdf_material.data() + payload_offset,
                                              samples_per_brick)) {
            if (error)
                *error = "far-LOD tile SDF brick CRC mismatch";
            return false;
        }
    }
    return true;
}

void PushError(std::vector<std::string>* errors, const std::string& message) {
    if (errors) {
        errors->push_back(message);
    }
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

i16 QuantizeFarLodSdf(float density) {
    if (!std::isfinite(density)) {
        return kFarLodSdfInvalid;
    }
    const float scaled = std::clamp(density * kFarLodSdfQuantScale, -32767.0f, 32767.0f);
    i16 quantized = static_cast<i16>(std::lround(scaled));
    if (density < 0.0f && quantized == 0) {
        quantized = -1;
    } else if (density > 0.0f && quantized == 0) {
        quantized = 1;
    }
    return quantized;
}

float DequantizeFarLodSdf(i16 density_q) {
    return static_cast<float>(density_q) / kFarLodSdfQuantScale;
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
    if (params.caves_enabled) {
        // The sole cave implementation now includes the complete noise router.
        FnvMixValue(hash, params.spaghetti_frequency);
        FnvMixValue(hash, params.spaghetti_thickness);
        FnvMixValue(hash, params.worley_frequency);
        FnvMixValue(hash, params.worley_threshold);
    }
    FnvMixValue(hash, static_cast<u8>(params.island_mask_enabled ? 1 : 0));
    FnvMixValue(hash, params.island_mask_frequency);
    // mix the biome-table content hash ONLY when biomes are enabled, so
    // pristine far-LOD tiles self-invalidate on a table content change
    // in the stable terrain identity. Worlds without biomes contribute nothing
    // here, keeping every pre-biome far-tile cache key byte-identical (the
    // disabled path stays byte-zero; FarLodStore fixtures pass unchanged).
    if (params.biomes_enabled && params.biome_table_content_hash != 0) {
        FnvMixValue(hash, static_cast<u8>(1));
        FnvMixValue(hash, params.biome_table_content_hash);
    }
    // mix river params ONLY when rivers are enabled, so river presets'
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
    //  mix lake params ONLY when lakes are enabled (byte-zero drift off).
    if (params.lakes_enabled) {
        FnvMixValue(hash, static_cast<u8>(4));
        FnvMixValue(hash, params.lake_frequency);
        FnvMixValue(hash, params.lake_threshold);
        FnvMixValue(hash, params.lake_depth);
        FnvMixValue(hash, params.lake_max_carve);
        FnvMixValue(hash, params.lake_bank_offset);
    }
    //  per-biome relief modulation (only when enabled).
    if (params.biome_relief_enabled) {
        FnvMixValue(hash, static_cast<u8>(5));
        FnvMixValue(hash, params.biome_relief_strength);
    }
    //  cliff terracing (only when enabled).
    if (params.cliffs_enabled) {
        FnvMixValue(hash, static_cast<u8>(6));
        FnvMixValue(hash, params.cliff_frequency);
        FnvMixValue(hash, params.cliff_threshold);
        FnvMixValue(hash, params.cliff_step);
    }
    // mix the structure template content hash ONLY when structures are
    // enabled, so structure presets' pristine far tiles invalidate on a template
    // change while non-structure worlds keep byte-identical far-tile cache keys.
    if (params.structures_enabled && params.structures_content_hash != 0) {
        FnvMixValue(hash, static_cast<u8>(3));
        FnvMixValue(hash, params.structures_content_hash);
    }
    // Shaping is unconditional after the cave_style/shaping_enabled retirement.
    // Hash every control frequency and spline so pristine tiles invalidate on edits.
    // Canonical spline encoding: uint64 count, then ordered IEEE input/output bits.

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

    // mix the hydraulic-relief params ONLY when hydro is enabled, so a
    // relief-tuning change invalidates shaped presets' pristine far-LOD tiles.
    // Disabled worlds skip the block (byte-stable cache key). marker 0x06.
    if (params.hydro_enabled) {
        FnvMixValue(hash, static_cast<u8>(5)); // marker 0x06 (5th conditional block)
        FnvMixValue(hash, kHydraulicErosionWorldgenVersion);
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
    // mix surface-break params ONLY when enabled, so dolines/cave-mouths
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

    // Preserve the original tile hash byte-for-byte for the zero-brick path.
    // Existing pristine baselines and legacy height-only authority already
    // encode their complete visible content in the streams above. FSD2 fields
    // become part of the hash only once three-dimensional brick data exists.
    if (tile.sdf_bricks.empty()) {
        return hash;
    }
    FnvMix(hash, kFarLodPayloadMagic, sizeof(kFarLodPayloadMagic));
    FnvMixValue(hash, kFarLodPayloadVersion);
    FnvMixValue(hash, tile.params_hash);
    FnvMixValue(hash, static_cast<u8>(tile.edited));
    FnvMixValue(hash, static_cast<u8>(tile.legacy_surface_authority));
    FnvMixValue(hash, static_cast<u64>(tile.sdf_bricks.size()));
    const std::size_t samples_per_brick = FarLodSdfBrickSampleCount(tile.tier);
    for (std::size_t brick = 0; brick < tile.sdf_bricks.size(); ++brick) {
        const FarLodSdfBrickDescriptor& descriptor = tile.sdf_bricks[brick];
        FnvMixValue(hash, descriptor.local_chunk_x);
        FnvMixValue(hash, descriptor.local_chunk_z);
        FnvMixValue(hash, static_cast<u8>(descriptor.source_kind));
        FnvMixValue(hash, descriptor.reserved);
        FnvMixValue(hash, descriptor.chunk_y);
        FnvMixValue(hash, descriptor.revision);
        FnvMixValue(hash, descriptor.payload_crc32);
        const std::size_t offset = brick * samples_per_brick;
        for (std::size_t sample = 0; sample < samples_per_brick; ++sample) {
            FnvMixValue(hash, tile.sdf_density_q[offset + sample]);
            FnvMixValue(hash, tile.sdf_material[offset + sample]);
        }
    }
    return hash;
}

FarLodTile BuildPristineFarLodTile(const Systems::SHIELD_WorldSystem& world_system,
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
            // coarse sampler anti-aliases the river
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

FarLodSdfReduceResult
ReduceChunkSdfIntoFarTile(FarLodTile& tile, const FarLodSdfSnapshot& snapshot, std::string* error) {
    const std::size_t source_side_x = static_cast<std::size_t>(CHUNK_SIZE_X) + 1u;
    const std::size_t source_side_y = static_cast<std::size_t>(CHUNK_SIZE_Y) + 1u;
    const std::size_t source_side_z = static_cast<std::size_t>(CHUNK_SIZE_Z) + 1u;
    const std::size_t source_count = source_side_x * source_side_y * source_side_z;
    if (source_side_x != 17u || source_side_y != 17u || source_side_z != 17u ||
        snapshot.sdf_data.size() != source_count) {
        if (error)
            *error = "far-LOD SDF reduction requires an exact 17^3 lattice";
        return FarLodSdfReduceResult::Error;
    }
    if (!snapshot.material_data.empty() && snapshot.material_data.size() != source_count) {
        if (error)
            *error = "far-LOD SDF material lattice is neither empty nor 17^3";
        return FarLodSdfReduceResult::Error;
    }
    if (!IsValidSourceKind(snapshot.source_kind)) {
        if (error)
            *error = "far-LOD SDF snapshot has an invalid source kind";
        return FarLodSdfReduceResult::Error;
    }
    const int chunks_per_region = kFarLodRegionSizeMeters / CHUNK_SIZE_X;
    if (FloorDiv(snapshot.coords.x, chunks_per_region) != tile.rx ||
        FloorDiv(snapshot.coords.z, chunks_per_region) != tile.rz) {
        if (error)
            *error = "far-LOD SDF snapshot lies outside the tile home region";
        return FarLodSdfReduceResult::Error;
    }
    std::string tile_error;
    if (!ValidateTileStreams(tile, &tile_error)) {
        if (error)
            *error = tile_error;
        return FarLodSdfReduceResult::Error;
    }

    const int step = FarLodSampleStepMeters(tile.tier);
    const std::size_t samples_per_brick = FarLodSdfBrickSampleCount(tile.tier);
    std::vector<i16> density_q;
    std::vector<u8> material;
    density_q.reserve(samples_per_brick);
    material.reserve(samples_per_brick);
    for (int z = 0; z <= CHUNK_SIZE_Z; z += step) {
        for (int y = 0; y <= CHUNK_SIZE_Y; y += step) {
            for (int x = 0; x <= CHUNK_SIZE_X; x += step) {
                const std::size_t source_index =
                    static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * source_side_x +
                    static_cast<std::size_t>(z) * source_side_x * source_side_y;
                const float density = snapshot.sdf_data[source_index];
                if (!std::isfinite(density)) {
                    if (error)
                        *error = "far-LOD SDF snapshot contains a non-finite density";
                    return FarLodSdfReduceResult::Error;
                }
                density_q.push_back(QuantizeFarLodSdf(density));
                material.push_back(snapshot.material_data.empty()
                                       ? kFarLodMaterialAnalytic
                                       : snapshot.material_data[source_index]);
            }
        }
    }

    FarLodSdfBrickDescriptor descriptor;
    descriptor.local_chunk_x = static_cast<u8>(snapshot.coords.x - tile.rx * chunks_per_region);
    descriptor.local_chunk_z = static_cast<u8>(snapshot.coords.z - tile.rz * chunks_per_region);
    descriptor.source_kind = snapshot.source_kind;
    descriptor.chunk_y = snapshot.coords.y;
    descriptor.revision = snapshot.revision;
    descriptor.payload_crc32 = Crc32(density_q.data(), material.data(), samples_per_brick);

    const auto supersede_legacy_surface = [&tile, &snapshot]() {
        const int step = FarLodSampleStepMeters(tile.tier);
        const int local_x0 = snapshot.coords.x * CHUNK_SIZE_X - tile.rx * kFarLodRegionSizeMeters;
        const int local_z0 = snapshot.coords.z * CHUNK_SIZE_Z - tile.rz * kFarLodRegionSizeMeters;
        const int sx0 = local_x0 / step;
        const int sz0 = local_z0 / step;
        const int samples_per_chunk = CHUNK_SIZE_X / step + 1;
        for (int dz = 0; dz < samples_per_chunk; ++dz) {
            for (int dx = 0; dx < samples_per_chunk; ++dx) {
                const std::size_t index =
                    static_cast<std::size_t>(sx0 + dx) +
                    static_cast<std::size_t>(sz0 + dz) * tile.samples_per_side;
                tile.flags[index] &= static_cast<u8>(~kFarLodSampleFlagEdited);
            }
        }
        tile.legacy_surface_authority =
            std::any_of(tile.flags.begin(), tile.flags.end(), [](u8 flags) {
                return (flags & kFarLodSampleFlagEdited) != 0;
            });
    };

    const auto found =
        std::lower_bound(tile.sdf_bricks.begin(), tile.sdf_bricks.end(), descriptor, BrickLess);
    const std::size_t insert_index = static_cast<std::size_t>(found - tile.sdf_bricks.begin());
    if (found != tile.sdf_bricks.end() && SameBrickKey(*found, descriptor)) {
        const std::size_t payload_offset = insert_index * samples_per_brick;
        const bool same_payload =
            found->payload_crc32 == descriptor.payload_crc32 &&
            std::equal(
                density_q.begin(), density_q.end(), tile.sdf_density_q.begin() + payload_offset) &&
            std::equal(
                material.begin(), material.end(), tile.sdf_material.begin() + payload_offset);
        if (same_payload && (found->source_kind == descriptor.source_kind ||
                             found->source_kind == FarLodBrickSourceKind::Authoritative)) {
            return FarLodSdfReduceResult::Unchanged;
        }

        // Persisted far revisions outlive the process-local Chunk revision.
        // A current owner-thread authoritative snapshot is therefore the source
        // of truth even when its post-reload counter is numerically lower. Keep
        // the far descriptor monotonic by advancing from the persisted value.
        if (descriptor.source_kind == FarLodBrickSourceKind::Authoritative) {
            const u32 next_revision = found->revision == std::numeric_limits<u32>::max()
                                          ? found->revision
                                          : found->revision + 1u;
            descriptor.revision = std::max(descriptor.revision, next_revision);
        } else {
            if (found->source_kind == FarLodBrickSourceKind::Authoritative ||
                found->revision >= descriptor.revision) {
                return FarLodSdfReduceResult::Unchanged;
            }
        }
        *found = descriptor;
        std::copy(density_q.begin(), density_q.end(), tile.sdf_density_q.begin() + payload_offset);
        std::copy(material.begin(), material.end(), tile.sdf_material.begin() + payload_offset);
        supersede_legacy_surface();
        tile.edited = tile.edited || descriptor.source_kind == FarLodBrickSourceKind::Authoritative;
        return FarLodSdfReduceResult::Replaced;
    }

    tile.sdf_bricks.insert(found, descriptor);
    tile.sdf_density_q.insert(tile.sdf_density_q.begin() + insert_index * samples_per_brick,
                              density_q.begin(),
                              density_q.end());
    tile.sdf_material.insert(tile.sdf_material.begin() + insert_index * samples_per_brick,
                             material.begin(),
                             material.end());
    supersede_legacy_surface();
    tile.edited = tile.edited || descriptor.source_kind == FarLodBrickSourceKind::Authoritative;
    return FarLodSdfReduceResult::Inserted;
}

std::size_t
ApplyChunkHeightmapToFarLodTile(FarLodTile& tile, const Chunk& chunk, bool mark_edited) {
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
            if (sx < 0 || sz < 0 || sx >= static_cast<int>(tile.samples_per_side) ||
                sz >= static_cast<int>(tile.samples_per_side)) {
                continue;
            }
            const float height =
                chunk.heightmap_data[static_cast<std::size_t>(chunk_x) +
                                     static_cast<std::size_t>(chunk_z) * kHeightmapSide];
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
        tile.legacy_surface_authority = true;
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
    std::string validation_error;
    if (!ValidateTileStreams(tile, &validation_error)) {
        PushError(errors, validation_error);
        return false;
    }
    const std::size_t count = tile.sample_count();
    const std::size_t samples_per_brick = FarLodSdfBrickSampleCount(tile.tier);
    if (tile.sdf_bricks.size() > std::numeric_limits<u32>::max()) {
        PushError(errors, "far-LOD tile has too many SDF brick descriptors to serialize");
        return false;
    }

    Persistence::WorldSaveService::ContainerRecord record;
    record.id = tile_record_id(tile.tier, tile.rx, tile.rz);
    record.lod_level = static_cast<u8>(tile.tier);
    record.flags = tile.edited ? kTileRecordFlagEdited : 0u;

    std::string& payload = record.payload;
    payload.reserve(64 + count * 4 + tile.sdf_bricks.size() * (16 + samples_per_brick * 3));
    AppendBytes(payload, kFarLodPayloadMagic, sizeof(kFarLodPayloadMagic));
    AppendValue(payload, kFarLodPayloadVersion);
    AppendValue(payload, static_cast<u8>(tile.tier));
    AppendValue(payload, tile.rx);
    AppendValue(payload, tile.rz);
    AppendValue(payload, tile.samples_per_side);
    AppendValue(payload, tile.params_hash);
    AppendValue(payload, static_cast<u8>(tile.edited ? 1 : 0));
    AppendValue(payload, static_cast<u8>(tile.legacy_surface_authority ? 1 : 0));
    AppendValue(payload, static_cast<u32>(count));
    AppendValue(payload, static_cast<u32>(count * sizeof(u16)));
    AppendBytes(payload, tile.height_q.data(), count * sizeof(u16));
    AppendValue(payload, static_cast<u32>(count));
    AppendValue(payload, static_cast<u32>(count));
    AppendBytes(payload, tile.material.data(), count);
    AppendValue(payload, static_cast<u32>(count));
    AppendValue(payload, static_cast<u32>(count));
    AppendBytes(payload, tile.flags.data(), count);
    AppendValue(payload, static_cast<u32>(tile.sdf_bricks.size()));
    for (const FarLodSdfBrickDescriptor& descriptor : tile.sdf_bricks) {
        AppendValue(payload, descriptor.local_chunk_x);
        AppendValue(payload, descriptor.local_chunk_z);
        AppendValue(payload, static_cast<u8>(descriptor.source_kind));
        AppendValue(payload, descriptor.reserved);
        AppendValue(payload, descriptor.chunk_y);
        AppendValue(payload, descriptor.revision);
        AppendValue(payload, descriptor.payload_crc32);
    }
    AppendValue(payload, static_cast<u32>(tile.sdf_density_q.size()));
    AppendValue(payload, static_cast<u32>(tile.sdf_density_q.size() * sizeof(i16)));
    AppendBytes(payload, tile.sdf_density_q.data(), tile.sdf_density_q.size() * sizeof(i16));
    AppendValue(payload, static_cast<u32>(tile.sdf_material.size()));
    AppendValue(payload, static_cast<u32>(tile.sdf_material.size()));
    AppendBytes(payload, tile.sdf_material.data(), tile.sdf_material.size());

    const std::filesystem::path region_file =
        Persistence::WorldSaveService::region_file_path(m_save_dir, tile.rx, tile.rz);
    return Persistence::WorldSaveService::upsert_container_records(region_file, {record}, errors);
}

bool FarLodStore::load_tile(FarLodTier tier,
                            i32 rx,
                            i32 rz,
                            u64 expected_params_hash,
                            FarLodTile& out_tile,
                            std::vector<std::string>* errors) const {
    if (!IsValidFarLodTier(tier)) {
        PushError(errors, "far-LOD load requested an invalid tier");
        return false;
    }
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
        const bool is_v2 =
            record.payload.size() >= sizeof(kFarLodPayloadMagic) &&
            std::memcmp(record.payload.data(), kFarLodPayloadMagic, sizeof(kFarLodPayloadMagic)) ==
                0;
        std::size_t offset = 0;
        u8 tier_byte = 0;
        u8 edited_byte = 0;

        if (!is_v2) {
            const auto fail_legacy = [&](const std::string& message) {
                // Legacy records have no payload magic/version. The container
                // edited flag is therefore the only authority signal available
                // when their header itself is truncated.
                if (tile.edited || (record.flags & kTileRecordFlagEdited) != 0u) {
                    PushError(errors, message + ": " + region_file.string());
                }
                return false;
            };
            // Named migration: the old unversioned payload carried only the
            // height surface. Its exact fixed-length shape prevents an
            // arbitrary malformed v2 record from becoming synthetic authority.
            if (!ReadValue(record.payload, offset, tier_byte) ||
                !ReadValue(record.payload, offset, tile.rx) ||
                !ReadValue(record.payload, offset, tile.rz) ||
                !ReadValue(record.payload, offset, tile.samples_per_side) ||
                !ReadValue(record.payload, offset, tile.params_hash) ||
                !ReadValue(record.payload, offset, edited_byte)) {
                return fail_legacy("far-LOD legacy payload header is truncated");
            }
            tile.tier = static_cast<FarLodTier>(tier_byte);
            tile.edited = edited_byte != 0;
            if (tile.tier != tier || tile.rx != rx || tile.rz != rz ||
                tile.samples_per_side != FarLodSamplesPerSide(tier)) {
                return fail_legacy("far-LOD legacy payload header mismatch");
            }
            const std::size_t count = tile.sample_count();
            if (offset > record.payload.size() || record.payload.size() - offset != count * 4u) {
                return fail_legacy("far-LOD legacy payload sample stream is truncated");
            }
            tile.height_q.resize(count);
            tile.material.resize(count);
            tile.flags.resize(count);
            ReadBytes(record.payload, offset, tile.height_q.data(), count * sizeof(u16));
            ReadBytes(record.payload, offset, tile.material.data(), count);
            ReadBytes(record.payload, offset, tile.flags.data(), count);
            if (!tile.edited && tile.params_hash != expected_params_hash) {
                return false;
            }
            if (tile.edited) {
                tile.legacy_surface_authority = true;
                const bool any_edited =
                    std::any_of(tile.flags.begin(), tile.flags.end(), [](u8 flags) {
                        return (flags & kFarLodSampleFlagEdited) != 0;
                    });
                if (!any_edited) {
                    for (u8& flags : tile.flags) {
                        flags |= kFarLodSampleFlagEdited;
                    }
                }
            }
            out_tile = std::move(tile);
            return true;
        }

        u16 version = 0;
        u8 legacy_byte = 0;
        u32 height_count = 0, height_bytes = 0;
        u32 material_count = 0, material_bytes = 0;
        u32 flags_count = 0, flags_bytes = 0;
        u32 brick_count = 0;
        const auto fail_v2 = [&](const std::string& message) {
            // A corrupt cache can be regenerated. Records that claim any
            // authority are never replaced silently by analytic terrain.
            if (tile.edited || tile.legacy_surface_authority ||
                (record.flags & kTileRecordFlagEdited) != 0u) {
                PushError(errors, message + ": " + region_file.string());
            }
            return false;
        };
        offset = sizeof(kFarLodPayloadMagic);
        if (!ReadValue(record.payload, offset, version) || version != kFarLodPayloadVersion ||
            !ReadValue(record.payload, offset, tier_byte) ||
            !ReadValue(record.payload, offset, tile.rx) ||
            !ReadValue(record.payload, offset, tile.rz) ||
            !ReadValue(record.payload, offset, tile.samples_per_side) ||
            !ReadValue(record.payload, offset, tile.params_hash) ||
            !ReadValue(record.payload, offset, edited_byte) ||
            !ReadValue(record.payload, offset, legacy_byte) ||
            !ReadValue(record.payload, offset, height_count) ||
            !ReadValue(record.payload, offset, height_bytes)) {
            return fail_v2("far-LOD FSD2 payload header is truncated or unsupported");
        }
        tile.tier = static_cast<FarLodTier>(tier_byte);
        tile.edited = edited_byte != 0;
        tile.legacy_surface_authority = legacy_byte != 0;
        if (tile.tier != tier || tile.rx != rx || tile.rz != rz ||
            tile.samples_per_side != FarLodSamplesPerSide(tier) || (edited_byte > 1u) ||
            (legacy_byte > 1u)) {
            return fail_v2("far-LOD FSD2 payload header mismatch");
        }
        const std::size_t count = tile.sample_count();
        if (height_count != count || height_bytes != count * sizeof(u16) ||
            height_bytes > record.payload.size() - offset) {
            return fail_v2("far-LOD FSD2 height stream is invalid");
        }
        tile.height_q.resize(count);
        if (!ReadBytes(record.payload, offset, tile.height_q.data(), height_bytes) ||
            !ReadValue(record.payload, offset, material_count) ||
            !ReadValue(record.payload, offset, material_bytes) || material_count != count ||
            material_bytes != count || material_bytes > record.payload.size() - offset) {
            return fail_v2("far-LOD FSD2 material stream is invalid");
        }
        tile.material.resize(count);
        if (!ReadBytes(record.payload, offset, tile.material.data(), material_bytes) ||
            !ReadValue(record.payload, offset, flags_count) ||
            !ReadValue(record.payload, offset, flags_bytes) || flags_count != count ||
            flags_bytes != count || flags_bytes > record.payload.size() - offset) {
            return fail_v2("far-LOD FSD2 flags stream is invalid");
        }
        tile.flags.resize(count);
        if (!ReadBytes(record.payload, offset, tile.flags.data(), flags_bytes) ||
            !ReadValue(record.payload, offset, brick_count)) {
            return fail_v2("far-LOD FSD2 brick descriptor stream is truncated");
        }
        const std::size_t samples_per_brick = FarLodSdfBrickSampleCount(tier);
        if (brick_count > std::numeric_limits<std::size_t>::max() / samples_per_brick ||
            brick_count > (record.payload.size() - offset) / 16u) {
            return fail_v2("far-LOD FSD2 brick count overflows payload");
        }
        tile.sdf_bricks.resize(brick_count);
        for (FarLodSdfBrickDescriptor& descriptor : tile.sdf_bricks) {
            u8 source_kind = 0;
            if (!ReadValue(record.payload, offset, descriptor.local_chunk_x) ||
                !ReadValue(record.payload, offset, descriptor.local_chunk_z) ||
                !ReadValue(record.payload, offset, source_kind) ||
                !ReadValue(record.payload, offset, descriptor.reserved) ||
                !ReadValue(record.payload, offset, descriptor.chunk_y) ||
                !ReadValue(record.payload, offset, descriptor.revision) ||
                !ReadValue(record.payload, offset, descriptor.payload_crc32)) {
                return fail_v2("far-LOD FSD2 brick descriptor is truncated");
            }
            descriptor.source_kind = static_cast<FarLodBrickSourceKind>(source_kind);
        }
        u32 density_count = 0, density_bytes = 0, sdf_material_count = 0, sdf_material_bytes = 0;
        const std::size_t expected_payload_count =
            static_cast<std::size_t>(brick_count) * samples_per_brick;
        if (!ReadValue(record.payload, offset, density_count) ||
            !ReadValue(record.payload, offset, density_bytes) ||
            density_count != expected_payload_count ||
            density_bytes != expected_payload_count * sizeof(i16) ||
            density_bytes > record.payload.size() - offset) {
            return fail_v2("far-LOD FSD2 density stream is invalid");
        }
        tile.sdf_density_q.resize(expected_payload_count);
        if (!ReadBytes(record.payload, offset, tile.sdf_density_q.data(), density_bytes) ||
            !ReadValue(record.payload, offset, sdf_material_count) ||
            !ReadValue(record.payload, offset, sdf_material_bytes) ||
            sdf_material_count != expected_payload_count ||
            sdf_material_bytes != expected_payload_count ||
            sdf_material_bytes > record.payload.size() - offset) {
            return fail_v2("far-LOD FSD2 SDF material stream is invalid");
        }
        tile.sdf_material.resize(expected_payload_count);
        if (!ReadBytes(record.payload, offset, tile.sdf_material.data(), sdf_material_bytes) ||
            offset != record.payload.size()) {
            return fail_v2("far-LOD FSD2 payload has trailing or truncated bytes");
        }
        std::string validation_error;
        if (!ValidateTileStreams(tile, &validation_error)) {
            return fail_v2(validation_error);
        }
        const bool has_authoritative_brick =
            std::any_of(tile.sdf_bricks.begin(),
                        tile.sdf_bricks.end(),
                        [](const FarLodSdfBrickDescriptor& descriptor) {
                            return descriptor.source_kind == FarLodBrickSourceKind::Authoritative;
                        });
        if (!tile.legacy_surface_authority && !has_authoritative_brick &&
            tile.params_hash != expected_params_hash) {
            return false;
        }
        out_tile = std::move(tile);
        return true;
    }
    return false; // clean miss
}

} // namespace Luminumbra::World
