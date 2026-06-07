#define GLM_ENABLE_EXPERIMENTAL

#include "WaterSystem.h"
#include "SHIELD_WorldSystem.h"
#include "../world/Chunk.h"
#include "../core/WaterComponents.h"
#include "../components/CoreComponents.h"
#include <algorithm>
#include <numeric>
#include <glm/gtx/compatibility.hpp>
#include "../core/Log.h"

namespace Luminumbra::Systems {

// --- Constants for Simulation ---
constexpr float FLOW_CONSTANT = 0.1f;
constexpr float MIN_FLOW_DIFF = 0.001f;
constexpr float MAX_WATER_COMPRESSION = 0.2f;

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
    for (const auto& [chunk_id, chunk_ptr] : active_chunks) {
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

    for (auto const& [id, chunk_ptr] : active_chunks) {
        if (!chunk_ptr->has_water_sim.load()) {
            // Determine initial water detail level based on distance and content
            Vec3 chunk_center = Vec3(chunk_ptr->get_coords()) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z) + Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z) * 0.5f;
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
        if (it != m_active_chunks->end()) {
            Chunk& chunk = *it->second;
            IVec3 local_pos = IVec3(transform.position) - (chunk_coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z));
            int sim_x = (local_pos.x * WATER_SIM_RESOLUTION_X) / CHUNK_SIZE_X;
            int sim_z = (local_pos.z * WATER_SIM_RESOLUTION_Z) / CHUNK_SIZE_Z;
            int index = std::clamp(sim_z, 0, WATER_SIM_RESOLUTION_Z - 1) * WATER_SIM_RESOLUTION_X + std::clamp(sim_x, 0, WATER_SIM_RESOLUTION_X - 1);
            float water_added = source.flow_rate * SECONDS_PER_TICK / (float)((CHUNK_SIZE_X / WATER_SIM_RESOLUTION_X) * (CHUNK_SIZE_Z / WATER_SIM_RESOLUTION_Z));
            if (water_added > 0.0f) {
                chunk.water_level_data[index] += water_added;
                // WAKE UP: An internal event occurred in this chunk.
                chunk.is_water_sleeping.store(false, std::memory_order_relaxed);
            }
        }
    }

    // --- Step 2: Wake up sleeping chunks that are adjacent to active ones (propagation) ---
    for (auto const& [id, chunk_ptr] : *m_active_chunks) {
        if (chunk_ptr->is_water_sleeping.load(std::memory_order_relaxed)) {
            IVec3 self_coords = chunk_ptr->get_coords();
            const IVec3 neighbor_offsets[] = {{0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}};
            for (const auto& offset : neighbor_offsets) {
                IVec3 neighbor_coords = self_coords + offset;
                auto it = m_active_chunks->find(Chunk::calculate_id(neighbor_coords));
                if (it != m_active_chunks->end() && !it->second->is_water_sleeping.load(std::memory_order_relaxed)) {
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
    for (auto const& [id, chunk_ptr] : *m_active_chunks) {
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
    std::vector<Job> jobs;
    m_next_water_levels.clear();
    m_next_water_levels.reserve(chunks_to_simulate.size());

    for (Chunk* chunk : chunks_to_simulate) {
        m_next_water_levels[chunk->get_id()].resize(chunk->water_level_data.size());

        WaterSimNeighbors neighbors;
        IVec3 self_coords = chunk->get_coords();

        auto find_neighbor = [&](int dx, int dz) -> const std::vector<float>* {
            IVec3 neighbor_coords = self_coords + IVec3(dx, 0, dz);
            auto it = m_active_chunks->find(Chunk::calculate_id(neighbor_coords));
            if (it != m_active_chunks->end() && it->second->has_water_sim.load()) {
                return &it->second->water_level_data;
            }
            return nullptr;
        };

        neighbors.north = find_neighbor(0, 1);
        neighbors.south = find_neighbor(0, -1);
        neighbors.east  = find_neighbor(1, 0);
        neighbors.west  = find_neighbor(-1, 0);

        jobs.emplace_back([this, chunk, neighbors]() {
            simulate_chunk_water(*chunk, neighbors);
        });
    }

    if (!jobs.empty()) {
        JobHandle handle = m_job_system->dispatch_batch(jobs);
        m_job_system->wait(handle);
    }

    for (Chunk* chunk : chunks_to_simulate) {
        chunk->water_level_data = std::move(m_next_water_levels.at(chunk->get_id()));
    }
}

void WaterSystem::simulate_chunk_water(Chunk& chunk, const WaterSimNeighbors& neighbors) {
    auto& next_state = m_next_water_levels.at(chunk.get_id());
    auto& current_flow = chunk.water_flow_data;
    const auto& current_state = chunk.water_level_data;
    float max_delta_this_tick = 0.0f;
    const IVec2 neighbors_offset[4] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};

    for (int z = 0; z < WATER_SIM_RESOLUTION_Z; ++z) {
        for (int x = 0; x < WATER_SIM_RESOLUTION_X; ++x) {
            const int idx_center = z * WATER_SIM_RESOLUTION_X + x;
            const float h_center = current_state[idx_center];
            
            float total_outflow = 0.0f;
            float total_inflow = 0.0f;
            Vec2 new_flow_vector = {0.0f, 0.0f};

            for(int i = 0; i < 4; ++i) {
                int nx = x + neighbors_offset[i].x;
                int nz = z + neighbors_offset[i].y;
                float h_neighbor = SEA_LEVEL;

                if (nx >= 0 && nx < WATER_SIM_RESOLUTION_X && nz >= 0 && nz < WATER_SIM_RESOLUTION_Z) {
                    h_neighbor = current_state[nz * WATER_SIM_RESOLUTION_X + nx];
                } else {
                    // Access neighbor chunks with bounds checking
                    if (nx < 0 && neighbors.west) {
                        int idx = nz * WATER_SIM_RESOLUTION_X + (WATER_SIM_RESOLUTION_X - 1);
                        if (idx >= 0 && idx < neighbors.west->size()) {
                            h_neighbor = (*neighbors.west)[idx];
                        }
                    }
                    else if (nx >= WATER_SIM_RESOLUTION_X && neighbors.east) {
                        int idx = nz * WATER_SIM_RESOLUTION_X;
                        if (idx >= 0 && idx < neighbors.east->size()) {
                            h_neighbor = (*neighbors.east)[idx];
                        }
                    }
                    else if (nz < 0 && neighbors.south) {
                        int idx = (WATER_SIM_RESOLUTION_Z - 1) * WATER_SIM_RESOLUTION_X + nx;
                        if (idx >= 0 && idx < neighbors.south->size()) {
                            h_neighbor = (*neighbors.south)[idx];
                        }
                    }
                    else if (nz >= WATER_SIM_RESOLUTION_Z && neighbors.north) {
                        int idx = nx;
                        if (idx >= 0 && idx < neighbors.north->size()) {
                            h_neighbor = (*neighbors.north)[idx];
                        }
                    }
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
            
            const float terrain_h = chunk.water_sim_terrain_height[idx_center];
            const float water_depth = h_center - terrain_h;

            if (water_depth > 0) {
                 total_outflow = std::min(total_outflow, water_depth * MAX_WATER_COMPRESSION);
            } else {
                 total_outflow = 0;
            }
            
            float new_height = h_center - total_outflow + total_inflow;
            next_state[idx_center] = std::max(new_height, terrain_h);
            
            // <<< OPTIMIZATION: Measure water activity >>>
            const float delta = std::abs(new_height - h_center);
            if (delta > max_delta_this_tick) {
                max_delta_this_tick = delta;
            }
            
            current_flow[idx_center] = glm::mix(current_flow[idx_center], new_flow_vector, 0.5f);
        }
    }
    
    // Store the measured activity for the main thread to use.
    chunk.max_water_delta_last_tick = max_delta_this_tick;
}

f32 WaterSystem::get_water_level_at(float world_x, float world_z) const {
    if (!m_active_chunks) return SEA_LEVEL;

    IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords({world_x, 0, world_z});
    auto it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));
    if (it == m_active_chunks->end() || !it->second->has_water_sim.load()) {
        return SEA_LEVEL;
    }

    const Chunk& chunk = *it->second;
    
    Vec3 base_pos = Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_x - base_pos.x;
    float local_z = world_z - base_pos.z;
    
    float sim_xf = (local_x / CHUNK_SIZE_X) * WATER_SIM_RESOLUTION_X - 0.5f;
    float sim_zf = (local_z / CHUNK_SIZE_Z) * WATER_SIM_RESOLUTION_Z - 0.5f;

    int x0 = static_cast<int>(sim_xf);
    int z0 = static_cast<int>(sim_zf);
    
    x0 = std::clamp(x0, 0, WATER_SIM_RESOLUTION_X - 2);
    z0 = std::clamp(z0, 0, WATER_SIM_RESOLUTION_Z - 2);

    float tx = sim_xf - x0;
    float tz = sim_zf - z0;

    float h00 = chunk.water_level_data[z0 * WATER_SIM_RESOLUTION_X + x0];
    float h10 = chunk.water_level_data[z0 * WATER_SIM_RESOLUTION_X + (x0 + 1)];
    float h01 = chunk.water_level_data[(z0 + 1) * WATER_SIM_RESOLUTION_X + x0];
    float h11 = chunk.water_level_data[(z0 + 1) * WATER_SIM_RESOLUTION_X + (x0 + 1)];

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

    if (it == m_active_chunks->end() || !it->second->has_water_sim) {
        return Vec2(0.0f);
    }

    const Chunk& chunk = *it->second;

    Vec3 base_pos = Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_x - base_pos.x;
    float local_z = world_z - base_pos.z;
    
    float sim_xf = (local_x / CHUNK_SIZE_X) * WATER_SIM_RESOLUTION_X;
    float sim_zf = (local_z / CHUNK_SIZE_Z) * WATER_SIM_RESOLUTION_Z;

    int x0 = static_cast<int>(sim_xf);
    int z0 = static_cast<int>(sim_zf);
    float tx = sim_xf - x0;
    float tz = sim_zf - z0;
    
    x0 = std::clamp(x0, 0, WATER_SIM_RESOLUTION_X - 2);
    z0 = std::clamp(z0, 0, WATER_SIM_RESOLUTION_Z - 2);
    int x1 = x0 + 1;
    int z1 = z0 + 1;

    const auto& flow_data = chunk.water_flow_data;
    const Vec2& f00 = flow_data[z0 * WATER_SIM_RESOLUTION_X + x0];
    const Vec2& f10 = flow_data[z0 * WATER_SIM_RESOLUTION_X + x1];
    const Vec2& f01 = flow_data[z1 * WATER_SIM_RESOLUTION_X + x0];
    const Vec2& f11 = flow_data[z1 * WATER_SIM_RESOLUTION_X + x1];

    Vec2 f_z0 = glm::lerp(f00, f10, tx);
    Vec2 f_z1 = glm::lerp(f01, f11, tx);
    
    return glm::lerp(f_z0, f_z1, tz);
}

void WaterSystem::apply_displacement(const Vec3& world_pos, f32 volume) {
    if (!m_active_chunks) return;
    
    const int radius = 2;
    const float total_cells = (2 * radius + 1) * (2 * radius + 1);

    const float cell_area = (CHUNK_SIZE_X / WATER_SIM_RESOLUTION_X) * (CHUNK_SIZE_Z / WATER_SIM_RESOLUTION_Z);
    const float base_height_delta = volume / cell_area;
    
    IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords(world_pos);
    Vec3 base_chunk_pos = Vec3(chunk_coords * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_pos.x - base_chunk_pos.x;
    float local_z = world_pos.z - base_chunk_pos.z;
    int center_sim_x = static_cast<int>((local_x / CHUNK_SIZE_X) * WATER_SIM_RESOLUTION_X);
    int center_sim_z = static_cast<int>((local_z / CHUNK_SIZE_Z) * WATER_SIM_RESOLUTION_Z);

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int current_sim_x = center_sim_x + dx;
            int current_sim_z = center_sim_z + dz;

            IVec3 target_chunk_coords = chunk_coords;
            target_chunk_coords.x += current_sim_x / WATER_SIM_RESOLUTION_X;
            target_chunk_coords.z += current_sim_z / WATER_SIM_RESOLUTION_Z;
            int final_sim_x = current_sim_x % WATER_SIM_RESOLUTION_X;
            int final_sim_z = current_sim_z % WATER_SIM_RESOLUTION_Z;
             if (final_sim_x < 0) final_sim_x += WATER_SIM_RESOLUTION_X;
             if (final_sim_z < 0) final_sim_z += WATER_SIM_RESOLUTION_Z;

            auto it = m_active_chunks->find(Chunk::calculate_id(target_chunk_coords));
            if (it != m_active_chunks->end() && it->second->has_water_sim) {
                Chunk& target_chunk = *it->second;
                
                float distance = sqrt(dx*dx + dz*dz);
                float falloff = 1.0f - std::min(distance / (radius + 1.0f), 1.0f);
                falloff = falloff * falloff; // Cosine-like falloff

                int index = final_sim_z * WATER_SIM_RESOLUTION_X + final_sim_x;
                target_chunk.water_level_data[index] += (base_height_delta / total_cells) * falloff;

                // WAKE UP: An external event disturbed this chunk.
                target_chunk.is_water_sleeping.store(false, std::memory_order_relaxed);
            }
        }
    }
}

// --- ADAPTIVE WATER GRID SYSTEM IMPLEMENTATION ---

WaterDetailLevel WaterSystem::CalculateRequiredDetail(const Chunk& chunk, float camera_distance, bool has_player_interaction) {
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
    
    if (new_resolution == current_resolution || new_resolution == 0) {
        return; // No change needed or disabling simulation
    }
    
    if (new_resolution == 0) { // Disable simulation
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
            if (!old_water_levels.empty() && current_resolution > 0) {
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
    
    chunk.current_water_resolution.store(new_resolution);
}

} // namespace Luminumbra::Systems