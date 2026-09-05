#include "FarLodSystem.h"

#include "Shader.h"
#include "core/Log.h"
#include "luminumbra_common/core/Crc32.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/MarchingCubes.h"
#include "passes/PassGlHelpers.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace Luminumbra::Rendering {
namespace {

constexpr float kRegionSize = static_cast<float>(Luminumbra::World::kFarLodRegionSizeMeters);
constexpr int kChunksPerFarLodRegion =
    Luminumbra::World::kFarLodRegionSizeMeters / Luminumbra::CHUNK_SIZE_X;

int floor_divide(int value, int divisor) {
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

bool belongs_to_region(const Luminumbra::Systems::FarLodSdfSnapshotEntry& entry, int rx, int rz) {
    return floor_divide(entry.coords.x, kChunksPerFarLodRegion) == rx &&
           floor_divide(entry.coords.z, kChunksPerFarLodRegion) == rz;
}

std::size_t far_lod_tile_bytes(const Luminumbra::World::FarLodTile& tile) {
    return tile.height_q.size() * sizeof(u16) + tile.material.size() * sizeof(u8) +
           tile.flags.size() * sizeof(u8) +
           tile.sdf_bricks.size() * sizeof(Luminumbra::World::FarLodSdfBrickDescriptor) +
           tile.sdf_density_q.size() * sizeof(i16) + tile.sdf_material.size() * sizeof(u8);
}

// Point-to-rect horizontal distances of a region footprint from a position.
float region_nearest_distance(int rx, int rz, const glm::vec3& position) {
    const float min_x = static_cast<float>(rx) * kRegionSize;
    const float min_z = static_cast<float>(rz) * kRegionSize;
    const float dx = std::max({min_x - position.x, 0.0f, position.x - (min_x + kRegionSize)});
    const float dz = std::max({min_z - position.z, 0.0f, position.z - (min_z + kRegionSize)});
    return std::sqrt(dx * dx + dz * dz);
}

float region_farthest_distance(int rx, int rz, const glm::vec3& position) {
    const float min_x = static_cast<float>(rx) * kRegionSize;
    const float min_z = static_cast<float>(rz) * kRegionSize;
    const float dx =
        std::max(std::abs(position.x - min_x), std::abs(position.x - (min_x + kRegionSize)));
    const float dz =
        std::max(std::abs(position.z - min_z), std::abs(position.z - (min_z + kRegionSize)));
    return std::sqrt(dx * dx + dz * dz);
}

bool aabb_outside_frustum(const glm::vec3& aabb_min,
                          const glm::vec3& aabb_max,
                          const glm::vec4 frustum_planes[6]) {
    for (int i = 0; i < 6; ++i) {
        const glm::vec4& plane = frustum_planes[i];
        const glm::vec3 positive(plane.x >= 0.0f ? aabb_max.x : aabb_min.x,
                                 plane.y >= 0.0f ? aabb_max.y : aabb_min.y,
                                 plane.z >= 0.0f ? aabb_max.z : aabb_min.z);
        if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f) {
            return true;
        }
    }
    return false;
}

// build a flat water sheet for a far tile. Emits one
// quad per sample-grid cell that touches a water-flagged sample, placed at the
// global waterline (SEA_LEVEL). Vertices are region-local in X/Z (matching the
// terrain mesh convention) and absolute in Y; normals point up; the material id
// is the dedicated far-water id (G-buffer tints it deep water). Cell-level
// coverage (any of the 4 corners water-flagged) keeps the sheet continuous
// across the channel/sea boundary instead of leaving sub-cell gaps.
void BuildFarLodWaterSheet(const Luminumbra::World::FarLodTile& tile,
                           Luminumbra::World::FarLodRegionMesh& out) {
    out.vertices.clear();
    out.indices.clear();
    const u32 n = tile.samples_per_side;
    if (n < 2 || tile.flags.size() < static_cast<std::size_t>(n) * n) {
        return;
    }
    const int step = Luminumbra::World::FarLodSampleStepMeters(tile.tier);
    const float waterline = Luminumbra::SEA_LEVEL;

    const auto water_at = [&](u32 x, u32 z) -> bool {
        return (tile.flags[static_cast<std::size_t>(z) * n + x] &
                Luminumbra::World::kFarLodSampleFlagWater) != 0u;
    };

    // Reserve a rough upper bound (every cell wet) to avoid reallocation churn.
    out.vertices.reserve(static_cast<std::size_t>(n) * n);
    out.indices.reserve(static_cast<std::size_t>(n - 1) * (n - 1) * 6u);

    for (u32 z = 0; z + 1 < n; ++z) {
        for (u32 x = 0; x + 1 < n; ++x) {
            if (!water_at(x, z) && !water_at(x + 1, z) && !water_at(x, z + 1) &&
                !water_at(x + 1, z + 1)) {
                continue;
            }
            const float x0 = static_cast<float>(x * static_cast<u32>(step));
            const float x1 = static_cast<float>((x + 1) * static_cast<u32>(step));
            const float z0 = static_cast<float>(z * static_cast<u32>(step));
            const float z1 = static_cast<float>((z + 1) * static_cast<u32>(step));
            const u32 base = static_cast<u32>(out.vertices.size());
            const Vec3 up(0.0f, 1.0f, 0.0f);
            out.vertices.push_back(
                {Vec3(x0, waterline, z0), up, FarLodSystem::kFarWaterMaterialId});
            out.vertices.push_back(
                {Vec3(x1, waterline, z0), up, FarLodSystem::kFarWaterMaterialId});
            out.vertices.push_back(
                {Vec3(x1, waterline, z1), up, FarLodSystem::kFarWaterMaterialId});
            out.vertices.push_back(
                {Vec3(x0, waterline, z1), up, FarLodSystem::kFarWaterMaterialId});
            // wind the quads COUNTER-clockwise as
            // seen from ABOVE (+Y front face). The original (0,1,2)/(0,2,3)
            // order produced -Y face normals, so with GL_CULL_FACE/GL_BACK in
            // the G-buffer pass EVERY sheet triangle was backface-culled - the
            // draws were submitted (water_sheet_draws ~17, ~1M indices) but
            // rasterized ZERO pixels, leaving the far sea as skybox haze
            // showing through the id-7-discarded far terrain.
            // (v2-v0)x(v1-v0) = +Y for (0,2,1); same for (0,3,2).
            out.indices.push_back(base + 0u);
            out.indices.push_back(base + 2u);
            out.indices.push_back(base + 1u);
            out.indices.push_back(base + 0u);
            out.indices.push_back(base + 3u);
            out.indices.push_back(base + 2u);
        }
    }
}

bool tile_has_authoritative_bricks(const Luminumbra::World::FarLodTile& tile) {
    return std::any_of(tile.sdf_bricks.begin(),
                       tile.sdf_bricks.end(),
                       [](const Luminumbra::World::FarLodSdfBrickDescriptor& brick) {
                           return brick.source_kind ==
                                  Luminumbra::World::FarLodBrickSourceKind::Authoritative;
                       });
}

Luminumbra::World::FarLodTile
rebase_authoritative_tile(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                          Luminumbra::World::FarLodTile&& stale,
                          Luminumbra::World::FarLodTier tier,
                          int rx,
                          int rz,
                          u64 params_hash) {
    auto fresh = Luminumbra::World::BuildPristineFarLodTile(world, tier, rx, rz, params_hash);
    const std::size_t samples = Luminumbra::World::FarLodSdfBrickSampleCount(tier);
    for (std::size_t brick_index = 0; brick_index < stale.sdf_bricks.size(); ++brick_index) {
        const auto& descriptor = stale.sdf_bricks[brick_index];
        if (descriptor.source_kind != Luminumbra::World::FarLodBrickSourceKind::Authoritative) {
            continue;
        }
        fresh.sdf_bricks.push_back(descriptor);
        const std::size_t begin = brick_index * samples;
        fresh.sdf_density_q.insert(fresh.sdf_density_q.end(),
                                   stale.sdf_density_q.begin() + static_cast<std::ptrdiff_t>(begin),
                                   stale.sdf_density_q.begin() +
                                       static_cast<std::ptrdiff_t>(begin + samples));
        fresh.sdf_material.insert(fresh.sdf_material.end(),
                                  stale.sdf_material.begin() + static_cast<std::ptrdiff_t>(begin),
                                  stale.sdf_material.begin() +
                                      static_cast<std::ptrdiff_t>(begin + samples));
        fresh.edited = true;
    }
    return fresh;
}

bool complete_authoritative_stacks(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                                   Luminumbra::World::FarLodTile& tile,
                                   std::string& error,
                                   bool& changed) {
    struct AuthorityColumn {
        int x = 0;
        int z = 0;
        int min_y = 0;
        int max_y = 0;
    };
    std::vector<AuthorityColumn> columns;
    for (const auto& brick : tile.sdf_bricks) {
        if (brick.source_kind != Luminumbra::World::FarLodBrickSourceKind::Authoritative) {
            continue;
        }
        auto found =
            std::find_if(columns.begin(), columns.end(), [&](const AuthorityColumn& column) {
                return column.x == brick.local_chunk_x && column.z == brick.local_chunk_z;
            });
        if (found == columns.end()) {
            columns.push_back(
                {brick.local_chunk_x, brick.local_chunk_z, brick.chunk_y, brick.chunk_y});
        } else {
            found->min_y = std::min(found->min_y, brick.chunk_y);
            found->max_y = std::max(found->max_y, brick.chunk_y);
        }
    }
    if (columns.empty()) {
        return true;
    }

    const int sample_step = Luminumbra::World::FarLodSampleStepMeters(tile.tier);
    std::set<std::tuple<int, int, int>> required; // (z, x, y), canonical
    for (const AuthorityColumn& column : columns) {
        if (column.x <= 0 || column.x >= 31 || column.z <= 0 || column.z >= 31) {
            error = "authoritative far-SDF column needs a cross-region halo";
            return false;
        }
        int min_y = column.min_y - 1;
        int max_y = column.max_y + 1;
        for (int halo_z = column.z - 1; halo_z <= column.z + 1; ++halo_z) {
            for (int halo_x = column.x - 1; halo_x <= column.x + 1; ++halo_x) {
                const int base_x = halo_x * Luminumbra::CHUNK_SIZE_X;
                const int base_z = halo_z * Luminumbra::CHUNK_SIZE_Z;
                for (int local_z = 0; local_z <= Luminumbra::CHUNK_SIZE_Z; local_z += sample_step) {
                    for (int local_x = 0; local_x <= Luminumbra::CHUNK_SIZE_X;
                         local_x += sample_step) {
                        const std::size_t sx =
                            static_cast<std::size_t>((base_x + local_x) / sample_step);
                        const std::size_t sz =
                            static_cast<std::size_t>((base_z + local_z) / sample_step);
                        const float height = Luminumbra::World::DequantizeFarLodHeight(
                            tile.height_q[sx + sz * tile.samples_per_side]);
                        const int surface_y = static_cast<int>(
                            std::floor(height / static_cast<float>(Luminumbra::CHUNK_SIZE_Y)));
                        min_y = std::min(min_y, surface_y - 1);
                        max_y = std::max(max_y, surface_y + 1);
                    }
                }
            }
        }
        for (int halo_z = column.z - 1; halo_z <= column.z + 1; ++halo_z) {
            for (int halo_x = column.x - 1; halo_x <= column.x + 1; ++halo_x) {
                for (int chunk_y = min_y; chunk_y <= max_y; ++chunk_y) {
                    required.emplace(halo_z, halo_x, chunk_y);
                }
            }
        }
    }

    const auto exists = [&](int x, int y, int z) {
        return std::any_of(tile.sdf_bricks.begin(), tile.sdf_bricks.end(), [&](const auto& brick) {
            return brick.local_chunk_x == x && brick.local_chunk_z == z && brick.chunk_y == y;
        });
    };
    const auto worldgen_scope = world.acquire_worldgen_sample_scope();
    for (const auto& [local_z, local_x, chunk_y] : required) {
        if (exists(local_x, chunk_y, local_z)) {
            continue;
        }
        const IVec3 coords(tile.rx * kChunksPerFarLodRegion + local_x,
                           chunk_y,
                           tile.rz * kChunksPerFarLodRegion + local_z);
        Luminumbra::Chunk scratch(coords);
        world.GenerateChunkData(scratch, 1);
        Luminumbra::World::FarLodSdfSnapshot generated;
        generated.coords = coords;
        generated.source_kind = Luminumbra::World::FarLodBrickSourceKind::RegenerableCache;
        generated.sdf_data = std::move(scratch.sdf_data);
        generated.material_data = std::move(scratch.material_data);
        std::string reduction_error;
        const auto reduced =
            Luminumbra::World::ReduceChunkSdfIntoFarTile(tile, generated, &reduction_error);
        if (reduced == Luminumbra::World::FarLodSdfReduceResult::Error) {
            error = "failed to generate far-SDF halo brick: " + reduction_error;
            return false;
        }
        changed = changed || reduced != Luminumbra::World::FarLodSdfReduceResult::Unchanged;
    }
    return true;
}

u32 far_brick_crc(const i16* density, const u8* material, std::size_t count) {
    Luminumbra::Core::Crc32Accumulator crc;
    crc.Update(density, count * sizeof(i16));
    crc.Update(material, count);
    return crc.Value();
}

bool synchronize_regenerable_boundaries(Luminumbra::World::FarLodTile& tile,
                                        std::string& error,
                                        bool& changed) {
    using Sample = std::pair<i16, u8>;
    std::map<std::tuple<int, int, int>, Sample> authority_samples;
    const u32 side = Luminumbra::World::FarLodSdfBrickSamplesPerSide(tile.tier);
    const int step = Luminumbra::World::FarLodSampleStepMeters(tile.tier);
    const std::size_t count = Luminumbra::World::FarLodSdfBrickSampleCount(tile.tier);
    for (std::size_t brick_index = 0; brick_index < tile.sdf_bricks.size(); ++brick_index) {
        const auto& brick = tile.sdf_bricks[brick_index];
        if (brick.source_kind != Luminumbra::World::FarLodBrickSourceKind::Authoritative) {
            continue;
        }
        const std::size_t payload = brick_index * count;
        for (u32 z = 0; z < side; ++z) {
            for (u32 y = 0; y < side; ++y) {
                for (u32 x = 0; x < side; ++x) {
                    const std::size_t sample = payload + static_cast<std::size_t>(x) +
                                               static_cast<std::size_t>(y) * side +
                                               static_cast<std::size_t>(z) * side * side;
                    const auto key = std::make_tuple(
                        static_cast<int>(brick.local_chunk_x) * Luminumbra::CHUNK_SIZE_X +
                            static_cast<int>(x) * step,
                        brick.chunk_y * Luminumbra::CHUNK_SIZE_Y + static_cast<int>(y) * step,
                        static_cast<int>(brick.local_chunk_z) * Luminumbra::CHUNK_SIZE_Z +
                            static_cast<int>(z) * step);
                    const Sample value(tile.sdf_density_q[sample], tile.sdf_material[sample]);
                    const auto [found, inserted] = authority_samples.emplace(key, value);
                    if (!inserted && found->second != value) {
                        error = "adjacent authoritative far-SDF bricks disagree on a shared sample";
                        return false;
                    }
                }
            }
        }
    }

    for (std::size_t brick_index = 0; brick_index < tile.sdf_bricks.size(); ++brick_index) {
        auto& brick = tile.sdf_bricks[brick_index];
        if (brick.source_kind != Luminumbra::World::FarLodBrickSourceKind::RegenerableCache) {
            continue;
        }
        const std::size_t payload = brick_index * count;
        bool brick_changed = false;
        for (u32 z = 0; z < side; ++z) {
            for (u32 y = 0; y < side; ++y) {
                for (u32 x = 0; x < side; ++x) {
                    const auto key = std::make_tuple(
                        static_cast<int>(brick.local_chunk_x) * Luminumbra::CHUNK_SIZE_X +
                            static_cast<int>(x) * step,
                        brick.chunk_y * Luminumbra::CHUNK_SIZE_Y + static_cast<int>(y) * step,
                        static_cast<int>(brick.local_chunk_z) * Luminumbra::CHUNK_SIZE_Z +
                            static_cast<int>(z) * step);
                    const auto authoritative = authority_samples.find(key);
                    if (authoritative == authority_samples.end()) {
                        continue;
                    }
                    const std::size_t sample = payload + static_cast<std::size_t>(x) +
                                               static_cast<std::size_t>(y) * side +
                                               static_cast<std::size_t>(z) * side * side;
                    if (tile.sdf_density_q[sample] != authoritative->second.first ||
                        tile.sdf_material[sample] != authoritative->second.second) {
                        tile.sdf_density_q[sample] = authoritative->second.first;
                        tile.sdf_material[sample] = authoritative->second.second;
                        brick_changed = true;
                    }
                }
            }
        }
        if (brick_changed) {
            brick.payload_crc32 = far_brick_crc(
                tile.sdf_density_q.data() + payload, tile.sdf_material.data() + payload, count);
            changed = true;
        }
    }
    return true;
}

bool derive_authoritative_surface_water(Luminumbra::World::FarLodTile& tile,
                                        std::string& error,
                                        bool& changed) {
    const u32 side = Luminumbra::World::FarLodSdfBrickSamplesPerSide(tile.tier);
    const int step = Luminumbra::World::FarLodSampleStepMeters(tile.tier);
    const std::size_t count = Luminumbra::World::FarLodSdfBrickSampleCount(tile.tier);
    std::set<std::pair<int, int>> authoritative_columns;
    for (const auto& brick : tile.sdf_bricks) {
        if (brick.source_kind == Luminumbra::World::FarLodBrickSourceKind::Authoritative) {
            authoritative_columns.emplace(brick.local_chunk_x, brick.local_chunk_z);
        }
    }
    for (const auto& [column_x, column_z] : authoritative_columns) {
        for (u32 local_z = 0; local_z < side; ++local_z) {
            for (u32 local_x = 0; local_x < side; ++local_x) {
                std::map<int, i16> vertical;
                for (std::size_t brick_index = 0; brick_index < tile.sdf_bricks.size();
                     ++brick_index) {
                    const auto& brick = tile.sdf_bricks[brick_index];
                    if (brick.local_chunk_x != column_x || brick.local_chunk_z != column_z) {
                        continue;
                    }
                    const std::size_t payload = brick_index * count;
                    for (u32 local_y = 0; local_y < side; ++local_y) {
                        const std::size_t sample = payload + static_cast<std::size_t>(local_x) +
                                                   static_cast<std::size_t>(local_y) * side +
                                                   static_cast<std::size_t>(local_z) * side * side;
                        const int world_y = brick.chunk_y * Luminumbra::CHUNK_SIZE_Y +
                                            static_cast<int>(local_y) * step;
                        const auto [found, inserted] =
                            vertical.emplace(world_y, tile.sdf_density_q[sample]);
                        if (!inserted && found->second != tile.sdf_density_q[sample]) {
                            error = "far-SDF water derivation found a mismatched vertical boundary "
                                    "sample";
                            return false;
                        }
                    }
                }
                bool found_surface = false;
                float top_surface_y = 0.0f;
                for (auto lower = vertical.begin(); lower != vertical.end(); ++lower) {
                    auto upper = std::next(lower);
                    if (upper == vertical.end()) {
                        break;
                    }
                    const float d0 = Luminumbra::World::DequantizeFarLodSdf(lower->second);
                    const float d1 = Luminumbra::World::DequantizeFarLodSdf(upper->second);
                    if (d0 <= 0.0f && d1 > 0.0f) {
                        const float denominator = d1 - d0;
                        const float t = denominator > 0.0f ? -d0 / denominator : 0.0f;
                        top_surface_y = static_cast<float>(lower->first) +
                                        t * static_cast<float>(upper->first - lower->first);
                        found_surface = true;
                    }
                }
                if (!found_surface) {
                    error = "far-SDF water derivation found no top solid crossing";
                    return false;
                }
                const std::size_t sample_x = static_cast<std::size_t>(
                    (column_x * Luminumbra::CHUNK_SIZE_X + static_cast<int>(local_x) * step) /
                    step);
                const std::size_t sample_z = static_cast<std::size_t>(
                    (column_z * Luminumbra::CHUNK_SIZE_Z + static_cast<int>(local_z) * step) /
                    step);
                u8& flags = tile.flags[sample_x + sample_z * tile.samples_per_side];
                const u8 next =
                    top_surface_y < Luminumbra::SEA_LEVEL
                        ? static_cast<u8>(flags | Luminumbra::World::kFarLodSampleFlagWater)
                        : static_cast<u8>(flags & ~Luminumbra::World::kFarLodSampleFlagWater);
                if (next != flags) {
                    flags = next;
                    changed = true;
                }
            }
        }
    }
    return true;
}

using WorldBrick = Luminumbra::World::FarLodWorldSdfBrickDescriptor;
using Assembly = Luminumbra::World::FarLodRegionSdfAssembly;

constexpr std::size_t kMaxTransientAssemblyBricks = 1u << 18;

bool valid_region_coordinate(int region) {
    const std::int64_t minimum = static_cast<std::int64_t>(region) * World::kFarLodRegionSizeMeters;
    const std::int64_t maximum = minimum + World::kFarLodRegionSizeMeters;
    const std::int64_t min_chunk = static_cast<std::int64_t>(region) * kChunksPerFarLodRegion - 1;
    const std::int64_t max_chunk = min_chunk + kChunksPerFarLodRegion + 1;
    return minimum >= std::numeric_limits<int>::min() &&
           maximum <= std::numeric_limits<int>::max() && min_chunk >= Chunk::kPackedMinXz &&
           max_chunk <= Chunk::kPackedMaxXz;
}

bool valid_chunk_coordinate(int chunk, int extent) {
    const std::int64_t minimum = static_cast<std::int64_t>(chunk) * extent;
    const std::int64_t maximum = minimum + extent;
    return minimum >= std::numeric_limits<int>::min() && maximum <= std::numeric_limits<int>::max();
}

bool valid_world_brick(const WorldBrick& brick) {
    return brick.chunk_x >= Chunk::kPackedMinXz && brick.chunk_x <= Chunk::kPackedMaxXz &&
           brick.chunk_y >= Chunk::kPackedMinY && brick.chunk_y <= Chunk::kPackedMaxY &&
           brick.chunk_z >= Chunk::kPackedMinXz && brick.chunk_z <= Chunk::kPackedMaxXz &&
           valid_chunk_coordinate(brick.chunk_x, CHUNK_SIZE_X) &&
           valid_chunk_coordinate(brick.chunk_y, CHUNK_SIZE_Y) &&
           valid_chunk_coordinate(brick.chunk_z, CHUNK_SIZE_Z);
}

int region_for_chunk(int chunk) {
    return floor_divide(chunk, kChunksPerFarLodRegion);
}

bool is_home_chunk(int chunk_x, int chunk_z, int rx, int rz) {
    const std::int64_t min_x = static_cast<std::int64_t>(rx) * kChunksPerFarLodRegion;
    const std::int64_t min_z = static_cast<std::int64_t>(rz) * kChunksPerFarLodRegion;
    return static_cast<std::int64_t>(chunk_x) >= min_x &&
           static_cast<std::int64_t>(chunk_x) < min_x + kChunksPerFarLodRegion &&
           static_cast<std::int64_t>(chunk_z) >= min_z &&
           static_cast<std::int64_t>(chunk_z) < min_z + kChunksPerFarLodRegion;
}

bool is_halo_chunk(int chunk_x, int chunk_z, int rx, int rz) {
    const std::int64_t min_x = static_cast<std::int64_t>(rx) * kChunksPerFarLodRegion - 1;
    const std::int64_t min_z = static_cast<std::int64_t>(rz) * kChunksPerFarLodRegion - 1;
    return static_cast<std::int64_t>(chunk_x) >= min_x &&
           static_cast<std::int64_t>(chunk_x) <= min_x + kChunksPerFarLodRegion + 1 &&
           static_cast<std::int64_t>(chunk_z) >= min_z &&
           static_cast<std::int64_t>(chunk_z) <= min_z + kChunksPerFarLodRegion + 1;
}

bool world_brick_less(const WorldBrick& a, const WorldBrick& b) {
    return std::tie(a.chunk_z, a.chunk_x, a.chunk_y) < std::tie(b.chunk_z, b.chunk_x, b.chunk_y);
}

u32 world_brick_crc(const std::vector<i16>& density, const std::vector<u8>& material) {
    return far_brick_crc(density.data(), material.data(), density.size());
}

// Use the persisted reduction operator as the single source of aligned
// quantization truth.  The short-lived home tile is only a conversion vehicle;
// no descriptor from it escapes as a persisted tile descriptor.
bool reduce_world_brick(World::FarLodTier tier,
                        const World::FarLodSdfSnapshot& snapshot,
                        WorldBrick& out_descriptor,
                        std::vector<i16>& out_density,
                        std::vector<u8>& out_material,
                        std::string& error) {
    const WorldBrick input_coordinates{snapshot.coords.x, snapshot.coords.z, snapshot.coords.y};
    if (!valid_world_brick(input_coordinates)) {
        error = "far-SDF brick coordinates exceed the supported world range";
        return false;
    }
    World::FarLodTile scratch;
    scratch.tier = tier;
    scratch.rx = region_for_chunk(snapshot.coords.x);
    scratch.rz = region_for_chunk(snapshot.coords.z);
    scratch.samples_per_side = World::FarLodSamplesPerSide(tier);
    scratch.height_q.resize(scratch.sample_count());
    scratch.material.resize(scratch.sample_count());
    scratch.flags.resize(scratch.sample_count());
    if (World::ReduceChunkSdfIntoFarTile(scratch, snapshot, &error) ==
        World::FarLodSdfReduceResult::Error) {
        return false;
    }
    if (scratch.sdf_bricks.size() != 1u) {
        error = "far-SDF reduction did not produce exactly one world brick";
        return false;
    }
    const auto& descriptor = scratch.sdf_bricks.front();
    out_descriptor = {snapshot.coords.x,
                      snapshot.coords.z,
                      descriptor.chunk_y,
                      descriptor.source_kind,
                      descriptor.revision,
                      descriptor.payload_crc32};
    out_density = std::move(scratch.sdf_density_q);
    out_material = std::move(scratch.sdf_material);
    return true;
}

struct PendingWorldBrick {
    WorldBrick descriptor;
    std::vector<i16> density;
    std::vector<u8> material;
    bool persisted = false;
};

bool add_or_overlay_world_brick(std::vector<PendingWorldBrick>& bricks,
                                PendingWorldBrick incoming,
                                bool snapshot_overlay,
                                std::string& error) {
    const auto key = std::tie(
        incoming.descriptor.chunk_z, incoming.descriptor.chunk_x, incoming.descriptor.chunk_y);
    const auto found =
        std::find_if(bricks.begin(), bricks.end(), [&](const PendingWorldBrick& existing) {
            return std::tie(existing.descriptor.chunk_z,
                            existing.descriptor.chunk_x,
                            existing.descriptor.chunk_y) == key;
        });
    if (found == bricks.end()) {
        bricks.push_back(std::move(incoming));
        return true;
    }
    if (snapshot_overlay) {
        // Current owner-thread authority always supersedes a persisted copy.
        if (found->descriptor.source_kind == World::FarLodBrickSourceKind::Authoritative &&
            incoming.descriptor.source_kind == World::FarLodBrickSourceKind::Authoritative) {
            const u32 next = found->descriptor.revision == std::numeric_limits<u32>::max()
                                 ? found->descriptor.revision
                                 : found->descriptor.revision + 1u;
            incoming.descriptor.revision = std::max(incoming.descriptor.revision, next);
        }
        *found = std::move(incoming);
        return true;
    }
    if (found->persisted && incoming.persisted) {
        error = "two persisted far records claim the same world SDF brick";
        return false;
    }
    // A regenerable cache can never replace authority.  Identical cache input
    // is harmless and is kept deterministically.
    if (found->descriptor.source_kind == World::FarLodBrickSourceKind::Authoritative) {
        return true;
    }
    if (incoming.descriptor.source_kind == World::FarLodBrickSourceKind::Authoritative) {
        *found = std::move(incoming);
    }
    return true;
}

bool finalize_assembly(Assembly& assembly,
                       std::vector<PendingWorldBrick>& pending,
                       std::string& error) {
    if (pending.size() > kMaxTransientAssemblyBricks) {
        error = "far-SDF transient assembly exceeds its brick budget";
        return false;
    }
    std::sort(
        pending.begin(), pending.end(), [](const PendingWorldBrick& a, const PendingWorldBrick& b) {
            return world_brick_less(a.descriptor, b.descriptor);
        });
    const int step = World::FarLodSampleStepMeters(assembly.tier);
    const u32 side = World::FarLodSdfBrickSamplesPerSide(assembly.tier);
    std::map<std::tuple<int, int, int>, std::pair<i16, u8>> canonical;
    std::map<std::tuple<int, int, int>, World::FarLodBrickSourceKind> source;
    // Establish authoritative values before inspecting regenerable payloads so
    // canonicalization is independent of chunk-coordinate sort order.
    for (const PendingWorldBrick& brick : pending) {
        if (!valid_world_brick(brick.descriptor) ||
            brick.density.size() != World::FarLodSdfBrickSampleCount(assembly.tier) ||
            brick.material.size() != brick.density.size() ||
            brick.descriptor.payload_crc32 != world_brick_crc(brick.density, brick.material)) {
            error = "assembled far-SDF brick has an invalid payload";
            return false;
        }
        if (brick.descriptor.source_kind != World::FarLodBrickSourceKind::Authoritative)
            continue;
        for (u32 z = 0; z < side; ++z)
            for (u32 y = 0; y < side; ++y)
                for (u32 x = 0; x < side; ++x) {
                    const std::size_t index = static_cast<std::size_t>(x) +
                                              static_cast<std::size_t>(y) * side +
                                              static_cast<std::size_t>(z) * side * side;
                    const auto key = std::make_tuple(
                        static_cast<int>(static_cast<std::int64_t>(brick.descriptor.chunk_x) *
                                             CHUNK_SIZE_X +
                                         static_cast<int>(x) * step),
                        static_cast<int>(static_cast<std::int64_t>(brick.descriptor.chunk_y) *
                                             CHUNK_SIZE_Y +
                                         static_cast<int>(y) * step),
                        static_cast<int>(static_cast<std::int64_t>(brick.descriptor.chunk_z) *
                                             CHUNK_SIZE_Z +
                                         static_cast<int>(z) * step));
                    const std::pair<i16, u8> value(brick.density[index], brick.material[index]);
                    const auto inserted = canonical.emplace(key, value);
                    if (!inserted.second && inserted.first->second != value) {
                        error = "authoritative far-SDF bricks disagree on a shared world sample";
                        return false;
                    }
                    source[key] = World::FarLodBrickSourceKind::Authoritative;
                }
    }
    for (PendingWorldBrick& brick : pending) {
        if (!valid_world_brick(brick.descriptor) ||
            brick.density.size() != World::FarLodSdfBrickSampleCount(assembly.tier) ||
            brick.material.size() != brick.density.size() ||
            brick.descriptor.payload_crc32 != world_brick_crc(brick.density, brick.material)) {
            error = "assembled far-SDF brick has an invalid payload";
            return false;
        }
        bool patched = false;
        for (u32 z = 0; z < side; ++z)
            for (u32 y = 0; y < side; ++y)
                for (u32 x = 0; x < side; ++x) {
                    const std::size_t index = static_cast<std::size_t>(x) +
                                              static_cast<std::size_t>(y) * side +
                                              static_cast<std::size_t>(z) * side * side;
                    if (brick.density[index] == World::kFarLodSdfInvalid) {
                        error = "assembled far-SDF brick contains an invalid density";
                        return false;
                    }
                    const auto key = std::make_tuple(
                        static_cast<int>(static_cast<std::int64_t>(brick.descriptor.chunk_x) *
                                             CHUNK_SIZE_X +
                                         static_cast<int>(x) * step),
                        static_cast<int>(static_cast<std::int64_t>(brick.descriptor.chunk_y) *
                                             CHUNK_SIZE_Y +
                                         static_cast<int>(y) * step),
                        static_cast<int>(static_cast<std::int64_t>(brick.descriptor.chunk_z) *
                                             CHUNK_SIZE_Z +
                                         static_cast<int>(z) * step));
                    const std::pair<i16, u8> value(brick.density[index], brick.material[index]);
                    const auto inserted = canonical.emplace(key, value);
                    if (inserted.second) {
                        source.emplace(key, brick.descriptor.source_kind);
                        continue;
                    }
                    const auto prior_source = source.find(key)->second;
                    if (prior_source == World::FarLodBrickSourceKind::Authoritative &&
                        brick.descriptor.source_kind ==
                            World::FarLodBrickSourceKind::Authoritative &&
                        inserted.first->second != value) {
                        error = "authoritative far-SDF bricks disagree on a shared world sample";
                        return false;
                    }
                    if (prior_source == World::FarLodBrickSourceKind::RegenerableCache &&
                        brick.descriptor.source_kind ==
                            World::FarLodBrickSourceKind::RegenerableCache &&
                        inserted.first->second != value) {
                        error = "regenerable far-SDF bricks disagree on a shared world sample";
                        return false;
                    }
                    if (prior_source == World::FarLodBrickSourceKind::Authoritative &&
                        brick.descriptor.source_kind ==
                            World::FarLodBrickSourceKind::RegenerableCache) {
                        brick.density[index] = inserted.first->second.first;
                        brick.material[index] = inserted.first->second.second;
                        patched = true;
                    } else if (prior_source == World::FarLodBrickSourceKind::RegenerableCache &&
                               brick.descriptor.source_kind ==
                                   World::FarLodBrickSourceKind::Authoritative) {
                        inserted.first->second = value;
                        source.find(key)->second = brick.descriptor.source_kind;
                    }
                }
        if (patched)
            brick.descriptor.payload_crc32 = world_brick_crc(brick.density, brick.material);
    }
    for (PendingWorldBrick& brick : pending) {
        assembly.bricks.push_back(brick.descriptor);
        assembly.density_q.insert(
            assembly.density_q.end(), brick.density.begin(), brick.density.end());
        assembly.material.insert(
            assembly.material.end(), brick.material.begin(), brick.material.end());
        if (brick.descriptor.source_kind == World::FarLodBrickSourceKind::Authoritative) {
            assembly.authority_columns.emplace_back(brick.descriptor.chunk_z,
                                                    brick.descriptor.chunk_x);
        }
    }
    std::sort(assembly.authority_columns.begin(), assembly.authority_columns.end());
    assembly.authority_columns.erase(
        std::unique(assembly.authority_columns.begin(), assembly.authority_columns.end()),
        assembly.authority_columns.end());
    for (const auto& [z, x] : assembly.authority_columns) {
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const std::int64_t owned_z = static_cast<std::int64_t>(z) + dz;
                const std::int64_t owned_x = static_cast<std::int64_t>(x) + dx;
                if (owned_z < std::numeric_limits<int>::min() ||
                    owned_z > std::numeric_limits<int>::max() ||
                    owned_x < std::numeric_limits<int>::min() ||
                    owned_x > std::numeric_limits<int>::max() || owned_z < Chunk::kPackedMinXz ||
                    owned_z > Chunk::kPackedMaxXz || owned_x < Chunk::kPackedMinXz ||
                    owned_x > Chunk::kPackedMaxXz ||
                    !valid_chunk_coordinate(static_cast<int>(owned_z), CHUNK_SIZE_Z) ||
                    !valid_chunk_coordinate(static_cast<int>(owned_x), CHUNK_SIZE_X)) {
                    error = "far-SDF owned-column expansion overflows world coordinates";
                    return false;
                }
                assembly.owned_columns.emplace_back(static_cast<int>(owned_z),
                                                    static_cast<int>(owned_x));
            }
    }
    std::sort(assembly.owned_columns.begin(), assembly.owned_columns.end());
    assembly.owned_columns.erase(
        std::unique(assembly.owned_columns.begin(), assembly.owned_columns.end()),
        assembly.owned_columns.end());
    return true;
}

