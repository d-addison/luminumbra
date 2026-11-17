#include "FarVolume.h"

#include "FarFrozenObservation.h"

#include "luminumbra/core/Types.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/FarLodStore.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeMesher.h"
#include "world/FarVolumeTile.h"
#include "world/HydraulicErosion.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

namespace Luminumbra::World {
namespace {
std::uint64_t ScopedIdentity(const Systems::SHIELD_WorldSystem& world,
                             FarCaveMode caves,
                             FarVolumeNumericProfile numeric,
                             int water_resolution) {
    const auto& params = world.get_params();
    if ((caves != FarCaveMode::BandLimited && caves != FarCaveMode::BoxFiltered) ||
        (numeric != FarVolumeNumericProfile::RawFloat &&
         numeric != FarVolumeNumericProfile::LegacySigned16) ||
        (water_resolution != 4 && water_resolution != 8 && water_resolution != 16 &&
         water_resolution != 32) ||
        ((params.biomes_enabled || params.structures_enabled) && !params.worldgen_dependencies)) {
        return 0;
    }
    auto identity = ComputeTerrainParamsHash(params, world.get_seed());
    const auto mix = [&identity](std::uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            identity ^= (value >> (8u * byte)) & 255u;
            identity *= 1099511628211ULL;
        }
    };
    mix(static_cast<std::uint64_t>(params.surface_sampling_revision));
    mix(params.biomes_enabled ? 1u : 0u);
    if (params.biomes_enabled) {
        mix(std::bit_cast<std::uint32_t>(params.temperature_frequency));
        mix(std::bit_cast<std::uint32_t>(params.humidity_frequency));
    }
    mix(params.structures_enabled ? 1u : 0u);
    mix(static_cast<std::uint8_t>(caves));
    mix(static_cast<std::uint8_t>(numeric));
    mix(static_cast<std::uint64_t>(water_resolution));
    mix(params.worldgen_dependencies ? 1u : 0u);
    if (params.worldgen_dependencies) {
        const auto& manifest = params.worldgen_dependencies->manifest;
        mix(manifest.size());
        for (const auto& record : manifest) {
            mix(record.size());
            for (const auto& field : record) {
                mix(field.size());
                for (const char byte : field) {
                    identity ^= static_cast<unsigned char>(byte);
                    identity *= 1099511628211ULL;
                }
            }
        }
    }
    return identity;
}
} // namespace

std::uint64_t WorldFarVolumeIdentity(const Systems::SHIELD_WorldSystem& world,
                                     FarCaveMode caves,
                                     FarVolumeNumericProfile numeric,
                                     int water_resolution) {
    const auto scope = world.acquire_worldgen_sample_scope();
    return ScopedIdentity(world, caves, numeric, water_resolution);
}

FarFrozenCaptureError
CaptureWorldFarVolumeObservation(const Systems::SHIELD_WorldSystem& world,
                                 const FarVolumeRequest& request,
                                 const FarFrozenCaptureSource& source,
                                 const FarFrozenCaptureLimits& limits,
                                 const std::shared_ptr<FarVolumeMemoryPool>& pool,
                                 std::shared_ptr<const FarFrozenObservation>& output) {
    try {
        const auto scope = world.acquire_worldgen_sample_scope();
        FarFrozenWorldSource cursor;
        cursor.Start(world, source);
        for (;;) {
            const auto result = cursor.Pump(std::numeric_limits<std::uint64_t>::max());
            if (result.error != FarFrozenCaptureError::None) {
                return result.error;
            }
            if (result.complete) {
                break;
            }
        }
        // Reference-wrapper function targets require no allocation. The existing
        // request/count/byte preflight therefore runs before any capture-owned
        // allocation, including when the supplied callbacks own large payloads.
        FarFrozenCaptureSource production;
        production.pristine.field_identity = source.pristine.field_identity;
        production.pristine.caves = source.pristine.caves;
        production.pristine.numeric = source.pristine.numeric;
        cursor.Bind(production);
        production.durable = source.durable;
        production.content_identity = source.content_identity;
        production.use = source.use;
        production.session_water_resolution = source.session_water_resolution;
        const auto borrow = [](auto& target, const auto& callback) {
            if (callback)
                target = std::cref(callback);
        };
        borrow(production.committed, source.committed);
        borrow(production.parked, source.parked);
        borrow(production.live, source.live);
        borrow(production.water, source.water);
        borrow(production.parked_water, source.parked_water);
        borrow(production.live_water, source.live_water);
        // All borrows end with this synchronous call; the frozen image retains
        // only scalar identities and sampled values, never these callbacks.
        return CaptureFarVolumeObservation(request, production, limits, pool, output);
    } catch (const std::bad_alloc&) {
        return FarFrozenCaptureError::AllocationFailure;
    } catch (...) {
        return FarFrozenCaptureError::Unavailable;
    }
}

