#define GLM_ENABLE_EXPERIMENTAL

#include "WaterSystem.h"
#include "SHIELD_WorldSystem.h"
#include "../world/Chunk.h"
#include "../core/WaterComponents.h"
#include "../components/CoreComponents.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <glm/gtx/compatibility.hpp>
#include "../core/Log.h"

namespace Luminumbra::Systems {

// --- Constants for Simulation ---
constexpr float FLOW_CONSTANT = 0.1f;
constexpr float MIN_FLOW_DIFF = 0.001f;
constexpr float MAX_WATER_COMPRESSION = 0.2f;

namespace {

int GetWaterResolution(const Chunk& chunk) {
    return chunk.current_water_resolution.load(std::memory_order_relaxed);
}

size_t GetWaterCellCount(int resolution) {
    return static_cast<size_t>(resolution) * static_cast<size_t>(resolution);
}

bool HasCompleteWaterGrid(const Chunk& chunk, int resolution) {
    if (!chunk.has_water_sim.load(std::memory_order_relaxed) || resolution <= 1) {
        return false;
    }

    const size_t cell_count = GetWaterCellCount(resolution);
    return chunk.water_level_data.size() >= cell_count
        && chunk.water_flow_data.size() >= cell_count
        && chunk.water_sim_terrain_height.size() >= cell_count;
}

int FloorDiv(int value, int divisor) {
    int quotient = value / divisor;
    int remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

int PositiveMod(int value, int divisor) {
    int result = value % divisor;
    return result < 0 ? result + divisor : result;
}

} // namespace

WaterSystem::WaterSystem(JobSystem* job_system, SHIELD_WorldSystem* shield_system)
    : m_job_system(job_system), m_shield_system(shield_system) {}

void WaterSystem::update(entt::registry& registry, const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>& active_chunks) {
    m_active_chunks = &active_chunks;
    if (m_active_chunks->empty()) return;

    // --- ADAPTIVE WATER GRID SYSTEM INTEGRATION ---
    // Get player/camera position for distance-based LOD calculations
    // For now, use origin as fallback - this should be updated to get actual camera position
    Vec3 camera_position = Vec3(0.0f);
    
    // Try to find player entity with transform component
    auto transform_view = registry.view<const Components::TransformComponent>();
    if (!transform_view.empty()) {
        // Use first transform component as camera position approximation
        // TODO: Add proper camera/player tagging system
        camera_position = transform_view.get<const Components::TransformComponent>(transform_view.front()).position;
    }
    
    // Apply adaptive resolution to all active water chunks
    for (const auto& active_chunk : active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (chunk_ptr && chunk_ptr->has_water_sim.load()) {
            Vec3 chunk_center = Vec3(chunk_ptr->get_coords()) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z) + Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z) * 0.5f;
            float camera_distance = glm::length(camera_position - chunk_center);
            
            // Check if player is interacting with water in this chunk (simplified check)
            bool has_player_interaction = camera_distance < 10.0f; // Player very close = likely interacting
            
            // Calculate required detail level
            WaterDetailLevel required_detail = CalculateRequiredDetail(*chunk_ptr, camera_distance, has_player_interaction);
            
            // Resize grid if needed
            if (required_detail != WaterDetailLevel::Off) {
                ResizeSimulationGrid(*chunk_ptr, required_detail);
            }
        }
    }

    auto source_view = registry.view<const Components::TransformComponent, const Components::WaterSourceComponent>();

    for (const auto& active_chunk : active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }

