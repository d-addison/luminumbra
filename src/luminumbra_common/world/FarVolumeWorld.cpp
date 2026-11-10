#include "FarVolume.h"

#include "FarFrozenObservation.h"

#include "luminumbra/core/Types.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/FarLodStore.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeMesher.h"
#include "world/FarVolumeTile.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <new>

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
        if (source.use == FarFrozenUse::SessionRender &&
            (source.pristine.field_identity == 0 ||
             source.pristine.field_identity != ScopedIdentity(world,
                                                              source.pristine.caves,
                                                              source.pristine.numeric,
                                                              source.session_water_resolution))) {
            return FarFrozenCaptureError::IdentityMismatch;
        }
        const auto height_sampler = [&world](std::int64_t x, std::int64_t z) {
            return world.GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z));
        };
        const auto density_sampler = [&world](const FarVolumePosition& position,
                                              float height,
                                              std::uint32_t spacing,
                                              FarCaveMode mode) {
            const Vec3 p(static_cast<float>(position.x),
                         static_cast<float>(position.y),
                         static_cast<float>(position.z));
            const auto material = p.y < height - 4.0f
                                      ? MaterialType::Stone
                                      : world.SurfaceVertexMaterial(p.x, p.z, height);
            return FarVolumeSample{world.SamplePristineFarDensity(p, height, spacing, mode),
                                   static_cast<std::uint8_t>(material)};
        };
        // Reference-wrapper function targets require no allocation. The existing
        // request/count/byte preflight therefore runs before any capture-owned
        // allocation, including when the supplied callbacks own large payloads.
        FarFrozenCaptureSource production;
        production.pristine.field_identity = source.pristine.field_identity;
        production.pristine.caves = source.pristine.caves;
        production.pristine.numeric = source.pristine.numeric;
        production.pristine.height = std::cref(height_sampler);
        production.pristine.density = std::cref(density_sampler);
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

bool CompilePristineFarVolumeCharged(const Systems::SHIELD_WorldSystem& world,
                                     const FarVolumeCoverageIntent& intent,
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
        return CompileFarVolumeCharged(intent, authority, limits, memory, output, failure);
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

} // namespace Luminumbra::World
