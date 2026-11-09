#pragma once

#include "ecs/EntitySnapshot.h"
#include "world/WorldStreamingState.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
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

// Process-local exact input identity for the canonical chunk serializer. Never persisted.
std::string ChunkSnapshotInputBytes(const Chunk& chunk);
// The size ChunkSnapshotInputBytes would return, computed without copying the arrays.
std::size_t ChunkSnapshotInputByteCount(const Chunk& chunk);
std::string SerializeChunkSnapshotJson(const Chunk& chunk);
// Authority snapshots retain ownership explicitly. Legacy callers keep their byte protocol.
std::string SerializeWorldStreamingStateSnapshotJson(const WorldStreamingState& state,
                                                     bool preserve_authority = false);
struct ChunkAuthorityLoadPolicy {
    // Indexed authority requires complete, bounded water continuation; legacy stays permissive.
    bool strict_water = false;
    bool terrain_edited = true;
    bool migrate_legacy_water = false;
    bool validate_record_authority = false;
    // With validation on: an unflagged header may still carry owned water (the loader's flag
    // repair keeps it), but it may not carry terrain ownership.
    bool allow_unflagged_water = false;
};
// Owner-thread eviction snapshot. Omits worker-owned staging arrays, which are
// discarded on admission; live simulation and render fields remain intact.
std::string SerializeChunkAuthoritySnapshotJson(const Chunk& chunk, bool omit_water = false);
// Region chunk record payload: one chunk, compact JSON, every array as little-endian base64
// (record_format 2). Storage only; hashes keep the canonical serializers above. The loader
// below accepts this form and the plain-JSON form.
std::string SerializeChunkRecordPayloadJson(const Chunk& chunk);
bool LoadWorldStreamingStateSnapshotJson(const std::string& json_text,
                                         WorldStreamingState& out_state,
                                         std::vector<std::string>& errors,
                                         const ChunkAuthorityLoadPolicy& authority = {});
bool ValidateWorldStreamingChunkFormatJson(const std::string& chunk_json,
                                           std::vector<std::string>& errors);
// Deterministic world hash over the canonical snapshot bytes (same
// fnv1a_64_stable_json machinery the WorldHash gate artifact records).
// Nonzero cursors append |waterSimCursor:v1: and a uint64 little-endian value
// to the canonical bytes. Zero/absent appends nothing, preserving legacy hashes.
std::string ComputeWorldStreamingStateHash(const WorldStreamingState& state,
                                           std::uint64_t water_sim_cursor = 0);

// Active-region freeze identity: voxel/material and water solver contents only.
// Caller filters incomplete live/parked inputs; durable edited records allow every lifecycle.
// Lifecycle, collider and render bookkeeping are excluded. Legacy hashes and
// persistence continue to use their existing projections.
// Incremental version of the same canonical region_content_v2 JSON projection.
// Append authoritative chunks in strictly increasing id order. Only one chunk's
// JSON is materialized at a time, including when reading a large durable region.
class RegionContentHasher {
public:
    RegionContentHasher();
    void append(const Chunk& chunk);
    [[nodiscard]] std::uint64_t value() const;
    [[nodiscard]] std::uint64_t terrain_value() const {
        return m_terrainHash;
    }
    [[nodiscard]] std::uint64_t water_value() const {
        return m_waterHash;
    }
    [[nodiscard]] bool empty() const {
        return !m_hasChunks;
    }

private:
    void feed(std::string_view bytes);
    std::uint64_t m_hash = 14695981039346656037ull;
    std::uint64_t m_waterHash = 14695981039346656037ull;
    std::uint64_t m_terrainHash = 14695981039346656037ull;
    ChunkID m_lastId = 0;
    bool m_hasChunks = false;
};
std::string ComputeRegionContentHash(const WorldStreamingState& state);

//  determinism contract: per-system sub-hashes for desync localization.
// Legacy world and sub-hashes remain byte-identical when the cursor is zero.
// Chunk fields use independent fnv1a_64_stable_json checksums over a PROJECTION of the same
// canonical per-chunk snapshot, grouping the persisted fields by subsystem so a future lockstep
// desync reports WHICH system diverged and at what tick (Factorio CRC-per-system playbook, research
// Area 2 takeaway 4). Same chunk-id-ascending ordering and same Checksum as the top-level hash, so
// sub-hashes are as reproducible as the whole.
struct WorldStreamingStateSubHashes {
    // Terrain field + heightmap + chunk identity/state (the worldgen output).
    std::string terrain;
    // Surface + water mesh geometry (the meshing output).
    std::string mesh;
    // Water fields checksum, plus a labelled cursor checksum only when nonzero.
    // The separate components allow checkpoint localization to name water/cursor.
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
    // (GameSession::ComputeAetherStateSubHash). The folded
    // |aether: composite embeds the tick-dependent re-derivable half, which the
    // heavy oracle recompute-and-excludes -- so the AUTHORITATIVE stateful
    // layer needs its own named slot for save/load + resim comparison (and
    // LREC1 localization) when sim.aether_state is ON. Empty when the layer is
    // off/absent/all-zero (the default world: always empty, byte-identical).
    std::string aether_state;
};

// Computes the chunk-derived per-system sub-hashes (terrain/mesh/water). Pure
// projection of the same snapshot and cursor the top-level hash uses. The
// entities field is left empty.
WorldStreamingStateSubHashes
ComputeWorldStreamingStateSubHashes(const WorldStreamingState& state,
                                    std::uint64_t water_sim_cursor = 0);

// Overload that also fills the entities sub-hash from a serialized ECS entity
// snapshot (the canonical SerializeEntityRegistrySnapshotJson output), checksummed
// with the SAME fnv1a_64_stable_json machinery as the chunk groups. Pass the
// empty-snapshot serialization for a terrain/water-only headless world.
WorldStreamingStateSubHashes
ComputeWorldStreamingStateSubHashes(const WorldStreamingState& state,
                                    const std::string& entity_snapshot_json,
                                    std::uint64_t water_sim_cursor = 0);

// Append-only runtime hash order; shared with the production server runner.
std::string ComposeWorldHash(const std::string& chunk_hash,
                             const std::string& wind_hash,
                             const std::string& weather_hash,
                             const std::string& aether_hash,
                             const std::string& scent_hash,
                             const std::string& ecology_hash,
                             const std::string& plant_hash);

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