bool derive_assembly_water(World::FarLodTile& tile,
                           const Assembly& assembly,
                           bool& changed,
                           std::string& error) {
    const int step = World::FarLodSampleStepMeters(assembly.tier);
    const u32 side = World::FarLodSdfBrickSamplesPerSide(assembly.tier);
    const std::size_t count = World::FarLodSdfBrickSampleCount(assembly.tier);
    std::map<std::pair<int, int>, std::map<int, i16>> vertical_columns; // (z,x) -> (y,density)
    const std::int64_t tile_origin_x =
        static_cast<std::int64_t>(tile.rx) * World::kFarLodRegionSizeMeters;
    const std::int64_t tile_origin_z =
        static_cast<std::int64_t>(tile.rz) * World::kFarLodRegionSizeMeters;
    for (std::size_t i = 0; i < assembly.bricks.size(); ++i) {
        const auto& brick = assembly.bricks[i];
        if (!valid_world_brick(brick)) {
            error = "far-SDF water derivation coordinate overflows";
            return false;
        }
        for (u32 z = 0; z < side; ++z)
            for (u32 y = 0; y < side; ++y)
                for (u32 x = 0; x < side; ++x) {
                    const std::size_t index = i * count + static_cast<std::size_t>(x) +
                                              static_cast<std::size_t>(y) * side +
                                              static_cast<std::size_t>(z) * side * side;
                    const int wx =
                        static_cast<int>(static_cast<std::int64_t>(brick.chunk_x) * CHUNK_SIZE_X +
                                         static_cast<int>(x) * step);
                    const int wy =
                        static_cast<int>(static_cast<std::int64_t>(brick.chunk_y) * CHUNK_SIZE_Y +
                                         static_cast<int>(y) * step);
                    const int wz =
                        static_cast<int>(static_cast<std::int64_t>(brick.chunk_z) * CHUNK_SIZE_Z +
                                         static_cast<int>(z) * step);
                    vertical_columns[{wz, wx}].emplace(wy, assembly.density_q[index]);
                }
    }
    // Every target-owned SDF column, including the requested side of a
    // foreign authoritative boundary, derives its surface-water flag from the
    // same canonical SDF view used by the mesher.  Restrict writes to the home
    // tile even though the transient assembly may contain foreign columns.
    for (const auto& [z_chunk, x_chunk] : assembly.owned_columns) {
        if (!is_home_chunk(x_chunk, z_chunk, tile.rx, tile.rz))
            continue;
        for (u32 z = 0; z < side; ++z)
            for (u32 x = 0; x < side; ++x) {
                const int wx = static_cast<int>(static_cast<std::int64_t>(x_chunk) * CHUNK_SIZE_X +
                                                static_cast<int>(x) * step);
                const int wz = static_cast<int>(static_cast<std::int64_t>(z_chunk) * CHUNK_SIZE_Z +
                                                static_cast<int>(z) * step);
                const auto column = vertical_columns.find({wz, wx});
                if (column == vertical_columns.end()) {
                    error = "far-SDF water derivation is missing a vertical column";
                    return false;
                }
                const std::map<int, i16>& vertical = column->second;
                bool crossing = false;
                float top = 0.0f;
                for (auto it = vertical.begin(); std::next(it) != vertical.end(); ++it) {
                    const auto next = std::next(it);
                    const float a = World::DequantizeFarLodSdf(it->second),
                                b = World::DequantizeFarLodSdf(next->second);
                    if (a <= 0.0f && b > 0.0f) {
                        top = static_cast<float>(it->first) +
                              (-a / (b - a)) * (next->first - it->first);
                        crossing = true;
                    }
                }
                if (!crossing) {
                    error = "far-SDF water derivation found no top solid crossing";
                    return false;
                }
                const std::int64_t local_x = static_cast<std::int64_t>(wx) - tile_origin_x;
                const std::int64_t local_z = static_cast<std::int64_t>(wz) - tile_origin_z;
                if (local_x < 0 || local_z < 0 || local_x % step != 0 || local_z % step != 0) {
                    error = "far-SDF water derivation produced an out-of-tile sample";
                    return false;
                }
                const std::size_t sx = static_cast<std::size_t>(local_x / step);
                const std::size_t sz = static_cast<std::size_t>(local_z / step);
                if (sx >= tile.samples_per_side || sz >= tile.samples_per_side) {
                    error = "far-SDF water derivation exceeded the requested tile";
                    return false;
                }
                u8& flags = tile.flags[sx + sz * tile.samples_per_side];
                const u8 next = top < SEA_LEVEL
                                    ? static_cast<u8>(flags | World::kFarLodSampleFlagWater)
                                    : static_cast<u8>(flags & ~World::kFarLodSampleFlagWater);
                changed = changed || flags != next;
                flags = next;
            }
    }
    return true;
}

} // namespace

