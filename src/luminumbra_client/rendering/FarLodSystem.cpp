#include "FarLodSystem.h"

#include "Shader.h"
#include "passes/PassGlHelpers.h"
#include "core/Log.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/MarchingCubes.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Luminumbra::Rendering {
namespace {

constexpr float kRegionSize = static_cast<float>(Luminumbra::World::kFarLodRegionSizeMeters);

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

} // namespace

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
        if (result.epoch != m_epoch || result.mesh.vertices.empty() || result.mesh.indices.empty()) {
            continue;
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

        auto existing = m_residents.find(key);
        if (existing != m_residents.end()) {
            release_region(existing->second);
            existing->second = region;
        } else {
            m_residents.emplace(key, region);
        }
        ++m_stats.builds_completed_total;
    }
}

void FarLodSystem::update(const Systems::SHIELD_WorldSystem& world_system, const glm::vec3& camera_position) {
    ++m_frame;
    m_last_camera_position = camera_position;
    m_stats.enabled = m_enabled;
    m_stats.region_draws = 0;
    m_stats.indices_drawn = 0;

    if (!m_enabled || !m_job_system) {
        m_stats.regions_wanted = 0;
        m_stats.regions_missing = 0;
        m_stats.regions_building = m_pending.size();
        m_stats.regions_resident = m_residents.size();
        m_stats.resident_bytes = total_resident_bytes();
        return;
    }

    bind_world(world_system);

    // --- Ring-diff wanted set out to the F2 outer range ---
    struct Wanted {
        int rx;
        int rz;
        World::FarLodTier tier;
        float nearest;
    };
    std::vector<Wanted> wanted;
    const int camera_rx = static_cast<int>(std::floor(camera_position.x / kRegionSize));
    const int camera_rz = static_cast<int>(std::floor(camera_position.z / kRegionSize));
    const int scan_radius = static_cast<int>(std::ceil(kF2OuterRangeMeters / kRegionSize)) + 1;
    for (int rz = camera_rz - scan_radius; rz <= camera_rz + scan_radius; ++rz) {
        for (int rx = camera_rx - scan_radius; rx <= camera_rx + scan_radius; ++rx) {
            const float nearest = region_nearest_distance(rx, rz, camera_position);
            if (nearest > kF2OuterRangeMeters) {
                continue;
            }
            // Live wins: a region the live chunk ring covers entirely is
            // never drawn far.
            if (region_farthest_distance(rx, rz, camera_position) <= kLiveRingRadiusMeters) {
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
        const bool resident_matches = resident != m_residents.end() && resident->second.tier == want.tier;
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

        m_pending[key] = want.tier;
        ++dispatched;
        auto shared = m_shared;
        const Systems::SHIELD_WorldSystem* world = m_world;
        const u64 params_hash = m_params_hash;
        const u64 epoch = m_epoch;
        const World::FarLodTier tier = want.tier;
        const int rx = want.rx;
        const int rz = want.rz;
        const std::filesystem::path save_dir = m_save_dir;
        const JobHandle handle = m_job_system->dispatch_batch({[shared, world, params_hash, epoch, tier, rx, rz, save_dir]() {
            World::FarLodTile tile;
            bool loaded = false;
            if (!save_dir.empty()) {
                // Edited (authoritative) tiles - and valid pristine cache
                // entries - come from the LMR1 store.
                const World::FarLodStore store(save_dir);
                loaded = store.load_tile(tier, rx, rz, params_hash, tile);
            }
            if (!loaded) {
                tile = World::BuildPristineFarLodTile(*world, tier, rx, rz, params_hash);
            }

            BuildResult result;
            result.epoch = epoch;
            result.tier = tier;
            result.rx = rx;
            result.rz = rz;
            result.tile_bytes = tile.sample_count() * 4u;
            u16 min_q = std::numeric_limits<u16>::max();
            u16 max_q = 0;
            for (const u16 q : tile.height_q) {
                min_q = std::min(min_q, q);
                max_q = std::max(max_q, q);
            }
            result.min_height = World::DequantizeFarLodHeight(min_q) -
                static_cast<float>(World::FarLodSampleStepMeters(tier));
            result.max_height = World::DequantizeFarLodHeight(max_q);
            World::MarchingCubes::GenerateFarLodRegionMesh(tile, result.mesh);
            // T-I4-DR-far-water-sheet: build the flat water sheet from the same
            // tile's water flags (river channels + seabeds beyond the live ring).
            BuildFarLodWaterSheet(tile, result.water_mesh);

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
        } else {
            ++it;
        }
    }
    std::size_t resident_bytes = total_resident_bytes();
    while (resident_bytes > kResidentBudgetBytes && !m_residents.empty()) {
        auto victim = m_residents.begin();
        float victim_distance = 0.0f;
        for (auto it = m_residents.begin(); it != m_residents.end(); ++it) {
            const float distance = region_nearest_distance(it->second.rx, it->second.rz, camera_position);
            if (distance > victim_distance) {
                victim_distance = distance;
                victim = it;
            }
        }
        resident_bytes -= victim->second.resident_bytes;
        release_region(victim->second);
        m_residents.erase(victim);
        ++m_stats.evictions_total;
    }

    m_stats.regions_wanted = wanted.size();
    m_stats.regions_missing = missing;
    m_stats.regions_building = m_pending.size();
    m_stats.regions_resident = m_residents.size();
    m_stats.resident_bytes = resident_bytes;
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
    const int camera_rx = static_cast<int>(std::floor(m_last_camera_position.x / kRegionSize));
    const int camera_rz = static_cast<int>(std::floor(m_last_camera_position.z / kRegionSize));
    const auto is_camera_region = [&](const ResidentRegion& region) {
        return region.rx == camera_rx && region.rz == camera_rz;
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

    m_stats.region_draws = draws_out;
    m_stats.indices_drawn = indices_out;
    m_stats.water_sheet_draws = water_draws;
    m_stats.water_sheet_indices = water_indices;
}

} // namespace Luminumbra::Rendering
