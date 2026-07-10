#include "FarLodSystem.h"

#include "Shader.h"
#include "passes/PassGlHelpers.h"
#include "core/Log.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/MarchingCubes.h"

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

bool belongs_to_region(const Luminumbra::Systems::FarLodSdfSnapshotEntry& entry,
                       int rx,
                       int rz) {
    return floor_divide(entry.coords.x, kChunksPerFarLodRegion) == rx &&
           floor_divide(entry.coords.z, kChunksPerFarLodRegion) == rz;
}

std::size_t far_lod_tile_bytes(const Luminumbra::World::FarLodTile& tile) {
    return tile.height_q.size() * sizeof(u16) +
           tile.material.size() * sizeof(u8) +
           tile.flags.size() * sizeof(u8) +
           tile.sdf_bricks.size() * sizeof(Luminumbra::World::FarLodSdfBrickDescriptor) +
           tile.sdf_density_q.size() * sizeof(i16) +
           tile.sdf_material.size() * sizeof(u8);
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
    const float dx = std::max(std::abs(position.x - min_x), std::abs(position.x - (min_x + kRegionSize)));
    const float dz = std::max(std::abs(position.z - min_z), std::abs(position.z - (min_z + kRegionSize)));
    return std::sqrt(dx * dx + dz * dz);
}

bool aabb_outside_frustum(const glm::vec3& aabb_min, const glm::vec3& aabb_max, const glm::vec4 frustum_planes[6]) {
    for (int i = 0; i < 6; ++i) {
        const glm::vec4& plane = frustum_planes[i];
        const glm::vec3 positive(
            plane.x >= 0.0f ? aabb_max.x : aabb_min.x,
            plane.y >= 0.0f ? aabb_max.y : aabb_min.y,
            plane.z >= 0.0f ? aabb_max.z : aabb_min.z);
        if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f) {
            return true;
        }
    }
    return false;
}

// T-I4-DR-far-water-sheet: build a flat water sheet for a far tile. Emits one
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
            if (!water_at(x, z) && !water_at(x + 1, z) &&
                !water_at(x, z + 1) && !water_at(x + 1, z + 1)) {
                continue;
            }
            const float x0 = static_cast<float>(x * static_cast<u32>(step));
            const float x1 = static_cast<float>((x + 1) * static_cast<u32>(step));
            const float z0 = static_cast<float>(z * static_cast<u32>(step));
            const float z1 = static_cast<float>((z + 1) * static_cast<u32>(step));
            const u32 base = static_cast<u32>(out.vertices.size());
            const Vec3 up(0.0f, 1.0f, 0.0f);
            out.vertices.push_back({Vec3(x0, waterline, z0), up, FarLodSystem::kFarWaterMaterialId});
            out.vertices.push_back({Vec3(x1, waterline, z0), up, FarLodSystem::kFarWaterMaterialId});
            out.vertices.push_back({Vec3(x1, waterline, z1), up, FarLodSystem::kFarWaterMaterialId});
            out.vertices.push_back({Vec3(x0, waterline, z1), up, FarLodSystem::kFarWaterMaterialId});
            // T-I4-DR-far-water-exposure: wind the quads COUNTER-clockwise as
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
    return std::any_of(tile.sdf_bricks.begin(), tile.sdf_bricks.end(),
        [](const Luminumbra::World::FarLodSdfBrickDescriptor& brick) {
            return brick.source_kind == Luminumbra::World::FarLodBrickSourceKind::Authoritative;
        });
}

