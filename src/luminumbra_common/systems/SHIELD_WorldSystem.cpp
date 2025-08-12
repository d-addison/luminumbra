#include "SHIELD_WorldSystem.h"
#include "entt/entt.hpp"
#include "FastNoiseLite.h"
#include "../world/MarchingCubes.h"
#include <cmath>
#include <algorithm> // Required for std::max and std::min
#include "systems/PhysicsSystem.h"
#include "../core/Log.h"

// FIX 1: Define the dynamic budget at the top of the .cpp file, not in the header.
// This prevents it from being defined in multiple translation units.
// Use the fully qualified Luminumbra::CHUNK_VOLUME.
constexpr int BASE_WORK_BUDGET_EQUIVALENT = 16; // A more conservative budget
const int MAX_CHUNKS_TO_PROCESS_PER_FRAME = std::max(1, 
    static_cast<int>(BASE_WORK_BUDGET_EQUIVALENT / (static_cast<float>(Luminumbra::CHUNK_VOLUME) / 4096.0f))
);

// Define a separate, smaller budget for expensive, main-thread physics creation.
const int MAX_COLLISION_MESHES_PER_FRAME = 2;


namespace Luminumbra::Systems {

SHIELD_WorldSystem::SHIELD_WorldSystem(JobSystem* job_system, const TerrainGenParams& params, int seed)
    : m_job_system(job_system), m_params(params), m_seed(seed) {
    reinitialize_noise();
}

void SHIELD_WorldSystem::reinitialize_noise() {
    m_terrainNoise = fnlCreateState();
    m_terrainNoise.seed = m_seed;
    m_terrainNoise.noise_type = FNL_NOISE_OPENSIMPLEX2;

    m_caveNoise = fnlCreateState();
    m_caveNoise.seed = m_seed + 1;
    m_caveNoise.noise_type = FNL_NOISE_PERLIN;
    m_caveNoise.frequency = m_params.cave_frequency;

    m_islandMaskNoise = fnlCreateState();
    m_islandMaskNoise.seed = m_seed + 2;
    m_islandMaskNoise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    m_islandMaskNoise.frequency = m_params.island_mask_frequency;
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

void SHIELD_WorldSystem::update(entt::registry& registry, const Vec3& camera_position, PhysicsSystem* physics_system) {
    // 1. Load/unload chunks based on player position
    update_chunk_activation(camera_position, physics_system);

    std::vector<std::pair<std::shared_ptr<Chunk>, int>> chunks_to_mesh_jobs;
    chunks_to_mesh_jobs.reserve(64);

    for (auto const& [id, chunk_ptr] : m_chunks) {
        bool needs_meshing = false;
        int required_lod = -1;

        // Condition 1: Chunk is freshly generated and needs its first mesh
        if (chunk_ptr->get_state() == ChunkState::Idle) {
            needs_meshing = true;
        }
        // Condition 2: Chunk already has a mesh, check if LOD needs to change
        else if (chunk_ptr->get_state() == ChunkState::Ready) {
            Vec3 chunk_center = (Vec3(chunk_ptr->get_coords()) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
            float dist = glm::distance(camera_position, chunk_center);
            required_lod = get_lod_level_for_distance(dist);

            if (required_lod != chunk_ptr->current_lod.load()) {
                needs_meshing = true;
            }
        }

        if (needs_meshing) {
            // If we didn't already calculate the required LOD, do it now
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

    // 3. Time-slice the creation of expensive physics colliders on the main thread
    if (physics_system) {
        int collision_meshes_created_this_frame = 0;
        for (auto const& [id, chunk_ptr] : m_chunks) {
            // Check if mesh is ready and collision hasn't been created yet
            if (chunk_ptr->get_state() == ChunkState::Ready && !chunk_ptr->has_collision.load()) {
                if (!chunk_ptr->mesh_vertices.empty()) {
                    physics_system->add_chunk_collision(*chunk_ptr);
                    chunk_ptr->has_collision.store(true);

                    collision_meshes_created_this_frame++;
                    if (collision_meshes_created_this_frame >= MAX_COLLISION_MESHES_PER_FRAME) {
                        break; // Stop after our budget is used
                    }
                }
            }
        }
    }
}

std::vector<Chunk*> SHIELD_WorldSystem::get_renderable_chunks() {
    std::vector<Chunk*> renderable;
    renderable.reserve(m_chunks.size());
    for (auto const& [id, chunk_ptr] : m_chunks) {
        if (chunk_ptr->get_state() == ChunkState::Ready && !chunk_ptr->mesh_vertices.empty()) {
            renderable.push_back(chunk_ptr.get());
        }
    }
    return renderable;
}

void SHIELD_WorldSystem::update_chunk_activation(const Vec3& player_pos, PhysicsSystem* physics_system) {
    const IVec3 camera_chunk = world_to_chunk_coords(player_pos);

    std::vector<IVec3> candidates;
    for (int dy = -RENDER_DISTANCE_DOWN; dy <= RENDER_DISTANCE_UP; ++dy) {
        for (int dz = -RENDER_DISTANCE; dz <= RENDER_DISTANCE; ++dz) {
            for (int dx = -RENDER_DISTANCE; dx <= RENDER_DISTANCE; ++dx) {
                candidates.emplace_back(camera_chunk + IVec3(dx, dy, dz));
            }
        }
    }

    std::vector<std::pair<IVec3, int>> to_create;
    for (const IVec3& c : candidates) {
        ChunkID id = Chunk::calculate_id(c);
        if (m_chunks.find(id) == m_chunks.end()) {
            IVec3 d = c - camera_chunk;
            int dist2 = d.x * d.x + d.z * d.z + d.y * d.y;
            to_create.emplace_back(c, dist2);
        }
    }

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

    if (!high_priority_generate.empty()) {
        dispatch_generation_jobs(high_priority_generate);
    }

    if (!low_priority_generate.empty()) {
        // --- FIX 2: Use the new dynamic budget for generation as well ---
        size_t budget = MAX_CHUNKS_TO_PROCESS_PER_FRAME > high_priority_generate.size() 
            ? MAX_CHUNKS_TO_PROCESS_PER_FRAME - high_priority_generate.size() 
            : 0;
        
        if (budget > 0) {
            low_priority_generate.resize(std::min(low_priority_generate.size(), budget));
            dispatch_generation_jobs(low_priority_generate);
        }
    }

    std::unordered_set<ChunkID> required;
    for (const IVec3& c : candidates)
        required.insert(Chunk::calculate_id(c));

    std::vector<ChunkID> to_unload;
    for (auto& [id, ptr] : m_chunks)
        if (required.find(id) == required.end())
            to_unload.push_back(id);

    for (ChunkID id : to_unload) {
        if (physics_system) {
            physics_system->remove_chunk_collision(id);
        }
        m_chunks.erase(id);
    }
}

float SHIELD_WorldSystem::GetTerrainHeightAt(float world_x, float world_z) const {
    float terrain_height = m_params.height_offset;
    float frequency = m_params.base_frequency;
    float amplitude = m_params.base_amplitude;

    for (int i = 0; i < m_params.octaves; ++i) {
        terrain_height += fnlGetNoise2D(&m_terrainNoise, world_x * frequency, world_z * frequency) * amplitude;
        frequency *= m_params.lacunarity;
        amplitude *= m_params.persistence;
    }
    return terrain_height;
}

float SHIELD_WorldSystem::get_density_at(const Vec3& world_pos) const {
    float terrain_height = GetTerrainHeightAt(world_pos.x, world_pos.z);

    if (m_params.island_mask_enabled) {
        float island_value = fnlGetNoise2D(&m_islandMaskNoise, world_pos.x, world_pos.z);
        float island_mask = glm::smoothstep(0.1f, 0.25f, island_value);
        terrain_height = glm::mix(m_params.height_offset, terrain_height, island_mask);
    }

    float density = world_pos.y - terrain_height;

    if (m_params.caves_enabled) {
        float cave_noise = (fnlGetNoise3D(&m_caveNoise, world_pos.x, world_pos.y, world_pos.z) + 1.0f) / 2.0f;
        if (cave_noise > 0.7f) {
            density = std::max(density, 2.0f * (1.0f - cave_noise));
        }
    }

    return density;
}

void SHIELD_WorldSystem::GenerateChunkData(Luminumbra::Chunk& chunk) const {
    constexpr size_t PADDED_VOLUME = (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    chunk.sdf_data.resize(PADDED_VOLUME);
    IVec3 base_pos = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);

    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                IVec3 world_pos = base_pos + IVec3(x, y, z);
                int index = x + y * (CHUNK_SIZE_X + 1) + z * (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1);
                chunk.sdf_data[index] = get_density_at(Vec3(world_pos));
            }
        }
    }
}

void SHIELD_WorldSystem::dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate) {
    std::vector<Job> jobs;
    for (const auto& coords : chunks_to_generate) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Loading);
        m_chunks[chunk->get_id()] = chunk;

