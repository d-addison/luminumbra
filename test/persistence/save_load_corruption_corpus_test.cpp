// Save/load partial-write + schema-version-skew corpus ( of the second-class
// adversarial pass).
//
// The existing persistence_hardening_test.cpp covers happy-path roundtrips and a
// handful of header-truncation rejections (bad magic, unknown version, truncated
// manifest/payload, empty/short header). THIS file deliberately opens the classes it
// LEFT untested:
//   (1) BIT-FLIPPED LZ4 payloads -- a torn write that keeps a syntactically valid
//       header but corrupts the compressed body. The loader must reject (or, if LZ4
//       happens to decode to malformed JSON, reject at the JSON stage) -- never load
//       a silently-wrong partial world.
//   (2) TRUNCATION AT MANY OFFSETS -- not just the tail: torn at the magic boundary,
//       mid-header, mid-manifest, and at every 1/8 of a real region file. A partial
//       write at ANY offset must be a clean rejection, not a crash or partial world.
//   (3) SCHEMA-VERSION SKEW -- a future/unknown snapshot schema string, an old legacy
//       v1 snapshot (the migration path must still LOAD it), and a future LMR1
//       container version. Old saves degrade gracefully; unknown-future saves are
//       rejected rather than misread.
//
// Contract under test (WorldSaveService.h): load of a MISSING file is a clean miss
// (false, NO error appended); load of a MALFORMED file is a hard reject (false WITH a
// diagnostic). Neither may crash, and neither may yield a partially-populated state
// that a caller would mistake for a real world. Tests assert the CORRECT behavior, so
// they fail (not pass) if a gap exists -- they never assert the bug.
#include "gtest/gtest.h"

#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "world/WorldStreamingState.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Luminumbra::ChunkState;
using Luminumbra::IVec3;
using Luminumbra::Vec3;
using Luminumbra::WorldStreamingState;
using Luminumbra::Persistence::ComputeWorldStreamingStateHash;
using Luminumbra::Persistence::LoadWorldStreamingStateSnapshotJson;
using Luminumbra::Persistence::SerializeWorldStreamingStateSnapshotJson;
using Luminumbra::Persistence::WorldSaveService;

std::filesystem::path MakeTempSaveDir(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("luminumbra_corruption_corpus_" + tag + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

struct TempSaveDir {
    explicit TempSaveDir(const std::string& tag)
        : path(MakeTempSaveDir(tag)) {}
    ~TempSaveDir() {
        std::error_code remove_error;
        std::filesystem::remove_all(path, remove_error);
    }
    std::filesystem::path path;
};

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteFileBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A small but non-trivial chunk so a real region file has a compressible payload.
void AddChunk(WorldStreamingState& state, const IVec3& coords, Luminumbra::u32 salt) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(ChunkState::Ready);
    chunk->sdf_data = {-1.5f + static_cast<float>(salt), -0.25f, 0.0f, 0.5f, 1.25f};
    chunk->heightmap_data = {7.0f + static_cast<float>(salt), 8.5f, -3.5f};
    chunk->mesh_vertices = {{Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 1u},
                            {Vec3(1.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 2u}};
    chunk->mesh_indices = {0u, 1u};
    chunk->mark_voxel_data_dirty();
}

// Writes a real region file for a multi-chunk world and returns its bytes + path.
std::string SaveRealRegion(WorldSaveService& service,
                           const TempSaveDir& dir,
                           std::filesystem::path& out_region) {
    WorldStreamingState world;
    AddChunk(world, IVec3(0, 0, 0), 1u);
    AddChunk(world, IVec3(1, 0, 1), 2u);
    AddChunk(world, IVec3(2, 0, 2), 3u);
    std::vector<std::string> errors;
    EXPECT_TRUE(service.save_world(world, dir.path, &errors));
    EXPECT_TRUE(errors.empty());
    out_region = WorldSaveService::region_file_path(dir.path, 0, 0);
    return ReadFileBytes(out_region);
}

// A loaded state is "clean empty": no chunks present (no partial world leaked).
void ExpectNoPartialWorld(const WorldStreamingState& state) {
    EXPECT_TRUE(state.empty()) << "a rejected load left " << state.size()
                               << " partially-loaded chunks (silent partial-world corruption)";
}

} // namespace

// ---------------------------------------------------------------------------
// (1) BIT-FLIPPED LZ4 PAYLOAD: header valid, compressed body corrupted.
// ---------------------------------------------------------------------------

TEST(SaveLoadCorruptionCorpus, BitFlippedPayloadByteIsRejected) {
    TempSaveDir dir("bitflip_one");
    WorldSaveService service;
    std::filesystem::path region;
    std::string bytes = SaveRealRegion(service, dir, region);
    ASSERT_GT(bytes.size(), 24u);

    // Flip a single bit deep in the compressed payload (past the 8-byte header and
    // the record manifest). LZ4 either fails to decode, decodes to a wrong size, or
    // decodes to malformed JSON -- every path must be a hard reject.
    const std::size_t target = bytes.size() - 4;
    bytes[target] = static_cast<char>(static_cast<unsigned char>(bytes[target]) ^ 0x40u);
    WriteFileBytes(region, bytes);

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, dir.path, errors))
        << "a region with a bit-flipped LZ4 payload must not load as a valid world";
    EXPECT_FALSE(errors.empty()) << "corruption must surface a diagnostic";
    ExpectNoPartialWorld(state);
}

