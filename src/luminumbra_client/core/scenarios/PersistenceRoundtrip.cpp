#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" //  species base_color -> creature tint
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CreatureProcgen.h" //  genome -> body-proportion build
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "rendering/Camera.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"
// lockstep transport seam (engine-generic; ILockstepTransport +
// LoopbackTransport + LockstepSession). Named SendFrame/TryReceiveFrame to dodge
// the <windows.h> SendMessage macro (see LockstepSession.h note).
#include "luminumbra_common/net/LockstepSession.h"
//  (AU1): atmosphere audio telemetry. The harness sweeps the replicated
// weather/wind state through the REAL EnvironmentalAudioSystem atmosphere model +
// the AudioPropagationSystem ambience bed and emits the AtmosphereAudio artifact.
// Client-side dressing only -- no world_hash, no visual-gate dependency.
#include "audio/AudioPropagationSystem.h"
#include "audio/EnvironmentalAudioSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

namespace {

constexpr const char* kPersistencePhaseSchema = "luminumbra.persistence_runtime_roundtrip_phase.v1";
constexpr const char* kPersistenceSavePhaseArtifact =
    "persistence-runtime-roundtrip-phase-save.json";
constexpr const char* kPersistenceLoadPhaseArtifact =
    "persistence-runtime-roundtrip-phase-load.json";

struct CarveSphereSpec {
    float offset_x;
    float offset_z;
    float radius;
};

// Deterministic scripted voxel edits for the persistence runtime roundtrip:
// three carved spheres at fixed horizontal offsets from spawn, each centered
// on the terrain surface so the edit lands in that column's surface chunk.
// Spawn and terrain height are pure functions of the fixed scenario seed, so
// the same edits land in the same chunks on every save-phase run.
constexpr std::array<CarveSphereSpec, 3> kPersistenceCarveSpheres{
    {{12.0f, 12.0f, 3.5f}, {28.0f, -20.0f, 3.5f}, {-20.0f, 28.0f, 3.5f}}};

// Carves an air sphere into the chunk's signed density field. Positive
// density is air, so each in-range sample is raised to at least
// (radius - distance). Returns false when the chunk has no generated sdf.
bool CarveSphereIntoChunk(Luminumbra::Chunk& chunk, const Luminumbra::Vec3& center, float radius) {
    const int size_x = Luminumbra::CHUNK_SIZE_X + 1;
    const int size_y = Luminumbra::CHUNK_SIZE_Y + 1;
    const int size_z = Luminumbra::CHUNK_SIZE_Z + 1;
    const std::size_t expected_samples = static_cast<std::size_t>(size_x) *
                                         static_cast<std::size_t>(size_y) *
                                         static_cast<std::size_t>(size_z);
    if (chunk.sdf_data.size() != expected_samples) {
        return false;
    }

    const Luminumbra::IVec3 base = chunk.get_coords() * Luminumbra::IVec3(Luminumbra::CHUNK_SIZE_X,
                                                                          Luminumbra::CHUNK_SIZE_Y,
                                                                          Luminumbra::CHUNK_SIZE_Z);
    bool carved = false;
    for (int z = 0; z < size_z; ++z) {
        for (int y = 0; y < size_y; ++y) {
            for (int x = 0; x < size_x; ++x) {
                const Luminumbra::Vec3 world_pos(static_cast<float>(base.x + x),
                                                 static_cast<float>(base.y + y),
                                                 static_cast<float>(base.z + z));
                const float distance = glm::distance(world_pos, center);
                if (distance > radius) {
                    continue;
                }
                const float carve_density = radius - distance;
                const std::size_t index = static_cast<std::size_t>(x) +
                                          static_cast<std::size_t>(y) * size_x +
                                          static_cast<std::size_t>(z) * size_x * size_y;
                if (carve_density > chunk.sdf_data[index]) {
                    chunk.sdf_data[index] = carve_density;
                    // carving to air clears any authored structure
                    // material at this voxel so a mined structure voxel reads as
                    // plain air (sdf air + material 0), not lingering Stone.
                    if (!chunk.material_data.empty() && index < chunk.material_data.size()) {
                        chunk.material_data[index] = 0u;
                    }
                    carved = true;
                }
            }
        }
    }
    return carved;
}

bool WritePersistencePhaseArtifact(const std::filesystem::path& artifact_dir,
                                   const char* file_name,
                                   const nlohmann::json& artifact) {
    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / file_name);
    output << std::setw(2) << artifact << '\n';
    return output.good();
}

} // namespace