Luminumbra::World::FarLodTile rebase_authoritative_tile(
    const Luminumbra::Systems::SHIELD_WorldSystem& world,
    Luminumbra::World::FarLodTile&& stale,
    Luminumbra::World::FarLodTier tier,
    int rx,
    int rz,
    u64 params_hash) {
    auto fresh = Luminumbra::World::BuildPristineFarLodTile(world, tier, rx, rz, params_hash);
    if (stale.legacy_surface_authority) {
        for (std::size_t i = 0; i < stale.sample_count(); ++i) {
            if ((stale.flags[i] & Luminumbra::World::kFarLodSampleFlagEdited) == 0u) {
                continue;
            }
            fresh.height_q[i] = stale.height_q[i];
            fresh.material[i] = stale.material[i];
            fresh.flags[i] = stale.flags[i];
        }
        fresh.legacy_surface_authority = true;
        fresh.edited = true;
    }

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
            stale.sdf_density_q.begin() + static_cast<std::ptrdiff_t>(begin + samples));
        fresh.sdf_material.insert(fresh.sdf_material.end(),
            stale.sdf_material.begin() + static_cast<std::ptrdiff_t>(begin),
            stale.sdf_material.begin() + static_cast<std::ptrdiff_t>(begin + samples));
        fresh.edited = true;
    }
    return fresh;
}

bool complete_authoritative_stacks(
    const Luminumbra::Systems::SHIELD_WorldSystem& world,
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
        auto found = std::find_if(columns.begin(), columns.end(), [&](const AuthorityColumn& column) {
            return column.x == brick.local_chunk_x && column.z == brick.local_chunk_z;
        });
        if (found == columns.end()) {
            columns.push_back({brick.local_chunk_x, brick.local_chunk_z, brick.chunk_y, brick.chunk_y});
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
                    for (int local_x = 0; local_x <= Luminumbra::CHUNK_SIZE_X; local_x += sample_step) {
                        const std::size_t sx = static_cast<std::size_t>((base_x + local_x) / sample_step);
                        const std::size_t sz = static_cast<std::size_t>((base_z + local_z) / sample_step);
                        const float height = Luminumbra::World::DequantizeFarLodHeight(
                            tile.height_q[sx + sz * tile.samples_per_side]);
                        const int surface_y = static_cast<int>(std::floor(
                            height / static_cast<float>(Luminumbra::CHUNK_SIZE_Y)));
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
        const IVec3 coords(
            tile.rx * kChunksPerFarLodRegion + local_x,
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
        const auto reduced = Luminumbra::World::ReduceChunkSdfIntoFarTile(
            tile, generated, &reduction_error);
        if (reduced == Luminumbra::World::FarLodSdfReduceResult::Error) {
            error = "failed to generate far-SDF halo brick: " + reduction_error;
            return false;
        }
        changed = changed || reduced != Luminumbra::World::FarLodSdfReduceResult::Unchanged;
    }
    return true;
}

u32 far_brick_crc(const i16* density, const u8* material, std::size_t count) {
    u32 crc = 0xffffffffu;
    const auto update = [&crc](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1u) ^ (0xedb88320u & static_cast<u32>(-(crc & 1u)));
            }
        }
    };
    update(density, count * sizeof(i16));
    update(material, count);
    return ~crc;
}

bool synchronize_regenerable_boundaries(
    Luminumbra::World::FarLodTile& tile,
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
                        static_cast<std::size_t>(y) * side + static_cast<std::size_t>(z) * side * side;
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
                        static_cast<std::size_t>(y) * side + static_cast<std::size_t>(z) * side * side;
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
                tile.sdf_density_q.data() + payload,
                tile.sdf_material.data() + payload,
                count);
            changed = true;
        }
    }
    return true;
}