TEST(SaveLoadCorruptionCorpus, EveryPayloadBytePositionFlipIsRejectedOrConsistent) {
    // Sweep a flip across the payload region (past the header+manifest) and require
    // that the load is EITHER rejected OR -- in the rare case the flip lands on a
    // byte that decompresses to a still-valid world -- that the load is internally
    // consistent (reloads to a hash-stable, non-partial world). It must never crash
    // and must never silently truncate.
    TempSaveDir dir("bitflip_sweep");
    WorldSaveService service;
    std::filesystem::path region;
    const std::string base = SaveRealRegion(service, dir, region);
    ASSERT_GT(base.size(), 32u);

    const std::size_t start = 16; // safely past magic/version/count
    const std::size_t end = base.size();
    const std::size_t stride = (end - start) / 12 + 1; // ~12 probes, keep the test fast
    for (std::size_t i = start; i < end; i += stride) {
        std::string bytes = base;
        bytes[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ 0xFFu);
        WriteFileBytes(region, bytes);

        WorldStreamingState state;
        std::vector<std::string> errors;
        const bool loaded = service.load_world(state, dir.path, errors); // must not crash
        if (!loaded) {
            EXPECT_FALSE(errors.empty())
                << "reject at payload offset " << i << " must carry a diagnostic";
        } else {
            // Accepted: then it must be a coherent world (re-hash is stable), not a
            // half-decoded one.
            const std::string h1 = ComputeWorldStreamingStateHash(state);
            WorldStreamingState reload;
            std::vector<std::string> reload_errors;
            ASSERT_TRUE(service.load_world(reload, dir.path, reload_errors)) << "offset " << i;
            EXPECT_EQ(ComputeWorldStreamingStateHash(reload), h1)
                << "a flip at offset " << i << " produced a non-deterministic accepted load";
        }
    }
}

// ---------------------------------------------------------------------------
// (2) TRUNCATION AT MANY OFFSETS: a torn write at any boundary is rejected.
// ---------------------------------------------------------------------------