FarLodWorkerBuildOutcome BuildFarLodWorkerTile(const Systems::SHIELD_WorldSystem& world,
                                               const Systems::FarLodSdfSnapshot& snapshot,
                                               World::FarLodTier tier,
                                               int rx,
                                               int rz,
                                               const std::filesystem::path& save_dir) {
    FarLodWorkerBuildOutcome outcome;
    if (!valid_region_coordinate(rx) || !valid_region_coordinate(rz)) {
        outcome.error = "far-LOD requested region exceeds the supported world range";
        return outcome;
    }
    World::FarLodTile tile;
    Assembly assembly;
    assembly.tier = tier;
    assembly.rx = rx;
    assembly.rz = rz;
    assembly.params_hash = snapshot.params_hash;
    std::vector<PendingWorldBrick> pending;
    bool loaded = false;
    if (!save_dir.empty()) {
        std::vector<std::string> errors;
        const World::FarLodStore store(save_dir);
        loaded = store.load_tile(tier, rx, rz, snapshot.params_hash, tile, &errors);
        if (!loaded && !errors.empty()) {
            outcome.error = errors.front();
            return outcome;
        }
    }
    if (!loaded) {
        const auto scope = world.acquire_worldgen_sample_scope();
        tile = World::BuildPristineFarLodTile(world, tier, rx, rz, snapshot.params_hash);
        outcome.changed = true;
    } else if (tile.params_hash != snapshot.params_hash) {
        const auto scope = world.acquire_worldgen_sample_scope();
        tile =
            rebase_authoritative_tile(world, std::move(tile), tier, rx, rz, snapshot.params_hash);
        outcome.changed = true;
    }

    const auto copy_persisted = [&](const World::FarLodTile& source) -> bool {
        const std::size_t count = World::FarLodSdfBrickSampleCount(tier);
        if (source.tier != tier || !valid_region_coordinate(source.rx) ||
            !valid_region_coordinate(source.rz) ||
            source.sdf_bricks.size() > std::numeric_limits<std::size_t>::max() / count ||
            source.sdf_density_q.size() != source.sdf_bricks.size() * count ||
            source.sdf_material.size() != source.sdf_density_q.size()) {
            outcome.error = "persisted far-SDF tile has invalid coordinates or streams";
            return false;
        }

        for (std::size_t i = 0; i < source.sdf_bricks.size(); ++i) {
            const auto& brick = source.sdf_bricks[i];
            const std::int64_t chunk_x64 =
                static_cast<std::int64_t>(source.rx) * kChunksPerFarLodRegion + brick.local_chunk_x;
            const std::int64_t chunk_z64 =
                static_cast<std::int64_t>(source.rz) * kChunksPerFarLodRegion + brick.local_chunk_z;
            if (chunk_x64 < std::numeric_limits<int>::min() ||
                chunk_x64 > std::numeric_limits<int>::max() ||
                chunk_z64 < std::numeric_limits<int>::min() ||
                chunk_z64 > std::numeric_limits<int>::max()) {
                outcome.error = "persisted far-SDF brick coordinate overflows";
                return false;
            }
            const int chunk_x = static_cast<int>(chunk_x64);
            const int chunk_z = static_cast<int>(chunk_z64);
            if (!is_home_chunk(chunk_x, chunk_z, source.rx, source.rz)) {
                outcome.error = "persisted far-SDF brick is outside its home region (chunk=" +
                                std::to_string(chunk_x) + "," + std::to_string(chunk_z) +
                                "; region=" + std::to_string(source.rx) + "," +
                                std::to_string(source.rz) + ")";
                return false;
            }
            if (brick.source_kind != World::FarLodBrickSourceKind::Authoritative ||
                !is_halo_chunk(chunk_x, chunk_z, rx, rz))
                continue;
            PendingWorldBrick incoming;
            incoming.descriptor = {chunk_x,
                                   chunk_z,
                                   brick.chunk_y,
                                   brick.source_kind,
                                   brick.revision,
                                   brick.payload_crc32};
            if (!valid_world_brick(incoming.descriptor)) {
                outcome.error = "persisted far-SDF brick exceeds the supported world range";
                return false;
            }
            const std::size_t begin = i * count;
            incoming.density.assign(
                source.sdf_density_q.begin() + static_cast<std::ptrdiff_t>(begin),
                source.sdf_density_q.begin() + static_cast<std::ptrdiff_t>(begin + count));
            incoming.material.assign(
                source.sdf_material.begin() + static_cast<std::ptrdiff_t>(begin),
                source.sdf_material.begin() + static_cast<std::ptrdiff_t>(begin + count));
            incoming.persisted = true;
            if (!add_or_overlay_world_brick(pending, std::move(incoming), false, outcome.error))
                return false;
        }
        return true;
    };

    const auto overlay_authority = [&](const World::FarLodSdfSnapshot& reduction,
                                       bool update_home,
                                       const char* source_label) -> bool {
        PendingWorldBrick incoming;
        if (!reduce_world_brick(tier,
                                reduction,
                                incoming.descriptor,
                                incoming.density,
                                incoming.material,
                                outcome.error)) {
            outcome.error =
                std::string("failed to reduce ") + source_label + " far SDF: " + outcome.error;
            return false;
        }
        if (!add_or_overlay_world_brick(pending, std::move(incoming), true, outcome.error)) {
            return false;
        }
        if (update_home) {
            std::string reduction_error;
            const auto reduced =
                World::ReduceChunkSdfIntoFarTile(tile, reduction, &reduction_error);
            if (reduced == World::FarLodSdfReduceResult::Error) {
                outcome.error = std::string("failed to update home ") + source_label +
                                " far SDF: " + reduction_error;
                return false;
            }
            outcome.changed = outcome.changed || reduced != World::FarLodSdfReduceResult::Unchanged;
        }
        return true;
    };

    if (!copy_persisted(tile))
        return outcome;
    if (!save_dir.empty()) {
        // Region records are read in canonical order. Derived FSD2 contributes
        // first; durable lod-0 chunks then overlay it as simulation truth, so a
        // crash window containing a new chunk plus old far cache converges even
        // when that chunk is not currently streamed.
        const World::FarLodStore store(save_dir);
        const int halo_min_chunk_x =
            static_cast<int>(static_cast<std::int64_t>(rx) * kChunksPerFarLodRegion - 1);
        const int halo_max_chunk_x = halo_min_chunk_x + kChunksPerFarLodRegion + 1;
        const int halo_min_chunk_z =
            static_cast<int>(static_cast<std::int64_t>(rz) * kChunksPerFarLodRegion - 1);
        const int halo_max_chunk_z = halo_min_chunk_z + kChunksPerFarLodRegion + 1;
        for (int neighbor_z = rz - 1; neighbor_z <= rz + 1; ++neighbor_z) {
            for (int neighbor_x = rx - 1; neighbor_x <= rx + 1; ++neighbor_x) {
                if (neighbor_x != rx || neighbor_z != rz) {
                    World::FarLodTile neighbor;
                    std::vector<std::string> errors;
                    if (store.load_tile(tier,
                                        neighbor_x,
                                        neighbor_z,
                                        snapshot.params_hash,
                                        neighbor,
                                        &errors)) {
                        if (!copy_persisted(neighbor))
                            return outcome;
                    } else if (!errors.empty()) {
                        outcome.error = errors.front();
                        return outcome;
                    }
                }

                const std::filesystem::path region_file =
                    Persistence::WorldSaveService::region_file_path(
                        save_dir, neighbor_x, neighbor_z);
                std::vector<std::shared_ptr<Chunk>> durable_chunks;
                std::vector<std::string> errors;
                if (!Persistence::WorldSaveService::read_authoritative_region_chunks(
                        region_file,
                        halo_min_chunk_x,
                        halo_max_chunk_x,
                        halo_min_chunk_z,
                        halo_max_chunk_z,
                        durable_chunks,
                        &errors)) {
                    outcome.error =
                        errors.empty() ? "failed to read durable chunk authority" : errors.front();
                    return outcome;
                }
                for (const std::shared_ptr<Chunk>& durable_chunk : durable_chunks) {
                    if (!durable_chunk)
                        continue;
                    const IVec3 coords = durable_chunk->get_coords();
                    if (region_for_chunk(coords.x) != neighbor_x ||
                        region_for_chunk(coords.z) != neighbor_z) {
                        outcome.error = "durable chunk record is outside its LMR1 home region";
                        return outcome;
                    }
                    if (durable_chunk->sdf_provenance() != ChunkSdfProvenance::LoadedOrEdited ||
                        !is_halo_chunk(coords.x, coords.z, rx, rz)) {
                        continue;
                    }
                    World::FarLodSdfSnapshot durable;
                    durable.coords = coords;
                    durable.revision = durable_chunk->voxel_revision();
                    durable.source_kind = World::FarLodBrickSourceKind::Authoritative;
                    durable.sdf_data = durable_chunk->sdf_data;
                    durable.material_data = durable_chunk->material_data;
                    const bool update_home =
                        region_for_chunk(coords.x) == rx && region_for_chunk(coords.z) == rz;
                    if (!overlay_authority(durable, update_home, "durable chunk")) {
                        return outcome;
                    }
                }
            }
        }
    }

    for (const Systems::FarLodSdfSnapshotEntry& entry : snapshot.entries) {
        if (entry.provenance != ChunkSdfProvenance::LoadedOrEdited ||
            !is_halo_chunk(entry.coords.x, entry.coords.z, rx, rz)) {
            continue;
        }
        const World::FarLodSdfSnapshot reduction = entry.as_reduction_snapshot();
        if (!overlay_authority(reduction, belongs_to_region(entry, rx, rz), "live authoritative")) {
            return outcome;
        }
    }

    // Complete every authority stack plus its horizontal halo in absolute
    // chunk coordinates.  Support is transient; only the home tile above is
    // ever modified or returned for persistence.
    std::map<std::pair<int, int>, std::pair<int, int>> authority_spans;
    for (const PendingWorldBrick& brick : pending)
        if (brick.descriptor.source_kind == World::FarLodBrickSourceKind::Authoritative) {
            const auto key = std::make_pair(brick.descriptor.chunk_z, brick.descriptor.chunk_x);
            const auto inserted = authority_spans.emplace(
                key, std::make_pair(brick.descriptor.chunk_y, brick.descriptor.chunk_y));
            if (!inserted.second) {
                inserted.first->second.first =
                    std::min(inserted.first->second.first, brick.descriptor.chunk_y);
                inserted.first->second.second =
                    std::max(inserted.first->second.second, brick.descriptor.chunk_y);
            }
        }
    if (!authority_spans.empty()) {
        std::set<std::tuple<int, int, int>> required;
        // Height sampling and scratch generation share one immutable worldgen
        // epoch.  Sampling outside this scope could size a stack for one
        // parameter set and populate it from another during hot reconfigure.
        const auto scope = world.acquire_worldgen_sample_scope();
        const int sample_step = World::FarLodSampleStepMeters(tier);
        for (const auto& [column, span] : authority_spans) {
            std::int64_t min_y = static_cast<std::int64_t>(span.first) - 1;
            std::int64_t max_y = static_cast<std::int64_t>(span.second) + 1;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const std::int64_t chunk_x64 = static_cast<std::int64_t>(column.second) + dx;
                    const std::int64_t chunk_z64 = static_cast<std::int64_t>(column.first) + dz;
                    if (chunk_x64 < std::numeric_limits<int>::min() ||
                        chunk_x64 > std::numeric_limits<int>::max() ||
                        chunk_z64 < std::numeric_limits<int>::min() ||
                        chunk_z64 > std::numeric_limits<int>::max() ||
                        chunk_x64 < Chunk::kPackedMinXz || chunk_x64 > Chunk::kPackedMaxXz ||
                        chunk_z64 < Chunk::kPackedMinXz || chunk_z64 > Chunk::kPackedMaxXz ||
                        !valid_chunk_coordinate(static_cast<int>(chunk_x64), CHUNK_SIZE_X) ||
                        !valid_chunk_coordinate(static_cast<int>(chunk_z64), CHUNK_SIZE_Z)) {
                        outcome.error = "far-SDF halo expansion exceeds the supported world range";
                        return outcome;
                    }
                    const int chunk_x = static_cast<int>(chunk_x64);
                    const int chunk_z = static_cast<int>(chunk_z64);
                    // Inspect the complete tier lattice, not merely the chunk
                    // origin.  A steep column may cross a vertical
                    // chunk boundary between its corners.
                    for (int local_z = 0; local_z <= CHUNK_SIZE_Z; local_z += sample_step) {
                        for (int local_x = 0; local_x <= CHUNK_SIZE_X; local_x += sample_step) {
                            const int world_x = static_cast<int>(
                                static_cast<std::int64_t>(chunk_x) * CHUNK_SIZE_X + local_x);
                            const int world_z = static_cast<int>(
                                static_cast<std::int64_t>(chunk_z) * CHUNK_SIZE_Z + local_z);
                            const float height = world.GetTerrainHeightAt(
                                static_cast<float>(world_x), static_cast<float>(world_z));
                            if (!std::isfinite(height)) {
                                outcome.error = "far-SDF halo surface height is non-finite";
                                return outcome;
                            }
                            const double surface_value = std::floor(
                                static_cast<double>(height) / static_cast<double>(CHUNK_SIZE_Y));
                            if (surface_value <
                                    static_cast<double>(std::numeric_limits<int>::min()) ||
                                surface_value >
                                    static_cast<double>(std::numeric_limits<int>::max())) {
                                outcome.error = "far-SDF halo surface chunk overflows";
                                return outcome;
                            }
                            const std::int64_t surface = static_cast<int>(surface_value);
                            min_y = std::min(min_y, surface - 1);
                            max_y = std::max(max_y, surface + 1);
                        }
                    }
                }
            if (min_y < std::numeric_limits<int>::min() ||
                max_y > std::numeric_limits<int>::max() || min_y > max_y ||
                static_cast<std::uint64_t>(max_y - min_y + 1) * 9u > kMaxTransientAssemblyBricks) {
                outcome.error = "far-SDF vertical completion exceeds its brick budget";
                return outcome;
            }
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int chunk_z =
                        static_cast<int>(static_cast<std::int64_t>(column.first) + dz);
                    const int chunk_x =
                        static_cast<int>(static_cast<std::int64_t>(column.second) + dx);
                    for (std::int64_t y = min_y; y <= max_y; ++y) {
                        if (y < Chunk::kPackedMinY || y > Chunk::kPackedMaxY ||
                            !valid_chunk_coordinate(static_cast<int>(y), CHUNK_SIZE_Y)) {
                            outcome.error =
                                "far-SDF vertical completion exceeds the supported world range";
                            return outcome;
                        }
                        required.emplace(chunk_z, chunk_x, static_cast<int>(y));
                        if (required.size() > kMaxTransientAssemblyBricks) {
                            outcome.error = "far-SDF transient assembly exceeds its brick budget";
                            return outcome;
                        }
                    }
                }
            }
        }
        for (const auto& [chunk_z, chunk_x, chunk_y] : required) {
            const auto exists =
                std::find_if(pending.begin(), pending.end(), [&](const PendingWorldBrick& brick) {
                    return brick.descriptor.chunk_x == chunk_x &&
                           brick.descriptor.chunk_z == chunk_z &&
                           brick.descriptor.chunk_y == chunk_y;
                });
            if (exists != pending.end())
                continue;
            Chunk generated_chunk(IVec3(chunk_x, chunk_y, chunk_z));
            world.GenerateChunkData(generated_chunk, 1);
            World::FarLodSdfSnapshot generated;
            generated.coords = generated_chunk.get_coords();
            generated.source_kind = World::FarLodBrickSourceKind::RegenerableCache;
            generated.sdf_data = std::move(generated_chunk.sdf_data);
            generated.material_data = std::move(generated_chunk.material_data);
            PendingWorldBrick incoming;
            if (!reduce_world_brick(tier,
                                    generated,
                                    incoming.descriptor,
                                    incoming.density,
                                    incoming.material,
                                    outcome.error)) {
                outcome.error = "failed to generate far-SDF halo brick: " + outcome.error;
                return outcome;
            }
            if (!add_or_overlay_world_brick(pending, std::move(incoming), false, outcome.error))
                return outcome;
        }
    }
    if (!finalize_assembly(assembly, pending, outcome.error))
        return outcome;
    if (!derive_assembly_water(tile, assembly, outcome.changed, outcome.error))
        return outcome;
    World::MarchingCubes::GenerateFarLodRegionMesh(tile, assembly, outcome.mesh);
    if (outcome.mesh.vertices.empty() || outcome.mesh.indices.empty()) {
        outcome.error = "far-SDF worker produced an empty mesh";
        return outcome;
    }
    outcome.ok = true;
    outcome.tile = std::move(tile);
    return outcome;
}

