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

const char* WorldPersistenceRoundtripSchema();
const char* WorldStreamingSnapshotSchema();
const char* ChunkFormatValidationSchema();
const char* ChunkFormatContract();

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

} // namespace Luminumbra::Persistence