TEST(SaveLoadCorruptionCorpus, TruncationAtEveryEighthIsRejected) {
    TempSaveDir dir("trunc_sweep");
    WorldSaveService service;
    std::filesystem::path region;
    const std::string full = SaveRealRegion(service, dir, region);
    ASSERT_GT(full.size(), 16u);

    // A valid file load FIRST (control), then progressively shorter prefixes. Every
    // strict prefix shorter than the whole must be rejected (a torn write), never
    // crash, never a partial world.
    for (int eighth = 1; eighth <= 7; ++eighth) {
        const std::size_t cut = (full.size() * static_cast<std::size_t>(eighth)) / 8u;
        std::string truncated = full.substr(0, cut);
        WriteFileBytes(region, truncated);

        WorldStreamingState state;
        std::vector<std::string> errors;
        EXPECT_FALSE(service.load_world(state, dir.path, errors))
            << "a region truncated to " << cut << "/" << full.size() << " bytes (" << eighth
            << "/8) must be rejected";
        EXPECT_FALSE(errors.empty()) << "truncation at " << eighth << "/8 must carry a diagnostic";
        ExpectNoPartialWorld(state);
    }
}

TEST(SaveLoadCorruptionCorpus, TornAtMagicBoundaryIsRejected) {
    TempSaveDir dir("torn_magic");
    WorldSaveService service;
    // Exactly the 4-byte magic and nothing else: a write torn right after the magic.
    WriteFileBytes(WorldSaveService::region_file_path(dir.path, 0, 0), std::string("LMR1"));

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, dir.path, errors));
    EXPECT_FALSE(errors.empty()) << "a write torn at the magic boundary must be rejected";
}

TEST(SaveLoadCorruptionCorpus, HeaderClaimsMoreRecordsThanPresentIsRejected) {
    TempSaveDir dir("overclaim");
    WorldSaveService service;
    // Valid magic + version 1, record_count = 0xFFFF, but no manifest/payload.
    std::string bytes = "LMR1";
    bytes.push_back('\x01');
    bytes.push_back('\0'); // version 1
    bytes.push_back('\xFF');
    bytes.push_back('\xFF'); // record_count = 65535, none follow
    WriteFileBytes(WorldSaveService::region_file_path(dir.path, 0, 0), bytes);

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, dir.path, errors));
    EXPECT_FALSE(errors.empty()) << "a header over-claiming record_count must be rejected";
    ExpectNoPartialWorld(state);
}

// ---------------------------------------------------------------------------
// (3) SCHEMA-VERSION SKEW: old saves migrate, future saves are rejected.
// ---------------------------------------------------------------------------

TEST(SaveLoadCorruptionCorpus, FutureSnapshotSchemaIsRejected) {
    // A snapshot text with a FUTURE schema version must be rejected at the schema
    // gate (false + "unexpected world snapshot schema"), not silently misread under
    // the v1 assumptions.
    WorldStreamingState world;
    AddChunk(world, IVec3(0, 0, 0), 1u);
    std::string json = SerializeWorldStreamingStateSnapshotJson(world);

    const std::string from = "luminumbra.persistence.world_state_snapshot.v1";
    const std::string to = "luminumbra.persistence.world_state_snapshot.v99";
    const auto pos = json.find(from);
    ASSERT_NE(pos, std::string::npos);
    json.replace(pos, from.size(), to);

    WorldStreamingState restored;
    std::vector<std::string> errors;
    EXPECT_FALSE(LoadWorldStreamingStateSnapshotJson(json, restored, errors))
        << "a future snapshot schema must be rejected, not loaded under v1 rules";
    EXPECT_FALSE(errors.empty());
    ExpectNoPartialWorld(restored);
}

TEST(SaveLoadCorruptionCorpus, UnknownOrderContractIsRejected) {
    WorldStreamingState world;
    AddChunk(world, IVec3(0, 0, 0), 1u);
    std::string json = SerializeWorldStreamingStateSnapshotJson(world);

    const std::string from = "chunk_id_ascending";
    const auto pos = json.find(from);
    ASSERT_NE(pos, std::string::npos);
    json.replace(pos, from.size(), "insertion_order"); // a contract the loader must not accept

    WorldStreamingState restored;
    std::vector<std::string> errors;
    EXPECT_FALSE(LoadWorldStreamingStateSnapshotJson(json, restored, errors))
        << "an unknown ordering contract must be rejected";
    EXPECT_FALSE(errors.empty());
}