FarLodSystem::FarLodSystem() = default;

FarLodSystem::~FarLodSystem() {
    // GL resources must have been released through shutdown while the
    // context was current; here we only make sure no build job can outlive
    // the world pointer it samples.
    wait_for_builds();
}

u64 FarLodSystem::region_key(int rx, int rz) {
    return (static_cast<u64>(static_cast<u32>(rx)) << 32) | static_cast<u64>(static_cast<u32>(rz));
}

void FarLodSystem::wait_for_builds() {
    if (m_job_system) {
        for (const JobHandle& handle : m_inflight_handles) {
            m_job_system->wait(handle);
        }
    }
    m_inflight_handles.clear();
    m_pending.clear();
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    m_shared->completed.clear();
}

void FarLodSystem::prepare_world_swap() {
    wait_for_builds();
    m_world = nullptr;
    ++m_epoch;
}

void FarLodSystem::release_region(ResidentRegion& region) {
    if (region.vao) {
        glDeleteVertexArrays(1, &region.vao);
        region.vao = 0;
    }
    if (region.vbo) {
        glDeleteBuffers(1, &region.vbo);
        region.vbo = 0;
    }
    if (region.ebo) {
        glDeleteBuffers(1, &region.ebo);
        region.ebo = 0;
    }
    if (region.water_vao) {
        glDeleteVertexArrays(1, &region.water_vao);
        region.water_vao = 0;
    }
    if (region.water_vbo) {
        glDeleteBuffers(1, &region.water_vbo);
        region.water_vbo = 0;
    }
    if (region.water_ebo) {
        glDeleteBuffers(1, &region.water_ebo);
        region.water_ebo = 0;
    }
    region.element_count = 0;
    region.water_element_count = 0;
    region.resident_bytes = 0;
}