        if (!chunk_ptr->has_water_sim.load()) {
            // For now, use medium resolution as default - adaptive resolution will be applied later
            int initial_resolution = static_cast<int>(WaterDetailLevel::Medium);
            chunk_ptr->current_water_resolution.store(initial_resolution);
            
            const size_t sim_size = initial_resolution * initial_resolution;
            chunk_ptr->water_level_data.assign(sim_size, SEA_LEVEL);
            chunk_ptr->water_flow_data.assign(sim_size, Vec2(0.0f));
            chunk_ptr->water_sim_terrain_height.resize(sim_size);
            const IVec3 c_coords = chunk_ptr->get_coords();
            const float cell_width_x = CHUNK_SIZE_X / (float)initial_resolution;
            const float cell_width_z = CHUNK_SIZE_Z / (float)initial_resolution;

            for (int z = 0; z < initial_resolution; ++z) {
                for (int x = 0; x < initial_resolution; ++x) {
                    float world_x = c_coords.x * CHUNK_SIZE_X + (x + 0.5f) * cell_width_x;
                    float world_z = c_coords.z * CHUNK_SIZE_Z + (z + 0.5f) * cell_width_z;
                    chunk_ptr->water_sim_terrain_height[z * initial_resolution + x] = m_shield_system->GetTerrainHeightAt(world_x, world_z);
                }
            }
            chunk_ptr->has_water_sim.store(true);
        }
    }

    for (auto entity : source_view) {
        auto& transform = source_view.get<const Components::TransformComponent>(entity);
        auto& source = source_view.get<const Components::WaterSourceComponent>(entity);
        IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords(transform.position);
        auto it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));
        if (it != m_active_chunks->end() && it->second) {
            Chunk& chunk = *it->second;
            const int resolution = GetWaterResolution(chunk);
            if (!HasCompleteWaterGrid(chunk, resolution)) {
                continue;
            }

            IVec3 local_pos = IVec3(transform.position) - (chunk_coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z));
            int sim_x = (local_pos.x * resolution) / CHUNK_SIZE_X;
            int sim_z = (local_pos.z * resolution) / CHUNK_SIZE_Z;
            int index = std::clamp(sim_z, 0, resolution - 1) * resolution + std::clamp(sim_x, 0, resolution - 1);
            float cell_area = (static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(resolution))
                * (static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(resolution));
            float water_added = source.flow_rate * SECONDS_PER_TICK / cell_area;
            if (water_added > 0.0f) {
                chunk.water_level_data[index] += water_added;
                // WAKE UP: An internal event occurred in this chunk.
                chunk.is_water_sleeping.store(false, std::memory_order_relaxed);
            }
        }
    }

    // --- Step 2: Wake up sleeping chunks that are adjacent to active ones (propagation) ---
    for (const auto& active_chunk : *m_active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }

        if (chunk_ptr->is_water_sleeping.load(std::memory_order_relaxed)) {
            IVec3 self_coords = chunk_ptr->get_coords();
            const IVec3 neighbor_offsets[] = {{0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}};
            for (const auto& offset : neighbor_offsets) {
                IVec3 neighbor_coords = self_coords + offset;
                auto it = m_active_chunks->find(Chunk::calculate_id(neighbor_coords));
                if (it != m_active_chunks->end() && it->second && !it->second->is_water_sleeping.load(std::memory_order_relaxed)) {
                    // WAKE UP: A neighbor is active, so this chunk must also become active.
                    chunk_ptr->is_water_sleeping.store(false, std::memory_order_relaxed);
                    break; 
                }
            }
        }
    }
    
    // --- Step 3: Collect only the active chunks for simulation ---
    std::vector<Chunk*> chunks_to_sim;
    chunks_to_sim.reserve(m_active_chunks->size());
    for (const auto& active_chunk : *m_active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }

        if (chunk_ptr->has_water_sim.load() && !chunk_ptr->is_water_sleeping.load(std::memory_order_relaxed)) {
            chunks_to_sim.push_back(chunk_ptr.get());
        }
    }

    // --- Step 4: Dispatch simulation jobs for active chunks ---
    if (!chunks_to_sim.empty()) {
        dispatch_simulation_jobs(chunks_to_sim);
    }

    // --- Step 5: Update sleep states for chunks that were just simulated ---
    const int TICKS_TO_SLEEP = 120;       // ~4 seconds at 30 ticks per second
    const float SLEEP_THRESHOLD = 0.0001f; // The max water level change to be considered "calm"

    for (Chunk* chunk : chunks_to_sim) {
        if (chunk->max_water_delta_last_tick < SLEEP_THRESHOLD) {
            chunk->ticks_below_threshold++;
            if (chunk->ticks_below_threshold >= TICKS_TO_SLEEP) {
                // GO TO SLEEP: Chunk has been calm for long enough.
                chunk->is_water_sleeping.store(true, std::memory_order_relaxed);
            }
        } else {
            // STAY AWAKE: Chunk is active, reset its calm counter.
            chunk->ticks_below_threshold = 0;
            chunk->is_water_sleeping.store(false, std::memory_order_relaxed);
        }
    }
}