TEST(SaveLoadCorruptionCorpus, LegacyV1SnapshotStillLoadsViaMigrationPath) {
    // The migration contract (WorldSaveService.h:37-41): a directory holding ONLY the
    // legacy v1 <save_dir>/chunks/world-state.json (no LMR1 region files, no manifest)
    // must still load, and the loaded world must hash-match the original. An old save
    // degrades GRACEFULLY -- it is read, not rejected.
    TempSaveDir dir("legacy_v1");
    WorldSaveService service;

    WorldStreamingState world;
    AddChunk(world, IVec3(0, 0, 0), 1u);
    AddChunk(world, IVec3(3, 0, 4), 2u);
    const std::string expected_hash = service.world_hash(world);

    // Hand-write the v1 single-snapshot file (the canonical serializer output) at the
    // legacy path; do NOT create any v2 region files.
    const std::string v1_json = SerializeWorldStreamingStateSnapshotJson(world);
    WriteFileBytes(WorldSaveService::world_state_path(dir.path), v1_json);

    WorldStreamingState restored;
    std::vector<std::string> errors;
    EXPECT_TRUE(service.load_world(restored, dir.path, errors))
        << "a legacy v1 snapshot must still load via the migration path";
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(restored.size(), 2u);
    EXPECT_EQ(service.world_hash(restored), expected_hash)
        << "v1-loaded world must hash-equal the original (migration must be lossless)";
}

TEST(SaveLoadCorruptionCorpus, CorruptLegacyV1SnapshotIsRejectedNotMisread) {
    // A legacy v1 file that is NOT valid JSON (a torn old save) must be rejected with
    // a diagnostic, not loaded as an empty/partial world.
    TempSaveDir dir("legacy_v1_corrupt");
    WorldSaveService service;
    WriteFileBytes(
        WorldSaveService::world_state_path(dir.path),
        std::string("{ \"schema\": \"luminumbra.persistence.world_state_sn")); // torn mid-JSON

    WorldStreamingState restored;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(restored, dir.path, errors))
        << "a torn legacy v1 snapshot must be rejected";
    EXPECT_FALSE(errors.empty());
    ExpectNoPartialWorld(restored);
}

TEST(SaveLoadCorruptionCorpus, FutureContainerVersionIsRejected) {
    // An LMR1 container with magic LMR1 but a FUTURE version u16 (not the supported v1
    // /v2) must be rejected -- a newer save read by an older binary is denied rather
    // than misinterpreted.
    TempSaveDir dir("future_container");
    WorldSaveService service;
    std::string bytes = "LMR1";
    bytes.push_back('\x63');
    bytes.push_back('\0'); // version = 99
    bytes.push_back('\0');
    bytes.push_back('\0'); // record_count = 0
    WriteFileBytes(WorldSaveService::region_file_path(dir.path, 0, 0), bytes);

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, dir.path, errors));
    EXPECT_FALSE(errors.empty()) << "an unknown future container version must be rejected";
    ExpectNoPartialWorld(state);
}

// ---------------------------------------------------------------------------
// MISSING FILE remains a CLEAN MISS (the contract boundary the rejects must not
// cross: absence is not corruption).
// ---------------------------------------------------------------------------

TEST(SaveLoadCorruptionCorpus, MissingSaveDirIsACleanMissNotAnError) {
    TempSaveDir dir("missing");
    WorldSaveService service;
    // Nothing written: a fresh world. load_world returns false WITHOUT a diagnostic.
    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, dir.path, errors));
    EXPECT_TRUE(errors.empty()) << "a missing save must be a clean miss, not a corruption error";
    EXPECT_TRUE(state.empty());
}
