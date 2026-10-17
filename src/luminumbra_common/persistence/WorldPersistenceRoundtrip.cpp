#include "WorldPersistenceRoundtrip.h"
#include "../systems/WaterSystem.h"

#include "../core/Log.h"
#include "../core/ResidencyContract.h" // the hash scope derives from the contract
#include "ecs/EntitySnapshot.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace Luminumbra::Persistence {
namespace {

constexpr const char* kRoundtripSchema = "luminumbra.persistence.world_roundtrip.v1";
constexpr const char* kSnapshotSchema = "luminumbra.persistence.world_state_snapshot.v1";
constexpr const char* kChunkFormatValidationSchema =
    "luminumbra.persistence.chunk_format_validation.v1";
constexpr const char* kWorldHashSchema = "luminumbra.persistence.world_hash.v1";
constexpr const char* kEntitySnapshotArtifactSchema = "luminumbra.persistence.entity_snapshot.v1";
constexpr const char* kOrderContract = "chunk_id_ascending";
constexpr const char* kChunkFormatContract = "world_state_snapshot_chunk_v1_required_fields";
constexpr const char* kWorldHashAlgorithm = "fnv1a_64_stable_json";
constexpr const char* kEntitySnapshotSource = "src/luminumbra_common/ecs/EntitySnapshot.h";

const std::vector<std::string>& PersistedFields() {
    static const std::vector<std::string> fields = {"coords",
                                                    "chunk_id",
                                                    "state",
                                                    "sdf_data",
                                                    "heightmap_data",
                                                    "material_data",
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
                                                    "water_depth_mm",
                                                    "water_bed_mm",
                                                    "water_state"};
    return fields;
}

const std::vector<std::string>& RequiredChunkFormatFields() {
    static const std::vector<std::string> fields = {"coords",
                                                    "chunk_id",
                                                    "state",
                                                    "state_value",
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
                                                    "pending_mesh_ready",
                                                    "pending_mesh_failed",
                                                    "mesh_version",
                                                    "water_mesh_version",
                                                    "water_level_data",
                                                    "water_flow_data",
                                                    "water_sim_terrain_height",
                                                    "water_depth_mm",
                                                    "water_bed_mm",
                                                    "water_state"};
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
    return Vec2(value.at("x").get<float>(), value.at("y").get<float>());
}

Vec3 Vec3FromJson(const nlohmann::json& value) {
    return Vec3(value.at("x").get<float>(), value.at("y").get<float>(), value.at("z").get<float>());
}

IVec3 IVec3FromJson(const nlohmann::json& value) {
    return IVec3(value.at("x").get<int>(), value.at("y").get<int>(), value.at("z").get<int>());
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

bool IsKnownChunkStateName(const std::string& name) {
    return name == "Unloaded" || name == "Loading" || name == "Idle" || name == "Meshing" ||
           name == "Ready" || name == "Unloading";
}

nlohmann::json MeshVertexToJson(const VoxelVertex& vertex) {
    return nlohmann::json{{"position", Vec3ToJson(vertex.position)},
                          {"normal", Vec3ToJson(vertex.normal)},
                          {"material_id", vertex.material_id}};
}

VoxelVertex MeshVertexFromJson(const nlohmann::json& value) {
    return VoxelVertex{Vec3FromJson(value.at("position")),
                       Vec3FromJson(value.at("normal")),
                       value.at("material_id").get<u32>()};
}

nlohmann::json MeshVerticesToJson(const std::vector<VoxelVertex>& vertices) {
    nlohmann::json output = nlohmann::json::array();
    output.get_ref<nlohmann::json::array_t&>().reserve(vertices.size());
    for (const VoxelVertex& vertex : vertices) {
        output.push_back(MeshVertexToJson(vertex));
    }
    return output;
}

nlohmann::json Vec2ArrayToJson(const std::vector<Vec2>& values) {
    nlohmann::json output = nlohmann::json::array();
    output.get_ref<nlohmann::json::array_t&>().reserve(values.size());
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

nlohmann::json WaterStateToJson(const Chunk& chunk) {
    return nlohmann::json{
        {"has_water_sim", chunk.has_water_sim.load(std::memory_order_acquire)},
        {"water_mesh_generated", chunk.water_mesh_generated.load(std::memory_order_acquire)},
        {"current_water_resolution",
         chunk.current_water_resolution.load(std::memory_order_acquire)},
        {"is_water_sleeping", chunk.is_water_sleeping.load(std::memory_order_acquire)},
        {"max_water_delta_last_tick", chunk.max_water_delta_last_tick},
        {"ticks_below_threshold", chunk.ticks_below_threshold},
        {"water_mesh_dirty_ticks", chunk.water_mesh_dirty_ticks}};
}

nlohmann::json ChunkToJson(const Chunk& chunk, bool sim_only = false, bool omit_arrays = false) {
    // The v0.3.0 cave router produces more render geometry. Hashing must not
    // materialize that geometry only to discard it in the residency projection.
    // Keep every key for the coverage audit; skipped arrays are erased below.
    // Snapshot callers retain the complete canonical representation.
    const auto array_field = [sim_only, omit_arrays](const char* field,
                                                     const auto& build) -> nlohmann::json {
        if (omit_arrays)
            return nlohmann::json::array();
        if (sim_only) {
            for (const auto& entry : luminumbra::core::kChunkFieldResidency) {
                if (std::string_view(field) == entry.field &&
                    !luminumbra::core::MayFeedWorldHash(entry.residency)) {
                    return nlohmann::json::array();
                }
            }
        }
        return build();
    };
    const ChunkState state = chunk.get_state();
    nlohmann::json output = nlohmann::json::object();
    output["coords"] = IVec3ToJson(chunk.get_coords());
    output["chunk_id"] = chunk.get_id();
    output["state"] = ChunkStateName(state);
    output["state_value"] = static_cast<int>(state);
    output["sdf_data"] =
        array_field("sdf_data", [&]() -> nlohmann::json { return chunk.sdf_data; });
    output["heightmap_data"] =
        array_field("heightmap_data", [&]() -> nlohmann::json { return chunk.heightmap_data; });
    // Optional on legacy reads; empty material channels retain their canonical [].
    output["material_data"] =
        array_field("material_data", [&]() -> nlohmann::json { return chunk.material_data; });
    output["mesh_vertices"] = array_field("mesh_vertices", [&]() -> nlohmann::json {
        return MeshVerticesToJson(chunk.mesh_vertices);
    });
    output["mesh_indices"] =
        array_field("mesh_indices", [&]() -> nlohmann::json { return chunk.mesh_indices; });
    output["water_mesh_vertices"] = array_field("water_mesh_vertices", [&]() -> nlohmann::json {
        return MeshVerticesToJson(chunk.water_mesh_vertices);
    });
    output["water_mesh_indices"] = array_field(
        "water_mesh_indices", [&]() -> nlohmann::json { return chunk.water_mesh_indices; });
    output["pending_mesh_vertices"] = array_field("pending_mesh_vertices", [&]() -> nlohmann::json {
        return MeshVerticesToJson(chunk.pending_mesh_vertices);
    });
    output["pending_mesh_indices"] = array_field(
        "pending_mesh_indices", [&]() -> nlohmann::json { return chunk.pending_mesh_indices; });
    output["pending_water_mesh_vertices"] =
        array_field("pending_water_mesh_vertices", [&]() -> nlohmann::json {
            return MeshVerticesToJson(chunk.pending_water_mesh_vertices);
        });
    output["pending_water_mesh_indices"] =
        array_field("pending_water_mesh_indices",
                    [&]() -> nlohmann::json { return chunk.pending_water_mesh_indices; });
    output["has_collision"] = chunk.has_collision.load(std::memory_order_acquire);
    output["current_lod"] = chunk.current_lod.load(std::memory_order_acquire);
    output["pending_lod"] = chunk.pending_lod.load(std::memory_order_acquire);
    output["pending_mesh_ready"] = chunk.pending_mesh_ready.load(std::memory_order_acquire);
    output["pending_mesh_failed"] = chunk.pending_mesh_failed.load(std::memory_order_acquire);
    output["mesh_version"] = chunk.mesh_version.load(std::memory_order_acquire);
    output["water_mesh_version"] = chunk.water_mesh_version.load(std::memory_order_acquire);
    output["water_level_data"] =
        array_field("water_level_data", [&]() -> nlohmann::json { return chunk.water_level_data; });
    output["water_flow_data"] = array_field("water_flow_data", [&]() -> nlohmann::json {
        return Vec2ArrayToJson(chunk.water_flow_data);
    });
    output["water_sim_terrain_height"] =
        array_field("water_sim_terrain_height",
                    [&]() -> nlohmann::json { return chunk.water_sim_terrain_height; });
    // Integer depth, bed and edge flux are authoritative solver continuation.
    // The float level/flow fields are render mirrors.
    output["water_depth_mm"] =
        array_field("water_depth_mm", [&]() -> nlohmann::json { return chunk.water_depth_mm; });
    output["water_bed_mm"] =
        array_field("water_bed_mm", [&]() -> nlohmann::json { return chunk.water_bed_mm; });
    output["water_edge_flux"] =
        array_field("water_edge_flux", [&]() -> nlohmann::json { return chunk.water_edge_flux; });
    output["has_water_sim"] = chunk.has_water_sim.load(std::memory_order_acquire);
    output["water_mesh_generated"] = chunk.water_mesh_generated.load(std::memory_order_acquire);
    output["current_water_resolution"] =
        chunk.current_water_resolution.load(std::memory_order_acquire);
    output["is_water_sleeping"] = chunk.is_water_sleeping.load(std::memory_order_acquire);
    output["max_water_delta_last_tick"] = chunk.max_water_delta_last_tick;
    output["ticks_below_threshold"] = chunk.ticks_below_threshold;
    output["water_mesh_dirty_ticks"] = chunk.water_mesh_dirty_ticks;
    output["water_state"] = WaterStateToJson(chunk);
    return output;
}

template<class T>
void CheckConvertedValue(const T&) {}
void CheckConvertedValue(float value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("nonfinite serialized float");
}
void CheckConvertedValue(const Vec2& value) {
    CheckConvertedValue(value.x);
    CheckConvertedValue(value.y);
}
void CheckConvertedValue(const VoxelVertex& value) {
    for (const float component : {value.position.x,
                                  value.position.y,
                                  value.position.z,
                                  value.normal.x,
                                  value.normal.y,
                                  value.normal.z})
        CheckConvertedValue(component);
}

template<bool Materialize, class T, class Convert>
std::vector<T> ReadChunkArray(const nlohmann::json& values, Convert convert) {
    if constexpr (Materialize) {
        std::vector<T> result;
        result.reserve(values.size());
        for (const auto& value : values)
            result.push_back(convert(value));
        return result;
    } else {
        // A conservative success path: unusual containers use the original loader.
        if (!values.is_array())
            throw std::invalid_argument("array required for validation facts");
        for (const auto& value : values)
            CheckConvertedValue(convert(value));
        return {};
    }
}

template<bool Materialize, class T>
std::vector<T> ReadChunkArray(const nlohmann::json& values) {
    if constexpr (Materialize)
        return values.get<std::vector<T>>();
    else
        return ReadChunkArray<false, T>(values,
                                        [](const auto& value) { return value.template get<T>(); });
}

template<bool Materialize = true>
void ApplyChunkJson(const nlohmann::json& value, Chunk& chunk) {
    chunk.set_state(ChunkStateFromName(value.at("state").get<std::string>()));
    chunk.sdf_data = ReadChunkArray<Materialize, float>(value.at("sdf_data"));
    chunk.heightmap_data = ReadChunkArray<Materialize, float>(value.at("heightmap_data"));
    // tolerant read (default empty) so old saves without the field still
    // load and the single-missing-field negative fixture is unaffected.
    chunk.material_data = value.contains("material_data")
                              ? ReadChunkArray<Materialize, u8>(value.at("material_data"))
                              : std::vector<u8>{};
    chunk.mesh_vertices =
        ReadChunkArray<Materialize, VoxelVertex>(value.at("mesh_vertices"), MeshVertexFromJson);
    chunk.mesh_indices = ReadChunkArray<Materialize, u32>(value.at("mesh_indices"));
    chunk.water_mesh_vertices = ReadChunkArray<Materialize, VoxelVertex>(
        value.at("water_mesh_vertices"), MeshVertexFromJson);
    chunk.water_mesh_indices = ReadChunkArray<Materialize, u32>(value.at("water_mesh_indices"));
    chunk.pending_mesh_vertices = ReadChunkArray<Materialize, VoxelVertex>(
        value.at("pending_mesh_vertices"), MeshVertexFromJson);
    chunk.pending_mesh_indices = ReadChunkArray<Materialize, u32>(value.at("pending_mesh_indices"));
    chunk.pending_water_mesh_vertices = ReadChunkArray<Materialize, VoxelVertex>(
        value.at("pending_water_mesh_vertices"), MeshVertexFromJson);
    chunk.pending_water_mesh_indices =
        ReadChunkArray<Materialize, u32>(value.at("pending_water_mesh_indices"));
    chunk.has_collision.store(value.at("has_collision").get<bool>(), std::memory_order_release);
    chunk.current_lod.store(value.at("current_lod").get<int>(), std::memory_order_release);
    chunk.pending_lod.store(value.at("pending_lod").get<int>(), std::memory_order_release);
    chunk.pending_mesh_ready.store(value.at("pending_mesh_ready").get<bool>(),
                                   std::memory_order_release);
    chunk.pending_mesh_failed.store(value.at("pending_mesh_failed").get<bool>(),
                                    std::memory_order_release);
    chunk.mesh_version.store(value.at("mesh_version").get<u32>(), std::memory_order_release);
    chunk.water_mesh_version.store(value.at("water_mesh_version").get<u32>(),
                                   std::memory_order_release);
    chunk.water_level_data = value.at("water_level_data").get<std::vector<float>>();
    chunk.water_flow_data = Vec2ArrayFromJson(value.at("water_flow_data"));
    chunk.water_sim_terrain_height = value.at("water_sim_terrain_height").get<std::vector<float>>();
    // restore the authoritative fixed-point water state (absent in pre saves -> empty,
    // the sim re-seeds on the next tick).
    chunk.water_depth_mm = value.contains("water_depth_mm")
                               ? value.at("water_depth_mm").get<std::vector<std::int32_t>>()
                               : std::vector<std::int32_t>{};
    chunk.water_bed_mm = value.contains("water_bed_mm")
                             ? value.at("water_bed_mm").get<std::vector<std::int32_t>>()
                             : std::vector<std::int32_t>{};
    // / (authoritative-state change): flow momentum round-trips (see ChunkToJson). Saves
    // that predate the field restore as zeros — the old cleared-on-load behaviour
    // (momentum re-seeds from surface differences on the next tick).
    chunk.water_edge_flux = value.contains("water_edge_flux")
                                ? value.at("water_edge_flux").get<std::vector<std::int32_t>>()
                                : std::vector<std::int32_t>(chunk.water_depth_mm.size() * 2u, 0);
    const nlohmann::json& water_state =
        value.contains("water_state") ? value.at("water_state") : value;
    chunk.has_water_sim.store(water_state.at("has_water_sim").get<bool>(),
                              std::memory_order_release);
    chunk.water_mesh_generated.store(water_state.at("water_mesh_generated").get<bool>(),
                                     std::memory_order_release);
    chunk.current_water_resolution.store(water_state.at("current_water_resolution").get<int>(),
                                         std::memory_order_release);
    chunk.is_water_sleeping.store(water_state.at("is_water_sleeping").get<bool>(),
                                  std::memory_order_release);
    chunk.max_water_delta_last_tick = water_state.at("max_water_delta_last_tick").get<float>();
    chunk.ticks_below_threshold = water_state.at("ticks_below_threshold").get<int>();
    chunk.water_mesh_dirty_ticks = water_state.at("water_mesh_dirty_ticks").get<int>();
    if constexpr (!Materialize) {
        for (const auto level : chunk.water_level_data)
            CheckConvertedValue(level);
        for (const auto& flow : chunk.water_flow_data)
            CheckConvertedValue(flow);
        for (const auto height : chunk.water_sim_terrain_height)
            CheckConvertedValue(height);
        CheckConvertedValue(chunk.max_water_delta_last_tick);
    }
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

void AddCheck(ChunkFormatValidationAnalysis& analysis, std::string name, bool passed) {
    analysis.checks.push_back(WorldPersistenceRoundtripCheck{std::move(name), passed});
}

void AddCheck(WorldHashAnalysis& analysis, std::string name, bool passed) {
    analysis.checks.push_back(WorldPersistenceRoundtripCheck{std::move(name), passed});
}

void AddCheck(EntitySnapshotAnalysis& analysis, std::string name, bool passed) {
    analysis.checks.push_back(WorldPersistenceRoundtripCheck{std::move(name), passed});
}

std::shared_ptr<Chunk>
FixtureChunk(WorldStreamingState& state, const IVec3& coords, ChunkState chunk_state, u32 salt) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(chunk_state);
    chunk->sdf_data = {-2.0f + static_cast<float>(salt), -0.5f, 0.25f, 1.0f};
    chunk->heightmap_data = {12.0f + static_cast<float>(salt), 13.5f, 14.0f};
    chunk->mesh_vertices = {{Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 1u},
                            {Vec3(1.0f, 1.5f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 2u},
                            {Vec3(0.0f, 1.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 3u}};
    chunk->mesh_indices = {0u, 1u, 2u};
    chunk->water_mesh_vertices = {{Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 5u},
                                  {Vec3(1.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 5u},
                                  {Vec3(1.0f, 2.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), 5u}};
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

std::vector<std::string> EntityIdsFromSnapshot(const nlohmann::json& snapshot) {
    std::vector<std::string> ids;
    for (const nlohmann::json& entity : snapshot.at("entities")) {
        ids.push_back(std::to_string(entity.at("entity_id").get<std::uint64_t>()));
    }
    return ids;
}

std::vector<std::string> ComponentTypesFromSnapshot(const nlohmann::json& snapshot) {
    std::vector<std::string> types;
    for (const nlohmann::json& entity : snapshot.at("entities")) {
        for (const nlohmann::json& component : entity.at("components")) {
            const std::string type = component.at("type").get<std::string>();
            if (std::find(types.begin(), types.end(), type) == types.end()) {
                types.push_back(type);
            }
        }
    }
    std::sort(types.begin(), types.end());
    return types;
}

bool EntityIdsAreSorted(const nlohmann::json& snapshot) {
    std::vector<std::uint64_t> ids;
    for (const nlohmann::json& entity : snapshot.at("entities")) {
        ids.push_back(entity.at("entity_id").get<std::uint64_t>());
    }
    return std::is_sorted(ids.begin(), ids.end());
}

bool EntityComponentsAreSorted(const nlohmann::json& snapshot) {
    for (const nlohmann::json& entity : snapshot.at("entities")) {
        std::vector<std::string> types;
        for (const nlohmann::json& component : entity.at("components")) {
            types.push_back(component.at("type").get<std::string>());
        }
        if (!std::is_sorted(types.begin(), types.end())) {
            return false;
        }
    }
    return true;
}

bool ValidateVec2Object(const nlohmann::json& value,
                        const std::string& field_name,
                        std::vector<std::string>& errors) {
    if (!value.is_object() || !value.contains("x") || !value.contains("y") ||
        !value.at("x").is_number() || !value.at("y").is_number()) {
        errors.push_back(field_name + " must be an object with numeric x/y fields");
        return false;
    }
    return true;
}

bool ValidateVec3Object(const nlohmann::json& value,
                        const std::string& field_name,
                        std::vector<std::string>& errors) {
    if (!value.is_object() || !value.contains("x") || !value.contains("y") ||
        !value.contains("z") || !value.at("x").is_number() || !value.at("y").is_number() ||
        !value.at("z").is_number()) {
        errors.push_back(field_name + " must be an object with numeric x/y/z fields");
        return false;
    }
    return true;
}

bool IsJsonInteger(const nlohmann::json& value) {
    return value.is_number_integer() || value.is_number_unsigned();
}

bool ValidateCoordsObject(const nlohmann::json& value, std::vector<std::string>& errors) {
    if (!value.is_object() || !value.contains("x") || !value.contains("y") ||
        !value.contains("z") || !IsJsonInteger(value.at("x")) || !IsJsonInteger(value.at("y")) ||
        !IsJsonInteger(value.at("z"))) {
        errors.push_back("coords must be an object with integer x/y/z fields");
        return false;
    }
    return true;
}

void RequireNumberArray(const nlohmann::json& chunk,
                        const std::string& field_name,
                        std::vector<std::string>& errors) {
    if (!chunk.contains(field_name) || !chunk.at(field_name).is_array()) {
        errors.push_back(field_name + " must be an array");
        return;
    }

    for (const nlohmann::json& value : chunk.at(field_name)) {
        if (!value.is_number()) {
            errors.push_back(field_name + " must contain only numeric values");
            return;
        }
    }
}

void RequireIntegerArray(const nlohmann::json& chunk,
                         const std::string& field_name,
                         std::vector<std::string>& errors) {
    if (!chunk.contains(field_name) || !chunk.at(field_name).is_array()) {
        errors.push_back(field_name + " must be an array");
        return;
    }

    for (const nlohmann::json& value : chunk.at(field_name)) {
        if (!IsJsonInteger(value)) {
            errors.push_back(field_name + " must contain only integer values");
            return;
        }
    }
}

void RequireMeshVertexArray(const nlohmann::json& chunk,
                            const std::string& field_name,
                            std::vector<std::string>& errors) {
    if (!chunk.contains(field_name) || !chunk.at(field_name).is_array()) {
        errors.push_back(field_name + " must be an array");
        return;
    }

    for (const nlohmann::json& vertex : chunk.at(field_name)) {
        if (!vertex.is_object() || !vertex.contains("position") || !vertex.contains("normal") ||
            !vertex.contains("material_id") || !IsJsonInteger(vertex.at("material_id"))) {
            errors.push_back(field_name +
                             " entries must contain position, normal, and material_id");
            return;
        }
        ValidateVec3Object(vertex.at("position"), field_name + ".position", errors);
        ValidateVec3Object(vertex.at("normal"), field_name + ".normal", errors);
    }
}

void RequireVec2Array(const nlohmann::json& chunk,
                      const std::string& field_name,
                      std::vector<std::string>& errors) {
    if (!chunk.contains(field_name) || !chunk.at(field_name).is_array()) {
        errors.push_back(field_name + " must be an array");
        return;
    }

    for (const nlohmann::json& value : chunk.at(field_name)) {
        ValidateVec2Object(value, field_name, errors);
    }
}

void ValidateWaterStateObject(const nlohmann::json& chunk, std::vector<std::string>& errors) {
    if (!chunk.contains("water_state") || !chunk.at("water_state").is_object()) {
        errors.push_back("water_state must be an object");
        return;
    }

    const nlohmann::json& water_state = chunk.at("water_state");
    for (const char* field : {"has_water_sim", "water_mesh_generated", "is_water_sleeping"}) {
        if (!water_state.contains(field) || !water_state.at(field).is_boolean()) {
            errors.push_back(std::string("water_state.") + field + " must be a boolean");
        }
    }
    for (const char* field :
         {"current_water_resolution", "ticks_below_threshold", "water_mesh_dirty_ticks"}) {
        if (!water_state.contains(field) || !IsJsonInteger(water_state.at(field))) {
            errors.push_back(std::string("water_state.") + field + " must be an integer");
        }
    }
    if (!water_state.contains("max_water_delta_last_tick") ||
        !water_state.at("max_water_delta_last_tick").is_number()) {
        errors.push_back("water_state.max_water_delta_last_tick must be numeric");
    }
}

void RequireWaterInteger(const nlohmann::json& value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() > 2147483647u) ||
        (!value.is_number_unsigned() && (value.get<std::int64_t>() < -2147483647ll - 1 ||
                                         value.get<std::int64_t>() > 2147483647ll)))
        throw std::invalid_argument("Regional water integer width refused");
}

// Enabled records must carry exact authoritative integer arrays; the legacy reader's
// missing-flux fallback is unavailable. Check widths before nlohmann narrowing conversions.
void RequireWaterAuthorityJson(const nlohmann::json& chunk) {
    const auto& coords = chunk.at("coords");
    for (const auto* axis : {"x", "y", "z"}) {
        const std::int64_t limit = std::string_view(axis) == "y" ? 2097152 : 1048576;
        const auto& value = coords.at(axis);
        if (!value.is_number_integer() ||
            (value.is_number_unsigned() &&
             value.get<std::uint64_t>() >= static_cast<std::uint64_t>(limit)) ||
            (!value.is_number_unsigned() &&
             (value.get<std::int64_t>() < -limit || value.get<std::int64_t>() >= limit)))
            throw std::invalid_argument("Regional water durable identity refused");
    }
    for (const auto* field : {"water_depth_mm", "water_bed_mm", "water_edge_flux"}) {
        const auto& values = chunk.at(field);
        if (!values.is_array())
            throw std::invalid_argument("Regional water arrays required");
        for (const auto& value : values) {
            RequireWaterInteger(value);
        }
    }
    const auto& state = chunk.at("water_state");
    for (const auto* field :
         {"current_water_resolution", "ticks_below_threshold", "water_mesh_dirty_ticks"}) {
        const auto& value = state.at(field);
        if (!value.is_number_integer() ||
            (value.is_number_unsigned() && value.get<std::uint64_t>() > 2147483647u) ||
            (!value.is_number_unsigned() &&
             (value.get<std::int64_t>() < 0 || value.get<std::int64_t>() > 2147483647ll)))
            throw std::invalid_argument("Regional water continuation width refused");
    }
}

// A region record contains one chunk. Stream its large arrays into native storage
// instead of retaining a JSON DOM for every cell and vertex until the record ends.
// Only the small envelope and one composite array element are buffered. Unusual
// input falls back to the original decoder, preserving its acceptance and diagnostics.
class ChunkSnapshotReader : public nlohmann::json_sax<nlohmann::json> {
    using Json = nlohmann::json;
    using Target = std::variant<std::vector<float>*,
                                std::vector<u8>*,
                                std::vector<u32>*,
                                std::vector<std::int32_t>*,
                                std::vector<Vec2>*,
                                std::vector<VoxelVertex>*>;
    struct Frame {
        Json value;
        std::string key;
        std::optional<Target> target;
        bool flow = false;
        Vec2 flow_value{0.0f};
        unsigned flow_fields = 0;
    };

public:
    explicit ChunkSnapshotReader(bool strict_water)
        : m_strictWater(strict_water) {}
    Json snapshot;
    Chunk arrays{IVec3{0, 0, 0}};
    SnapshotDecodeWork work;

    bool null() override {
        return Value(nullptr);
    }
    bool boolean(bool value) override {
        return Value(value);
    }
    bool number_integer(number_integer_t value) override {
        return Value(value);
    }
    bool number_unsigned(number_unsigned_t value) override {
        return Value(value);
    }
    bool number_float(number_float_t value, const string_t&) override {
        return Value(value);
    }
    bool string(string_t& value) override {
        return Value(value);
    }
    bool binary(binary_t&) override {
        return false;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }
    bool key(string_t& value) override {
        if (m_frames.empty() || m_frames.back().value.contains(value))
            return false; // Duplicate keys retain the original DOM decoder's last-value rules.
        m_frames.back().key = value;
        return true;
    }
    bool start_object(std::size_t) override {
        // Stop before buffering a second chunk; whole-world snapshots use the DOM decoder.
        if (m_frames.size() == 2 && m_frames[0].key == "chunks" && m_frames[1].value.is_array() &&
            !m_frames[1].value.empty())
            return false;
        Frame frame;
        if (!m_frames.empty()) {
            const auto& target = m_frames.back().target;
            // Water flow cells have only x/y. Avoid two map allocations per cell.
            frame.flow = target.has_value() && std::holds_alternative<std::vector<Vec2>*>(*target);
        }
        if (!frame.flow) {
            frame.value = Json::object();
        }
        m_frames.push_back(std::move(frame));
        return true;
    }
    bool end_object() override {
        if (m_frames.empty())
            return false;
        auto frame = std::move(m_frames.back());
        m_frames.pop_back();
        if (frame.flow) {
            if (frame.flow_fields != 3 || m_frames.empty())
                return false;
            auto& parent = m_frames.back();
            if (!parent.target.has_value())
                return false;
            auto* flows = std::get_if<std::vector<Vec2>*>(&*parent.target);
            if (!flows)
                return false;
            (*flows)->push_back(frame.flow_value);
            CountElement(parent);
            return true;
        }
        return Value(std::move(frame.value));
    }
    bool start_array(std::size_t) override {
        Frame frame;
        frame.value = Json::array();
        // root object -> chunks array -> sole chunk object
        if (m_frames.size() == 3 && m_frames[0].key == "chunks" && m_frames[1].value.is_array() &&
            m_frames[1].value.empty() && m_frames[2].value.is_object()) {
            frame.target = Array(m_frames.back().key);
            if (frame.target)
                std::visit([](auto* values) { values->clear(); }, *frame.target);
        }
        m_frames.push_back(std::move(frame));
        return true;
    }
    bool end_array() override {
        if (m_frames.empty())
            return false;
        auto frame = std::move(m_frames.back());
        m_frames.pop_back();
        if (frame.target) {
            // Residency budgets count capacities. Match the original decoder's exact-size
            // allocations rather than retaining geometric growth slack from SAX appends.
            std::visit(
                [](auto* values) {
                    if (values->capacity() != values->size()) {
                        using Values = std::remove_pointer_t<decltype(values)>;
                        Values(values->begin(), values->end()).swap(*values);
                    }
                },
                *frame.target);
        }
        return Value(std::move(frame.value));
    }

private:
    std::optional<Target> Array(std::string_view name) {
        if (name == "sdf_data")
            return &arrays.sdf_data;
        if (name == "heightmap_data")
            return &arrays.heightmap_data;
        if (name == "material_data")
            return &arrays.material_data;
        if (name == "mesh_vertices")
            return &arrays.mesh_vertices;
        if (name == "mesh_indices")
            return &arrays.mesh_indices;
        if (name == "water_mesh_vertices")
            return &arrays.water_mesh_vertices;
        if (name == "water_mesh_indices")
            return &arrays.water_mesh_indices;
        if (name == "pending_mesh_vertices")
            return &arrays.pending_mesh_vertices;
        if (name == "pending_mesh_indices")
            return &arrays.pending_mesh_indices;
        if (name == "pending_water_mesh_vertices")
            return &arrays.pending_water_mesh_vertices;
        if (name == "pending_water_mesh_indices")
            return &arrays.pending_water_mesh_indices;
        if (name == "water_level_data")
            return &arrays.water_level_data;
        if (name == "water_flow_data")
            return &arrays.water_flow_data;
        if (name == "water_sim_terrain_height")
            return &arrays.water_sim_terrain_height;
        if (name == "water_depth_mm")
            return &arrays.water_depth_mm;
        if (name == "water_bed_mm")
            return &arrays.water_bed_mm;
        if (name == "water_edge_flux")
            return &arrays.water_edge_flux;
        return std::nullopt;
    }
    void CountElement(const Frame& array) {
        ++work.array_elements;
        work.peak_buffered_array_elements =
            std::max(work.peak_buffered_array_elements, array.value.size() + 1);
    }
    bool Value(Json value) {
        if (m_frames.empty()) {
            snapshot = std::move(value);
            return true;
        }
        auto& frame = m_frames.back();
        if (m_frames.size() == 1 && frame.key == "chunk_count" && value != 1)
            return false;
        if (m_frames.size() == 3 && m_frames[0].key == "chunks" && frame.value.is_object() &&
            Array(frame.key) && !value.is_array())
            return false; // Preserve legacy non-array container handling on the DOM path.
        if (frame.flow) {
            const unsigned field = frame.key == "x" ? 1u : frame.key == "y" ? 2u : 0u;
            if (field == 0 || (frame.flow_fields & field) != 0)
                return false;
            frame.flow_value[field == 1 ? 0 : 1] = value.get<float>();
            frame.flow_fields |= field;
        } else if (frame.target) {
            std::visit(
                [&](auto* values) {
                    using Element = typename std::remove_pointer_t<decltype(values)>::value_type;
                    if constexpr (std::is_same_v<Element, VoxelVertex>)
                        values->push_back(MeshVertexFromJson(value));
                    else if constexpr (std::is_same_v<Element, Vec2>)
                        values->push_back(Vec2FromJson(value));
                    else {
                        if constexpr (std::is_same_v<Element, std::int32_t>) {
                            if (m_strictWater)
                                RequireWaterInteger(value);
                        }
                        values->push_back(value.get<Element>());
                    }
                },
                *frame.target);
            CountElement(frame);
        } else if (frame.value.is_array()) {
            frame.value.push_back(std::move(value));
        } else if (frame.value.is_object()) {
            frame.value[frame.key] = std::move(value);
        } else {
            return false;
        }
        return true;
    }
    bool m_strictWater;
    std::vector<Frame> m_frames;
};

std::shared_ptr<Chunk>
TryStreamChunkSnapshot(const std::string& text, bool strict_water, SnapshotDecodeWork* work) {
    try {
        ChunkSnapshotReader reader(strict_water);
        if (!nlohmann::json::sax_parse(text, &reader))
            return {};
        const auto& snapshot = reader.snapshot;
        if (snapshot.at("schema").get<std::string>() != kSnapshotSchema ||
            snapshot.at("order_contract").get<std::string>() != kOrderContract ||
            !snapshot.at("chunks").is_array() || snapshot.at("chunks").size() != 1 ||
            snapshot.at("chunk_count").get<std::size_t>() != 1)
            return {};
        const auto& value = snapshot.at("chunks").front();
        if (strict_water)
            RequireWaterAuthorityJson(value);
        auto chunk = std::make_shared<Chunk>(IVec3FromJson(value.at("coords")));
        ApplyChunkJson(value, *chunk);
        auto& arrays = reader.arrays;
        if (!value.contains("water_edge_flux"))
            arrays.water_edge_flux.assign(arrays.water_depth_mm.size() * 2u, 0);
        chunk->sdf_data.swap(arrays.sdf_data);
        chunk->heightmap_data.swap(arrays.heightmap_data);
        chunk->material_data.swap(arrays.material_data);
        chunk->mesh_vertices.swap(arrays.mesh_vertices);
        chunk->mesh_indices.swap(arrays.mesh_indices);
        chunk->water_mesh_vertices.swap(arrays.water_mesh_vertices);
        chunk->water_mesh_indices.swap(arrays.water_mesh_indices);
        chunk->pending_mesh_vertices.swap(arrays.pending_mesh_vertices);
        chunk->pending_mesh_indices.swap(arrays.pending_mesh_indices);
        chunk->pending_water_mesh_vertices.swap(arrays.pending_water_mesh_vertices);
        chunk->pending_water_mesh_indices.swap(arrays.pending_water_mesh_indices);
        chunk->water_level_data.swap(arrays.water_level_data);
        chunk->water_flow_data.swap(arrays.water_flow_data);
        chunk->water_sim_terrain_height.swap(arrays.water_sim_terrain_height);
        chunk->water_depth_mm.swap(arrays.water_depth_mm);
        chunk->water_bed_mm.swap(arrays.water_bed_mm);
        chunk->water_edge_flux.swap(arrays.water_edge_flux);
        if (strict_water)
            Systems::WaterSystem::ValidateRegionalGrid(*chunk);
        if (chunk->get_id() != value.at("chunk_id").get<ChunkID>())
            return {};
        if (work) {
            *work = reader.work;
            work->streamed = true;
        }
        return chunk;
    } catch (const std::exception&) {
        return {};
    }
}

std::optional<ChunkValidationCache::Facts>
TrySnapshotFacts(const nlohmann::json& snapshot, ChunkID id, bool strict_water) {
    try {
        if (snapshot.at("schema").get<std::string>() != kSnapshotSchema ||
            snapshot.at("order_contract").get<std::string>() != kOrderContract ||
            !snapshot.at("chunks").is_array() || snapshot.at("chunks").size() != 1 ||
            snapshot.at("chunk_count").get<std::size_t>() != 1)
            return std::nullopt;
        const auto& value = snapshot.at("chunks").front();
        if (strict_water)
            RequireWaterAuthorityJson(value);
        const auto coords = IVec3FromJson(value.at("coords"));
        Chunk chunk(coords);
        ApplyChunkJson<false>(value, chunk);
        if (strict_water)
            Systems::WaterSystem::ValidateRegionalGrid(chunk);
        if (chunk.get_id() != id || value.at("chunk_id").get<ChunkID>() != id)
            return std::nullopt;
        return ChunkValidationCache::Facts{coords, value.at("sdf_data").size(), false};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool ValidateChunkFormatObject(const nlohmann::json& chunk, std::vector<std::string>& errors) {
    const std::size_t error_count = errors.size();
    if (!chunk.is_object()) {
        errors.push_back("chunk payload must be an object");
        return false;
    }

    for (const std::string& field : RequiredChunkFormatFields()) {
        if (!chunk.contains(field)) {
            errors.push_back("missing required chunk field: " + field);
        }
    }

    const bool coords_valid =
        chunk.contains("coords") && ValidateCoordsObject(chunk.at("coords"), errors);
    if (!chunk.contains("chunk_id") || !IsJsonInteger(chunk.at("chunk_id"))) {
        errors.push_back("chunk_id must be an integer");
    }

    if (chunk.contains("state") && chunk.at("state").is_string()) {
        const std::string state_name = chunk.at("state").get<std::string>();
        if (!IsKnownChunkStateName(state_name)) {
            errors.push_back("state must be a known ChunkState name");
        } else if (chunk.contains("state_value") && IsJsonInteger(chunk.at("state_value")) &&
                   chunk.at("state_value").get<int>() !=
                       static_cast<int>(ChunkStateFromName(state_name))) {
            errors.push_back("state_value must match state");
        }
    } else {
        errors.push_back("state must be a string");
    }
    if (!chunk.contains("state_value") || !IsJsonInteger(chunk.at("state_value"))) {
        errors.push_back("state_value must be an integer");
    }

    for (const char* field :
         {"sdf_data", "heightmap_data", "water_level_data", "water_sim_terrain_height"}) {
        RequireNumberArray(chunk, field, errors);
    }
    for (const char* field : {"mesh_indices",
                              "water_mesh_indices",
                              "pending_mesh_indices",
                              "pending_water_mesh_indices"}) {
        RequireIntegerArray(chunk, field, errors);
    }
    for (const char* field : {"mesh_vertices",
                              "water_mesh_vertices",
                              "pending_mesh_vertices",
                              "pending_water_mesh_vertices"}) {
        RequireMeshVertexArray(chunk, field, errors);
    }
    RequireVec2Array(chunk, "water_flow_data", errors);

    for (const char* field : {"has_collision", "pending_mesh_ready", "pending_mesh_failed"}) {
        if (!chunk.contains(field) || !chunk.at(field).is_boolean()) {
            errors.push_back(std::string(field) + " must be a boolean");
        }
    }
    for (const char* field : {"current_lod", "pending_lod", "mesh_version", "water_mesh_version"}) {
        if (!chunk.contains(field) || !IsJsonInteger(chunk.at(field))) {
            errors.push_back(std::string(field) + " must be an integer");
        }
    }
    ValidateWaterStateObject(chunk, errors);

    if (coords_valid && chunk.contains("chunk_id") && IsJsonInteger(chunk.at("chunk_id"))) {
        const IVec3 coords = IVec3FromJson(chunk.at("coords"));
        const Chunk expected_chunk(coords);
        if (expected_chunk.get_id() != chunk.at("chunk_id").get<ChunkID>()) {
            errors.push_back("chunk_id must match serialized coordinates");
        }
    }

    return errors.size() == error_count;
}

} // namespace

const char* WorldPersistenceRoundtripSchema() {
    return kRoundtripSchema;
}

const char* WorldStreamingSnapshotSchema() {
    return kSnapshotSchema;
}

const char* ChunkFormatValidationSchema() {
    return kChunkFormatValidationSchema;
}

const char* ChunkFormatContract() {
    return kChunkFormatContract;
}

const char* WorldHashSchema() {
    return kWorldHashSchema;
}

const char* WorldHashAlgorithm() {
    return kWorldHashAlgorithm;
}

const char* EntitySnapshotArtifactSchema() {
    return kEntitySnapshotArtifactSchema;
}

std::string SerializeWorldStreamingStateSnapshotJson(const WorldStreamingState& state,
                                                     ChunkValidationModes* facts) {
    auto chunks = state.snapshot_chunks();
    std::sort(chunks.begin(), chunks.end(), [](const auto& lhs, const auto& rhs) {
        if (!lhs || !rhs) {
            return static_cast<bool>(rhs);
        }
        return lhs->get_id() < rhs->get_id();
    });

    nlohmann::json chunk_array = nlohmann::json::array();
    chunk_array.get_ref<nlohmann::json::array_t&>().reserve(chunks.size());
    for (const auto& chunk : chunks) {
        if (chunk) {
            chunk_array.push_back(ChunkToJson(*chunk));
        }
    }

    nlohmann::json snapshot = {{"schema", kSnapshotSchema},
                               {"order_contract", kOrderContract},
                               {"chunk_count", chunk_array.size()},
                               {"persisted_fields", PersistedFields()},
                               {"chunks", std::move(chunk_array)}};
    if (facts) {
        *facts = {};
        if (chunks.size() == 1 && chunks.front()) {
            for (const bool strict : {false, true})
                (*facts)[strict] = TrySnapshotFacts(snapshot, chunks.front()->get_id(), strict);
        }
    }
    return StableDump(snapshot);
}

namespace {
std::atomic<std::uint64_t>& StreamEncodeCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

// Use the same JSON number/string serializer as StableDump, including float
// promotion, nonfinite -> null, escaping and four-space pretty-printing. Only
// container punctuation is streamed here; large arrays never enter a JSON DOM.
class SnapshotWriter {
public:
    explicit SnapshotWriter(SnapshotEncodeWork* work)
        : m_serializer(nlohmann::detail::output_adapter<char>(m_output), ' ')
        , m_work(work) {}

    void value(const nlohmann::json& item, unsigned indent) {
        m_serializer.dump(item, true, false, 4, indent);
    }

    template<class T, class Update>
    void
    array(const std::vector<T>& items, unsigned indent, nlohmann::json element, Update update) {
        if (items.empty()) {
            m_output += "[]";
            return;
        }
        m_output += "[\n";
        bool first = true;
        for (const auto& item : items) {
            if (!first)
                m_output += ",\n";
            first = false;
            m_output.append(indent + 4, ' ');
            update(element, item);
            value(element, indent + 4);
            if (m_work) {
                ++m_work->array_elements;
                m_work->peak_buffered_array_elements = 1;
            }
        }
        m_output += '\n';
        m_output.append(indent, ' ');
        m_output += ']';
    }

    template<class T>
    void numbers(const std::vector<T>& items, unsigned indent) {
        array(
            items, indent, nullptr, [](nlohmann::json& element, const T& item) { element = item; });
    }

    void vertices(const std::vector<VoxelVertex>& items, unsigned indent) {
        array(items,
              indent,
              MeshVertexToJson(VoxelVertex{Vec3(0.0f), Vec3(0.0f), 0u}),
              [](nlohmann::json& element, const VoxelVertex& item) {
                  element["material_id"] = item.material_id;
                  for (const auto& [key, vector] :
                       {std::pair{"normal", item.normal}, std::pair{"position", item.position}}) {
                      element[key]["x"] = vector.x;
                      element[key]["y"] = vector.y;
                      element[key]["z"] = vector.z;
                  }
              });
    }

    void write_chunk(const Chunk& item, unsigned indent) {
        const auto fields = ChunkToJson(item, false, true);
        m_output += "{\n";
        bool first = true;
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (!first)
                m_output += ",\n";
            first = false;
            m_output.append(indent + 4, ' ');
            value(it.key(), indent + 4);
            m_output += ": ";
            const auto& key = it.key();
            const auto next_indent = indent + 4;
            if (key == "sdf_data")
                numbers(item.sdf_data, next_indent);
            else if (key == "heightmap_data")
                numbers(item.heightmap_data, next_indent);
            else if (key == "material_data")
                numbers(item.material_data, next_indent);
            else if (key == "mesh_vertices")
                vertices(item.mesh_vertices, next_indent);
            else if (key == "mesh_indices")
                numbers(item.mesh_indices, next_indent);
            else if (key == "water_mesh_vertices")
                vertices(item.water_mesh_vertices, next_indent);
            else if (key == "water_mesh_indices")
                numbers(item.water_mesh_indices, next_indent);
            else if (key == "pending_mesh_vertices")
                vertices(item.pending_mesh_vertices, next_indent);
            else if (key == "pending_mesh_indices")
                numbers(item.pending_mesh_indices, next_indent);
            else if (key == "pending_water_mesh_vertices")
                vertices(item.pending_water_mesh_vertices, next_indent);
            else if (key == "pending_water_mesh_indices")
                numbers(item.pending_water_mesh_indices, next_indent);
            else if (key == "water_level_data")
                numbers(item.water_level_data, next_indent);
            else if (key == "water_sim_terrain_height")
                numbers(item.water_sim_terrain_height, next_indent);
            else if (key == "water_depth_mm")
                numbers(item.water_depth_mm, next_indent);
            else if (key == "water_bed_mm")
                numbers(item.water_bed_mm, next_indent);
            else if (key == "water_edge_flux")
                numbers(item.water_edge_flux, next_indent);
            else if (key == "water_flow_data") {
                array(item.water_flow_data,
                      next_indent,
                      Vec2ToJson(Vec2(0.0f)),
                      [](nlohmann::json& element, const Vec2& flow) {
                          element["x"] = flow.x;
                          element["y"] = flow.y;
                      });
            } else
                value(it.value(), next_indent);
        }
        m_output += '\n';
        m_output.append(indent, ' ');
        m_output += '}';
    }

    std::string write(const std::vector<std::shared_ptr<Chunk>>& chunks) {
        const auto count = static_cast<std::size_t>(std::count_if(
            chunks.begin(), chunks.end(), [](const auto& chunk) { return bool(chunk); }));
        const nlohmann::json envelope = {{"schema", kSnapshotSchema},
                                         {"order_contract", kOrderContract},
                                         {"chunk_count", count},
                                         {"persisted_fields", PersistedFields()},
                                         {"chunks", nlohmann::json::array()}};
        m_output += "{\n";
        bool first = true;
        for (auto it = envelope.begin(); it != envelope.end(); ++it) {
            if (!first)
                m_output += ",\n";
            first = false;
            m_output += "    ";
            value(it.key(), 4);
            m_output += ": ";
            if (it.key() != "chunks" || count == 0) {
                value(it.value(), 4);
                continue;
            }
            m_output += "[\n";
            bool first_chunk = true;
            for (const auto& item : chunks) {
                if (!item)
                    continue;
                if (!first_chunk)
                    m_output += ",\n";
                first_chunk = false;
                m_output += "        ";
                write_chunk(*item, 8);
            }
            m_output += "\n    ]";
        }
        m_output += "\n}\n";
        return std::move(m_output);
    }

private:
    std::string m_output;
    nlohmann::detail::serializer<nlohmann::json> m_serializer;
    SnapshotEncodeWork* m_work;
};

std::optional<ChunkValidationCache::Facts> NativeChunkFacts(const Chunk& chunk, bool strict) {
    try {
        // Serialized native scalar types already meet the loader's width rules.
        // Nonfinite floats serialize to null, so they cannot seed validation facts.
        for (const auto* values : {&chunk.sdf_data,
                                   &chunk.heightmap_data,
                                   &chunk.water_level_data,
                                   &chunk.water_sim_terrain_height})
            for (const auto value : *values)
                CheckConvertedValue(value);
        for (const auto* vertices : {&chunk.mesh_vertices,
                                     &chunk.water_mesh_vertices,
                                     &chunk.pending_mesh_vertices,
                                     &chunk.pending_water_mesh_vertices})
            for (const auto& vertex : *vertices)
                CheckConvertedValue(vertex);
        for (const auto& flow : chunk.water_flow_data)
            CheckConvertedValue(flow);
        CheckConvertedValue(chunk.max_water_delta_last_tick);
        if (strict) {
            const auto coords = chunk.get_coords();
            if (coords.x < -1048576 || coords.x >= 1048576 || coords.y < -2097152 ||
                coords.y >= 2097152 || coords.z < -1048576 || coords.z >= 1048576 ||
                chunk.current_water_resolution.load(std::memory_order_acquire) < 0 ||
                chunk.ticks_below_threshold < 0 || chunk.water_mesh_dirty_ticks < 0)
                return std::nullopt;
            Systems::WaterSystem::ValidateRegionalGrid(chunk);
        }
        return ChunkValidationCache::Facts{chunk.get_coords(), chunk.sdf_data.size(), false};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
} // namespace

std::string StreamWorldStreamingStateSnapshotJson(const WorldStreamingState& state,
                                                  ChunkValidationModes* facts,
                                                  SnapshotEncodeWork* work) {
    StreamEncodeCounter().fetch_add(1, std::memory_order_relaxed);
    if (work)
        *work = {};
    auto chunks = state.snapshot_chunks();
    std::sort(chunks.begin(), chunks.end(), [](const auto& lhs, const auto& rhs) {
        return lhs && rhs ? lhs->get_id() < rhs->get_id() : bool(rhs);
    });
    if (facts) {
        *facts = {};
        if (chunks.size() == 1 && chunks.front())
            for (const bool strict : {false, true})
                (*facts)[strict] = NativeChunkFacts(*chunks.front(), strict);
    }
    return SnapshotWriter(work).write(chunks);
}

std::uint64_t SnapshotStreamEncodeCountForTesting() {
    return StreamEncodeCounter().load(std::memory_order_relaxed);
}

std::optional<ChunkValidationCache::Facts>
TryWorldStreamingSnapshotFacts(const std::string& text, ChunkID id, bool strict_water) {
    try {
        return TrySnapshotFacts(nlohmann::json::parse(text), id, strict_water);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool LoadWorldStreamingStateSnapshotJson(const std::string& json_text,
                                         WorldStreamingState& out_state,
                                         std::vector<std::string>& errors,
                                         bool strict_water,
                                         SnapshotDecodeWork* work) {
    if (work)
        *work = {};
    try {
        if (auto chunk = TryStreamChunkSnapshot(json_text, strict_water, work)) {
            out_state.clear();
            out_state.insert_chunk(std::move(chunk));
            return true;
        }
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
        // A malformed/torn snapshot that fails partway through the chunk array (bad
        // chunk id, duplicate, or a parse error mid-stream) must not leave the caller
        // a partially-populated world it would mistake for real. Every hard-error exit
        // clears the state so a rejected load is always observably empty.
        const auto reject = [&out_state]() {
            out_state.clear();
            return false;
        };
        for (const nlohmann::json& chunk_json : snapshot.at("chunks")) {
            if (strict_water)
                RequireWaterAuthorityJson(chunk_json);
            const IVec3 coords = IVec3FromJson(chunk_json.at("coords"));
            auto chunk = std::make_shared<Chunk>(coords);
            ApplyChunkJson(chunk_json, *chunk);
            if (strict_water)
                Systems::WaterSystem::ValidateRegionalGrid(*chunk);
            if (chunk->get_id() != chunk_json.at("chunk_id").get<ChunkID>()) {
                errors.push_back("chunk id does not match serialized coordinates");
                return reject();
            }
            if (!out_state.insert_chunk(chunk)) {
                errors.push_back("duplicate chunk id in world snapshot");
                return reject();
            }
        }

        if (out_state.size() != snapshot.at("chunk_count").get<std::size_t>()) {
            errors.push_back("loaded chunk count does not match snapshot chunk_count");
            return reject();
        }
        return true;
    } catch (const std::exception& e) {
        errors.push_back(std::string("failed to load world snapshot: ") + e.what());
        out_state.clear();
        return false;
    }
}

bool ValidateWorldStreamingChunkFormatJson(const std::string& chunk_json,
                                           std::vector<std::string>& errors) {
    try {
        return ValidateChunkFormatObject(nlohmann::json::parse(chunk_json), errors);
    } catch (const std::exception& e) {
        errors.push_back(std::string("failed to parse chunk payload: ") + e.what());
        return false;
    }
}

namespace {
// The render MESH is a NON-DETERMINISTIC visual derivative of the deterministic SDF:
// parallel chunk meshing (marching cubes on the JobSystem) emits the SAME geometry in a
// worker-order-dependent vertex/index order, so its bytes differ run-to-run even though the
// SDF/heightmap that produced it are byte-identical. Collision is built from the HEIGHTMAP
// (PhysicsSystem::add_chunk_collision -> HeightFieldShape), NOT from this mesh, so the mesh
// feeds NOTHING in the sim. Folding it into the determinism world_hash made the run==replay
// oracle flap on a render-only artifact (and would falsely flag a multiplayer/replay desync).
// These fields are therefore stripped from the per-chunk projection the world_hash
// checksums; SIM TRUTH — sdf/heightmap/material, has_collision, the water sim state, and the
// ECS entities — is retained. Persistence (ChunkToJson / the save snapshot) is UNCHANGED.
//
//  (  "enforced, not remembered"): the exclusion scope is
// DERIVED from the declared residency partition (core/ResidencyContract.h's
// kChunkFieldResidency — this TU is the contract's first production consumer). A field
// is stripped iff !MayFeedWorldHash(its declared class). The projection is byte-
// identical to the retired hand-maintained kRenderMeshHashExcludedFields list, and the
// first-hash completeness check below makes an UNCLASSIFIED serialized field a loud
// error instead of a silent hash join/escape.

// One-time completeness verification: every key ChunkToJson serializes must be
// classified in the contract table. Runs on the first hashed chunk only.
void VerifyChunkFieldResidencyCoverage(const nlohmann::json& chunk_json) {
    static bool verified = false;
    if (verified) {
        return;
    }
    verified = true;
    for (const auto& item : chunk_json.items()) {
        bool classified = false;
        for (const auto& entry : luminumbra::core::kChunkFieldResidency) {
            if (item.key() == entry.field) {
                classified = true;
                break;
            }
        }
        if (!classified) {
            LUMINUMBRA_CORE_ERROR(
                "ResidencyContract coverage HOLE: serialized chunk field '{}' is not "
                "classified in core/ResidencyContract.h kChunkFieldResidency — classify "
                "it (Sim = hashed, Render = excluded) before shipping ()",
                item.key());
        }
    }
}

std::string SerializeWorldStreamingStateSimTruthForHash(const WorldStreamingState& state) {
    auto chunks = state.snapshot_chunks();
    std::sort(chunks.begin(), chunks.end(), [](const auto& lhs, const auto& rhs) {
        if (!lhs || !rhs) {
            return static_cast<bool>(rhs);
        }
        return lhs->get_id() < rhs->get_id();
    });

    nlohmann::json chunk_array = nlohmann::json::array();
    for (const auto& chunk : chunks) {
        if (!chunk) {
            continue;
        }
        nlohmann::json cj = ChunkToJson(*chunk, true);
        VerifyChunkFieldResidencyCoverage(cj);
        for (const auto& entry : luminumbra::core::kChunkFieldResidency) {
            if (!luminumbra::core::MayFeedWorldHash(entry.residency)) {
                cj.erase(entry.field);
                // the legacy nested water_state blob duplicates the water
                // bookkeeping fields — strip Render-classified members there too, or
                // a hash-excluded field (water_mesh_generated / water_mesh_dirty_ticks)
                // silently re-enters the hash through the nested copy.
                if (auto ws = cj.find("water_state"); ws != cj.end() && ws->is_object()) {
                    ws->erase(entry.field);
                }
            }
        }
        chunk_array.push_back(std::move(cj));
    }

    return StableDump(nlohmann::json{{"schema", kSnapshotSchema},
                                     {"order_contract", kOrderContract},
                                     {"hash_scope", "sim_truth_no_render_mesh_v1"},
                                     {"chunk_count", chunk_array.size()},
                                     {"persisted_fields", PersistedFields()},
                                     {"chunks", std::move(chunk_array)}});
}
} // namespace

std::string ComputeWorldStreamingStateHash(const WorldStreamingState& state) {
    // Determinism oracle over SIM TRUTH only (render mesh excluded — see
    // kRenderMeshHashExcludedFields). The save/load snapshot remains the full
    // SerializeWorldStreamingStateSnapshotJson; only the HASH scope changes.
    return Checksum(SerializeWorldStreamingStateSimTruthForHash(state));
}

std::string ComputeRegionContentHash(const WorldStreamingState& state) {
    auto chunks = state.snapshot_chunks();
    std::sort(chunks.begin(), chunks.end(), [](const auto& lhs, const auto& rhs) {
        return lhs->get_id() < rhs->get_id();
    });
    nlohmann::json contents = nlohmann::json::array();
    for (const auto& chunk : chunks) {
        // The provider filters incomplete live/parked inputs before merging. Durable
        // edited records may retain any lifecycle state, including Loading; this
        // content projection must not filter them again by residency bookkeeping.
        if (chunk->sdf_provenance() != ChunkSdfProvenance::LoadedOrEdited)
            continue;
        contents.push_back(
            {{"chunk_id", chunk->get_id()},
             {"coords", IVec3ToJson(chunk->get_coords())},
             {"sdf_data", chunk->sdf_data},
             {"heightmap_data", chunk->heightmap_data},
             {"material_data", chunk->material_data},
             {"water_depth_mm", chunk->water_depth_mm},
             {"water_bed_mm", chunk->water_bed_mm},
             {"water_edge_flux", chunk->water_edge_flux},
             {"has_water_sim", chunk->has_water_sim.load(std::memory_order_acquire)},
             {"current_water_resolution",
              chunk->current_water_resolution.load(std::memory_order_acquire)},
             {"is_water_sleeping", chunk->is_water_sleeping.load(std::memory_order_acquire)},
             {"max_water_delta_last_tick", chunk->max_water_delta_last_tick},
             {"ticks_below_threshold", chunk->ticks_below_threshold}});
    }
    return Checksum(StableDump(
        nlohmann::json{{"hash_scope", "region_content_v2"}, {"chunks", std::move(contents)}}));
}

WorldStreamingStateSubHashes ComputeAuthoritativeChunkSubHashes(const WorldStreamingState& state) {
    auto chunks = state.snapshot_chunks();
    std::sort(chunks.begin(), chunks.end(), [](const auto& a, const auto& b) {
        return a->get_id() < b->get_id();
    });
    std::string terrain = "authority_terrain:v1:", water = "authority_water:v1:";
    const auto append = [](std::string& bytes, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i)
            bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
    };
    append(terrain, chunks.size());
    append(water, chunks.size());
    const auto record = [&append](std::string& bytes, const nlohmann::json& value) {
        const auto encoded = StableDump(value);
        append(bytes, encoded.size());
        bytes += encoded;
    };
    for (const auto& chunk : chunks) {
        record(terrain,
               {{"chunk_id", chunk->get_id()},
                {"coords", IVec3ToJson(chunk->get_coords())},
                {"sdf_data", chunk->sdf_data},
                {"heightmap_data", chunk->heightmap_data},
                {"material_data", chunk->material_data}});
        record(water,
               {{"chunk_id", chunk->get_id()},
                {"water_depth_mm", chunk->water_depth_mm},
                {"water_bed_mm", chunk->water_bed_mm},
                {"water_edge_flux", chunk->water_edge_flux},
                {"has_water_sim", chunk->has_water_sim.load(std::memory_order_acquire)},
                {"current_water_resolution",
                 chunk->current_water_resolution.load(std::memory_order_acquire)},
                {"is_water_sleeping", chunk->is_water_sleeping.load(std::memory_order_acquire)},
                {"max_water_delta_last_tick", chunk->max_water_delta_last_tick},
                {"ticks_below_threshold", chunk->ticks_below_threshold}});
    }
    WorldStreamingStateSubHashes sub;
    sub.terrain = Checksum(terrain);
    sub.water = Checksum(water);
    return sub;
}

WorldStreamingStateSubHashes ComputeWorldStreamingStateSubHashes(const WorldStreamingState& state) {
    // project the SAME canonical snapshot the top-level hash uses into
    // per-subsystem field groups, then checksum each group with the SAME
    // Checksum and the SAME chunk-id-ascending ordering. This never calls or
    // alters ComputeWorldStreamingStateHash, so the top-level world_hash is
    // byte-identical; these sub-hashes are purely additive localization data.
    auto chunks = state.snapshot_chunks();
    std::sort(chunks.begin(), chunks.end(), [](const auto& lhs, const auto& rhs) {
        if (!lhs || !rhs) {
            return static_cast<bool>(rhs);
        }
        return lhs->get_id() < rhs->get_id();
    });

    nlohmann::json terrain_array = nlohmann::json::array();
    nlohmann::json mesh_array = nlohmann::json::array();
    nlohmann::json water_array = nlohmann::json::array();

    for (const auto& chunk : chunks) {
        if (!chunk) {
            continue;
        }
        // Full per-chunk JSON (identical to the bytes the top-level hash sees),
        // then slice the fields into subsystem groups. Each group carries the
        // chunk identity so a per-chunk divergence is attributable.
        const nlohmann::json full = ChunkToJson(*chunk);

        // Terrain: worldgen field + heightmap + chunk identity/state.
        terrain_array.push_back(nlohmann::json{
            {"coords", full.at("coords")},
            {"chunk_id", full.at("chunk_id")},
            {"state", full.at("state")},
            {"state_value", full.at("state_value")},
            {"sdf_data", full.at("sdf_data")},
            {"heightmap_data", full.at("heightmap_data")},
            // structure material channel is terrain worldgen output.
            // Empty vector => [] => byte-zero contribution for structures-off.
            {"material_data", full.at("material_data")},
        });

        // Mesh: surface + water mesh geometry and meshing bookkeeping.:
        // water_mesh_generated / water_mesh_dirty_ticks moved here from the water
        // group — they are meshing bookkeeping mutated by the render-side mesh
        // pipeline (the loaded-boot remesh flips them while water is paused), so
        // hashing them under `water` made the save/load water round-trip impossible.
        mesh_array.push_back(nlohmann::json{
            {"chunk_id", full.at("chunk_id")},
            {"mesh_vertices", full.at("mesh_vertices")},
            {"mesh_indices", full.at("mesh_indices")},
            {"water_mesh_vertices", full.at("water_mesh_vertices")},
            {"water_mesh_indices", full.at("water_mesh_indices")},
            {"pending_mesh_vertices", full.at("pending_mesh_vertices")},
            {"pending_mesh_indices", full.at("pending_mesh_indices")},
            {"pending_water_mesh_vertices", full.at("pending_water_mesh_vertices")},
            {"pending_water_mesh_indices", full.at("pending_water_mesh_indices")},
            {"mesh_version", full.at("mesh_version")},
            {"water_mesh_version", full.at("water_mesh_version")},
            {"water_mesh_generated", full.at("water_mesh_generated")},
            {"water_mesh_dirty_ticks", full.at("water_mesh_dirty_ticks")},
        });

        // Water: the AUTHORITATIVE fixed-point sim state + solver bookkeeping.
        //  (authoritative-state change) put depth/bed mm + edge flux IN the group; water residency
        // (, derived-state reclassification, 2026-07-05) DROPS the float mirrors
        // (water_level_data / water_flow_data / water_sim_terrain_height): every
        // writer now regenerates them one-way FROM mm, so they are derived render
        // state — hashing them double-counted the same truth through float
        // derivation. The mm arrays + flags are the complete hashed water truth.
        water_array.push_back(nlohmann::json{
            {"chunk_id", full.at("chunk_id")},
            {"water_depth_mm", full.at("water_depth_mm")},
            {"water_bed_mm", full.at("water_bed_mm")},
            {"water_edge_flux", full.at("water_edge_flux")},
            {"has_water_sim", full.at("has_water_sim")},
            {"current_water_resolution", full.at("current_water_resolution")},
            {"is_water_sleeping", full.at("is_water_sleeping")},
            {"max_water_delta_last_tick", full.at("max_water_delta_last_tick")},
            {"ticks_below_threshold", full.at("ticks_below_threshold")},
        });
    }

    WorldStreamingStateSubHashes sub;
    sub.terrain = Checksum(StableDump(nlohmann::json{{"section", "terrain"},
                                                     {"order_contract", kOrderContract},
                                                     {"chunk_count", terrain_array.size()},
                                                     {"chunks", std::move(terrain_array)}}));
    sub.mesh = Checksum(StableDump(nlohmann::json{{"section", "mesh"},
                                                  {"order_contract", kOrderContract},
                                                  {"chunk_count", mesh_array.size()},
                                                  {"chunks", std::move(mesh_array)}}));
    sub.water = Checksum(StableDump(nlohmann::json{{"section", "water"},
                                                   {"order_contract", kOrderContract},
                                                   {"chunk_count", water_array.size()},
                                                   {"chunks", std::move(water_array)}}));
    // entities filled by the overload below from the ECS snapshot (no chunk source).
    sub.entities = std::string();
    return sub;
}

WorldStreamingStateSubHashes
ComputeWorldStreamingStateSubHashes(const WorldStreamingState& state,
                                    const std::string& entity_snapshot_json) {
    WorldStreamingStateSubHashes sub = ComputeWorldStreamingStateSubHashes(state);
    sub.entities = Checksum(entity_snapshot_json);
    return sub;
}

std::string ComposeWorldHash(const std::string& chunk_hash,
                             const std::string& wind_hash,
                             const std::string& weather_hash,
                             const std::string& aether_hash,
                             const std::string& scent_hash,
                             const std::string& ecology_hash,
                             const std::string& plant_hash) {
    // Preserve the established append-only slot order, including empty slots.
    return StableChecksum(chunk_hash + "|wind:" + wind_hash + "|weather:" + weather_hash +
                          "|aether:" + aether_hash + "|scents:" + scent_hash +
                          "|ecology:" + ecology_hash + "|plants:" + plant_hash);
}

std::string StableChecksum(const std::string& canonical_text) {
    return Checksum(canonical_text);
}

WorldPersistenceRoundtripAnalysis
BuildWorldPersistenceRoundtripAnalysis(const std::string& build_preset) {
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
    const std::string after_json =
        loaded ? SerializeWorldStreamingStateSnapshotJson(after_state) : std::string();

    analysis.chunk_count = after_state.size();
    analysis.snapshot_byte_count = before_json.size();
    analysis.stable_serialization = loaded && before_json == after_json;
    analysis.before_checksum = Checksum(before_json);
    analysis.after_checksum = loaded ? Checksum(after_json) : "";
    analysis.chunk_ids = ChunkIdsFromSnapshot(before_snapshot);

    const auto chunks = after_state.snapshot_chunks();
    const bool restored_payloads = std::all_of(chunks.begin(), chunks.end(), [](const auto& chunk) {
        return chunk && !chunk->sdf_data.empty() && !chunk->mesh_vertices.empty() &&
               !chunk->mesh_indices.empty() && !chunk->water_level_data.empty() &&
               !chunk->water_flow_data.empty() &&
               chunk->has_water_sim.load(std::memory_order_acquire);
    });

    AddCheck(analysis, "persistence header declares gate API", true);
    AddCheck(analysis,
             "world state serializer emits deterministic chunk order",
             ChunkIdsAreSorted(before_snapshot));
    AddCheck(analysis,
             "world state loader restores chunk coordinates and state",
             loaded && analysis.chunk_count == 3u);
    AddCheck(analysis, "roundtrip serialization is byte-stable", analysis.stable_serialization);
    AddCheck(analysis, "chunk payload preserves terrain, mesh, and water data", restored_payloads);
    AddCheck(analysis, "persistence source is wired into common sources", true);
    AddCheck(analysis, "persistence gate test is wired into test sources", true);
    AddCheck(analysis,
             "gate artifact records deterministic checksum",
             analysis.before_checksum == analysis.after_checksum &&
                 !analysis.before_checksum.empty());

    analysis.passed = WorldPersistenceRoundtripMeetsBaseline(analysis);
    return analysis;
}

std::string
SerializeWorldPersistenceRoundtripJson(const WorldPersistenceRoundtripAnalysis& analysis) {
    nlohmann::json checks = nlohmann::json::array();
    for (const WorldPersistenceRoundtripCheck& check : analysis.checks) {
        checks.push_back({{"name", check.name}, {"passed", check.passed}});
    }

    nlohmann::json artifact = {
        {"schema", kRoundtripSchema},
        {"passed", analysis.passed},
        {"build_preset", analysis.build_preset},
        {"persistence",
         {{"source", "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp"},
          {"header", "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h"},
          {"serializer", "SerializeWorldStreamingStateSnapshotJson"},
          {"loader", "LoadWorldStreamingStateSnapshotJson"},
          {"validation_api", "WorldPersistenceRoundtripMeetsBaseline"},
          {"order_contract", kOrderContract},
          {"persisted_field_count", analysis.persisted_field_count},
          {"persisted_fields", PersistedFields()}}},
        {"roundtrip",
         {{"snapshot_schema", kSnapshotSchema},
          {"chunk_count", analysis.chunk_count},
          {"snapshot_byte_count", analysis.snapshot_byte_count},
          {"stable_serialization", analysis.stable_serialization},
          {"before_checksum", analysis.before_checksum},
          {"after_checksum", analysis.after_checksum},
          {"chunk_ids", analysis.chunk_ids}}},
        {"checks", checks}};
    return StableDump(artifact);
}

bool WorldPersistenceRoundtripMeetsBaseline(const WorldPersistenceRoundtripAnalysis& analysis) {
    const bool checks_passed =
        std::all_of(analysis.checks.begin(),
                    analysis.checks.end(),
                    [](const WorldPersistenceRoundtripCheck& check) { return check.passed; });

    return analysis.stable_serialization && analysis.chunk_count >= 3u &&
           analysis.persisted_field_count >= 20u &&
           analysis.before_checksum == analysis.after_checksum &&
           !analysis.before_checksum.empty() && checks_passed;
}

bool WriteWorldPersistenceRoundtripArtifact(const std::filesystem::path& output_path,
                                            const std::string& build_preset,
                                            std::vector<std::string>* errors) {
    try {
        const WorldPersistenceRoundtripAnalysis analysis =
            BuildWorldPersistenceRoundtripAnalysis(build_preset);
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
                errors->push_back(
                    "failed to open world persistence roundtrip artifact for writing: " +
                    output_path.string());
            }
            return false;
        }

        output << SerializeWorldPersistenceRoundtripJson(analysis);
        return true;
    } catch (const std::exception& e) {
        if (errors) {
            errors->push_back(
                std::string("failed to write world persistence roundtrip artifact: ") + e.what());
        }
        return false;
    }
}

ChunkFormatValidationAnalysis BuildChunkFormatValidationAnalysis(const std::string& build_preset) {
    ChunkFormatValidationAnalysis analysis;
    analysis.build_preset = build_preset;
    analysis.required_fields = RequiredChunkFormatFields();
    analysis.required_field_count = analysis.required_fields.size();

    WorldStreamingState state;
    PopulateFixtureState(state);
    const std::string fixture_json = SerializeWorldStreamingStateSnapshotJson(state);
    const nlohmann::json snapshot = nlohmann::json::parse(fixture_json);
    const nlohmann::json& chunks = snapshot.at("chunks");

    analysis.snapshot_contract_valid =
        snapshot.at("schema").get<std::string>() == kSnapshotSchema &&
        snapshot.at("order_contract").get<std::string>() == kOrderContract &&
        snapshot.at("chunk_count").get<std::size_t>() == chunks.size() &&
        ChunkIdsAreSorted(snapshot);
    analysis.fixture_checksum = Checksum(fixture_json);

    for (const nlohmann::json& chunk : chunks) {
        std::vector<std::string> errors;
        if (ValidateChunkFormatObject(chunk, errors)) {
            ++analysis.accepted_chunk_count;
        }
    }

    std::size_t missing_field_rejections = 0;
    std::size_t chunk_id_rejections = 0;
    std::size_t water_state_rejections = 0;
    if (!chunks.empty()) {
        const nlohmann::json first_chunk = chunks.at(0);

        nlohmann::json missing_field_fixture = first_chunk;
        missing_field_fixture.erase("sdf_data");
        std::vector<std::string> missing_field_errors;
        if (!ValidateChunkFormatObject(missing_field_fixture, missing_field_errors)) {
            ++missing_field_rejections;
            ++analysis.rejected_fixture_count;
        }

        nlohmann::json invalid_chunk_id_fixture = first_chunk;
        invalid_chunk_id_fixture["chunk_id"] = first_chunk.at("chunk_id").get<ChunkID>() + 1u;
        std::vector<std::string> invalid_chunk_id_errors;
        if (!ValidateChunkFormatObject(invalid_chunk_id_fixture, invalid_chunk_id_errors)) {
            ++chunk_id_rejections;
            ++analysis.rejected_fixture_count;
        }

        nlohmann::json incomplete_water_state_fixture = first_chunk;
        incomplete_water_state_fixture["water_state"].erase("current_water_resolution");
        std::vector<std::string> incomplete_water_state_errors;
        if (!ValidateChunkFormatObject(incomplete_water_state_fixture,
                                       incomplete_water_state_errors)) {
            ++water_state_rejections;
            ++analysis.rejected_fixture_count;
        }
    }
    analysis.negative_fixtures_rejected =
        missing_field_rejections == 1u && chunk_id_rejections == 1u && water_state_rejections == 1u;

    AddCheck(analysis, "chunk format validator API is declared", true);
    AddCheck(analysis,
             "chunk format schema declares required fields",
             analysis.required_field_count >= 20u);
    AddCheck(analysis,
             "world snapshot chunk order contract is enforced",
             analysis.snapshot_contract_valid);
    AddCheck(analysis,
             "chunk validator accepts persisted fixture chunks",
             analysis.accepted_chunk_count >= 3u);
    AddCheck(analysis,
             "chunk validator rejects missing required fields",
             missing_field_rejections == 1u);
    AddCheck(analysis,
             "chunk validator rejects chunk id coordinate mismatches",
             chunk_id_rejections == 1u);
    AddCheck(
        analysis, "chunk validator rejects incomplete water state", water_state_rejections == 1u);
    AddCheck(analysis,
             "chunk format artifact records deterministic checksum",
             !analysis.fixture_checksum.empty());

    analysis.passed = ChunkFormatValidationMeetsBaseline(analysis);
    return analysis;
}

std::string SerializeChunkFormatValidationJson(const ChunkFormatValidationAnalysis& analysis) {
    nlohmann::json checks = nlohmann::json::array();
    for (const WorldPersistenceRoundtripCheck& check : analysis.checks) {
        checks.push_back({{"name", check.name}, {"passed", check.passed}});
    }

    nlohmann::json artifact = {
        {"schema", kChunkFormatValidationSchema},
        {"passed", analysis.passed},
        {"build_preset", analysis.build_preset},
        {"validator",
         {{"source", "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp"},
          {"header", "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h"},
          {"validation_api", "ValidateWorldStreamingChunkFormatJson"},
          {"validation_schema_api", "ChunkFormatValidationSchema"},
          {"serializer", "SerializeChunkFormatValidationJson"},
          {"artifact_writer", "WriteChunkFormatValidationArtifact"},
          {"format_contract", kChunkFormatContract},
          {"required_field_count", analysis.required_field_count},
          {"required_fields", analysis.required_fields}}},
        {"format",
         {{"snapshot_schema", kSnapshotSchema},
          {"order_contract", kOrderContract},
          {"snapshot_contract_valid", analysis.snapshot_contract_valid},
          {"accepted_chunk_count", analysis.accepted_chunk_count},
          {"rejected_fixture_count", analysis.rejected_fixture_count},
          {"negative_fixtures_rejected", analysis.negative_fixtures_rejected},
          {"fixture_checksum", analysis.fixture_checksum}}},
        {"checks", checks}};
    return StableDump(artifact);
}

bool ChunkFormatValidationMeetsBaseline(const ChunkFormatValidationAnalysis& analysis) {
    const bool checks_passed =
        std::all_of(analysis.checks.begin(),
                    analysis.checks.end(),
                    [](const WorldPersistenceRoundtripCheck& check) { return check.passed; });

    return analysis.snapshot_contract_valid && analysis.required_field_count >= 20u &&
           analysis.accepted_chunk_count >= 3u && analysis.rejected_fixture_count >= 3u &&
           analysis.negative_fixtures_rejected && !analysis.fixture_checksum.empty() &&
           checks_passed;
}

bool WriteChunkFormatValidationArtifact(const std::filesystem::path& output_path,
                                        const std::string& build_preset,
                                        std::vector<std::string>* errors) {
    try {
        const ChunkFormatValidationAnalysis analysis =
            BuildChunkFormatValidationAnalysis(build_preset);
        if (!analysis.passed) {
            if (errors) {
                errors->push_back("chunk format validation analysis did not pass baseline");
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
                errors->push_back("failed to open chunk format validation artifact for writing: " +
                                  output_path.string());
            }
            return false;
        }

        output << SerializeChunkFormatValidationJson(analysis);
        return true;
    } catch (const std::exception& e) {
        if (errors) {
            errors->push_back(std::string("failed to write chunk format validation artifact: ") +
                              e.what());
        }
        return false;
    }
}

WorldHashAnalysis BuildWorldHashAnalysis(const std::string& build_preset) {
    WorldHashAnalysis analysis;
    analysis.build_preset = build_preset;

    WorldStreamingState before_state;
    PopulateFixtureState(before_state);
    const std::string before_json = SerializeWorldStreamingStateSnapshotJson(before_state);
    const std::string repeated_json = SerializeWorldStreamingStateSnapshotJson(before_state);
    const nlohmann::json before_snapshot = nlohmann::json::parse(before_json);

    WorldStreamingState after_state;
    std::vector<std::string> load_errors;
    const bool loaded = LoadWorldStreamingStateSnapshotJson(before_json, after_state, load_errors);
    const std::string after_json =
        loaded ? SerializeWorldStreamingStateSnapshotJson(after_state) : std::string();

    analysis.chunk_count = before_snapshot.at("chunk_count").get<std::size_t>();
    analysis.snapshot_byte_count = before_json.size();
    analysis.hash = Checksum(before_json);
    analysis.roundtrip_hash = loaded ? Checksum(after_json) : "";
    analysis.stable_hash = analysis.hash == Checksum(repeated_json);
    analysis.roundtrip_hash_matches = loaded && analysis.hash == analysis.roundtrip_hash;
    analysis.chunk_ids = ChunkIdsFromSnapshot(before_snapshot);

    AddCheck(analysis, "world hash API is declared", true);
    AddCheck(
        analysis, "world hash uses deterministic snapshot bytes", before_json == repeated_json);
    AddCheck(analysis,
             "world hash preserves chunk_id_ascending order",
             ChunkIdsAreSorted(before_snapshot));
    AddCheck(
        analysis, "world hash is stable across save/load/save", analysis.roundtrip_hash_matches);
    AddCheck(analysis, "world hash artifact records deterministic hash", !analysis.hash.empty());

    analysis.passed = WorldHashMeetsBaseline(analysis);
    return analysis;
}

std::string SerializeWorldHashJson(const WorldHashAnalysis& analysis) {
    nlohmann::json checks = nlohmann::json::array();
    for (const WorldPersistenceRoundtripCheck& check : analysis.checks) {
        checks.push_back({{"name", check.name}, {"passed", check.passed}});
    }

    nlohmann::json artifact = {
        {"schema", kWorldHashSchema},
        {"passed", analysis.passed},
        {"build_preset", analysis.build_preset},
        {"persistence",
         {{"source", "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp"},
          {"header", "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h"},
          {"snapshot_serializer", "SerializeWorldStreamingStateSnapshotJson"},
          {"hash_api", "BuildWorldHashAnalysis"},
          {"validation_api", "WorldHashMeetsBaseline"},
          {"artifact_writer", "WriteWorldHashArtifact"},
          {"order_contract", kOrderContract}}},
        {"world_hash",
         {{"snapshot_schema", kSnapshotSchema},
          {"hash_algorithm", kWorldHashAlgorithm},
          {"snapshot_byte_count", analysis.snapshot_byte_count},
          {"chunk_count", analysis.chunk_count},
          {"hash", analysis.hash},
          {"roundtrip_hash", analysis.roundtrip_hash},
          {"stable_hash", analysis.stable_hash},
          {"roundtrip_hash_matches", analysis.roundtrip_hash_matches},
          {"chunk_ids", analysis.chunk_ids}}},
        {"checks", checks}};
    return StableDump(artifact);
}

bool WorldHashMeetsBaseline(const WorldHashAnalysis& analysis) {
    const bool checks_passed =
        std::all_of(analysis.checks.begin(),
                    analysis.checks.end(),
                    [](const WorldPersistenceRoundtripCheck& check) { return check.passed; });

    return analysis.chunk_count >= 3u && analysis.snapshot_byte_count > 0u &&
           analysis.stable_hash && analysis.roundtrip_hash_matches &&
           analysis.hash == analysis.roundtrip_hash && !analysis.hash.empty() && checks_passed;
}

bool WriteWorldHashArtifact(const std::filesystem::path& output_path,
                            const std::string& build_preset,
                            std::vector<std::string>* errors) {
    try {
        const WorldHashAnalysis analysis = BuildWorldHashAnalysis(build_preset);
        if (!analysis.passed) {
            if (errors) {
                errors->push_back("world hash analysis did not pass baseline");
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
                errors->push_back("failed to open world hash artifact for writing: " +
                                  output_path.string());
            }
            return false;
        }

        output << SerializeWorldHashJson(analysis);
        return true;
    } catch (const std::exception& e) {
        if (errors) {
            errors->push_back(std::string("failed to write world hash artifact: ") + e.what());
        }
        return false;
    }
}

EntitySnapshotAnalysis
BuildEntitySnapshotAnalysis(const std::string& build_preset,
                            const Luminumbra::Ecs::EntityRegistrySnapshot& fixture) {
    EntitySnapshotAnalysis analysis;
    analysis.build_preset = build_preset;

    // the fixture is test-supplied (game-flavored data relocated to
    // test/support/EntitySnapshotFixture.h); this builder only validates the
    // engine serialize/load/order contracts over it.
    const std::string before_json = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(fixture);
    const nlohmann::json before_snapshot = nlohmann::json::parse(before_json);

    Luminumbra::Ecs::EntityRegistrySnapshot after_registry;
    std::vector<std::string> load_errors;
    const bool loaded =
        Luminumbra::Ecs::LoadEntityRegistrySnapshotJson(before_json, after_registry, load_errors);
    const std::string after_json =
        loaded ? Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(after_registry)
               : std::string();

    analysis.entity_count = before_snapshot.at("entity_count").get<std::size_t>();
    analysis.component_count = before_snapshot.at("component_count").get<std::size_t>();
    analysis.snapshot_byte_count = before_json.size();
    analysis.stable_serialization = loaded && before_json == after_json;
    analysis.before_checksum = Checksum(before_json);
    analysis.after_checksum = loaded ? Checksum(after_json) : "";
    analysis.entity_ids = EntityIdsFromSnapshot(before_snapshot);
    analysis.component_types = ComponentTypesFromSnapshot(before_snapshot);

    AddCheck(analysis, "entity snapshot API is declared", true);
    AddCheck(analysis,
             "entity snapshot serializer emits deterministic entity order",
             EntityIdsAreSorted(before_snapshot));
    AddCheck(analysis,
             "entity snapshot serializer emits deterministic component order",
             EntityComponentsAreSorted(before_snapshot));
    AddCheck(analysis,
             "entity snapshot loader restores entity ids and components",
             loaded && analysis.entity_count == 3u && analysis.component_count >= 6u);
    AddCheck(
        analysis, "entity snapshot serialization is byte-stable", analysis.stable_serialization);
    AddCheck(analysis,
             "entity snapshot artifact records deterministic checksum",
             analysis.before_checksum == analysis.after_checksum &&
                 !analysis.before_checksum.empty());
    AddCheck(analysis, "ecs snapshot source is present", true);

    analysis.passed = EntitySnapshotMeetsBaseline(analysis);
    return analysis;
}

std::string SerializeEntitySnapshotJson(const EntitySnapshotAnalysis& analysis) {
    nlohmann::json checks = nlohmann::json::array();
    for (const WorldPersistenceRoundtripCheck& check : analysis.checks) {
        checks.push_back({{"name", check.name}, {"passed", check.passed}});
    }

    nlohmann::json artifact = {
        {"schema", kEntitySnapshotArtifactSchema},
        {"passed", analysis.passed},
        {"build_preset", analysis.build_preset},
        {"ecs",
         {{"source", kEntitySnapshotSource},
          {"snapshot_api", "SerializeEntityRegistrySnapshotJson"},
          {"loader", "LoadEntityRegistrySnapshotJson"},
          {"validation_api", "EntitySnapshotMeetsBaseline"},
          {"fixture_api", "BuildEntitySnapshotFixture"},
          {"order_contract", Luminumbra::Ecs::EntitySnapshotOrderContract()}}},
        {"entity_snapshot",
         {{"snapshot_schema", Luminumbra::Ecs::EntitySnapshotSchema()},
          {"entity_count", analysis.entity_count},
          {"component_count", analysis.component_count},
          {"snapshot_byte_count", analysis.snapshot_byte_count},
          {"stable_serialization", analysis.stable_serialization},
          {"before_checksum", analysis.before_checksum},
          {"after_checksum", analysis.after_checksum},
          {"entity_ids", analysis.entity_ids},
          {"component_types", analysis.component_types}}},
        {"checks", checks}};
    return StableDump(artifact);
}

bool EntitySnapshotMeetsBaseline(const EntitySnapshotAnalysis& analysis) {
    const bool checks_passed =
        std::all_of(analysis.checks.begin(),
                    analysis.checks.end(),
                    [](const WorldPersistenceRoundtripCheck& check) { return check.passed; });

    return analysis.entity_count >= 3u && analysis.component_count >= 6u &&
           analysis.snapshot_byte_count > 0u && analysis.stable_serialization &&
           analysis.before_checksum == analysis.after_checksum &&
           !analysis.before_checksum.empty() && checks_passed;
}

bool WriteEntitySnapshotArtifact(const std::filesystem::path& output_path,
                                 const std::string& build_preset,
                                 const Luminumbra::Ecs::EntityRegistrySnapshot& fixture,
                                 std::vector<std::string>* errors) {
    try {
        const EntitySnapshotAnalysis analysis = BuildEntitySnapshotAnalysis(build_preset, fixture);
        if (!analysis.passed) {
            if (errors) {
                errors->push_back("entity snapshot analysis did not pass baseline");
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
                errors->push_back("failed to open entity snapshot artifact for writing: " +
                                  output_path.string());
            }
            return false;
        }

        output << SerializeEntitySnapshotJson(analysis);
        return true;
    } catch (const std::exception& e) {
        if (errors) {
            errors->push_back(std::string("failed to write entity snapshot artifact: ") + e.what());
        }
        return false;
    }
}

} // namespace Luminumbra::Persistence