void WaterSystem::dispatch_simulation_jobs(const std::vector<Chunk*>& chunks_to_simulate) {
    if (!m_active_chunks) {
        return;
    }

    std::unordered_map<ChunkID, WaterChunkSnapshot> water_snapshots;
    water_snapshots.reserve(m_active_chunks->size());

    for (const auto& active_chunk : *m_active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }

        const Chunk& chunk = *chunk_ptr;
        const int resolution = GetWaterResolution(chunk);
        if (!HasCompleteWaterGrid(chunk, resolution)) {
            continue;
        }

        const size_t cell_count = GetWaterCellCount(resolution);
        WaterChunkSnapshot snapshot;
        snapshot.id = chunk.get_id();
        snapshot.coords = chunk.get_coords();
        snapshot.resolution = resolution;
        snapshot.water_levels.assign(chunk.water_level_data.begin(), chunk.water_level_data.begin() + cell_count);
        snapshot.flow_data.assign(chunk.water_flow_data.begin(), chunk.water_flow_data.begin() + cell_count);
        snapshot.terrain_height.assign(chunk.water_sim_terrain_height.begin(), chunk.water_sim_terrain_height.begin() + cell_count);
        water_snapshots.emplace(snapshot.id, std::move(snapshot));
    }

    struct WaterSimulationTask {
        Chunk* chunk = nullptr;
        const WaterChunkSnapshot* snapshot = nullptr;
        WaterSimNeighbors neighbors;
        WaterChunkSimulationOutput output;
    };

    std::vector<WaterSimulationTask> tasks;
    tasks.reserve(chunks_to_simulate.size());

    for (Chunk* chunk : chunks_to_simulate) {
        if (!chunk) {
            continue;
        }

        auto snapshot_it = water_snapshots.find(chunk->get_id());
        if (snapshot_it == water_snapshots.end()) {
            continue;
        }

        const WaterChunkSnapshot& snapshot = snapshot_it->second;
        WaterSimulationTask task;
        task.chunk = chunk;
        task.snapshot = &snapshot;
        task.output.water_levels.resize(GetWaterCellCount(snapshot.resolution));
        task.output.flow_data.resize(GetWaterCellCount(snapshot.resolution));

        auto find_neighbor = [&](int dx, int dz) -> const WaterChunkSnapshot* {
            IVec3 neighbor_coords = snapshot.coords + IVec3(dx, 0, dz);
            auto active_it = m_active_chunks->find(Chunk::calculate_id(neighbor_coords));
            if (active_it == m_active_chunks->end() || !active_it->second) {
                return nullptr;
            }

            auto neighbor_snapshot_it = water_snapshots.find(active_it->second->get_id());
            return neighbor_snapshot_it != water_snapshots.end() ? &neighbor_snapshot_it->second : nullptr;
        };

        task.neighbors.north = find_neighbor(0, 1);
        task.neighbors.south = find_neighbor(0, -1);
        task.neighbors.east  = find_neighbor(1, 0);
        task.neighbors.west  = find_neighbor(-1, 0);

        tasks.push_back(std::move(task));
    }

    std::vector<Job> jobs;
    jobs.reserve(tasks.size());

    for (WaterSimulationTask& task : tasks) {
        WaterSimulationTask* task_ptr = &task;
        jobs.emplace_back([this, task_ptr]() {
            simulate_chunk_water(*task_ptr->snapshot, task_ptr->neighbors, task_ptr->output);
        });
    }

    if (!jobs.empty()) {
        JobHandle handle = m_job_system->dispatch_batch(jobs);
        m_job_system->wait(handle);
    }

    for (WaterSimulationTask& task : tasks) {
        task.chunk->water_level_data = std::move(task.output.water_levels);
        task.chunk->water_flow_data = std::move(task.output.flow_data);
        task.chunk->max_water_delta_last_tick = task.output.max_delta;
    }
}