void FarLodSystem::shutdown() {
    prepare_world_swap();
    for (auto& [key, region] : m_residents) {
        (void)key;
        release_region(region);
    }
    m_residents.clear();
    m_stats = {};
}

void FarLodSystem::bind_world(const Systems::SHIELD_WorldSystem& world_system) {
    const u64 params_hash =
        World::ComputeTerrainParamsHash(world_system.get_params(), world_system.get_seed());
    if (m_world == &world_system && m_params_hash == params_hash) {
        return;
    }

    // New or re-parameterized world: drain builds that sample the previous
    // binding and drop every resident mesh (its terrain is stale).
    wait_for_builds();
    ++m_epoch;
    for (auto& [key, region] : m_residents) {
        (void)key;
        release_region(region);
    }
    m_residents.clear();
    m_world = &world_system;
    m_params_hash = params_hash;
}

std::size_t FarLodSystem::total_resident_bytes() const {
    std::size_t bytes = 0;
    for (const auto& [key, region] : m_residents) {
        (void)key;
        bytes += region.resident_bytes;
    }
    return bytes;
}

void FarLodSystem::integrate_completed_builds() {
    std::vector<BuildResult> completed;
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        if (m_shared->completed.empty()) {
            return;
        }
        const std::size_t take = std::min(kMaxUploadsPerFrame, m_shared->completed.size());
        completed.assign(std::make_move_iterator(m_shared->completed.begin()),
                         std::make_move_iterator(m_shared->completed.begin() +
                                                 static_cast<std::ptrdiff_t>(take)));
        m_shared->completed.erase(m_shared->completed.begin(),
                                  m_shared->completed.begin() + static_cast<std::ptrdiff_t>(take));
    }

    for (BuildResult& result : completed) {
        const u64 key = region_key(result.rx, result.rz);
        m_pending.erase(key);
        const bool snapshot_stale =
            !m_world || !result.sdf_snapshot ||
            result.capture_epoch != result.sdf_snapshot->capture_epoch ||
            result.params_hash != result.sdf_snapshot->params_hash ||
            result.params_hash != m_params_hash ||
            result.authority_revision != result.sdf_snapshot->authority_revision ||
            result.region_authority_revision != result.sdf_snapshot->region_authority_revision ||
            result.region_authority_revision !=
                m_world->far_lod_region_authority_revision(result.rx, result.rz) ||
            !m_world->is_far_lod_sdf_snapshot_current(*result.sdf_snapshot);
        if (snapshot_stale) {
            ++m_stats.stale_results_rejected;
        }
        if (result.authority_build_failed) {
            ++m_stats.authority_build_failures;
        }
        if (result.epoch != m_epoch || snapshot_stale || result.mesh.vertices.empty() ||
            result.mesh.indices.empty()) {
            //  a build that returned an empty mesh (or raced an epoch swap)
            // never becomes resident — counted so the gate can tell empty-mesh failures
            // apart from build-throttle starvation on the mountains preset.
            ++m_stats.builds_integrated_failed;
            ++m_stats.builds_failed_total;
            continue;
        }
        bool persistence_pending = false;
        if (result.tile_changed && !result.save_dir.empty() && result.persistence_allowed) {
            std::vector<std::string> errors;
            if (!World::FarLodStore(result.save_dir).save_tile(result.tile, &errors)) {
                ++m_stats.authority_build_failures;
                ++m_stats.builds_integrated_failed;
                ++m_stats.builds_failed_total;
                continue;
            }
        } else if (result.tile_changed && !result.save_dir.empty()) {
            // Dirty authoritative chunks are persisted by WorldSaveService
            // first. Keep the resident visual result, but schedule another CPU
            // build after the dirty bit clears so derived FSD2 can follow.
            persistence_pending = true;
        }

        ResidentRegion region;
        region.tier = result.tier;
        region.rx = result.rx;
        region.rz = result.rz;

        glGenVertexArrays(1, &region.vao);
        glGenBuffers(1, &region.vbo);
        glGenBuffers(1, &region.ebo);
        const std::string label_prefix =
            "farlod.region." + std::to_string(result.rx) + "." + std::to_string(result.rz);
        PassGl::label_gl_object(GL_VERTEX_ARRAY, region.vao, label_prefix + ".vao");
        PassGl::label_gl_object(GL_BUFFER, region.vbo, label_prefix + ".vbo");
        PassGl::label_gl_object(GL_BUFFER, region.ebo, label_prefix + ".ebo");

        glBindVertexArray(region.vao);
        glBindBuffer(GL_ARRAY_BUFFER, region.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(result.mesh.vertices.size() * sizeof(VoxelVertex)),
                     result.mesh.vertices.data(),
                     GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, region.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(result.mesh.indices.size() * sizeof(u32)),
                     result.mesh.indices.data(),
                     GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(
            0, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, position));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            1, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, normal));
        glEnableVertexAttribArray(2);
        glVertexAttribIPointer(
            2, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, material_id));
        glBindVertexArray(0);

        region.element_count = static_cast<u32>(result.mesh.indices.size());

        // upload the flat water sheet (separate VAO so
        // it draws with the far-water material in its own depth-biased sub-pass).
        if (!result.water_mesh.vertices.empty() && !result.water_mesh.indices.empty()) {
            glGenVertexArrays(1, &region.water_vao);
            glGenBuffers(1, &region.water_vbo);
            glGenBuffers(1, &region.water_ebo);
            PassGl::label_gl_object(GL_VERTEX_ARRAY, region.water_vao, label_prefix + ".water.vao");
            PassGl::label_gl_object(GL_BUFFER, region.water_vbo, label_prefix + ".water.vbo");
            PassGl::label_gl_object(GL_BUFFER, region.water_ebo, label_prefix + ".water.ebo");
            glBindVertexArray(region.water_vao);
            glBindBuffer(GL_ARRAY_BUFFER, region.water_vbo);
            glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(result.water_mesh.vertices.size() * sizeof(VoxelVertex)),
                result.water_mesh.vertices.data(),
                GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, region.water_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(result.water_mesh.indices.size() * sizeof(u32)),
                         result.water_mesh.indices.data(),
                         GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0,
                                  3,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  sizeof(VoxelVertex),
                                  (void*)offsetof(VoxelVertex, position));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1,
                                  3,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  sizeof(VoxelVertex),
                                  (void*)offsetof(VoxelVertex, normal));
            glEnableVertexAttribArray(2);
            glVertexAttribIPointer(2,
                                   1,
                                   GL_UNSIGNED_INT,
                                   sizeof(VoxelVertex),
                                   (void*)offsetof(VoxelVertex, material_id));
            glBindVertexArray(0);
            region.water_element_count = static_cast<u32>(result.water_mesh.indices.size());
        }

        const float origin_x = static_cast<float>(result.rx) * kRegionSize;
        const float origin_z = static_cast<float>(result.rz) * kRegionSize;
        region.aabb_min = glm::vec3(origin_x, result.min_height - kFarDepthBiasMeters, origin_z);
        region.aabb_max = glm::vec3(origin_x + kRegionSize,
                                    // The water sheet sits at the global waterline; include it so a
                                    // fully-submerged region (max terrain height below SEA_LEVEL)
                                    // is not frustum-culled and drops its water sheet.
                                    std::max(result.max_height, Luminumbra::SEA_LEVEL) + 1.0f,
                                    origin_z + kRegionSize);
        region.resident_bytes = result.tile_bytes +
                                result.mesh.vertices.size() * sizeof(VoxelVertex) +
                                result.mesh.indices.size() * sizeof(u32) +
                                result.water_mesh.vertices.size() * sizeof(VoxelVertex) +
                                result.water_mesh.indices.size() * sizeof(u32);
        region.last_wanted_frame = m_frame;
        region.region_authority_revision = result.region_authority_revision;
        region.persistence_pending = persistence_pending;

        auto existing = m_residents.find(key);
        if (existing != m_residents.end()) {
            release_region(existing->second);
            existing->second = region;
        } else {
            m_residents.emplace(key, region);
        }
        ++m_stats.builds_completed_total;
        ++m_stats.builds_integrated_ok; //  completed build became resident this frame
    }
}

