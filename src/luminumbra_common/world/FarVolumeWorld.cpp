#include "FarVolume.h"

#include "systems/SHIELD_WorldSystem.h"

namespace Luminumbra::World {

bool CompilePristineFarVolume(const Systems::SHIELD_WorldSystem& world,
                              const FarVolumeCoverageIntent& intent,
                              std::uint64_t field_identity,
                              FarCaveMode caves,
                              FarVolumeNumericProfile numeric,
                              const FarVolumeCompilationLimits& limits,
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
        return CompileFarVolume(intent, authority, limits, output, failure);
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