void WaterSystem::simulate_chunk_water(const WaterChunkSnapshot& snapshot, const WaterSimNeighbors& neighbors, WaterChunkSimulationOutput& output) {
    const int resolution = snapshot.resolution;
    if (resolution <= 1) {
        return;
    }

    const size_t cell_count = GetWaterCellCount(resolution);
    output.water_levels.resize(cell_count);
    output.flow_data.resize(cell_count);

    float max_delta_this_tick = 0.0f;
    const IVec2 neighbors_offset[4] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};

    auto sample_neighbor_edge = [&](const WaterChunkSnapshot* neighbor, int x, int z, int dx, int dz) -> float {
        if (!neighbor || neighbor->resolution <= 1) {
            return SEA_LEVEL;
        }

        const int neighbor_resolution = neighbor->resolution;
        if (neighbor->water_levels.size() < GetWaterCellCount(neighbor_resolution)) {
            return SEA_LEVEL;
        }

        int sample_x = 0;
        int sample_z = 0;

        if (dx < 0) {
            sample_x = neighbor_resolution - 1;
            sample_z = std::clamp(static_cast<int>(((z + 0.5f) / resolution) * neighbor_resolution), 0, neighbor_resolution - 1);
        } else if (dx > 0) {
            sample_x = 0;
            sample_z = std::clamp(static_cast<int>(((z + 0.5f) / resolution) * neighbor_resolution), 0, neighbor_resolution - 1);
        } else if (dz < 0) {
            sample_x = std::clamp(static_cast<int>(((x + 0.5f) / resolution) * neighbor_resolution), 0, neighbor_resolution - 1);
            sample_z = neighbor_resolution - 1;
        } else {
            sample_x = std::clamp(static_cast<int>(((x + 0.5f) / resolution) * neighbor_resolution), 0, neighbor_resolution - 1);
            sample_z = 0;
        }

        return neighbor->water_levels[sample_z * neighbor_resolution + sample_x];
    };

    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const int idx_center = z * resolution + x;
            const float h_center = snapshot.water_levels[idx_center];
            
            float total_outflow = 0.0f;
            float total_inflow = 0.0f;
            Vec2 new_flow_vector = {0.0f, 0.0f};

            for(int i = 0; i < 4; ++i) {
                int nx = x + neighbors_offset[i].x;
                int nz = z + neighbors_offset[i].y;
                float h_neighbor = SEA_LEVEL;

                if (nx >= 0 && nx < resolution && nz >= 0 && nz < resolution) {
                    h_neighbor = snapshot.water_levels[nz * resolution + nx];
                } else if (nx < 0) {
                    h_neighbor = sample_neighbor_edge(neighbors.west, x, z, -1, 0);
                } else if (nx >= resolution) {
                    h_neighbor = sample_neighbor_edge(neighbors.east, x, z, 1, 0);
                } else if (nz < 0) {
                    h_neighbor = sample_neighbor_edge(neighbors.south, x, z, 0, -1);
                } else if (nz >= resolution) {
                    h_neighbor = sample_neighbor_edge(neighbors.north, x, z, 0, 1);
                }

                float diff = h_center - h_neighbor;
                
                if (diff > MIN_FLOW_DIFF) {
                    float outflow = diff * FLOW_CONSTANT;
                    total_outflow += outflow;
                    new_flow_vector += Vec2(neighbors_offset[i]) * outflow;
                } else if (-diff > MIN_FLOW_DIFF) {
                    float inflow = -diff * FLOW_CONSTANT;
                    total_inflow += inflow;
                }
            }
            
            const float terrain_h = snapshot.terrain_height[idx_center];
            const float water_depth = h_center - terrain_h;

            if (water_depth > 0) {
                 total_outflow = std::min(total_outflow, water_depth * MAX_WATER_COMPRESSION);
            } else {
                 total_outflow = 0;
            }
            
            float new_height = h_center - total_outflow + total_inflow;
            float final_height = std::max(new_height, terrain_h);
            output.water_levels[idx_center] = final_height;
            
            // <<< OPTIMIZATION: Measure water activity >>>
            const float delta = std::abs(final_height - h_center);
            if (delta > max_delta_this_tick) {
                max_delta_this_tick = delta;
            }
            
            output.flow_data[idx_center] = glm::mix(snapshot.flow_data[idx_center], new_flow_vector, 0.5f);
        }
    }
    
    output.max_delta = max_delta_this_tick;
}