bool derive_authoritative_surface_water(
    Luminumbra::World::FarLodTile& tile,
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
                for (std::size_t brick_index = 0; brick_index < tile.sdf_bricks.size(); ++brick_index) {
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
                        const auto [found, inserted] = vertical.emplace(world_y, tile.sdf_density_q[sample]);
                        if (!inserted && found->second != tile.sdf_density_q[sample]) {
                            error = "far-SDF water derivation found a mismatched vertical boundary sample";
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
                    (column_x * Luminumbra::CHUNK_SIZE_X + static_cast<int>(local_x) * step) / step);
                const std::size_t sample_z = static_cast<std::size_t>(
                    (column_z * Luminumbra::CHUNK_SIZE_Z + static_cast<int>(local_z) * step) / step);
                u8& flags = tile.flags[sample_x + sample_z * tile.samples_per_side];
                const u8 next = top_surface_y < Luminumbra::SEA_LEVEL
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

} // namespace

FarLodWorkerBuildOutcome BuildFarLodWorkerTile(
    const Systems::SHIELD_WorldSystem& world,
    const Systems::FarLodSdfSnapshot& snapshot,
    World::FarLodTier tier,
    int rx,
    int rz,
    const std::filesystem::path& save_dir) {
    FarLodWorkerBuildOutcome outcome;
    World::FarLodTile tile;
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
        tile = rebase_authoritative_tile(
            world, std::move(tile), tier, rx, rz, snapshot.params_hash);
        outcome.changed = true;
    }

    for (const Systems::FarLodSdfSnapshotEntry& entry : snapshot.entries) {
        if (!belongs_to_region(entry, rx, rz) ||
            entry.provenance != ChunkSdfProvenance::LoadedOrEdited) {
            continue;
        }
        std::string reduction_error;
        const auto reduced = World::ReduceChunkSdfIntoFarTile(
            tile, entry.as_reduction_snapshot(), &reduction_error);
        if (reduced == World::FarLodSdfReduceResult::Error) {
            outcome.error = "failed to reduce authoritative far SDF: " + reduction_error;
            return outcome;
        }
        outcome.changed = outcome.changed || reduced != World::FarLodSdfReduceResult::Unchanged;
    }

    if (!tile_has_authoritative_bricks(tile) && !tile.sdf_bricks.empty()) {
        tile.sdf_bricks.clear();
        tile.sdf_density_q.clear();
        tile.sdf_material.clear();
        outcome.changed = true;
    }
    if (!complete_authoritative_stacks(world, tile, outcome.error, outcome.changed)) {
        return outcome;
    }
    if (!synchronize_regenerable_boundaries(tile, outcome.error, outcome.changed)) {
        return outcome;
    }
    if (!derive_authoritative_surface_water(tile, outcome.error, outcome.changed)) {
        return outcome;
    }
    World::MarchingCubes::GenerateFarLodRegionMesh(tile, outcome.mesh);
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
    // GL resources must have been released through shutdown() while the
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
    if (region.vao) { glDeleteVertexArrays(1, &region.vao); region.vao = 0; }
    if (region.vbo) { glDeleteBuffers(1, &region.vbo); region.vbo = 0; }
    if (region.ebo) { glDeleteBuffers(1, &region.ebo); region.ebo = 0; }
    if (region.water_vao) { glDeleteVertexArrays(1, &region.water_vao); region.water_vao = 0; }
    if (region.water_vbo) { glDeleteBuffers(1, &region.water_vbo); region.water_vbo = 0; }
    if (region.water_ebo) { glDeleteBuffers(1, &region.water_ebo); region.water_ebo = 0; }
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
        completed.assign(
            std::make_move_iterator(m_shared->completed.begin()),
            std::make_move_iterator(m_shared->completed.begin() + static_cast<std::ptrdiff_t>(take)));
        m_shared->completed.erase(
            m_shared->completed.begin(),
            m_shared->completed.begin() + static_cast<std::ptrdiff_t>(take));
    }

    for (BuildResult& result : completed) {
        const u64 key = region_key(result.rx, result.rz);
        m_pending.erase(key);
        const bool snapshot_stale =
            !m_world ||
            !result.sdf_snapshot ||
            result.capture_epoch != result.sdf_snapshot->capture_epoch ||
            result.params_hash != result.sdf_snapshot->params_hash ||
            result.params_hash != m_params_hash ||
            result.authority_revision != result.sdf_snapshot->authority_revision ||
            !m_world->is_far_lod_sdf_snapshot_current(*result.sdf_snapshot);
        if (snapshot_stale) {
            ++m_stats.stale_results_rejected;
        }
        if (result.authority_build_failed) {
            ++m_stats.authority_build_failures;
        }
        if (result.epoch != m_epoch || snapshot_stale ||
            result.mesh.vertices.empty() || result.mesh.indices.empty()) {
            // spec 008 WS-2: a build that returned an empty mesh (or raced an epoch swap)
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
                     result.mesh.vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, region.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(result.mesh.indices.size() * sizeof(u32)),
                     result.mesh.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, position));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, normal));
        glEnableVertexAttribArray(2);
        glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, material_id));
        glBindVertexArray(0);

        region.element_count = static_cast<u32>(result.mesh.indices.size());

        // T-I4-DR-far-water-sheet: upload the flat water sheet (separate VAO so
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
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(result.water_mesh.vertices.size() * sizeof(VoxelVertex)),
                         result.water_mesh.vertices.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, region.water_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(result.water_mesh.indices.size() * sizeof(u32)),
                         result.water_mesh.indices.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, position));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, normal));
            glEnableVertexAttribArray(2);
            glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, material_id));
            glBindVertexArray(0);
            region.water_element_count = static_cast<u32>(result.water_mesh.indices.size());
        }

        const float origin_x = static_cast<float>(result.rx) * kRegionSize;
        const float origin_z = static_cast<float>(result.rz) * kRegionSize;
        region.aabb_min = glm::vec3(
            origin_x,
            result.min_height - kFarDepthBiasMeters,
            origin_z);
        region.aabb_max = glm::vec3(
            origin_x + kRegionSize,
            // The water sheet sits at the global waterline; include it so a
            // fully-submerged region (max terrain height below SEA_LEVEL) is not
            // frustum-culled and drops its water sheet (T-I4-DR-far-water-sheet).
            std::max(result.max_height, Luminumbra::SEA_LEVEL) + 1.0f,
            origin_z + kRegionSize);
        region.resident_bytes =
            result.tile_bytes +
            result.mesh.vertices.size() * sizeof(VoxelVertex) +
            result.mesh.indices.size() * sizeof(u32) +
            result.water_mesh.vertices.size() * sizeof(VoxelVertex) +
            result.water_mesh.indices.size() * sizeof(u32);
        region.last_wanted_frame = m_frame;
        region.authority_revision = result.authority_revision;
        region.persistence_pending = persistence_pending;

        auto existing = m_residents.find(key);
        if (existing != m_residents.end()) {
            release_region(existing->second);
            existing->second = region;
        } else {
            m_residents.emplace(key, region);
        }
        ++m_stats.builds_completed_total;
        ++m_stats.builds_integrated_ok;  // spec 008 WS-2: completed build became resident this frame
    }
}