        jobs.emplace_back([this, chunk]() {
            GenerateChunkData(*chunk);
            chunk->set_state(Luminumbra::ChunkState::Idle);
        });
    }
    if (m_job_system) m_job_system->dispatch_batch(jobs);
}

void SHIELD_WorldSystem::dispatch_meshing_jobs(const std::vector<std::pair<std::shared_ptr<Chunk>, int>>& chunks_to_mesh) {
    std::vector<Job> jobs;
    for (const auto& pair : chunks_to_mesh) {
        auto& chunk = pair.first;
        int lod_level = pair.second;
        int step = m_lod_levels[lod_level].step;

        chunk->set_state(ChunkState::Meshing);
        chunk->current_lod.store(lod_level);

        jobs.emplace_back([this, chunk, step]() {
            // Worker thread generates the mesh at the requested resolution
            World::MarchingCubes::PolygoniseChunk(*this, *chunk, 0.0f, step);
            
            // Increment the mesh version to signal to the renderer that the data is new
            chunk->mesh_version++; 
            
            // Invalidate old physics body (if any)
            chunk->has_collision.store(false); 
            
            // Set state to ready for rendering and collision creation on the main thread
            chunk->set_state(ChunkState::Ready);
        });
    }
    if (m_job_system) m_job_system->dispatch_batch(jobs);
}