namespace {
template<class T>
void MixScalar(std::uint64_t& hash, const T& value) {
    std::array<unsigned char, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        std::reverse(bytes.begin(), bytes.end());
    }
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
}
} // namespace
void FarFrozenWorldSource::Start(const Systems::SHIELD_WorldSystem& world,
                                 const FarFrozenCaptureSource& source) {
    world_ = &world;
    generation_ = world.water_seed_terrain_generation();
    expected_ = source.pristine.field_identity;
    caves_ = source.pristine.caves;
    numeric_ = source.pristine.numeric;
    resolution_ = source.session_water_resolution;
    session_ = source.use == FarFrozenUse::SessionRender;
    phase_ = cursor_ = record_ = field_ = 0;
    identity_ = 0;
}
bool FarFrozenWorldSource::current() const noexcept {
    return world_ && generation_ != std::numeric_limits<std::uint64_t>::max() &&
           generation_ == world_->water_seed_terrain_generation();
}
FarFrozenCapturePump FarFrozenWorldSource::Pump(std::uint64_t budget) {
    FarFrozenCapturePump result;
    while (result.work < budget) {
        if (!current()) {
            result.error = FarFrozenCaptureError::IdentityMismatch;
            return result;
        }
        ++result.work;
        result.error = Step();
        if (result.error != FarFrozenCaptureError::None) {
            return result;
        }
        if (phase_ == 9) {
            result.complete = true;
            return result;
        }
    }
    return result;
}
FarFrozenCaptureError FarFrozenWorldSource::Step() {
    const auto& params = world_->get_params();
    // Fixed parameter blocks fold at most 64 scalar values. No vectors are copied.
    switch (phase_) {
        case 0: {
            if (!session_) {
                phase_ = 9;
                break;
            }
            if (!expected_ ||
                (caves_ != FarCaveMode::BandLimited && caves_ != FarCaveMode::BoxFiltered) ||
                (numeric_ != FarVolumeNumericProfile::RawFloat &&
                 numeric_ != FarVolumeNumericProfile::LegacySigned16) ||
                (resolution_ != 4 && resolution_ != 8 && resolution_ != 16 && resolution_ != 32) ||
                ((params.biomes_enabled || params.structures_enabled) &&
                 !params.worldgen_dependencies)) {
                return FarFrozenCaptureError::IdentityMismatch;
            }
            auto& hash = identity_;
            hash = 14695981039346656037ULL;
            MixScalar(hash, world_->get_seed());
            MixScalar(hash, params.base_frequency);
            MixScalar(hash, params.base_amplitude);
            MixScalar(hash, params.octaves);
            MixScalar(hash, params.persistence);
            MixScalar(hash, params.lacunarity);
            MixScalar(hash, params.height_offset);
            MixScalar(hash, static_cast<u8>(params.caves_enabled ? 1 : 0));
            MixScalar(hash, params.cave_frequency);
            MixScalar(hash, params.cave_threshold);
            MixScalar(hash, params.cave_carve_value);
            if (params.caves_enabled) {
                MixScalar(hash, params.spaghetti_frequency);
                MixScalar(hash, params.spaghetti_thickness);
                MixScalar(hash, params.worley_frequency);
                MixScalar(hash, params.worley_threshold);
            }
            MixScalar(hash, static_cast<u8>(params.island_mask_enabled ? 1 : 0));
            MixScalar(hash, params.island_mask_frequency);
            if (params.biomes_enabled && params.biome_table_content_hash != 0) {
                MixScalar(hash, static_cast<u8>(1));
                MixScalar(hash, params.biome_table_content_hash);
            }
            if (params.rivers_enabled) {
                MixScalar(hash, static_cast<u8>(2));
                MixScalar(hash, params.river_frequency);
                MixScalar(hash, params.river_pv_min);
                MixScalar(hash, params.river_pv_max);
                MixScalar(hash, params.river_depth);
                MixScalar(hash, params.river_max_carve);
            }
            if (params.lakes_enabled) {
                MixScalar(hash, static_cast<u8>(4));
                MixScalar(hash, params.lake_frequency);
                MixScalar(hash, params.lake_threshold);
                MixScalar(hash, params.lake_depth);
                MixScalar(hash, params.lake_max_carve);
                MixScalar(hash, params.lake_bank_offset);
            }
            if (params.biome_relief_enabled) {
                MixScalar(hash, static_cast<u8>(5));
                MixScalar(hash, params.biome_relief_strength);
            }
            if (params.cliffs_enabled) {
                MixScalar(hash, static_cast<u8>(6));
                MixScalar(hash, params.cliff_frequency);
                MixScalar(hash, params.cliff_threshold);
                MixScalar(hash, params.cliff_step);
            }
            if (params.structures_enabled && params.structures_content_hash != 0) {
                MixScalar(hash, static_cast<u8>(3));
                MixScalar(hash, params.structures_content_hash);
            }

            MixScalar(hash, static_cast<u8>(4)); // marker 0x05 (4th conditional block)
            MixScalar(hash, params.continentalness_frequency);
            MixScalar(hash, params.erosion_frequency);
            MixScalar(hash, params.peaks_frequency);
            MixScalar(hash, params.peaks_amplitude);
            MixScalar(hash, params.domain_warp_amplitude);
            MixScalar(hash, params.domain_warp_frequency);
            phase_ = 1;
            break;
        }
        case 1:
        case 2:
        case 3: {
            const auto& spline = phase_ == 1   ? params.continental_spline
                                 : phase_ == 2 ? params.erosion_spline
                                               : params.peaks_spline;
            if (cursor_ == 0) {
                MixScalar(identity_, static_cast<std::uint64_t>(spline.size()));
            } else {
                MixScalar(identity_, spline[cursor_ - 1][0]);
                MixScalar(identity_, spline[cursor_ - 1][1]);
            }
            if (cursor_ == spline.size()) {
                cursor_ = 0;
                ++phase_;
            } else {
                ++cursor_;
            }
            break;
        }
        case 4: {
            if (params.hydro_enabled) {
                MixScalar(identity_, static_cast<u8>(5)); // marker 0x06 (5th conditional block)
                MixScalar(identity_, kHydraulicErosionWorldgenVersion);
                MixScalar(identity_, params.hydro_iterations);
                MixScalar(identity_, params.hydro_cell_size_m);
                MixScalar(identity_, params.hydro_talus_height);
                MixScalar(identity_, params.hydro_thermal_rate);
                MixScalar(identity_, params.hydro_rain_per_sweep);
                MixScalar(identity_, params.hydro_solubility);
                MixScalar(identity_, params.hydro_deposition);
                MixScalar(identity_, params.hydro_evaporation);
                MixScalar(identity_, params.hydro_sediment_capacity);
                MixScalar(identity_, params.hydro_max_offset);
            }
            if (params.surface_breaks_enabled) {
                MixScalar(identity_, static_cast<u8>(6)); // marker 0x07 (6th conditional block)
                MixScalar(identity_, params.surface_break_density);
                MixScalar(identity_, params.feature_cell_size);
                MixScalar(identity_, params.max_feature_radius);
                MixScalar(identity_, params.carve_smoothness);
                MixScalar(identity_, params.entrance_min_cap);
            }
            // ScopedIdentity's additional render-only scope uses uint64 scalars.
            const auto mix = [this](std::uint64_t value) {
                MixScalar(identity_, value);
            };
            mix(static_cast<std::uint64_t>(params.surface_sampling_revision));
            mix(params.biomes_enabled ? 1u : 0u);
            if (params.biomes_enabled) {
                mix(std::bit_cast<std::uint32_t>(params.temperature_frequency));
                mix(std::bit_cast<std::uint32_t>(params.humidity_frequency));
            }
            mix(params.structures_enabled ? 1u : 0u);
            mix(static_cast<std::uint8_t>(caves_));
            mix(static_cast<std::uint8_t>(numeric_));
            mix(static_cast<std::uint64_t>(resolution_));
            mix(params.worldgen_dependencies ? 1u : 0u);
            phase_ = params.worldgen_dependencies ? 5 : 8;
            break;
        }
        case 5:
            MixScalar(identity_,
                      static_cast<std::uint64_t>(params.worldgen_dependencies->manifest.size()));
            phase_ = 6;
            break;
        case 6: {
            const auto& manifest = params.worldgen_dependencies->manifest;
            if (record_ == manifest.size()) {
                phase_ = 8;
                break;
            }
            MixScalar(identity_, static_cast<std::uint64_t>(manifest[record_].size()));
            phase_ = 7;
            break;
        }
        case 7: {
            const auto& record = params.worldgen_dependencies->manifest[record_];
            if (field_ == record.size()) {
                field_ = 0;
                ++record_;
                phase_ = 6;
                break;
            }
            const auto& field = record[field_];
            if (cursor_ == 0) {
                MixScalar(identity_, static_cast<std::uint64_t>(field.size()));
            }
            const auto end = cursor_ + std::min(std::size_t{8}, field.size() - cursor_);
            for (; cursor_ < end; ++cursor_) {
                identity_ ^= static_cast<unsigned char>(field[cursor_]);
                identity_ *= 1099511628211ULL;
            }
            if (cursor_ == field.size()) {
                cursor_ = 0;
                ++field_;
            }
            break;
        }
        case 8:
            if (identity_ != expected_) {
                return FarFrozenCaptureError::IdentityMismatch;
            }
            phase_ = 9;
            break;
        default:
            break;
    }
    return FarFrozenCaptureError::None;
}
void FarFrozenWorldSource::Bind(FarFrozenCaptureSource& source) const {
    source.pristine.height = [world = world_](std::int64_t x, std::int64_t z) {
        return world->GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z));
    };
    source.pristine.density = [world = world_](const FarVolumePosition& position,
                                               float height,
                                               std::uint32_t spacing,
                                               FarCaveMode mode) {
        const Vec3 p(static_cast<float>(position.x),
                     static_cast<float>(position.y),
                     static_cast<float>(position.z));
        const auto material = p.y < height - 4.0f ? MaterialType::Stone
                                                  : world->SurfaceVertexMaterial(p.x, p.z, height);
        return FarVolumeSample{world->SamplePristineFarDensity(p, height, spacing, mode),
                               static_cast<std::uint8_t>(material)};
    };
}