void FarLodSystem::update(const Systems::SHIELD_WorldSystem& world_system, const glm::vec3& camera_position) {
    ++m_frame;
    m_last_camera_position = camera_position;
    m_stats.enabled = m_enabled;
    m_stats.region_draws = 0;
    m_stats.indices_drawn = 0;
    // spec 008 WS-2: reset the per-frame diagnostic counters before this tick's
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
        const bool resident_matches = resident != m_residents.end() &&
            resident->second.tier == want.tier &&
            resident->second.authority_revision == world_system.far_lod_authority_revision() &&
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

        // FR-B3: warm the hydraulic-erosion regions this far tile will sample
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
        const bool persistence_allowed = std::none_of(
            sdf_snapshot->entries.begin(), sdf_snapshot->entries.end(),
            [&](const Systems::FarLodSdfSnapshotEntry& entry) {
                return belongs_to_region(entry, want.rx, want.rz) &&
                    entry.provenance == ChunkSdfProvenance::LoadedOrEdited &&
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
        const JobHandle handle = m_job_system->dispatch_batch({[shared, world, sdf_snapshot, epoch, tier, rx, rz, save_dir, persistence_allowed]() {
            FarLodWorkerBuildOutcome outcome = BuildFarLodWorkerTile(
                *world, *sdf_snapshot, tier, rx, rz, save_dir);

            BuildResult result;
            result.epoch = epoch;
            result.sdf_snapshot = sdf_snapshot;
            result.capture_epoch = sdf_snapshot->capture_epoch;
            result.params_hash = sdf_snapshot->params_hash;
            result.authority_revision = sdf_snapshot->authority_revision;
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
            // T-I4-DR-far-water-sheet: build the flat water sheet from the same
            // tile's water flags (river channels + seabeds beyond the live ring).
            BuildFarLodWaterSheet(tile, result.water_mesh);
            result.tile = std::move(tile);

            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->completed.push_back(std::move(result));
        }}, JobPriority::Normal);
        if (handle.counter) {
            m_inflight_handles.push_back(handle);
        }
    }

    // Drop completed handles so the wait list stays bounded.
    m_inflight_handles.erase(
        std::remove_if(m_inflight_handles.begin(), m_inflight_handles.end(), [](const JobHandle& handle) {
            return !handle.counter || handle.counter->load(std::memory_order_acquire) <= 0;
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
            const float distance = region_nearest_distance(it->second.rx, it->second.rz, stream_pos);
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
    // spec 008 WS-2: build jobs dispatched this frame + jobs still in flight.
    m_stats.builds_dispatched = dispatched;
    m_stats.pending_depth = m_pending.size();
}

void FarLodSystem::draw_gbuffer(
    Shader& geometry_shader,
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
    // T-I4-DR-horizon-sliver-render: clip far geometry at the GEOMETRY level
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
        const glm::vec3 origin(
            static_cast<float>(region.rx) * kRegionSize,
            -kFarDepthBiasMeters,
            static_cast<float>(region.rz) * kRegionSize);
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), origin);
        const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(view * model)));
        geometry_shader.setMat4("model", model);
        geometry_shader.setMat3("normalMatrix", normal_matrix);
        glBindVertexArray(region.vao);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(region.element_count), GL_UNSIGNED_INT, 0);
        ++draws_out;
        indices_out += region.element_count;
    }

    // T-I4-DR-far-water-sheet: the flat water sheets draw after the far terrain,
    // at the global waterline, with the same inner-radius discard so the live
    // water ring owns the close range. A slightly smaller depth bias than the
    // terrain keeps the sheet from z-fighting the seabed beneath it while still
    // sitting under the live surface. Material id kFarWaterMaterialId tints them
    // deep water in the G-buffer (no live water.frag reflections far out).
    for (const auto& [key, region] : m_residents) {
        (void)key;
        // T-I4-DR-far-water-exposure note: drawing the camera region's sheet
        // here (to cover the live-disc sea where the live water sim does not
        // reach) was tried and reverted - the pale sheet behind the live
        // transparent water shifts water.frag's blend result enough to break
        // the boundary-band blue-dominance classifier. Who renders the
        // live-disc sea is the live-water-coverage task's design question.
        if (region.water_element_count == 0 || is_camera_region(region) ||
            aabb_outside_frustum(region.aabb_min, region.aabb_max, frustum_planes)) {
            continue;
        }
        const glm::vec3 origin(
            static_cast<float>(region.rx) * kRegionSize,
            -kFarWaterDepthBiasMeters,
            static_cast<float>(region.rz) * kRegionSize);
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), origin);
        const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(view * model)));
        geometry_shader.setMat4("model", model);
        geometry_shader.setMat3("normalMatrix", normal_matrix);
        glBindVertexArray(region.water_vao);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(region.water_element_count), GL_UNSIGNED_INT, 0);
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