f32 WaterSystem::get_water_level_at(float world_x, float world_z) const {
    if (!m_active_chunks) return SEA_LEVEL;

    IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords({world_x, 0, world_z});
    auto it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));
    if (it == m_active_chunks->end() || !it->second || !it->second->has_water_sim.load()) {
        return SEA_LEVEL;
    }

    const Chunk& chunk = *it->second;
    const int resolution = GetWaterResolution(chunk);
    if (!HasCompleteWaterGrid(chunk, resolution)) {
        return SEA_LEVEL;
    }
    
    Vec3 base_pos = Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_x - base_pos.x;
    float local_z = world_z - base_pos.z;
    
    float sim_xf = (local_x / CHUNK_SIZE_X) * resolution - 0.5f;
    float sim_zf = (local_z / CHUNK_SIZE_Z) * resolution - 0.5f;

    int x0 = static_cast<int>(sim_xf);
    int z0 = static_cast<int>(sim_zf);
    
    x0 = std::clamp(x0, 0, resolution - 2);
    z0 = std::clamp(z0, 0, resolution - 2);

    float tx = sim_xf - x0;
    float tz = sim_zf - z0;

    float h00 = chunk.water_level_data[z0 * resolution + x0];
    float h10 = chunk.water_level_data[z0 * resolution + (x0 + 1)];
    float h01 = chunk.water_level_data[(z0 + 1) * resolution + x0];
    float h11 = chunk.water_level_data[(z0 + 1) * resolution + (x0 + 1)];

    float h_z0 = glm::mix(h00, h10, tx);
    float h_z1 = glm::mix(h01, h11, tx);

    return glm::mix(h_z0, h_z1, tz);
}

Vec2 WaterSystem::get_water_flow_at(float world_x, float world_z) const {
    if (!m_active_chunks) {
        return Vec2(0.0f);
    }

    IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords({world_x, 0.0f, world_z});
    auto it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));

    if (it == m_active_chunks->end() || !it->second || !it->second->has_water_sim.load()) {
        return Vec2(0.0f);
    }

    const Chunk& chunk = *it->second;
    const int resolution = GetWaterResolution(chunk);
    if (!HasCompleteWaterGrid(chunk, resolution)) {
        return Vec2(0.0f);
    }

    Vec3 base_pos = Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_x - base_pos.x;
    float local_z = world_z - base_pos.z;
    
    float sim_xf = (local_x / CHUNK_SIZE_X) * resolution;
    float sim_zf = (local_z / CHUNK_SIZE_Z) * resolution;

    int x0 = static_cast<int>(sim_xf);
    int z0 = static_cast<int>(sim_zf);
    float tx = sim_xf - x0;
    float tz = sim_zf - z0;
    
    x0 = std::clamp(x0, 0, resolution - 2);
    z0 = std::clamp(z0, 0, resolution - 2);
    int x1 = x0 + 1;
    int z1 = z0 + 1;

    const auto& flow_data = chunk.water_flow_data;
    const Vec2& f00 = flow_data[z0 * resolution + x0];
    const Vec2& f10 = flow_data[z0 * resolution + x1];
    const Vec2& f01 = flow_data[z1 * resolution + x0];
    const Vec2& f11 = flow_data[z1 * resolution + x1];

    Vec2 f_z0 = glm::lerp(f00, f10, tx);
    Vec2 f_z1 = glm::lerp(f01, f11, tx);
    
    return glm::lerp(f_z0, f_z1, tz);
}

