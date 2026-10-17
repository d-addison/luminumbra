#pragma once

#include "UniqueTempPath.h"
#include "persistence/WorldSaveService.h"
#include "world/Chunk.h"
#include "world/FarEditedCache.h"
#include <algorithm>
#include <fstream>
#include <iterator>

namespace Luminumbra::TestSupport {
inline constexpr std::size_t kCacheLatticeSamples = 17 * 17 * 17;
inline std::shared_ptr<World::FarEditedCachePolicy> FarCachePolicy() {
    using namespace World;
    auto policy = std::make_shared<FarEditedCachePolicy>();
    auto& limits = policy->limits;
    limits.catalog.max_tiles = 128;
    limits.catalog.max_dependencies = 512;
    limits.catalog.max_bytes = 128 * 1024;
    auto& source_limits = limits.source;
    source_limits.max_tiles = 64;
    source_limits.max_members = 128;
    source_limits.max_records = 128;
    source_limits.max_directory_entries = 256;
    source_limits.max_file_bytes = 16 * 1024 * 1024;
    source_limits.max_decode_bytes = 32 * 1024 * 1024;
    source_limits.max_samples = 128 * kCacheLatticeSamples;
    source_limits.max_boundary_probes = 4'000'000;
    source_limits.max_selection_probes = 1'000'000;
    source_limits.max_image_bytes = 8 * 1024 * 1024;
    source_limits.max_scratch_bytes = 256 * 1024 * 1024;
    source_limits.memory = std::make_shared<FarVolumeMemoryPool>(2ull * 1024 * 1024 * 1024);
    auto& bake = limits.bake;
    bake.max_edits = 256;
    bake.max_source_records = 128;
    bake.max_affected_bricks = 8192;
    bake.max_tiles = 256;
    bake.max_brick_visits_per_tier.fill(500'000);
    bake.max_source_samples = 1'000'000;
    bake.max_source_probes = 20'000'000;
    bake.max_density_samples = 2'000'000;
    bake.max_scratch_bytes = 16 * 1024 * 1024;
    bake.max_output_bytes = 16 * 1024 * 1024;
    limits.frozen.max_regions = 8192;
    limits.frozen.max_height_samples = 1'000'000;
    limits.frozen.max_density_samples = 4'000'000;
    limits.frozen.max_water_samples = 1'000'000;
    limits.frozen.max_bytes = 128 * 1024 * 1024;
    limits.max_reconcile_probes = 100'000;
    limits.max_pipeline_bytes = 384 * 1024 * 1024;
    limits.max_cache_bytes = 256 * 1024 * 1024;
    limits.terrain_identity = 17;
    limits.content_identity = 29;
    limits.capture_source = [](const Persistence::EditedSourceImage& image) {
        FarFrozenCaptureSource source;
        source.durable = image.Identity();
        source.content_identity = 29;
        source.pristine.field_identity = 17;
        source.pristine.numeric = FarVolumeNumericProfile::LegacySigned16;
        source.pristine.height = [](std::int64_t, std::int64_t) {
            return std::optional<float>{0};
        };
        source.pristine.density = [](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
            return std::optional<FarVolumeSample>{{1, 0}};
        };
        // This fixture defines an explicitly dry pristine world and dry edited chunks.
        const bool dry = std::all_of(
            image.Water().begin(), image.Water().end(), [](const auto& w) { return !w.has_sim; });
        source.water = [dry](std::int64_t, std::int64_t) -> std::optional<FarFrozenWaterSample> {
            if (!dry)
                return std::nullopt;
            return FarFrozenWaterSample{};
        };
        return source;
    };
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        const auto edge = static_cast<std::int64_t>(kFarTierTable[tier - 1].tile_edge_meters);
        for (std::int64_t z = -1; z <= 0; ++z)
            for (std::int64_t x = -1; x <= 0; ++x)
                policy->selection.push_back(
                    {{tier, x, z},
                     {{x * edge, -256, z * edge}, {(x + 1) * edge, 256, (z + 1) * edge}}});
    }
    return policy;
}
inline std::shared_ptr<Chunk> FarCacheEditedChunk(WorldStreamingState& state,
                                                  IVec3 coords = {0, 0, 0}) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->sdf_data.assign(kCacheLatticeSamples, 1);
    chunk->material_data.assign(kCacheLatticeSamples, 0);
    chunk->sdf_data[1 + 17 * (1 + 17)] = -3;
    chunk->material_data[1 + 17 * (1 + 17)] = 7;
    chunk->mark_voxel_data_dirty();
    return chunk;
}
inline std::string FarCacheRead(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace Luminumbra::TestSupport
