#include "FarWorldWaterMesh.h"

#include <cmath>
#include <glm/common.hpp>
#include <limits>

namespace Luminumbra::Rendering {
bool ValidFarWorldWaterBound(const FarWorldWaterBound& bound) noexcept {
    return std::isfinite(bound.min_y) && std::isfinite(bound.max_y) && bound.min_y <= bound.max_y;
}
bool BuildFarWorldWaterMesh(const FarVolumeRenderMesh& terrain,
                            const FarWorldWaterObservation& water,
                            const FarWorldWaterBound& bound,
                            const FarWorldWaterLimits& limits,
                            const World::FarVolumeMemory& memory,
                            FarVolumeRenderMesh& output,
                            FarWorldWaterError* error) {
    const auto fail = [error](FarWorldWaterError why) {
        if (error)
            *error = why;
        return false;
    };
    World::FarVolumeMemoryScope scope(memory, true);
    if (!memory.pool || !scope.valid())
        return fail(FarWorldWaterError::InvalidMemory);
    if (water.WaterCapability() == FarWorldWaterCapability::Unavailable)
        return fail(FarWorldWaterError::Unavailable);
    if (water.WaterCapability() != FarWorldWaterCapability::ExplicitlyDry &&
        water.WaterCapability() != FarWorldWaterCapability::Wet)
        return fail(FarWorldWaterError::InvalidObservation);
    const auto tier = World::FarTierAt(terrain.request.key.tier);
    if (!tier || water.content_identity() != terrain.request.field_identity ||
        !ValidFarWorldWaterBound(bound) || terrain.water_complete ||
        terrain.indices.size() != terrain.terrain_indices)
        return fail(FarWorldWaterError::InvalidObservation);
    if (limits.samples < kFarWorldWaterSamples || limits.cells < kFarWorldWaterCells)
        return fail(FarWorldWaterError::WorkLimit);
    if (!bound.known_dry && (bound.min_y < double(terrain.request.span.min_y) ||
                             bound.max_y > double(terrain.request.span.max_y)))
        return fail(FarWorldWaterError::SpanRefused);
    try {
        std::pmr::vector<FarWorldWaterSample> samples(scope.resource());
        samples.resize(kFarWorldWaterSamples);
        const auto spacing = tier->sample_spacing_meters;
        for (std::size_t z = 0; z < 129; ++z) {
            for (std::size_t x = 0; x < 129; ++x) {
                const auto sample =
                    water.WaterSample(terrain.origin_meters.x + std::int64_t(x) * spacing,
                                      terrain.origin_meters.z + std::int64_t(z) * spacing);
                if (!sample)
                    return fail(FarWorldWaterError::Unavailable);
                if (!std::isfinite(sample->surface_height) ||
                    sample->surface_height < bound.min_y || sample->surface_height > bound.max_y ||
                    (sample->wet &&
                     (bound.known_dry ||
                      water.WaterCapability() == FarWorldWaterCapability::ExplicitlyDry)))
                    return fail(FarWorldWaterError::InvalidObservation);
                samples[z * 129 + x] = *sample;
            }
        }
        const auto wet_cell = [&samples](std::size_t x, std::size_t z) {
            const auto base = z * 129 + x;
            return samples[base].wet || samples[base + 1].wet || samples[base + 129].wet ||
                   samples[base + 130].wet;
        };
        std::size_t cells = 0;
        for (std::size_t z = 0; z < 128; ++z)
            for (std::size_t x = 0; x < 128; ++x)
                cells += std::size_t(wet_cell(x, z));
        const auto added_vertices = cells * 4, added_indices = cells * 6;
        if (added_vertices > limits.vertices ||
            terrain.vertices.size() > limits.vertices - added_vertices ||
            added_indices > limits.indices ||
            terrain.indices.size() > limits.indices - added_indices)
            return fail(FarWorldWaterError::WorkLimit);
        const auto vertex_count = terrain.vertices.size() + added_vertices;
        const auto index_count = terrain.indices.size() + added_indices;
        if (vertex_count > std::numeric_limits<std::uint32_t>::max() ||
            index_count > std::size_t(std::numeric_limits<std::int32_t>::max()) ||
            vertex_count > limits.bytes / sizeof(VoxelVertex))
            return fail(FarWorldWaterError::ByteLimit);
        const auto vertex_bytes = std::uint64_t(vertex_count) * sizeof(VoxelVertex);
        if (index_count > (limits.bytes - vertex_bytes) / sizeof(std::uint32_t))
            return fail(FarWorldWaterError::ByteLimit);
        FarVolumeRenderMesh candidate;
        candidate.request = terrain.request;
        candidate.origin_meters = terrain.origin_meters;
        candidate.sampled_max_meters = terrain.sampled_max_meters;
        candidate.terrain_content = terrain.terrain_content;
        candidate.content = index_count ? World::FarVolumeContent::Mixed : terrain.terrain_content;
        candidate.terrain_indices = terrain.terrain_indices;
        candidate.water_indices = added_indices;
        candidate.water_complete = true;
        candidate.water_known_dry = cells == 0;
        candidate.bounds = terrain.bounds;
        if (!World::FarVolumeReserveCharged(
                candidate.vertices, vertex_count, vertex_count, scope.output()) ||
            !World::FarVolumeReserveCharged(
                candidate.indices, index_count, index_count, scope.output()))
            return fail(FarWorldWaterError::AllocationFailure);
        if (FarVolumeRenderRetainedBytes(candidate) > limits.bytes)
            return fail(FarWorldWaterError::ByteLimit);
        candidate.vertices.insert(
            candidate.vertices.end(), terrain.vertices.begin(), terrain.vertices.end());
        candidate.indices.insert(
            candidate.indices.end(), terrain.indices.begin(), terrain.indices.end());
        for (std::size_t z = 0; z < 128; ++z) {
            for (std::size_t x = 0; x < 128; ++x) {
                if (!wet_cell(x, z))
                    continue;
                const auto base = static_cast<std::uint32_t>(candidate.vertices.size());
                // (0,0), (0,1), (1,0), (1,1): cross(+Z,+X) is +Y.
                for (std::size_t corner = 0; corner < 4; ++corner) {
                    const auto sx = x + corner / 2, sz = z + corner % 2;
                    const Vec3 position{float(sx * spacing),
                                        float(double(samples[sz * 129 + sx].surface_height) -
                                              double(terrain.origin_meters.y)),
                                        float(sz * spacing)};
                    candidate.vertices.push_back({position, Vec3{0, 1, 0}, kFarWorldWaterMaterial});
                    if (auto& bounds = candidate.bounds; bounds.has_value()) {
                        auto& value = *bounds;
                        value.min = glm::min(value.min, position);
                        value.max = glm::max(value.max, position);
                    } else
                        candidate.bounds = FarVolumeRenderBounds{position, position};
                }
                for (const auto offset : {0u, 1u, 2u, 2u, 1u, 3u})
                    candidate.indices.push_back(base + offset);
            }
        }
        output = std::move(candidate);
        scope.Commit();
        if (error)
            *error = FarWorldWaterError::None;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(FarWorldWaterError::AllocationFailure);
    }
}
} // namespace Luminumbra::Rendering