void WaterSystem::apply_displacement(const Vec3& world_pos, f32 volume) {
    if (!m_active_chunks) return;
    
    const int radius = 2;
    const float total_cells = (2 * radius + 1) * (2 * radius + 1);
    
    IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords(world_pos);
    auto source_it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));
    if (source_it == m_active_chunks->end() || !source_it->second) {
        return;
    }

    const int source_resolution = GetWaterResolution(*source_it->second);
    if (!HasCompleteWaterGrid(*source_it->second, source_resolution)) {
        return;
    }

    Vec3 base_chunk_pos = Vec3(chunk_coords * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_pos.x - base_chunk_pos.x;
    float local_z = world_pos.z - base_chunk_pos.z;
    int center_sim_x = static_cast<int>((local_x / CHUNK_SIZE_X) * source_resolution);
    int center_sim_z = static_cast<int>((local_z / CHUNK_SIZE_Z) * source_resolution);

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int current_sim_x = center_sim_x + dx;
            int current_sim_z = center_sim_z + dz;

            IVec3 target_chunk_coords = chunk_coords;
            target_chunk_coords.x += FloorDiv(current_sim_x, source_resolution);
            target_chunk_coords.z += FloorDiv(current_sim_z, source_resolution);

            int source_cell_x = PositiveMod(current_sim_x, source_resolution);
            int source_cell_z = PositiveMod(current_sim_z, source_resolution);

            auto it = m_active_chunks->find(Chunk::calculate_id(target_chunk_coords));
            if (it != m_active_chunks->end() && it->second) {
                Chunk& target_chunk = *it->second;
                const int target_resolution = GetWaterResolution(target_chunk);
                if (!HasCompleteWaterGrid(target_chunk, target_resolution)) {
                    continue;
                }

                int final_sim_x = std::clamp(static_cast<int>(((source_cell_x + 0.5f) / source_resolution) * target_resolution), 0, target_resolution - 1);
                int final_sim_z = std::clamp(static_cast<int>(((source_cell_z + 0.5f) / source_resolution) * target_resolution), 0, target_resolution - 1);
                
                float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                float falloff = 1.0f - std::min(distance / (radius + 1.0f), 1.0f);
                falloff = falloff * falloff; // Cosine-like falloff

                const float target_cell_area = (static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(target_resolution))
                    * (static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(target_resolution));
                const float base_height_delta = volume / target_cell_area;
                int index = final_sim_z * target_resolution + final_sim_x;
                target_chunk.water_level_data[index] += (base_height_delta / total_cells) * falloff;

                // WAKE UP: An external event disturbed this chunk.
                target_chunk.is_water_sleeping.store(false, std::memory_order_relaxed);
            }
        }
    }
}

// --- ADAPTIVE WATER GRID SYSTEM IMPLEMENTATION ---

WaterDetailLevel WaterSystem::CalculateRequiredDetail(const Chunk& chunk, float camera_distance, bool has_player_interaction) {
    (void)chunk;

    // Distance-based LOD
    if (camera_distance < 50.0f || has_player_interaction) {
        return WaterDetailLevel::Ultra;  // 32x32 grid
    } else if (camera_distance < 100.0f) {
        return WaterDetailLevel::High;   // 16x16 grid  
    } else if (camera_distance < 200.0f) {
        return WaterDetailLevel::Medium; // 8x8 grid (current default)
    } else if (camera_distance < 400.0f) {
        return WaterDetailLevel::Low;    // 4x4 grid
    } else {
        return WaterDetailLevel::Off;    // No simulation
    }
}

