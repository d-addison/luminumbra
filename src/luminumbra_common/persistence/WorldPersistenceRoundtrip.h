#pragma once

#include "ecs/EntitySnapshot.h"
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
bool LoadWorldStreamingStateSnapshotJson(const std::string& json_text,
                                         WorldStreamingState& out_state,
                                         std::vector<std::string>& errors);
bool ValidateWorldStreamingChunkFormatJson(const std::string& chunk_json,
                                           std::vector<std::string>& errors);
// Deterministic world hash over the canonical snapshot bytes (same
// fnv1a_64_stable_json machinery the WorldHash gate artifact records).
std::string ComputeWorldStreamingStateHash(const WorldStreamingState& state);

//  determinism contract: per-system sub-hashes for desync localization.
// The TOP-LEVEL world_hash (ComputeWorldStreamingStateHash above) is unchanged
// byte-for-byte; these are ADDITIVE. Each field is an independent
// fnv1a_64_stable_json checksum over a PROJECTION of the same canonical
// per-chunk snapshot, grouping the persisted fields by subsystem so a future
// lockstep desync reports WHICH system diverged and at what tick
// (Factorio CRC-per-system playbook, research Area 2 takeaway 4). Same
// chunk-id-ascending ordering and same Checksum as the top-level hash, so
// sub-hashes are as reproducible as the whole.
struct WorldStreamingStateSubHashes {
    // Terrain field + heightmap + chunk identity/state (the worldgen output).
    std::string terrain;
    // Surface + water mesh geometry (the meshing output).
    std::string mesh;
    // Water simulation level/flow/terrain-height fields and water flags.
    std::string water;
    // ECS entity snapshot hash (RNG-bearing sim entities live here). Empty when
    // no entity snapshot is supplied; ServerWorldRunner fills it from the
    // session registry.
    std::string entities;
    // the deterministic wind field cell values (its own
    // world_hash sub-hash slot, the deterministic runtime contract ). NOT chunk-derived: the
    // wind field lives on GameSession, so the runner supplies this string from
    // WindFieldSystem::ComputeWindSubHash. Empty when no wind field exists
    // (e.g. the persistence fixtures, which never construct one).
    std::string wind;
    // the deterministic weather state (region category map + storm
    // cells + precipitation field, with a reserved lightning strike-schedule
    // slot for ) -- its own world_hash sub-hash slot (the deterministic runtime contract
    // ). NOT chunk-derived: the weather core lives on GameSession, so the runner
    // supplies this string from WeatherSystem::ComputeWeatherSubHash. Empty when
    // no weather core exists (e.g. the persistence fixtures, which never make one).
    std::string weather;
    // the deterministic Aether scalar-field cell values -- its own
    // world_hash sub-hash slot. NOT chunk-derived: the field lives on
    // GameSession, so the runner supplies this from
    // AetherFieldSystem::ComputeAetherSubHash. Empty when no field exists
    // (e.g. the persistence fixtures, which never construct one).
    std::string aether;
    // the STATE-ONLY aether_state:v1: sub-hash
    // (GameSession::ComputeAetherStateSubHash). Codex finding A: the folded
    // |aether: composite embeds the tick-dependent re-derivable half, which the
    // heavy oracle recompute-and-excludes -- so the AUTHORITATIVE stateful
    // layer needs its own named slot for save/load + resim comparison (and
    // LREC1 localization) when sim.aether_state is ON. Empty when the layer is
    // off/absent/all-zero (the default world: always empty, byte-identical).
    std::string aether_state;
};

// Computes the chunk-derived per-system sub-hashes (terrain/mesh/water). Pure
// projection of the same snapshot the top-level hash uses; does not touch
// ComputeWorldStreamingStateHash. The entities field is left empty.
WorldStreamingStateSubHashes ComputeWorldStreamingStateSubHashes(const WorldStreamingState& state);

// Overload that also fills the entities sub-hash from a serialized ECS entity
// snapshot (the canonical SerializeEntityRegistrySnapshotJson output), checksummed
// with the SAME fnv1a_64_stable_json machinery as the chunk groups. Pass the
// empty-snapshot serialization for a terrain/water-only headless world.
WorldStreamingStateSubHashes
ComputeWorldStreamingStateSubHashes(const WorldStreamingState& state,
                                    const std::string& entity_snapshot_json);

// Exposes the determinism-stable checksum (fnv1a_64_stable_json) used by every
// world/sub hash so callers can hash auxiliary canonical strings (e.g. an
// entity snapshot) with the identical algorithm.
std::string StableChecksum(const std::string& canonical_text);

WorldPersistenceRoundtripAnalysis
BuildWorldPersistenceRoundtripAnalysis(const std::string& build_preset);
std::string
SerializeWorldPersistenceRoundtripJson(const WorldPersistenceRoundtripAnalysis& analysis);
bool WorldPersistenceRoundtripMeetsBaseline(const WorldPersistenceRoundtripAnalysis& analysis);
bool WriteWorldPersistenceRoundtripArtifact(const std::filesystem::path& output_path,
                                            const std::string& build_preset,
                                            std::vector<std::string>* errors = nullptr);
ChunkFormatValidationAnalysis BuildChunkFormatValidationAnalysis(const std::string& build_preset);
std::string SerializeChunkFormatValidationJson(const ChunkFormatValidationAnalysis& analysis);
bool ChunkFormatValidationMeetsBaseline(const ChunkFormatValidationAnalysis& analysis);
bool WriteChunkFormatValidationArtifact(const std::filesystem::path& output_path,
                                        const std::string& build_preset,
                                        std::vector<std::string>* errors = nullptr);
WorldHashAnalysis BuildWorldHashAnalysis(const std::string& build_preset);
std::string SerializeWorldHashJson(const WorldHashAnalysis& analysis);
bool WorldHashMeetsBaseline(const WorldHashAnalysis& analysis);
bool WriteWorldHashArtifact(const std::filesystem::path& output_path,
                            const std::string& build_preset,
                            std::vector<std::string>* errors = nullptr);
// the game-flavored snapshot fixture moved to test-support code
// (test/support/EntitySnapshotFixture.h); the analysis/artifact builders now
// receive the fixture registry as a parameter.
EntitySnapshotAnalysis
BuildEntitySnapshotAnalysis(const std::string& build_preset,
                            const Luminumbra::Ecs::EntityRegistrySnapshot& fixture);
std::string SerializeEntitySnapshotJson(const EntitySnapshotAnalysis& analysis);
bool EntitySnapshotMeetsBaseline(const EntitySnapshotAnalysis& analysis);
bool WriteEntitySnapshotArtifact(const std::filesystem::path& output_path,
                                 const std::string& build_preset,
                                 const Luminumbra::Ecs::EntityRegistrySnapshot& fixture,
                                 std::vector<std::string>* errors = nullptr);

} // namespace Luminumbra::Persistence