bool CompilePristineFarVolume(const Systems::SHIELD_WorldSystem& world,
                              const FarVolumeCoverageIntent& intent,
                              std::uint64_t field_identity,
                              FarCaveMode caves,
                              FarVolumeNumericProfile numeric,
                              const FarVolumeCompilationLimits& limits,
                              FarVolumeCompilation& output,
                              FarVolumeFacadeFailure* failure) {
    return CompilePristineFarVolumeCharged(
        world, intent, field_identity, caves, numeric, limits, {}, output, failure);
}

namespace {
template<class Input>
bool CompilePristineWith(const Systems::SHIELD_WorldSystem& world,
                         const Input& input,
                         std::uint64_t field_identity,
                         FarCaveMode caves,
                         FarVolumeNumericProfile numeric,
                         const FarVolumeCompilationLimits& limits,
                         const FarVolumeMemory& memory,
                         FarVolumeCompilation& output,
                         FarVolumeFacadeFailure* failure) {
    try {
        // One scope owns every world observation, including the normal halo. The
        // synchronous operation never hands a borrowed world pointer to a worker.
        const auto scope = world.acquire_worldgen_sample_scope();
        FarVolumeAuthority authority;
        authority.field_identity = field_identity;
        authority.caves = caves;
        authority.numeric = numeric;
        authority.height = [&world](std::int64_t x, std::int64_t z) {
            return world.GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z));
        };
        authority.density = [&world](const FarVolumePosition& position,
                                     float height,
                                     std::uint32_t spacing,
                                     FarCaveMode mode) {
            // The facade checks integer coordinate representability before this
            // conversion. The existing sampler separately retains its filter guard.
            const Vec3 p(static_cast<float>(position.x),
                         static_cast<float>(position.y),
                         static_cast<float>(position.z));
            const auto material = p.y < height - 4.0f
                                      ? MaterialType::Stone
                                      : world.SurfaceVertexMaterial(p.x, p.z, height);
            return FarVolumeSample{world.SamplePristineFarDensity(p, height, spacing, mode),
                                   static_cast<std::uint8_t>(material)};
        };
        if constexpr (std::is_same_v<Input, FarVolumeRequest>) {
            return CompileFarVolumeSpanCharged(input, authority, limits, memory, output, failure);
        } else {
            return CompileFarVolumeCharged(input, authority, limits, memory, output, failure);
        }
    } catch (const std::bad_alloc&) {
        if (failure)
            *failure = {FarVolumeFacadeError::AllocationFailure,
                        FarVolumeBuildError::None,
                        FarVolumeMeshError::None,
                        {}};
        return false;
    } catch (...) {
        if (failure)
            *failure = {FarVolumeFacadeError::SamplerFailure,
                        FarVolumeBuildError::None,
                        FarVolumeMeshError::None,
                        {}};
        return false;
    }
}

} // namespace