void WaterSystem::ResizeSimulationGrid(Chunk& chunk, WaterDetailLevel new_level) {
    int new_resolution = static_cast<int>(new_level);
    int current_resolution = chunk.current_water_resolution.load();
    
    if (new_resolution == current_resolution) {
        return; // No change needed
    }
    
    if (new_resolution <= 0) { // Disable simulation
        chunk.water_level_data.clear();
        chunk.water_flow_data.clear();
        chunk.water_sim_terrain_height.clear();
        chunk.has_water_sim.store(false);
        chunk.current_water_resolution.store(0);
        return;
    }
    
    // Store old data for interpolation
    std::vector<f32> old_water_levels = chunk.water_level_data;
    std::vector<Vec2> old_flow_data = chunk.water_flow_data;
    const bool can_interpolate = current_resolution > 1
        && old_water_levels.size() >= GetWaterCellCount(current_resolution)
        && old_flow_data.size() >= GetWaterCellCount(current_resolution);
    
    // Resize arrays
    const size_t new_sim_size = new_resolution * new_resolution;
    chunk.water_level_data.resize(new_sim_size);
    chunk.water_flow_data.resize(new_sim_size);
    chunk.water_sim_terrain_height.resize(new_sim_size);
    
    // Regenerate terrain height data for new resolution
    const IVec3 c_coords = chunk.get_coords();
    const float cell_width_x = CHUNK_SIZE_X / (float)new_resolution;
    const float cell_width_z = CHUNK_SIZE_Z / (float)new_resolution;
    
    for (int z = 0; z < new_resolution; ++z) {
        for (int x = 0; x < new_resolution; ++x) {
            int idx = z * new_resolution + x;
            float world_x = c_coords.x * CHUNK_SIZE_X + (x + 0.5f) * cell_width_x;
            float world_z = c_coords.z * CHUNK_SIZE_Z + (z + 0.5f) * cell_width_z;
            chunk.water_sim_terrain_height[idx] = m_shield_system->GetTerrainHeightAt(world_x, world_z);
            
            // Bilinear interpolation from old grid
            if (can_interpolate) {
                float old_x = (x / (float)(new_resolution - 1)) * (current_resolution - 1);
                float old_z = (z / (float)(new_resolution - 1)) * (current_resolution - 1);
                
                int x0 = (int)std::floor(old_x);
                int z0 = (int)std::floor(old_z);
                int x1 = std::min(x0 + 1, current_resolution - 1);
                int z1 = std::min(z0 + 1, current_resolution - 1);
                
                float fx = old_x - x0;
                float fz = old_z - z0;
                
                // Sample old grid
                float w00 = old_water_levels[z0 * current_resolution + x0];
                float w10 = old_water_levels[z0 * current_resolution + x1];
                float w01 = old_water_levels[z1 * current_resolution + x0];
                float w11 = old_water_levels[z1 * current_resolution + x1];
                
                // Bilinear interpolation
                float w0 = w00 * (1.0f - fx) + w10 * fx;
                float w1 = w01 * (1.0f - fx) + w11 * fx;
                chunk.water_level_data[idx] = w0 * (1.0f - fz) + w1 * fz;
                
                // Similar interpolation for flow data
                Vec2 f00 = old_flow_data[z0 * current_resolution + x0];
                Vec2 f10 = old_flow_data[z0 * current_resolution + x1];
                Vec2 f01 = old_flow_data[z1 * current_resolution + x0];
                Vec2 f11 = old_flow_data[z1 * current_resolution + x1];
                
                Vec2 f0 = f00 * (1.0f - fx) + f10 * fx;
                Vec2 f1 = f01 * (1.0f - fx) + f11 * fx;
                chunk.water_flow_data[idx] = f0 * (1.0f - fz) + f1 * fz;
            } else {
                // Initialize with default values
                chunk.water_level_data[idx] = SEA_LEVEL;
                chunk.water_flow_data[idx] = Vec2(0.0f);
            }
        }
    }
    
    chunk.has_water_sim.store(true);
    chunk.current_water_resolution.store(new_resolution);
}

} // namespace Luminumbra::Systems