void SHIELD_WorldSystem::set_params(const TerrainGenParams& params) {
    m_params = params;
    reinitialize_noise();
}

void SHIELD_WorldSystem::set_seed(int seed) {
    m_seed = seed;
    reinitialize_noise();
}

void SHIELD_WorldSystem::clear_world(PhysicsSystem* physics_system) {
    if (physics_system) {
        for (const auto& [id, chunk] : m_chunks) {
            physics_system->remove_chunk_collision(id);
        }
    }
    m_chunks.clear();
    LUMINUMBRA_CORE_INFO("World cleared.");
}

void SHIELD_WorldSystem::regenerate_all_chunks(PhysicsSystem* physics_system) {
    LUMINUMBRA_CORE_INFO("Regenerating all active chunks...");
    std::vector<IVec3> coords_to_regenerate;
    coords_to_regenerate.reserve(m_chunks.size());
    for(const auto& [id, chunk] : m_chunks) {
        coords_to_regenerate.push_back(chunk->get_coords());
    }
    clear_world(physics_system);
    dispatch_generation_jobs(coords_to_regenerate);
}

IVec3 SHIELD_WorldSystem::world_to_chunk_coords(const Vec3& position) {
    return IVec3(
        static_cast<int>(std::floor(position.x / CHUNK_SIZE_X)),
        static_cast<int>(std::floor(position.y / CHUNK_SIZE_Y)),
        static_cast<int>(std::floor(position.z / CHUNK_SIZE_Z))
    );
}

} // namespace Luminumbra::Systems