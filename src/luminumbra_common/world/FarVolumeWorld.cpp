#include "FarVolume.h"

#include "FarFrozenObservation.h"

#include "luminumbra/core/Types.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeMesher.h"
#include "world/FarVolumeTile.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <new>

namespace Luminumbra::World {

FarFrozenCaptureError
CaptureWorldFarVolumeObservation(const Systems::SHIELD_WorldSystem& world,
                                 const FarVolumeRequest& request,
                                 const FarFrozenCaptureSource& source,
                                 const FarFrozenCaptureLimits& limits,
                                 const std::shared_ptr<FarVolumeMemoryPool>& pool,
                                 std::shared_ptr<const FarFrozenObservation>& output) {
    try {
        const auto scope = world.acquire_worldgen_sample_scope();
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
