#pragma once

#include "world/WorldStreamingState.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Persistence {

struct WorldPersistenceRoundtripCheck {
    std::string name;
    bool passed = false;
};

struct WorldPersistenceRoundtripAnalysis {
    bool passed = false;
    std::string build_preset;
    std::size_t chunk_count = 0;
    std::size_t persisted_field_count = 0;
    std::size_t snapshot_byte_count = 0;
    bool stable_serialization = false;
    std::string before_checksum;
    std::string after_checksum;
    std::vector<std::string> chunk_ids;
    std::vector<WorldPersistenceRoundtripCheck> checks;
};

struct ChunkFormatValidationAnalysis {
    bool passed = false;
    std::string build_preset;
    std::size_t required_field_count = 0;
    std::size_t accepted_chunk_count = 0;
    std::size_t rejected_fixture_count = 0;
    bool snapshot_contract_valid = false;
    bool negative_fixtures_rejected = false;
    std::string fixture_checksum;
    std::vector<std::string> required_fields;
    std::vector<WorldPersistenceRoundtripCheck> checks;
};

struct WorldHashAnalysis {
    bool passed = false;
    std::string build_preset;
    std::size_t chunk_count = 0;
    std::size_t snapshot_byte_count = 0;
    bool stable_hash = false;
    bool roundtrip_hash_matches = false;
    std::string hash;
    std::string roundtrip_hash;
    std::vector<std::string> chunk_ids;
    std::vector<WorldPersistenceRoundtripCheck> checks;
};

struct EntitySnapshotAnalysis {
    bool passed = false;
    std::string build_preset;
    std::size_t entity_count = 0;
    std::size_t component_count = 0;
    std::size_t snapshot_byte_count = 0;
    bool stable_serialization = false;
    std::string before_checksum;
    std::string after_checksum;
    std::vector<std::string> entity_ids;
    std::vector<std::string> component_types;
    std::vector<WorldPersistenceRoundtripCheck> checks;
};

const char* WorldPersistenceRoundtripSchema();
const char* WorldStreamingSnapshotSchema();
const char* ChunkFormatValidationSchema();
const char* ChunkFormatContract();
const char* WorldHashSchema();
const char* WorldHashAlgorithm();
const char* EntitySnapshotArtifactSchema();

std::string SerializeWorldStreamingStateSnapshotJson(const WorldStreamingState& state);
bool LoadWorldStreamingStateSnapshotJson(
    const std::string& json_text,
    WorldStreamingState& out_state,
    std::vector<std::string>& errors);
bool ValidateWorldStreamingChunkFormatJson(
    const std::string& chunk_json,
    std::vector<std::string>& errors);

WorldPersistenceRoundtripAnalysis BuildWorldPersistenceRoundtripAnalysis(const std::string& build_preset);
std::string SerializeWorldPersistenceRoundtripJson(const WorldPersistenceRoundtripAnalysis& analysis);
bool WorldPersistenceRoundtripMeetsBaseline(const WorldPersistenceRoundtripAnalysis& analysis);
bool WriteWorldPersistenceRoundtripArtifact(
    const std::filesystem::path& output_path,
    const std::string& build_preset,
    std::vector<std::string>* errors = nullptr);
ChunkFormatValidationAnalysis BuildChunkFormatValidationAnalysis(const std::string& build_preset);
std::string SerializeChunkFormatValidationJson(const ChunkFormatValidationAnalysis& analysis);
bool ChunkFormatValidationMeetsBaseline(const ChunkFormatValidationAnalysis& analysis);
bool WriteChunkFormatValidationArtifact(
    const std::filesystem::path& output_path,
    const std::string& build_preset,
    std::vector<std::string>* errors = nullptr);
WorldHashAnalysis BuildWorldHashAnalysis(const std::string& build_preset);
std::string SerializeWorldHashJson(const WorldHashAnalysis& analysis);
bool WorldHashMeetsBaseline(const WorldHashAnalysis& analysis);
bool WriteWorldHashArtifact(
    const std::filesystem::path& output_path,
    const std::string& build_preset,
    std::vector<std::string>* errors = nullptr);
EntitySnapshotAnalysis BuildEntitySnapshotAnalysis(const std::string& build_preset);
std::string SerializeEntitySnapshotJson(const EntitySnapshotAnalysis& analysis);
bool EntitySnapshotMeetsBaseline(const EntitySnapshotAnalysis& analysis);
bool WriteEntitySnapshotArtifact(
    const std::filesystem::path& output_path,
    const std::string& build_preset,
    std::vector<std::string>* errors = nullptr);

} // namespace Luminumbra::Persistence
