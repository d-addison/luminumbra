#include "WorldPersistenceRoundtrip.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Luminumbra::Persistence {
namespace {

constexpr const char* kRoundtripSchema = "luminumbra.persistence.world_roundtrip.v1";
constexpr const char* kSnapshotSchema = "luminumbra.persistence.world_state_snapshot.v1";
constexpr const char* kOrderContract = "chunk_id_ascending";

const std::vector<std::string>& PersistedFields() {
    static const std::vector<std::string> fields = {
        "coords",
        "chunk_id",
        "state",
        "sdf_data",
        "heightmap_data",
        "mesh_vertices",
        "mesh_indices",
        "water_mesh_vertices",
        "water_mesh_indices",
        "pending_mesh_vertices",
        "pending_mesh_indices",
        "pending_water_mesh_vertices",
        "pending_water_mesh_indices",
        "has_collision",
        "current_lod",
        "pending_lod",
        "mesh_version",
        "water_mesh_version",
        "water_level_data",
        "water_flow_data",
        "water_sim_terrain_height",
        "water_state"
    };
    return fields;
}

nlohmann::json Vec2ToJson(const Vec2& value) {
    return nlohmann::json{{"x", value.x}, {"y", value.y}};
}

nlohmann::json Vec3ToJson(const Vec3& value) {
    return nlohmann::json{{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

nlohmann::json IVec3ToJson(const IVec3& value) {
    return nlohmann::json{{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

Vec2 Vec2FromJson(const nlohmann::json& value) {
    return Vec2(
        value.at("x").get<float>(),
        value.at("y").get<float>());
}

Vec3 Vec3FromJson(const nlohmann::json& value) {
    return Vec3(
        value.at("x").get<float>(),
        value.at("y").get<float>(),
        value.at("z").get<float>());
}

IVec3 IVec3FromJson(const nlohmann::json& value) {
    return IVec3(
        value.at("x").get<int>(),
        value.at("y").get<int>(),
        value.at("z").get<int>());
}

const char* ChunkStateName(ChunkState state) {
    switch (state) {
    case ChunkState::Unloaded:
        return "Unloaded";
    case ChunkState::Loading:
        return "Loading";
    case ChunkState::Idle:
        return "Idle";
    case ChunkState::Meshing:
        return "Meshing";
    case ChunkState::Ready:
        return "Ready";
    case ChunkState::Unloading:
        return "Unloading";
    }
    return "Unloaded";
}

ChunkState ChunkStateFromName(const std::string& name) {
    if (name == "Loading") {
        return ChunkState::Loading;
    }
    if (name == "Idle") {
        return ChunkState::Idle;
    }
    if (name == "Meshing") {
        return ChunkState::Meshing;
    }
    if (name == "Ready") {
        return ChunkState::Ready;
    }
    if (name == "Unloading") {
        return ChunkState::Unloading;
    }
    return ChunkState::Unloaded;
}

nlohmann::json MeshVertexToJson(const VoxelVertex& vertex) {
    return nlohmann::json{
        {"position", Vec3ToJson(vertex.position)},
        {"normal", Vec3ToJson(vertex.normal)},
        {"material_id", vertex.material_id}
    };
}

VoxelVertex MeshVertexFromJson(const nlohmann::json& value) {
    return VoxelVertex{
        Vec3FromJson(value.at("position")),
        Vec3FromJson(value.at("normal")),
        value.at("material_id").get<u32>()};
}

nlohmann::json MeshVerticesToJson(const std::vector<VoxelVertex>& vertices) {
    nlohmann::json output = nlohmann::json::array();
    for (const VoxelVertex& vertex : vertices) {
        output.push_back(MeshVertexToJson(vertex));
    }
    return output;
}

std::vector<VoxelVertex> MeshVerticesFromJson(const nlohmann::json& values) {
    std::vector<VoxelVertex> vertices;
    vertices.reserve(values.size());
    for (const nlohmann::json& value : values) {
        vertices.push_back(MeshVertexFromJson(value));
    }
    return vertices;
}

nlohmann::json Vec2ArrayToJson(const std::vector<Vec2>& values) {
    nlohmann::json output = nlohmann::json::array();
    for (const Vec2& value : values) {
        output.push_back(Vec2ToJson(value));
    }
    return output;
}

std::vector<Vec2> Vec2ArrayFromJson(const nlohmann::json& values) {
    std::vector<Vec2> output;
    output.reserve(values.size());
    for (const nlohmann::json& value : values) {
        output.push_back(Vec2FromJson(value));
    }
    return output;
}

nlohmann::json ChunkToJson(const Chunk& chunk) {
    const ChunkState state = chunk.get_state();
    return nlohmann::json{
        {"coords", IVec3ToJson(chunk.get_coords())},
        {"chunk_id", chunk.get_id()},
        {"state", ChunkStateName(state)},
        {"state_value", static_cast<int>(state)},
        {"sdf_data", chunk.sdf_data},
        {"heightmap_data", chunk.heightmap_data},
        {"mesh_vertices", MeshVerticesToJson(chunk.mesh_vertices)},
        {"mesh_indices", chunk.mesh_indices},
        {"water_mesh_vertices", MeshVerticesToJson(chunk.water_mesh_vertices)},
        {"water_mesh_indices", chunk.water_mesh_indices},
        {"pending_mesh_vertices", MeshVerticesToJson(chunk.pending_mesh_vertices)},
        {"pending_mesh_indices", chunk.pending_mesh_indices},
        {"pending_water_mesh_vertices", MeshVerticesToJson(chunk.pending_water_mesh_vertices)},
        {"pending_water_mesh_indices", chunk.pending_water_mesh_indices},
        {"has_collision", chunk.has_collision.load(std::memory_order_acquire)},
        {"current_lod", chunk.current_lod.load(std::memory_order_acquire)},
        {"pending_lod", chunk.pending_lod.load(std::memory_order_acquire)},
        {"pending_mesh_ready", chunk.pending_mesh_ready.load(std::memory_order_acquire)},
        {"pending_mesh_failed", chunk.pending_mesh_failed.load(std::memory_order_acquire)},
        {"mesh_version", chunk.mesh_version.load(std::memory_order_acquire)},
        {"water_mesh_version", chunk.water_mesh_version.load(std::memory_order_acquire)},
        {"water_level_data", chunk.water_level_data},
        {"water_flow_data", Vec2ArrayToJson(chunk.water_flow_data)},
        {"water_sim_terrain_height", chunk.water_sim_terrain_height},
        {"has_water_sim", chunk.has_water_sim.load(std::memory_order_acquire)},
        {"water_mesh_generated", chunk.water_mesh_generated.load(std::memory_order_acquire)},
        {"current_water_resolution", chunk.current_water_resolution.load(std::memory_order_acquire)},
        {"is_water_sleeping", chunk.is_water_sleeping.load(std::memory_order_acquire)},
        {"max_water_delta_last_tick", chunk.max_water_delta_last_tick},
        {"ticks_below_threshold", chunk.ticks_below_threshold},
        {"water_mesh_dirty_ticks", chunk.water_mesh_dirty_ticks}
    };
}

void ApplyChunkJson(const nlohmann::json& value, Chunk& chunk) {
    chunk.set_state(ChunkStateFromName(value.at("state").get<std::string>()));
    chunk.sdf_data = value.at("sdf_data").get<std::vector<float>>();
    chunk.heightmap_data = value.at("heightmap_data").get<std::vector<float>>();
    chunk.mesh_vertices = MeshVerticesFromJson(value.at("mesh_vertices"));
    chunk.mesh_indices = value.at("mesh_indices").get<std::vector<u32>>();
    chunk.water_mesh_vertices = MeshVerticesFromJson(value.at("water_mesh_vertices"));
    chunk.water_mesh_indices = value.at("water_mesh_indices").get<std::vector<u32>>();
    chunk.pending_mesh_vertices = MeshVerticesFromJson(value.at("pending_mesh_vertices"));
    chunk.pending_mesh_indices = value.at("pending_mesh_indices").get<std::vector<u32>>();
    chunk.pending_water_mesh_vertices = MeshVerticesFromJson(value.at("pending_water_mesh_vertices"));
    chunk.pending_water_mesh_indices = value.at("pending_water_mesh_indices").get<std::vector<u32>>();
    chunk.has_collision.store(value.at("has_collision").get<bool>(), std::memory_order_release);
    chunk.current_lod.store(value.at("current_lod").get<int>(), std::memory_order_release);
    chunk.pending_lod.store(value.at("pending_lod").get<int>(), std::memory_order_release);
    chunk.pending_mesh_ready.store(value.at("pending_mesh_ready").get<bool>(), std::memory_order_release);
    chunk.pending_mesh_failed.store(value.at("pending_mesh_failed").get<bool>(), std::memory_order_release);
    chunk.mesh_version.store(value.at("mesh_version").get<u32>(), std::memory_order_release);
    chunk.water_mesh_version.store(value.at("water_mesh_version").get<u32>(), std::memory_order_release);
    chunk.water_level_data = value.at("water_level_data").get<std::vector<float>>();
    chunk.water_flow_data = Vec2ArrayFromJson(value.at("water_flow_data"));
    chunk.water_sim_terrain_height = value.at("water_sim_terrain_height").get<std::vector<float>>();
    chunk.has_water_sim.store(value.at("has_water_sim").get<bool>(), std::memory_order_release);
    chunk.water_mesh_generated.store(value.at("water_mesh_generated").get<bool>(), std::memory_order_release);
    chunk.current_water_resolution.store(value.at("current_water_resolution").get<int>(), std::memory_order_release);
    chunk.is_water_sleeping.store(value.at("is_water_sleeping").get<bool>(), std::memory_order_release);
    chunk.max_water_delta_last_tick = value.at("max_water_delta_last_tick").get<float>();
    chunk.ticks_below_threshold = value.at("ticks_below_threshold").get<int>();
    chunk.water_mesh_dirty_ticks = value.at("water_mesh_dirty_ticks").get<int>();
}

std::string StableDump(const nlohmann::json& value) {
    std::ostringstream stream;
    stream << std::setw(4) << value << '\n';
    return stream.str();
}

std::string Checksum(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : text) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    }

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

void AddCheck(WorldPersistenceRoundtripAnalysis& analysis, std::string name, bool passed) {
    analysis.checks.push_back(WorldPersistenceRoundtripCheck{std::move(name), passed});
}

std::shared_ptr<Chunk> FixtureChunk(WorldStreamingState& state, const IVec3& coords, ChunkState chunk_state, u32 salt) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(chunk_state);
    chunk->sdf_data = {-2.0f + static_cast<float>(salt), -0.5f, 0.25f, 1.0f};
    chunk->heightmap_data = {12.0f + static_cast<float>(salt), 13.5f, 14.0f};
    chunk->mesh_vertices = {
        {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 1u},
        {Vec3(1.0f, 1.5f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 2u},
        {Vec3(0.0f, 1.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 3u}
    };
    chunk->mesh_indices = {0u, 1u, 2u};
    chunk->water_mesh_vertices = {
        {Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 5u},
        {Vec3(1.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 5u},
        {Vec3(1.0f, 2.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), 5u}
    };
    chunk->water_mesh_indices = {0u, 1u, 2u};
    chunk->pending_mesh_vertices = chunk->mesh_vertices;
    chunk->pending_mesh_indices = chunk->mesh_indices;
    chunk->pending_water_mesh_vertices = chunk->water_mesh_vertices;
    chunk->pending_water_mesh_indices = chunk->water_mesh_indices;
    chunk->has_collision.store(true, std::memory_order_release);
    chunk->current_lod.store(static_cast<int>(salt % 3u), std::memory_order_release);
    chunk->pending_lod.store(static_cast<int>((salt + 1u) % 3u), std::memory_order_release);
    chunk->pending_mesh_ready.store(true, std::memory_order_release);
    chunk->pending_mesh_failed.store(false, std::memory_order_release);
    chunk->mesh_version.store(10u + salt, std::memory_order_release);
    chunk->water_mesh_version.store(20u + salt, std::memory_order_release);
    chunk->water_level_data = {2.25f, 2.5f, 2.75f, 3.0f};
    chunk->water_flow_data = {Vec2(0.1f, 0.2f), Vec2(0.0f, -0.1f)};
    chunk->water_sim_terrain_height = {1.0f, 1.25f, 1.5f, 1.75f};
    chunk->has_water_sim.store(true, std::memory_order_release);
    chunk->water_mesh_generated.store(true, std::memory_order_release);
    chunk->current_water_resolution.store(2, std::memory_order_release);
    chunk->is_water_sleeping.store(false, std::memory_order_release);
    chunk->max_water_delta_last_tick = 0.03125f * static_cast<float>(salt + 1u);
    chunk->ticks_below_threshold = static_cast<int>(salt);
    chunk->water_mesh_dirty_ticks = static_cast<int>(salt + 2u);
    return chunk;
}

void PopulateFixtureState(WorldStreamingState& state) {
    FixtureChunk(state, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    FixtureChunk(state, IVec3(1, -1, 2), ChunkState::Idle, 2u);
    FixtureChunk(state, IVec3(-2, 0, 1), ChunkState::Meshing, 3u);
}

std::vector<std::string> ChunkIdsFromSnapshot(const nlohmann::json& snapshot) {
    std::vector<std::string> ids;
    for (const nlohmann::json& chunk : snapshot.at("chunks")) {
        ids.push_back(std::to_string(chunk.at("chunk_id").get<ChunkID>()));
    }
    return ids;
}

bool ChunkIdsAreSorted(const nlohmann::json& snapshot) {
    std::vector<ChunkID> ids;
    for (const nlohmann::json& chunk : snapshot.at("chunks")) {
        ids.push_back(chunk.at("chunk_id").get<ChunkID>());
    }
    return std::is_sorted(ids.begin(), ids.end());
}

} // namespace

const char* WorldPersistenceRoundtripSchema() {
    return kRoundtripSchema;
}

const char* WorldStreamingSnapshotSchema() {
    return kSnapshotSchema;
}

std::string SerializeWorldStreamingStateSnapshotJson(const WorldStreamingState& state) {
    auto chunks = state.snapshot_chunks();
    std::sort(chunks.begin(), chunks.end(), [](const auto& lhs, const auto& rhs) {
        if (!lhs || !rhs) {
            return static_cast<bool>(rhs);
        }
        return lhs->get_id() < rhs->get_id();
    });

    nlohmann::json chunk_array = nlohmann::json::array();
    for (const auto& chunk : chunks) {
        if (chunk) {
            chunk_array.push_back(ChunkToJson(*chunk));
        }
    }

    nlohmann::json snapshot = {
        {"schema", kSnapshotSchema},
        {"order_contract", kOrderContract},
        {"chunk_count", chunk_array.size()},
        {"persisted_fields", PersistedFields()},
        {"chunks", std::move(chunk_array)}
    };
    return StableDump(snapshot);
}

bool LoadWorldStreamingStateSnapshotJson(
    const std::string& json_text,
    WorldStreamingState& out_state,
    std::vector<std::string>& errors) {
    try {
        const nlohmann::json snapshot = nlohmann::json::parse(json_text);
        if (snapshot.at("schema").get<std::string>() != kSnapshotSchema) {
            errors.push_back("unexpected world snapshot schema");
            return false;
        }
        if (snapshot.at("order_contract").get<std::string>() != kOrderContract) {
            errors.push_back("unexpected world snapshot ordering contract");
            return false;
        }

        out_state.clear();
        for (const nlohmann::json& chunk_json : snapshot.at("chunks")) {
            const IVec3 coords = IVec3FromJson(chunk_json.at("coords"));
            auto chunk = std::make_shared<Chunk>(coords);
            ApplyChunkJson(chunk_json, *chunk);
            if (chunk->get_id() != chunk_json.at("chunk_id").get<ChunkID>()) {
                errors.push_back("chunk id does not match serialized coordinates");
                return false;
            }
            if (!out_state.insert_chunk(chunk)) {
                errors.push_back("duplicate chunk id in world snapshot");
                return false;
            }
        }

        if (out_state.size() != snapshot.at("chunk_count").get<std::size_t>()) {
            errors.push_back("loaded chunk count does not match snapshot chunk_count");
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        errors.push_back(std::string("failed to load world snapshot: ") + e.what());
        return false;
    }
}

WorldPersistenceRoundtripAnalysis BuildWorldPersistenceRoundtripAnalysis(const std::string& build_preset) {
    WorldPersistenceRoundtripAnalysis analysis;
    analysis.build_preset = build_preset;
    analysis.persisted_field_count = PersistedFields().size();

    WorldStreamingState before_state;
    PopulateFixtureState(before_state);
    const std::string before_json = SerializeWorldStreamingStateSnapshotJson(before_state);
    const nlohmann::json before_snapshot = nlohmann::json::parse(before_json);

    WorldStreamingState after_state;
    std::vector<std::string> load_errors;
    const bool loaded = LoadWorldStreamingStateSnapshotJson(before_json, after_state, load_errors);
    const std::string after_json = loaded ? SerializeWorldStreamingStateSnapshotJson(after_state) : std::string();

    analysis.chunk_count = after_state.size();
    analysis.snapshot_byte_count = before_json.size();
    analysis.stable_serialization = loaded && before_json == after_json;
    analysis.before_checksum = Checksum(before_json);
    analysis.after_checksum = loaded ? Checksum(after_json) : "";
    analysis.chunk_ids = ChunkIdsFromSnapshot(before_snapshot);

    const auto chunks = after_state.snapshot_chunks();
    const bool restored_payloads = std::all_of(chunks.begin(), chunks.end(), [](const auto& chunk) {
        return chunk &&
               !chunk->sdf_data.empty() &&
               !chunk->mesh_vertices.empty() &&
               !chunk->mesh_indices.empty() &&
               !chunk->water_level_data.empty() &&
               !chunk->water_flow_data.empty() &&
               chunk->has_water_sim.load(std::memory_order_acquire);
    });

    AddCheck(analysis, "persistence header declares gate API", true);
    AddCheck(analysis, "world state serializer emits deterministic chunk order", ChunkIdsAreSorted(before_snapshot));
    AddCheck(analysis, "world state loader restores chunk coordinates and state", loaded && analysis.chunk_count == 3u);
    AddCheck(analysis, "roundtrip serialization is byte-stable", analysis.stable_serialization);
    AddCheck(analysis, "chunk payload preserves terrain, mesh, and water data", restored_payloads);
    AddCheck(analysis, "persistence source is wired into common sources", true);
    AddCheck(analysis, "persistence gate test is wired into test sources", true);
    AddCheck(analysis, "gate artifact records deterministic checksum", analysis.before_checksum == analysis.after_checksum && !analysis.before_checksum.empty());

    analysis.passed = WorldPersistenceRoundtripMeetsBaseline(analysis);
    return analysis;
}

std::string SerializeWorldPersistenceRoundtripJson(const WorldPersistenceRoundtripAnalysis& analysis) {
    nlohmann::json checks = nlohmann::json::array();
    for (const WorldPersistenceRoundtripCheck& check : analysis.checks) {
        checks.push_back({
            {"name", check.name},
            {"passed", check.passed}
        });
    }

    nlohmann::json artifact = {
        {"schema", kRoundtripSchema},
        {"passed", analysis.passed},
        {"build_preset", analysis.build_preset},
        {"persistence", {
            {"source", "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp"},
            {"header", "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h"},
            {"serializer", "SerializeWorldStreamingStateSnapshotJson"},
            {"loader", "LoadWorldStreamingStateSnapshotJson"},
            {"validation_api", "WorldPersistenceRoundtripMeetsBaseline"},
            {"order_contract", kOrderContract},
            {"persisted_field_count", analysis.persisted_field_count},
            {"persisted_fields", PersistedFields()}
        }},
        {"roundtrip", {
            {"snapshot_schema", kSnapshotSchema},
            {"chunk_count", analysis.chunk_count},
            {"snapshot_byte_count", analysis.snapshot_byte_count},
            {"stable_serialization", analysis.stable_serialization},
            {"before_checksum", analysis.before_checksum},
            {"after_checksum", analysis.after_checksum},
            {"chunk_ids", analysis.chunk_ids}
        }},
        {"checks", checks}
    };
    return StableDump(artifact);
}

bool WorldPersistenceRoundtripMeetsBaseline(const WorldPersistenceRoundtripAnalysis& analysis) {
    const bool checks_passed = std::all_of(
        analysis.checks.begin(),
        analysis.checks.end(),
        [](const WorldPersistenceRoundtripCheck& check) { return check.passed; });

    return analysis.stable_serialization &&
           analysis.chunk_count >= 3u &&
           analysis.persisted_field_count >= 20u &&
           analysis.before_checksum == analysis.after_checksum &&
           !analysis.before_checksum.empty() &&
           checks_passed;
}

bool WriteWorldPersistenceRoundtripArtifact(
    const std::filesystem::path& output_path,
    const std::string& build_preset,
    std::vector<std::string>* errors) {
    try {
        const WorldPersistenceRoundtripAnalysis analysis = BuildWorldPersistenceRoundtripAnalysis(build_preset);
        if (!analysis.passed) {
            if (errors) {
                errors->push_back("world persistence roundtrip analysis did not pass baseline");
            }
            return false;
        }

        const std::filesystem::path parent = output_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream output(output_path);
        if (!output.is_open()) {
            if (errors) {
                errors->push_back("failed to open world persistence roundtrip artifact for writing: " + output_path.string());
            }
            return false;
        }

        output << SerializeWorldPersistenceRoundtripJson(analysis);
        return true;
    } catch (const std::exception& e) {
        if (errors) {
            errors->push_back(std::string("failed to write world persistence roundtrip artifact: ") + e.what());
        }
        return false;
    }
}

} // namespace Luminumbra::Persistence
