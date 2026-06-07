#include "SHIELD_WorldSystem.h"
#include "entt/entt.hpp"
#include "../world/MarchingCubes.h"
#include <cmath>
#include <algorithm> // Required for std::max and std::min
#include "systems/PhysicsSystem.h"
#include "../core/Log.h"
#include "WaterSystem.h"

constexpr int BASE_WORK_BUDGET_EQUIVALENT = 16;
const int MAX_CHUNKS_TO_PROCESS_PER_FRAME = std::max(1, 
    static_cast<int>(BASE_WORK_BUDGET_EQUIVALENT / (static_cast<float>(Luminumbra::CHUNK_VOLUME) / 4096.0f))
);
const int MAX_COLLISION_MESHES_PER_FRAME = 2;
constexpr size_t MAX_ACTIVE_CHUNKS = 2048;  // Increased from 5 to support proper world streaming
namespace Luminumbra::Systems {

constexpr float FLOW_CONSTANT = 0.1f;
constexpr float MIN_FLOW_DIFF = 0.001f;
constexpr float MAX_WATER_COMPRESSION = 0.2f;

namespace {

float apply_cave_field(float terrain_density, float raw_cave_noise, const TerrainGenParams& params) {
    const float cave_val = std::clamp((raw_cave_noise + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float cave_density = (cave_val - params.cave_threshold) * params.cave_carve_value;
    return std::max(terrain_density, cave_density);
}

}

SHIELD_WorldSystem::SHIELD_WorldSystem(JobSystem* job_system, WaterSystem* water_system, const TerrainGenParams& params, int seed)
    : m_job_system(job_system), m_water_system(water_system), m_params(params), m_seed(seed) {
    reinitialize_noise();
}

SHIELD_WorldSystem::~SHIELD_WorldSystem() {
    wait_for_meshing_jobs();
}

void SHIELD_WorldSystem::wait_for_meshing_jobs() {
    if (m_job_system && m_meshing_job_handle.counter) {
        m_job_system->wait(m_meshing_job_handle);
    }

    m_meshing_job_handle = {};
}

void SHIELD_WorldSystem::reinitialize_noise() {
    // FastNoise2 uses a node-based system to build complex generators.
    
    // 1. Terrain Height Generator (Fractal Simplex Noise)
    auto terrain_noise = FastNoise::New<FastNoise::Simplex>();
    auto terrain_fractal = FastNoise::New<FastNoise::FractalFBm>();
    terrain_fractal->SetSource(terrain_noise);
    terrain_fractal->SetOctaveCount(m_params.octaves);
    terrain_fractal->SetLacunarity(m_params.lacunarity);
    terrain_fractal->SetGain(m_params.persistence);
    m_terrain_generator = terrain_fractal;

    // 2. Cave Generator (3D Perlin Noise)
    auto cave_noise = FastNoise::New<FastNoise::Perlin>();
    m_cave_generator = cave_noise;

    // 3. Island Mask Generator (Low-frequency Simplex)
    auto island_noise = FastNoise::New<FastNoise::Simplex>();
    m_island_mask_generator = island_noise;
}

int SHIELD_WorldSystem::get_lod_level_for_distance(float dist) const {
    for (const auto& lod : m_lod_levels) {
        if (dist <= lod.distance) {
            return lod.level;
        }
    }
    // If it's further than our max LOD distance, use the lowest detail level
    return m_lod_levels.back().level;
}

std::vector<IVec3> SHIELD_WorldSystem::GetInitialChunkLoadList(const Vec3& center_pos) const {
    // Get the terrain height at the spawn position
    float terrain_height = GetTerrainHeightAt(center_pos.x, center_pos.z);
    
    // Calculate which chunk contains the spawn position (not just terrain)
    const IVec3 spawn_chunk_coords = world_to_chunk_coords(center_pos);
    const IVec3 terrain_chunk_coords = world_to_chunk_coords(Vec3(center_pos.x, terrain_height, center_pos.z));
    
    std::vector<IVec3> initial_chunks;
    
    // Load a 3x3 region horizontally, and vertically from terrain to spawn + some extra
    const int INITIAL_LOAD_RADIUS = 1;
    int min_y = std::min(spawn_chunk_coords.y - 1, terrain_chunk_coords.y - 1);
    int max_y = std::max(spawn_chunk_coords.y + 1, terrain_chunk_coords.y + 3);  // Extra chunks above terrain

    for (int dy = min_y; dy <= max_y; ++dy) {
        for (int dz = -INITIAL_LOAD_RADIUS; dz <= INITIAL_LOAD_RADIUS; ++dz) {
            for (int dx = -INITIAL_LOAD_RADIUS; dx <= INITIAL_LOAD_RADIUS; ++dx) {
                // Simple cylindrical check around spawn position
                if (dx * dx + dz * dz <= INITIAL_LOAD_RADIUS * INITIAL_LOAD_RADIUS) {
                    initial_chunks.emplace_back(spawn_chunk_coords + IVec3(dx, dy - spawn_chunk_coords.y, dz));
                }
            }
        }
    }
    
    // Debug: Log initial chunk loading
    LUMINUMBRA_CORE_WARN("Loading {} chunks - Spawn Y={}, Terrain Y={}, Chunk range Y={}..{}", 
        initial_chunks.size(), center_pos.y, terrain_height, min_y * CHUNK_SIZE_Y, max_y * CHUNK_SIZE_Y);
    
    return initial_chunks;
}

void SHIELD_WorldSystem::update(entt::registry& registry, const Vec3& camera_position, PhysicsSystem* physics_system) {
    wait_for_meshing_jobs();

    // Decouple the expensive chunk activation/deactivation logic from the frame rate.
    m_update_tick_counter++;
    if (m_update_tick_counter >= 10) { // Run this logic only every 10 frames.
        update_chunk_activation(camera_position, physics_system);
        m_update_tick_counter = 0;
    }

    // Step 2: Update the water system using the now-current list of active chunks.
    // This MUST happen before meshing jobs are dispatched.
    if (m_water_system) {
        m_water_system->update(registry, m_chunks);
    }

     // Step 3: Schedule meshing jobs for chunks that need it
    std::vector<std::pair<std::shared_ptr<Luminumbra::Chunk>, int>> chunks_to_mesh_jobs;
    chunks_to_mesh_jobs.reserve(64);

    // Chunk state logging removed from hot path - too expensive

    for (auto const& [id, chunk_ptr] : m_chunks) {
        bool needs_meshing = false;
        int required_lod = -1;

        ChunkState state = chunk_ptr->get_state();
        
        if (state == Luminumbra::ChunkState::Idle) {
            needs_meshing = true;
        } else if (state == Luminumbra::ChunkState::Ready) {
            Vec3 chunk_center = (Vec3(chunk_ptr->get_coords()) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
            float dist = glm::distance(camera_position, chunk_center);
            required_lod = get_lod_level_for_distance(dist);

            if (required_lod != chunk_ptr->current_lod.load()) {
                needs_meshing = true;
            }
        }
        
        if (needs_meshing) {
            if (required_lod == -1) {
                Vec3 chunk_center = (Vec3(chunk_ptr->get_coords()) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
                float dist = glm::distance(camera_position, chunk_center);
                required_lod = get_lod_level_for_distance(dist);
            }
            chunks_to_mesh_jobs.push_back({chunk_ptr, required_lod});
            if (chunks_to_mesh_jobs.size() >= MAX_CHUNKS_TO_PROCESS_PER_FRAME) break;
        }
    }

    if (!chunks_to_mesh_jobs.empty()) {
        dispatch_meshing_jobs(chunks_to_mesh_jobs);
    }
    
    // Step 4. Time-slice the creation of expensive physics colliders on the main thread
    if (physics_system) {
        int collision_meshes_created_this_frame = 0;
        for (auto const& [id, chunk_ptr] : m_chunks) {
            if (chunk_ptr->get_state() == ChunkState::Ready && !chunk_ptr->has_collision.load()) {
                // Only create collision for the highest LOD terrain mesh
                if (chunk_ptr->current_lod.load() == 0 && !chunk_ptr->mesh_vertices.empty() && !chunk_ptr->mesh_indices.empty()) {
                    physics_system->add_chunk_collision(*chunk_ptr);
                    chunk_ptr->has_collision.store(true);

                    collision_meshes_created_this_frame++;
                    if (collision_meshes_created_this_frame >= MAX_COLLISION_MESHES_PER_FRAME) {
                        break;
                    }
                }
            }
        }
    }
}

void SHIELD_WorldSystem::update_chunk_activation(const Vec3& player_pos, PhysicsSystem* physics_system) {
    const IVec3 camera_chunk = world_to_chunk_coords(player_pos);
    
    // Also get the terrain chunk for reference
    float terrain_height = GetTerrainHeightAt(player_pos.x, player_pos.z);
    const IVec3 terrain_chunk = world_to_chunk_coords(Vec3(player_pos.x, terrain_height, player_pos.z));

    // 1. Determine all chunks that *should* be loaded
    std::vector<IVec3> candidates;
    candidates.reserve((2 * RENDER_DISTANCE + 1) * (2 * RENDER_DISTANCE + 1) * (RENDER_DISTANCE_UP + RENDER_DISTANCE_DOWN + 1));
    
    // Use a blend of player position and terrain position for vertical range
    int min_y_chunk = std::min(camera_chunk.y - RENDER_DISTANCE_DOWN, terrain_chunk.y - 2);
    int max_y_chunk = std::max(camera_chunk.y + RENDER_DISTANCE_UP, terrain_chunk.y + 2);
    
    for (int dy = min_y_chunk; dy <= max_y_chunk; ++dy) {
        for (int dz = camera_chunk.z - RENDER_DISTANCE; dz <= camera_chunk.z + RENDER_DISTANCE; ++dz) {
            for (int dx = camera_chunk.x - RENDER_DISTANCE; dx <= camera_chunk.x + RENDER_DISTANCE; ++dx) {
                // Only load chunks within horizontal render distance
                int hdist_x = dx - camera_chunk.x;
                int hdist_z = dz - camera_chunk.z;
                if (hdist_x * hdist_x + hdist_z * hdist_z <= RENDER_DISTANCE * RENDER_DISTANCE) {
                    candidates.emplace_back(IVec3(dx, dy, dz));
                }
            }
        }
    }
    // 2. Identify which of these candidate chunks are new and need to be created.
    std::vector<std::pair<IVec3, int>> to_create;
    for (const IVec3& c : candidates) {
        if (m_chunks.size() + to_create.size() >= MAX_ACTIVE_CHUNKS) {
            break; 
        }

        ChunkID id = Chunk::calculate_id(c);
        if (m_chunks.find(id) == m_chunks.end()) {
            IVec3 d = c - camera_chunk;
            int dist2 = d.x * d.x + d.z * d.z + d.y * d.y;
            to_create.emplace_back(c, dist2);
        }
    }

    // 3. Prioritize the creation of chunks closest to the player.
    std::sort(to_create.begin(), to_create.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    std::vector<IVec3> high_priority_generate;
    std::vector<IVec3> low_priority_generate;
    const int PRIORITY_DISTANCE_SQ = 4 * 4;

    for (const auto& pair : to_create) {
        if (pair.second <= PRIORITY_DISTANCE_SQ) {
            high_priority_generate.push_back(pair.first);
        } else {
            low_priority_generate.push_back(pair.first);
        }
    }

    // 4. Dispatch generation jobs according to priority and budget.
    if (!high_priority_generate.empty()) {
        dispatch_generation_jobs(high_priority_generate);
    }

    if (!low_priority_generate.empty()) {
        size_t budget = MAX_CHUNKS_TO_PROCESS_PER_FRAME > high_priority_generate.size() 
            ? MAX_CHUNKS_TO_PROCESS_PER_FRAME - high_priority_generate.size() 
            : 0;
        
        if (budget > 0) {
            low_priority_generate.resize(std::min(low_priority_generate.size(), budget));
            dispatch_generation_jobs(low_priority_generate);
        }
    }

    // <<< OPTIMIZATION START: Efficiently find chunks to unload >>>
    //
    // 5. Unload chunks that are now out of range.
    // Instead of building a large set of required chunks, we iterate the active
    // chunks and check if they are outside an expanded unload radius.
    // This avoids a costly data structure creation and many hash lookups.
    std::vector<ChunkID> to_unload;

    // Add a small buffer (hysteresis) to the render distance to prevent rapid
    // loading/unloading of chunks at the very edge of the view distance.
    const int UNLOAD_DISTANCE_XZ = RENDER_DISTANCE + 2;
    const int UNLOAD_DISTANCE_UP = RENDER_DISTANCE_UP + 2;
    const int UNLOAD_DISTANCE_DOWN = RENDER_DISTANCE_DOWN + 2;

    for (const auto& [id, chunk_ptr] : m_chunks) {
        const IVec3 d = chunk_ptr->get_coords() - camera_chunk;
        if (std::abs(d.x) > UNLOAD_DISTANCE_XZ ||
            std::abs(d.z) > UNLOAD_DISTANCE_XZ ||
            d.y > UNLOAD_DISTANCE_UP ||
            d.y < -UNLOAD_DISTANCE_DOWN)
        {
            to_unload.push_back(id);
        }
    }

    for (ChunkID id : to_unload) {
        if (physics_system) {
            physics_system->remove_chunk_collision(id);
        }
        m_chunks.erase(id);
    }
    // <<< OPTIMIZATION END >>>
}

float SHIELD_WorldSystem::GetTerrainHeightAt(float world_x, float world_z) const {
    // <<< FIX: The function returns the value directly.
    float noise_value = m_terrain_generator->GenSingle2D(world_x * m_params.base_frequency, world_z * m_params.base_frequency, m_seed);
    return m_params.height_offset + noise_value * m_params.base_amplitude;
}

float SHIELD_WorldSystem::get_density_at_from_precalculated(const Vec3& world_pos, float terrain_height) const {
    float terrain_density = world_pos.y - terrain_height;
    if (m_params.caves_enabled) {
        // <<< FIX: The function returns the value directly.
        float cave_noise = m_cave_generator->GenSingle3D(world_pos.x * m_params.cave_frequency, world_pos.y * m_params.cave_frequency, world_pos.z * m_params.cave_frequency, m_seed + 1);
        terrain_density = apply_cave_field(terrain_density, cave_noise, m_params);
    }
    return terrain_density;
}

float SHIELD_WorldSystem::get_density_at(const Vec3& world_pos) const {
    float terrain_height = GetTerrainHeightAt(world_pos.x, world_pos.z);
    if (m_params.island_mask_enabled) {
        // <<< FIX: The function returns the value directly.
        float island_value = m_island_mask_generator->GenSingle2D(world_pos.x * m_params.island_mask_frequency, world_pos.z * m_params.island_mask_frequency, m_seed + 2);
        float island_mask = glm::smoothstep(0.1f, 0.25f, island_value);
        terrain_height = glm::mix(m_params.height_offset, terrain_height, island_mask);
    }
    
    return get_density_at_from_precalculated(world_pos, terrain_height);
}

std::vector<Luminumbra::Chunk*> SHIELD_WorldSystem::get_renderable_chunks() {
    wait_for_meshing_jobs();

    std::vector<Luminumbra::Chunk*> renderable;
    renderable.reserve(m_chunks.size());
    
    int total_chunks = 0;
    int ready_chunks = 0;
    int chunks_with_mesh = 0;
    
    for (auto const& [id, chunk_ptr] : m_chunks) {
        total_chunks++;
        if (chunk_ptr->get_state() == Luminumbra::ChunkState::Ready) {
            ready_chunks++;
            if (!chunk_ptr->mesh_vertices.empty()) {
                chunks_with_mesh++;
                renderable.push_back(chunk_ptr.get());
                
                // Debug logging disabled to prevent segfault from static atomics
            }
        }
    }
    
    // Performance logging removed from hot path - use profiler instead
    
    return renderable;
}

void SHIELD_WorldSystem::GenerateChunkData(Luminumbra::Chunk& chunk) const {
   const IVec3 coords = chunk.get_coords();
   const IVec3 base_pos = coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
   const int size_x = CHUNK_SIZE_X + 1;
   const int size_y = CHUNK_SIZE_Y + 1;
   const int size_z = CHUNK_SIZE_Z + 1;
   const size_t padded_volume = static_cast<size_t>(size_x) * size_y * size_z;
   
   // Try GPU generation first
   if (m_gpu_sdf_callback && m_gpu_sdf_callback(coords, m_params, m_seed, chunk.sdf_data)) {
       // GPU generation successful - still need to generate heightmap for physics
       const size_t heightmap_size = static_cast<size_t>(size_x) * size_z;
       chunk.heightmap_data.resize(heightmap_size);
       
       // Generate heightmap from SDF data
       for (int z = 0; z < size_z; ++z) {
           for (int x = 0; x < size_x; ++x) {
               int heightmap_idx = z * size_x + x;
               
               // Find surface by marching down through SDF
               float surface_height = base_pos.y + size_y; // Start from top
               for (int y = size_y - 1; y >= 0; --y) {
                   int sdf_idx = z * (size_x * size_y) + y * size_x + x;
                   if (chunk.sdf_data[sdf_idx] <= 0.0f) {
                       surface_height = base_pos.y + y;
                       break;
                   }
               }
               chunk.heightmap_data[heightmap_idx] = surface_height;
           }
       }
       
       chunk.set_state(ChunkState::Idle);
       return;
   }
   
   // Fallback to CPU generation
   chunk.sdf_data.resize(padded_volume);

   const size_t heightmap_size = static_cast<size_t>(size_x) * size_z;
   chunk.heightmap_data.resize(heightmap_size);

   // --- Step 1: Generate all noise data for the chunk in large, SIMD-accelerated batches ---
   std::vector<float> heightmap_noise(heightmap_size);
   std::vector<float> island_mask_noise(heightmap_size);
   std::vector<float> cave_noise;
   if (m_params.caves_enabled) {
       cave_noise.resize(padded_volume);
   }
   
   // FastNoise GenUniformGrid2D populates its buffer in [x][z] layout where x varies fastest
   m_terrain_generator->GenUniformGrid2D(heightmap_noise.data(), base_pos.x, base_pos.z, size_x, size_z, m_params.base_frequency, m_seed);
   m_island_mask_generator->GenUniformGrid2D(island_mask_noise.data(), base_pos.x, base_pos.z, size_x, size_z, m_params.island_mask_frequency, m_seed + 2);
   
   // FastNoise GenUniformGrid3D populates its buffer in [x][y][z] layout where x varies fastest
   if (m_params.caves_enabled) {
       m_cave_generator->GenUniformGrid3D(cave_noise.data(), base_pos.x, base_pos.y, base_pos.z, size_x, size_y, size_z, m_params.cave_frequency, m_seed + 1);
   }
   
   // --- Step 2: Combine the pre-generated noise to calculate the final SDF values ---
   for (int z = 0; z < size_z; ++z) {
       for (int y = 0; y < size_y; ++y) {
           for (int x = 0; x < size_x; ++x) {
               // For 2D noise buffers in [x][z] layout (x varies fastest):
               size_t index_2d_read = static_cast<size_t>(x) + static_cast<size_t>(z) * size_x;

               // Index for WRITING to our sdf_data and heightmap_data with bounds checking
               size_t sdf_write_idx = static_cast<size_t>(x) + static_cast<size_t>(y) * size_x + static_cast<size_t>(z) * size_x * size_y;
               size_t heightmap_write_idx = static_cast<size_t>(x) + static_cast<size_t>(z) * size_x;
               
               // Bounds checking
               if (sdf_write_idx >= chunk.sdf_data.size()) {
                   LUMINUMBRA_CORE_ERROR("SDF index out of bounds: {} >= {}", sdf_write_idx, chunk.sdf_data.size());
                   continue;
               }
               if (y == 0 && heightmap_write_idx >= chunk.heightmap_data.size()) {
                   LUMINUMBRA_CORE_ERROR("Heightmap index out of bounds: {} >= {}", heightmap_write_idx, chunk.heightmap_data.size());
                   continue;
               }
               
               // A. Calculate final terrain height for this (x,z) column
               float terrain_h = m_params.height_offset + heightmap_noise[index_2d_read] * m_params.base_amplitude;
               if (m_params.island_mask_enabled) {
                   float island_mask = glm::smoothstep(0.1f, 0.25f, island_mask_noise[index_2d_read]);
                   terrain_h = glm::mix(m_params.height_offset, terrain_h, island_mask);
               }

               // B. Calculate base terrain density
               float current_world_y = base_pos.y + y;
               float terrain_density = current_world_y - terrain_h;

               // C. Carve caves using the 3D noise buffer
               if (m_params.caves_enabled) {
                   // For 3D noise buffer in [x][y][z] layout (x varies fastest):
                   size_t cave_read_idx = static_cast<size_t>(x) + static_cast<size_t>(y) * size_x + static_cast<size_t>(z) * size_x * size_y;
                   terrain_density = apply_cave_field(terrain_density, cave_noise[cave_read_idx], m_params);
               }
               
               if (y == 0) {
                   chunk.heightmap_data[heightmap_write_idx] = terrain_h;
               }

               // D. Write final density to chunk data using our consistent internal layout.
               // SDF debug logging removed to prevent segfault
               chunk.sdf_data[sdf_write_idx] = terrain_density;
           }
       }
   }
}

JobHandle SHIELD_WorldSystem::dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate) {
    std::vector<Luminumbra::Job> jobs;
    for (const auto& coords : chunks_to_generate) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Loading);
        m_chunks[chunk->get_id()] = chunk;

        // Chunk generation job created

        jobs.emplace_back([this, chunk, coords]() {
            GenerateChunkData(*chunk);
            
            // DEBUG: Check if chunk has valid SDF data
            bool has_surface = false;
            for (float val : chunk->sdf_data) {
                if (val < 0.0f && val > -10.0f) {  // Near-surface values
                    has_surface = true;
                    break;
                }
            }
            // Surface detection debug logging removed to prevent segfault
            
            chunk->set_state(Luminumbra::ChunkState::Idle);
        });
    }
    
    JobHandle handle; // Declare handle outside the if block
    
    if (m_job_system && !jobs.empty()) {
        handle = m_job_system->dispatch_batch(jobs);
        m_job_system->wait(handle);
    }
        
    // DEBUG: After generation completes, check SDF data from main thread
    for (const auto& coords : chunks_to_generate) {
        ChunkID id = Chunk::calculate_id(coords);
        auto it = m_chunks.find(id);
        if (it != m_chunks.end()) {
            auto& chunk = it->second;
            
            // Check SDF range
            float min_val = 999999.0f, max_val = -999999.0f;
            int neg_count = 0, pos_count = 0;
            for (float val : chunk->sdf_data) {
                if (val < min_val) min_val = val;
                if (val > max_val) max_val = val;
                if (val < 0.0f) neg_count++;
                if (val > 0.0f) pos_count++;
            }
            
            // SDF analysis logging removed from hot path
        }
    }
    
    return handle; // Return the handle (will be default-constructed/invalid if no jobs were dispatched)
}

void SHIELD_WorldSystem::dispatch_meshing_jobs(const std::vector<std::pair<std::shared_ptr<Luminumbra::Chunk>, int>>& chunks_to_mesh) {
    std::vector<Luminumbra::Job> jobs;
    for (const auto& pair : chunks_to_mesh) {
        auto& chunk = pair.first;
        int step = pair.second; // The LOD level is the step size

        chunk->set_state(Luminumbra::ChunkState::Meshing);
        chunk->current_lod.store(step);

        jobs.emplace_back([this, chunk, step]() {
            try {
                // 1. Generate the terrain mesh from the SDF data
                Luminumbra::World::MarchingCubes::PolygoniseTerrain(*this, *chunk, 0.0f, step);
                
                // Terrain mesh generation debug logging removed to prevent segfault
                
                // 2. Generate the water surface mesh if the water system exists
                if (m_water_system) {
                    // TEMP: Disable water mesh generation to test terrain rendering
                    // LUMINUMBRA_CORE_WARN("WATER DEBUG: Skipping water mesh for chunk ({},{},{})", 
                    //     chunk->get_coords().x, chunk->get_coords().y, chunk->get_coords().z);
                    chunk->water_mesh_vertices.clear();
                    chunk->water_mesh_indices.clear();
                    /*
                    // Only generate high-detail water mesh for highest LOD terrain
                    if (step == 0 || step == 1) {  // Changed from step == 1
                        Luminumbra::World::MarchingCubes::GenerateWaterMesh(*m_water_system, *this, *chunk);
                    } else {
                        chunk->water_mesh_vertices.clear();
                        chunk->water_mesh_indices.clear();
                    }
                    */
                }

                chunk->mesh_version++;
                chunk->has_collision.store(false); 
                chunk->set_state(Luminumbra::ChunkState::Ready);
                
                // Job completion debug logging removed to prevent segfault
            } catch (const std::exception& e) {
                LUMINUMBRA_CORE_ERROR("MESHING JOB CRASH: Chunk ({},{},{}) failed: {}", 
                    chunk->get_coords().x, chunk->get_coords().y, chunk->get_coords().z, e.what());
                chunk->set_state(Luminumbra::ChunkState::Idle); // Reset to allow retry
            } catch (...) {
                LUMINUMBRA_CORE_ERROR("MESHING JOB CRASH: Chunk ({},{},{}) failed with unknown exception", 
                    chunk->get_coords().x, chunk->get_coords().y, chunk->get_coords().z);
                chunk->set_state(Luminumbra::ChunkState::Idle); // Reset to allow retry
            }
        });
    }
    if (m_job_system && !jobs.empty()) {
        m_meshing_job_handle = m_job_system->dispatch_batch(jobs);
    }
}

void SHIELD_WorldSystem::set_params(const TerrainGenParams& params) {
    wait_for_meshing_jobs();

    m_params = params;
    reinitialize_noise();
}

void SHIELD_WorldSystem::set_seed(int seed) {
    wait_for_meshing_jobs();

    m_seed = seed;
    reinitialize_noise();
}

void SHIELD_WorldSystem::clear_world(PhysicsSystem* physics_system) {
    wait_for_meshing_jobs();

    if (physics_system) {
        for (const auto& [id, chunk] : m_chunks) {
            physics_system->remove_chunk_collision(id);
        }
    }
    m_chunks.clear();
    LUMINUMBRA_CORE_INFO("World cleared.");
}

void SHIELD_WorldSystem::regenerate_all_chunks(PhysicsSystem* physics_system) {
    wait_for_meshing_jobs();

    LUMINUMBRA_CORE_INFO("Regenerating all active chunks...");
    std::vector<IVec3> coords_to_regenerate;
    coords_to_regenerate.reserve(m_chunks.size());
    for(const auto& [id, chunk] : m_chunks) {
        coords_to_regenerate.push_back(chunk->get_coords());
    }
    clear_world(physics_system);
    dispatch_generation_jobs(coords_to_regenerate);
}

void SHIELD_WorldSystem::SetWaterSystem(WaterSystem* water_system) {
    wait_for_meshing_jobs();

    m_water_system = water_system;
}

IVec3 SHIELD_WorldSystem::world_to_chunk_coords(const Vec3& position) {
    return IVec3(
        static_cast<int>(std::floor(position.x / CHUNK_SIZE_X)),
        static_cast<int>(std::floor(position.y / CHUNK_SIZE_Y)),
        static_cast<int>(std::floor(position.z / CHUNK_SIZE_Z))
    );
}

void SHIELD_WorldSystem::SetGPUSDFCallback(std::function<bool(const IVec3&, const TerrainGenParams&, int, std::vector<float>&)> callback) {
    m_gpu_sdf_callback = callback;
}

} // namespace Luminumbra::Systems