PersistenceRoundtripPhaseResult
RunPersistenceRoundtripSavePhase(const RuntimeScenarioConfig& config,
                                 Luminumbra::world::GameSession* game_session) {
    PersistenceRoundtripPhaseResult result;
    if (!game_session || !game_session->GetWorldSystem()) {
        result.failure_reason = "world_system_missing";
        return result;
    }
    if (!game_session->GetWorldOpenError().empty()) {
        result.failure_reason = game_session->GetWorldOpenError();
        return result;
    }
    if (config.persistence_session_dir.empty()) {
        result.failure_reason = "session_dir_missing";
        return result;
    }

    auto* world_system = game_session->GetWorldSystem();
    world_system->wait_for_streaming_jobs();

    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    std::vector<std::shared_ptr<Luminumbra::Chunk>> edited_chunks;
    for (const CarveSphereSpec& sphere : kPersistenceCarveSpheres) {
        // Anchor each carve to the column's surface chunk exactly the way the
        // streaming system selects it (terrain height sampled at the chunk
        // column center), so the target chunk is guaranteed to be streamed.
        const Luminumbra::IVec3 column =
            Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
                Luminumbra::Vec3(spawn.x + sphere.offset_x, 0.0f, spawn.z + sphere.offset_z));
        const float sample_x = static_cast<float>(column.x * Luminumbra::CHUNK_SIZE_X) +
                               Luminumbra::CHUNK_SIZE_X * 0.5f;
        const float sample_z = static_cast<float>(column.z * Luminumbra::CHUNK_SIZE_Z) +
                               Luminumbra::CHUNK_SIZE_Z * 0.5f;
        const float terrain_height = world_system->GetTerrainHeightAt(sample_x, sample_z);
        const auto chunk = world_system->find_streamed_chunk(
            Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
                Luminumbra::Vec3(sample_x, terrain_height, sample_z)));
        if (!chunk) {
            result.failure_reason = "carve_target_chunk_missing";
            return result;
        }
        // Center the sphere just below the surface and clamp it fully inside
        // the chunk so the carve always raises solid (negative) density.
        const float chunk_base_y =
            static_cast<float>(chunk->get_coords().y * Luminumbra::CHUNK_SIZE_Y);
        const float center_y =
            std::clamp(terrain_height - sphere.radius,
                       chunk_base_y + sphere.radius,
                       chunk_base_y + static_cast<float>(Luminumbra::CHUNK_SIZE_Y) - sphere.radius);
        const Luminumbra::Vec3 center(sample_x, center_y, sample_z);
        if (!CarveSphereIntoChunk(*chunk, center, sphere.radius)) {
            result.failure_reason = "carve_edit_had_no_effect";
            return result;
        }
        chunk->mark_voxel_data_dirty();
        // Invalidate the LOD so the existing surface-horizon rebuild path
        // remeshes the edited voxel data (generation is skipped for chunks
        // that already carry sdf data, so the carve survives the rebuild).
        chunk->current_lod.store(-1, std::memory_order_release);
        if (std::find(edited_chunks.begin(), edited_chunks.end(), chunk) == edited_chunks.end()) {
            edited_chunks.push_back(chunk);
        }
    }

    world_system->EnsureSurfaceReadyNear(
        spawn, game_session->GetPhysicsSystem(), config.horizon_radius, config.collision_radius);
    world_system->wait_for_streaming_jobs();

    // Hash contract: only the edited (dirty-at-save) chunks are hashed; the
    // load phase re-hashes exactly the chunk ids recorded here, so the hash
    // stays comparable regardless of how much untouched terrain streams in.
    Luminumbra::WorldStreamingState restricted;
    std::vector<Luminumbra::ChunkID> edited_chunk_ids;
    for (const auto& chunk : edited_chunks) {
        restricted.insert_chunk(chunk);
        edited_chunk_ids.push_back(chunk->get_id());
    }
    std::sort(edited_chunk_ids.begin(), edited_chunk_ids.end());

    Luminumbra::Persistence::WorldSaveService save_service;
    const std::string world_hash = save_service.world_hash(restricted);

    Luminumbra::world::WorldStateSaveReport save_report;
    if (!game_session->SaveWorldStateTo(config.persistence_session_dir, &save_report) ||
        !save_report.saved) {
        result.failure_reason = "world_state_save_failed";
        return result;
    }

    nlohmann::json chunk_id_json = nlohmann::json::array();
    for (const Luminumbra::ChunkID id : edited_chunk_ids) {
        // ChunkIDs are 64-bit; serialize as strings so JSON consumers cannot
        // lose precision.
        chunk_id_json.push_back(std::to_string(id));
    }

    const nlohmann::json artifact = {
        {"schema", kPersistencePhaseSchema},
        {"timestamp_utc", TimestampUtc()},
        {"phase", "save"},
        {"world_hash", world_hash},
        {"chunks_total", save_report.chunks_total},
        {"chunks_dirty", save_report.chunks_dirty},
        {"chunks_saved", save_report.chunks_saved},
        {"edited_chunk_ids", chunk_id_json},
        {"session_dir", config.persistence_session_dir.generic_string()},
        {"spawn", Vec3ToJson(spawn)}};
    if (!WritePersistencePhaseArtifact(
            config.artifact_dir, kPersistenceSavePhaseArtifact, artifact)) {
        result.failure_reason = "artifact_write_failed";
        return result;
    }

    LUMINUMBRA_CORE_INFO(
        "Persistence roundtrip save phase complete: hash={}, chunks_saved={}, chunks_dirty={}",
        world_hash,
        save_report.chunks_saved,
        save_report.chunks_dirty);
    result.passed = true;
    return result;
}