void FarLodSystem::update(const Systems::SHIELD_WorldSystem& world_system,
                          const glm::vec3& camera_position) {
    ++m_frame;
    m_last_camera_position = camera_position;
    m_stats.enabled = m_enabled;
    m_stats.region_draws = 0;
    m_stats.indices_drawn = 0;
    //  reset the per-frame diagnostic counters before this tick's
    // integrate/dispatch/evict so they reflect THIS frame only.
    m_stats.builds_dispatched = 0;
    m_stats.builds_integrated_ok = 0;
    m_stats.builds_integrated_failed = 0;
    m_stats.evictions_this_frame = 0;

    if (!m_enabled || !m_job_system) {
        m_stats.pending_depth = m_pending.size();
        m_stats.regions_wanted = 0;
        m_stats.regions_missing = 0;
        m_stats.regions_building = m_pending.size();
        m_stats.regions_resident = m_residents.size();
        m_stats.resident_bytes = total_resident_bytes();
        return;
    }

    bind_world(world_system);

    // Worldgen-preview far-field: stream the wanted set around the FIXED diorama
    // centre (a stable tile set, no orbit churn) instead of the orbiting camera.
    // Eviction below keys off the same anchor so resident centre tiles are not
    // evicted as the camera orbits away. Normal (first-person) mode streams
    // around the live camera as before.
    const glm::vec3 stream_pos = m_preview_mode ? m_preview_anchor : camera_position;

    // --- Ring-diff wanted set out to the F2 outer range ---
    struct Wanted {
        int rx;
        int rz;
        World::FarLodTier tier;
        float nearest;
    };
    std::vector<Wanted> wanted;
    const int camera_rx = static_cast<int>(std::floor(stream_pos.x / kRegionSize));
    const int camera_rz = static_cast<int>(std::floor(stream_pos.z / kRegionSize));
    const int scan_radius = static_cast<int>(std::ceil(kF2OuterRangeMeters / kRegionSize)) + 1;
    for (int rz = camera_rz - scan_radius; rz <= camera_rz + scan_radius; ++rz) {
        for (int rx = camera_rx - scan_radius; rx <= camera_rx + scan_radius; ++rx) {
            const float nearest = region_nearest_distance(rx, rz, stream_pos);
            if (nearest > kF2OuterRangeMeters) {
                continue;
            }
            // Live wins: a region the live chunk ring covers entirely is
            // never drawn far. (In preview mode the live slice is sub-region, so
            // every region around the centre is wanted — its far mesh draws under
            // the slice and the centre-relative inner discard hides it there.)
            if (region_farthest_distance(rx, rz, stream_pos) <= kLiveRingRadiusMeters) {
                continue;
            }
            const World::FarLodTier tier =
                nearest < kF1OuterRangeMeters ? World::FarLodTier::F1 : World::FarLodTier::F2;
            wanted.push_back({rx, rz, tier, nearest});
        }
    }
    // Nearest-first build order.
    std::sort(wanted.begin(), wanted.end(), [](const Wanted& lhs, const Wanted& rhs) {
        return lhs.nearest < rhs.nearest;
    });

    integrate_completed_builds();

    // Schedule missing/tier-changed regions on the Normal job lane.
    std::size_t missing = 0;
    std::size_t dispatched = 0;
    for (const Wanted& want : wanted) {
        const u64 key = region_key(want.rx, want.rz);
        const auto resident = m_residents.find(key);
        const bool resident_matches =
            resident != m_residents.end() && resident->second.tier == want.tier &&
            resident->second.region_authority_revision ==
                world_system.far_lod_region_authority_revision(want.rx, want.rz) &&
            !resident->second.persistence_pending;
        if (resident != m_residents.end()) {
            resident->second.last_wanted_frame = m_frame;
        }
        if (resident_matches) {
            continue;
        }
        if (resident == m_residents.end()) {
            ++missing;
        }
        if (m_pending.count(key) != 0 && m_pending[key] == want.tier) {
            continue;
        }
        if (dispatched >= kMaxBuildDispatchesPerFrame) {
            continue;
        }

        // warm the hydraulic-erosion regions this far tile will sample
        // BEFORE dispatching its build, so BuildPristineFarLodTile ->
        // GetTerrainHeightAtCoarse -> SampleHydroOffsetMeters finds the regions
        // already baked instead of stalling the build worker on a cold per-region
        // bake. The radius covers the tile's own 512 m region plus a margin so its
        // border samples (which read into the neighbour region) are also warm.
        // No-op when hydro is disabled. Deterministic (recompute-on-load), so this
        // only affects timing, never the tile bytes -> run==replay holds.
        {
            const float region_cx = (static_cast<float>(want.rx) + 0.5f) * kRegionSize;
            const float region_cz = (static_cast<float>(want.rz) + 0.5f) * kRegionSize;
            m_world->PrefetchHydroRegions(region_cx, region_cz, kRegionSize);
        }

        // Capture on the owner thread after streaming publication.  The worker
        // receives only this immutable value; it must never retain a streamed
        // Chunk or borrow one of its mutable voxel vectors.
        const std::shared_ptr<const Systems::FarLodSdfSnapshot> sdf_snapshot =
            world_system.capture_far_lod_sdf_snapshot(want.rx, want.rz);
        if (!sdf_snapshot) {
            continue;
        }
        const bool persistence_allowed =
            std::none_of(sdf_snapshot->entries.begin(),
                         sdf_snapshot->entries.end(),
                         [](const Systems::FarLodSdfSnapshotEntry& entry) {
                             // The transient assembly may derive requested-tile flags from
                             // authoritative halo samples.  Permit the visual rebuild, but
                             // persist none of those derived bytes until every contributing
                             // authority record in the captured halo is durable.
                             return entry.provenance == ChunkSdfProvenance::LoadedOrEdited &&
                                    !entry.authority_durable;
                         });
        if (resident != m_residents.end() && resident->second.persistence_pending &&
            !persistence_allowed) {
            continue;
        }
        m_pending[key] = want.tier;
        ++dispatched;

        auto shared = m_shared;
        const Systems::SHIELD_WorldSystem* world = m_world;
        const u64 epoch = m_epoch;
        const World::FarLodTier tier = want.tier;
        const int rx = want.rx;
        const int rz = want.rz;
        const std::filesystem::path save_dir = m_save_dir;
        const JobHandle handle = m_job_system->dispatch_batch(
            {[shared, world, sdf_snapshot, epoch, tier, rx, rz, save_dir, persistence_allowed]() {
                FarLodWorkerBuildOutcome outcome =
                    BuildFarLodWorkerTile(*world, *sdf_snapshot, tier, rx, rz, save_dir);

                BuildResult result;
                result.epoch = epoch;
                result.sdf_snapshot = sdf_snapshot;
                result.capture_epoch = sdf_snapshot->capture_epoch;
                result.params_hash = sdf_snapshot->params_hash;
                result.authority_revision = sdf_snapshot->authority_revision;
                result.region_authority_revision = sdf_snapshot->region_authority_revision;
                result.persistence_allowed = persistence_allowed;
                result.tier = tier;
                result.rx = rx;
                result.rz = rz;
                result.authority_build_failed = !outcome.ok;
                if (!outcome.ok) {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    shared->completed.push_back(std::move(result));
                    return;
                }
                World::FarLodTile& tile = outcome.tile;
                result.tile_changed = outcome.changed;
                result.save_dir = save_dir;
                result.tile_bytes = far_lod_tile_bytes(tile);
                u16 min_q = std::numeric_limits<u16>::max();
                u16 max_q = 0;
                for (const u16 q : tile.height_q) {
                    min_q = std::min(min_q, q);
                    max_q = std::max(max_q, q);
                }
                result.min_height = World::DequantizeFarLodHeight(min_q) -
                                    static_cast<float>(World::FarLodSampleStepMeters(tier));
                result.max_height = World::DequantizeFarLodHeight(max_q);
                result.mesh = std::move(outcome.mesh);
                // build the flat water sheet from the same
                // tile's water flags (river channels + seabeds beyond the live ring).
                BuildFarLodWaterSheet(tile, result.water_mesh);
                result.tile = std::move(tile);

                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->completed.push_back(std::move(result));
            }},
            JobPriority::Normal);
        if (handle.counter) {
            m_inflight_handles.push_back(handle);
        }
    }

    // Drop completed handles so the wait list stays bounded.
    m_inflight_handles.erase(std::remove_if(m_inflight_handles.begin(),
                                            m_inflight_handles.end(),
                                            [](const JobHandle& handle) {
                                                return !handle.counter ||
                                                       handle.counter->load(
                                                           std::memory_order_acquire) <= 0;
                                            }),
                             m_inflight_handles.end());

    // --- Eviction: leave-wanted-set frees immediately; byte budget evicts
    // least-recently-wanted first. ---
    for (auto it = m_residents.begin(); it != m_residents.end();) {
        if (it->second.last_wanted_frame != m_frame) {
            release_region(it->second);
            it = m_residents.erase(it);
            ++m_stats.evictions_total;
            ++m_stats.evictions_this_frame;
        } else {
            ++it;
        }
    }
    std::size_t resident_bytes = total_resident_bytes();
    while (resident_bytes > kResidentBudgetBytes && !m_residents.empty()) {
        auto victim = m_residents.begin();
        float victim_distance = 0.0f;
        for (auto it = m_residents.begin(); it != m_residents.end(); ++it) {
            const float distance =
                region_nearest_distance(it->second.rx, it->second.rz, stream_pos);
            if (distance > victim_distance) {
                victim_distance = distance;
                victim = it;
            }
        }
        resident_bytes -= victim->second.resident_bytes;
        release_region(victim->second);
        m_residents.erase(victim);
        ++m_stats.evictions_total;
        ++m_stats.evictions_this_frame;
    }

    m_stats.regions_wanted = wanted.size();
    m_stats.regions_missing = missing;
    m_stats.regions_building = m_pending.size();
    m_stats.regions_resident = m_residents.size();
    m_stats.resident_bytes = resident_bytes;
    //  build jobs dispatched this frame + jobs still in flight.
    m_stats.builds_dispatched = dispatched;
    m_stats.pending_depth = m_pending.size();
}