bool CompilePristineFarVolumeCharged(const Systems::SHIELD_WorldSystem& world,
                                     const FarVolumeCoverageIntent& intent,
                                     std::uint64_t field_identity,
                                     FarCaveMode caves,
                                     FarVolumeNumericProfile numeric,
                                     const FarVolumeCompilationLimits& limits,
                                     const FarVolumeMemory& memory,
                                     FarVolumeCompilation& output,
                                     FarVolumeFacadeFailure* failure) {
    return CompilePristineWith(
        world, intent, field_identity, caves, numeric, limits, memory, output, failure);
}

bool CompilePristineFarVolumeSpan(const Systems::SHIELD_WorldSystem& world,
                                  const FarVolumeRequest& request,
                                  FarCaveMode caves,
                                  FarVolumeNumericProfile numeric,
                                  const FarVolumeCompilationLimits& limits,
                                  FarVolumeCompilation& output,
                                  FarVolumeFacadeFailure* failure) {
    return CompilePristineFarVolumeSpanCharged(
        world, request, caves, numeric, limits, {}, output, failure);
}

bool CompilePristineFarVolumeSpanCharged(const Systems::SHIELD_WorldSystem& world,
                                         const FarVolumeRequest& request,
                                         FarCaveMode caves,
                                         FarVolumeNumericProfile numeric,
                                         const FarVolumeCompilationLimits& limits,
                                         const FarVolumeMemory& memory,
                                         FarVolumeCompilation& output,
                                         FarVolumeFacadeFailure* failure) {
    return CompilePristineWith(
        world, request, request.field_identity, caves, numeric, limits, memory, output, failure);
}

} // namespace Luminumbra::World
