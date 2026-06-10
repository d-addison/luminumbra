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

const char* WorldPersistenceRoundtripSchema();
const char* WorldStreamingSnapshotSchema();

std::string SerializeWorldStreamingStateSnapshotJson(const WorldStreamingState& state);
bool LoadWorldStreamingStateSnapshotJson(
    const std::string& json_text,
    WorldStreamingState& out_state,
    std::vector<std::string>& errors);

WorldPersistenceRoundtripAnalysis BuildWorldPersistenceRoundtripAnalysis(const std::string& build_preset);
std::string SerializeWorldPersistenceRoundtripJson(const WorldPersistenceRoundtripAnalysis& analysis);
bool WorldPersistenceRoundtripMeetsBaseline(const WorldPersistenceRoundtripAnalysis& analysis);
bool WriteWorldPersistenceRoundtripArtifact(
    const std::filesystem::path& output_path,
    const std::string& build_preset,
    std::vector<std::string>* errors = nullptr);

} // namespace Luminumbra::Persistence
