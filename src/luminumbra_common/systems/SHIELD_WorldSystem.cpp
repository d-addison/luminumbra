#include "SHIELD_WorldSystem.h"
#include "entt/entt.hpp"
#include "FastNoiseLite.h"
#include "../world/MarchingCubes.h"
#include <cmath>

constexpr int RENDER_DISTANCE = 8;
constexpr int MAX_CHUNKS_GENERATE_PER_FRAME = 16;
constexpr int MAX_CHUNKS_MESH_PER_FRAME     = 16;

namespace Luminumbra::Systems {

SHIELD_WorldSystem::SHIELD_WorldSystem(JobSystem* job_system, const TerrainGenParams& params, int seed)
    : m_job_system(job_system), m_params(params), m_seed(seed) 
{}

void SHIELD_WorldSystem::update(entt::registry& registry, const Vec3& camera_position) {
    update_chunk_activation(camera_position);

    std::vector<Chunk*> chunks_to_mesh;
    chunks_to_mesh.reserve(64);
    for (auto const& [id, chunk_ptr] : m_chunks) {
        if (chunk_ptr->get_state() == ChunkState::Idle) {
            chunks_to_mesh.push_back(chunk_ptr.get());
            if ((int)chunks_to_mesh.size() >= MAX_CHUNKS_MESH_PER_FRAME) break;
        }
    }
    if (!chunks_to_mesh.empty()) {
        dispatch_meshing_jobs(chunks_to_mesh);
    }
}

std::vector<Chunk*> SHIELD_WorldSystem::get_renderable_chunks() {
    std::vector<Chunk*> renderable;
    renderable.reserve(m_chunks.size());
    for (auto const& [id, chunk_ptr] : m_chunks) {
        if (chunk_ptr->get_state() == ChunkState::Ready && !chunk_ptr->mesh_vertices.empty()) {
            // --- FIXED THIS LINE ---
            renderable.push_back(chunk_ptr.get());
        }
    }
    return renderable;
}

// In src/luminumbra_common/systems/SHIELD_WorldSystem.cpp

void SHIELD_WorldSystem::update_chunk_activation(const Vec3& player_pos) {
    const IVec3 camera_chunk_coords = world_to_chunk_coords(player_pos);

    // Which chunks we want active this frame
    std::unordered_set<ChunkID> required_chunks;
    required_chunks.reserve((2 * RENDER_DISTANCE + 1) * (2 * RENDER_DISTANCE + 1));

    // Budgeted list of chunks to generate this frame
    std::vector<IVec3> chunks_to_generate;
    chunks_to_generate.reserve(MAX_CHUNKS_GENERATE_PER_FRAME);

    // Build the set around the camera
    for (int dz = -RENDER_DISTANCE; dz <= RENDER_DISTANCE; ++dz) {
        for (int dx = -RENDER_DISTANCE; dx <= RENDER_DISTANCE; ++dx) {
            const IVec3 coords = camera_chunk_coords + IVec3(dx, 0, dz);
            const ChunkID id = Chunk::calculate_id(coords);
            required_chunks.insert(id);

            if (chunks_to_generate.size() < static_cast<size_t>(MAX_CHUNKS_GENERATE_PER_FRAME)) {
                if (m_chunks.find(id) == m_chunks.end()) {
                    chunks_to_generate.push_back(coords);
                }
            }
        }
    }

    // Kick off generation for the new chunks (budgeted)
    if (!chunks_to_generate.empty()) {
        dispatch_generation_jobs(chunks_to_generate);
    }

    std::vector<ChunkID> to_unload;
    to_unload.reserve(m_chunks.size());
    for (const auto& [id, chunk_ptr] : m_chunks) {
        if (required_chunks.find(id) == required_chunks.end()) {
            to_unload.push_back(id);
        }
    }
    for (ChunkID id : to_unload) {
        auto it = m_chunks.find(id);
        if (it != m_chunks.end()) {
            m_chunks.erase(it); // unique_ptr cleanup
        }
    }
}

float SHIELD_WorldSystem::get_density_at(const Vec3& world_pos) const {
    // The base density is now controlled by the preset
    float density = (world_pos.y - m_params.height_offset) * -0.1f; // A simple gradient

    // --- Fractal Noise using loaded parameters ---
    fnl_state noise = fnlCreateState();
    noise.seed = m_seed; // CRITICAL FOR DETERMINISM
    noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    
    float frequency = m_params.base_frequency;
    float amplitude = m_params.base_amplitude;
    float noise_sum = 0.0f;

    for (int i = 0; i < m_params.octaves; ++i) {
        noise_sum += fnlGetNoise3D(&noise, world_pos.x * frequency, world_pos.y * frequency, world_pos.z * frequency) * amplitude;
        frequency *= m_params.lacunarity;
        amplitude *= m_params.persistence;
    }
    density += noise_sum;

    // --- Cave Generation using loaded parameters ---
    if (m_params.caves_enabled) {
        fnl_state cave_noise_state = fnlCreateState();
        cave_noise_state.seed = m_seed + 1; // Use a different seed for caves!
        cave_noise_state.noise_type = FNL_NOISE_PERLIN;
        cave_noise_state.frequency = m_params.cave_frequency;
        
        float cave_noise = (fnlGetNoise3D(&cave_noise_state, world_pos.x, world_pos.y, world_pos.z) + 1.0f) / 2.0f;
        
        if (pow(cave_noise, 3.0) > 0.7f) {
            density += 10.0f; // Carve out material
        }
    }
    
    return density;
}

void SHIELD_WorldSystem::GenerateChunkData(Luminumbra::Chunk& chunk) const {
    // This function now just calls the new method for every point in the chunk.
    chunk.sdf_data.resize(Luminumbra::CHUNK_VOLUME);
    IVec3 base_pos = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);

    for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y < CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                IVec3 world_pos = base_pos + IVec3(x, y, z);
                int index = x + y * CHUNK_SIZE_X + z * CHUNK_SIZE_X * CHUNK_SIZE_Y;
                chunk.sdf_data[index] = get_density_at(Vec3(world_pos));
            }
        }
    }
}

void SHIELD_WorldSystem::dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate) {
    std::vector<Job> jobs;
    for (const auto& coords : chunks_to_generate) {
        auto chunk = std::make_unique<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Loading);
        
        Luminumbra::Chunk* chunk_ptr = chunk.get();
        m_chunks[chunk_ptr->get_id()] = std::move(chunk);

        jobs.emplace_back([this, chunk_ptr]() {
            GenerateChunkData(*chunk_ptr);
            chunk_ptr->set_state(Luminumbra::ChunkState::Idle);
        });
    }
    if(m_job_system) m_job_system->dispatch_batch(jobs);
}

void SHIELD_WorldSystem::dispatch_meshing_jobs(const std::vector<Chunk*>& chunks_to_mesh) {
    std::vector<Job> jobs;
    for (Chunk* chunk : chunks_to_mesh) {
        chunk->set_state(ChunkState::Meshing);
        jobs.emplace_back([chunk]() {
            World::MarchingCubes::PolygoniseChunk(*chunk, 0.0f);
            chunk->set_state(ChunkState::Ready);
        });
    }
    if(m_job_system) m_job_system->dispatch_batch(jobs);
}

IVec3 SHIELD_WorldSystem::world_to_chunk_coords(const Vec3& position) {
    return IVec3(
        static_cast<int>(std::floor(position.x / CHUNK_SIZE_X)),
        static_cast<int>(std::floor(position.y / CHUNK_SIZE_Y)),
        static_cast<int>(std::floor(position.z / CHUNK_SIZE_Z))
    );
}

} // namespace Luminumbra::Systems