PersistenceRoundtripPhaseResult
RunPersistenceRoundtripLoadPhase(const RuntimeScenarioConfig& config,
                                 Luminumbra::world::GameSession* game_session) {
    PersistenceRoundtripPhaseResult result;
    if (!game_session || !game_session->GetWorldSystem()) {
        result.failure_reason = "world_system_missing";
        return result;
    }
    if (!game_session->GetWorldOpenError().empty()) {
        result.failure_reason = game_session->GetWorldOpenError();
        return result;
    }
    if (config.persistence_session_dir.empty()) {
        result.failure_reason = "session_dir_missing";
        return result;
    }

    // The runtime adopted the snapshot before chunk generation at world
    // enter (GameSession::LoadWorldStateFrom); a zero count means the
    // load-before-generate wiring is broken.
    if (game_session->GetLastLoadedChunkCount() == 0) {
        result.failure_reason = "runtime_adopted_no_chunks";
        return result;
    }

    // The save-phase artifact carries the edited chunk id list (the hash
    // restriction contract).
    nlohmann::json save_artifact;
    {
        std::ifstream input(config.artifact_dir / kPersistenceSavePhaseArtifact);
        if (!input.is_open()) {
            result.failure_reason = "save_phase_artifact_missing";
            return result;
        }
        try {
            save_artifact = nlohmann::json::parse(input);
        } catch (const std::exception&) {
            result.failure_reason = "save_phase_artifact_unreadable";
            return result;
        }
    }
    if (save_artifact.value("schema", "") != kPersistencePhaseSchema ||
        save_artifact.value("phase", "") != "save" || !save_artifact.contains("edited_chunk_ids") ||
        !save_artifact["edited_chunk_ids"].is_array() ||
        save_artifact["edited_chunk_ids"].empty()) {
        result.failure_reason = "save_phase_artifact_invalid";
        return result;
    }

    std::vector<Luminumbra::ChunkID> edited_chunk_ids;
    try {
        for (const nlohmann::json& id_json : save_artifact["edited_chunk_ids"]) {
            edited_chunk_ids.push_back(
                static_cast<Luminumbra::ChunkID>(std::stoull(id_json.get<std::string>())));
        }
    } catch (const std::exception&) {
        result.failure_reason = "save_phase_chunk_ids_invalid";
        return result;
    }

    // Hash over a fresh WorldSaveService::load_world pass (the load path under
    // test) so post-adoption remeshing in the live world cannot skew the
    // comparison; restrict to exactly the chunk ids the save phase recorded.
    Luminumbra::Persistence::WorldSaveService save_service;
    Luminumbra::WorldStreamingState loaded;
    std::vector<std::string> load_errors;
    if (!save_service.load_world(loaded, config.persistence_session_dir, load_errors)) {
        for (const std::string& error : load_errors) {
            LUMINUMBRA_CORE_ERROR("Persistence roundtrip load phase: {}", error);
        }
        result.failure_reason = "world_state_load_failed";
        return result;
    }

    Luminumbra::WorldStreamingState restricted;
    for (const Luminumbra::ChunkID id : edited_chunk_ids) {
        const auto chunk = loaded.find_chunk(id);
        if (!chunk) {
            result.failure_reason = "saved_chunk_missing_after_load";
            return result;
        }
        restricted.insert_chunk(chunk);
    }
    const std::string world_hash = save_service.world_hash(restricted);

    const nlohmann::json artifact = {
        {"schema", kPersistencePhaseSchema},
        {"timestamp_utc", TimestampUtc()},
        {"phase", "load"},
        {"world_hash", world_hash},
        {"chunks_loaded", loaded.size()},
        {"chunks_restricted", edited_chunk_ids.size()},
        {"chunks_adopted_runtime", game_session->GetLastLoadedChunkCount()},
        {"session_dir", config.persistence_session_dir.generic_string()}};
    if (!WritePersistencePhaseArtifact(
            config.artifact_dir, kPersistenceLoadPhaseArtifact, artifact)) {
        result.failure_reason = "artifact_write_failed";
        return result;
    }

    LUMINUMBRA_CORE_INFO(
        "Persistence roundtrip load phase complete: hash={}, chunks_loaded={}, adopted={}",
        world_hash,
        loaded.size(),
        game_session->GetLastLoadedChunkCount());
    result.passed = true;
    return result;
}

} // namespace Luminumbra::Client::ScenarioHarness