void FarLodSystem::draw_gbuffer(Shader& geometry_shader,
                                const glm::mat4& view,
                                const glm::vec4 frustum_planes[6],
                                std::size_t& draws_out,
                                std::size_t& indices_out) {
    draws_out = 0;
    indices_out = 0;
    if (!m_enabled || m_residents.empty()) {
        return;
    }

    // Push far geometry behind coincident live geometry (live wins), and
    // discard far fragments inside the guaranteed-live ring so the
    // under-terrain fill cannot peek through live seam cracks at close range.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);
    geometry_shader.setFloat("u_farClipInnerRadius", kFarClipInnerRadiusMeters);
    // clip far geometry at the GEOMETRY level
    // (gl_ClipDistance[0]) to a radial band. The near radius removes the camera-
    // straddling triangles; the far radius removes the far-plane/frustum-edge
    // triangles - both rasterized as the horizon sky-sliver. The clipped band is
    // invisible (inside the live ring / past the 1000 m far plane), so nothing is
    // lost. Camera-region skip also drops the one region the camera sits in,
    // whose near triangles straddle the camera even after the radial clip.
    glEnable(GL_CLIP_DISTANCE0);
    geometry_shader.setFloat("u_farClipNearRadius", kFarClipInnerRadiusMeters);
    geometry_shader.setFloat("u_farClipFarRadius", kFarClipOuterRadiusMeters);
    // Worldgen-preview far-field: for the external orbit camera the camera-
    // relative inner discard (u_farClipInnerRadius) + near radial clip would carve
    // a moving void disc around the camera, and the camera-region skip would drop
    // the centre region (where the slice sits). So in preview mode disable those
    // and instead discard far fragments inside the FIXED live slice via the
    // centre-relative world-space radius (u_farPreviewInnerRadius). Live geometry
    // still wins in the slice via the depth bias + polygon offset above.
    if (m_preview_mode) {
        geometry_shader.setFloat("u_farClipInnerRadius", 0.0f);
        geometry_shader.setFloat("u_farClipNearRadius", 0.0f);
        geometry_shader.setVec2("u_farPreviewCenterXZ",
                                glm::vec2(m_preview_anchor.x, m_preview_anchor.z));
        geometry_shader.setFloat("u_farPreviewInnerRadius", m_preview_inner_radius);
    } else {
        geometry_shader.setFloat("u_farPreviewInnerRadius", 0.0f);
    }
    const int camera_rx = static_cast<int>(std::floor(m_last_camera_position.x / kRegionSize));
    const int camera_rz = static_cast<int>(std::floor(m_last_camera_position.z / kRegionSize));
    const auto is_camera_region = [&](const ResidentRegion& region) {
        // Preview mode (external orbit camera over a fixed sub-region slice): skip
        // no region — the centre region carries the diorama's far field.
        return !m_preview_mode && region.rx == camera_rx && region.rz == camera_rz;
    };

    std::size_t water_draws = 0;
    std::size_t water_indices = 0;
    for (const auto& [key, region] : m_residents) {
        (void)key;
        if (region.element_count == 0 || is_camera_region(region) ||
            aabb_outside_frustum(region.aabb_min, region.aabb_max, frustum_planes)) {
            continue;
        }
        const glm::vec3 origin(static_cast<float>(region.rx) * kRegionSize,
                               -kFarDepthBiasMeters,
                               static_cast<float>(region.rz) * kRegionSize);
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), origin);
        const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(view * model)));
        geometry_shader.setMat4("model", model);
        geometry_shader.setMat3("normalMatrix", normal_matrix);
        glBindVertexArray(region.vao);
        glDrawElements(
            GL_TRIANGLES, static_cast<GLsizei>(region.element_count), GL_UNSIGNED_INT, nullptr);
        ++draws_out;
        indices_out += region.element_count;
    }

    // the flat water sheets draw after the far terrain,
    // at the global waterline, with the same inner-radius discard so the live
    // water ring owns the close range. A slightly smaller depth bias than the
    // terrain keeps the sheet from z-fighting the seabed beneath it while still
    // sitting under the live surface. Material id kFarWaterMaterialId tints them
    // deep water in the G-buffer (no live water.frag reflections far out).
    for (const auto& [key, region] : m_residents) {
        (void)key;
        //  note: drawing the camera region's sheet
        // here (to cover the live-disc sea where the live water sim does not
        // reach) was tried and reverted - the pale sheet behind the live
        // transparent water shifts water.frag's blend result enough to break
        // the boundary-band blue-dominance classifier. Who renders the
        // live-disc sea is the live-water-coverage task's design question.
        if (region.water_element_count == 0 || is_camera_region(region) ||
            aabb_outside_frustum(region.aabb_min, region.aabb_max, frustum_planes)) {
            continue;
        }
        const glm::vec3 origin(static_cast<float>(region.rx) * kRegionSize,
                               -kFarWaterDepthBiasMeters,
                               static_cast<float>(region.rz) * kRegionSize);
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), origin);
        const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(view * model)));
        geometry_shader.setMat4("model", model);
        geometry_shader.setMat3("normalMatrix", normal_matrix);
        glBindVertexArray(region.water_vao);
        glDrawElements(GL_TRIANGLES,
                       static_cast<GLsizei>(region.water_element_count),
                       GL_UNSIGNED_INT,
                       nullptr);
        ++draws_out;
        indices_out += region.water_element_count;
        ++water_draws;
        water_indices += region.water_element_count;
    }

    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.0f, 0.0f);
    glDisable(GL_CLIP_DISTANCE0);
    // The geometry program is shared with the live chunk draws: the clip
    // uniforms MUST reset to their inert defaults.
    geometry_shader.setFloat("u_farClipInnerRadius", 0.0f);
    geometry_shader.setFloat("u_farClipNearRadius", 0.0f);
    geometry_shader.setFloat("u_farClipFarRadius", 0.0f);
    geometry_shader.setFloat("u_farPreviewInnerRadius", 0.0f);

    m_stats.region_draws = draws_out;
    m_stats.indices_drawn = indices_out;
    m_stats.water_sheet_draws = water_draws;
    m_stats.water_sheet_indices = water_indices;
}

} // namespace Luminumbra::Rendering
