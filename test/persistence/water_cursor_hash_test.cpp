#include "../support/UniqueTempPath.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/HashCheckpoint.h"
#include "luminumbra_common/core/ResidencyContract.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/WorldClock.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "luminumbra_server/HeavyComparison.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <ios>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace Luminumbra;
namespace P = Persistence;

std::string IndependentChecksum(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string LegacyWorldHashInput(const WorldStreamingState& state) {
    auto projected = nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(state));
    for (auto& chunk : projected.at("chunks")) {
        for (const auto& entry : luminumbra::core::kChunkFieldResidency) {
            if (!luminumbra::core::MayFeedWorldHash(entry.residency)) {
                chunk.erase(entry.field);
                chunk.at("water_state").erase(entry.field);
            }
        }
    }
    projected["hash_scope"] = "sim_truth_no_render_mesh_v1";
    return projected.dump(4) + "\n";
}

TEST(WaterCursorHash, CanonicalEncodingAndZeroLegacyBytes) {
    const std::array<std::uint64_t, 6> cursors{
        0, 1, 65, 256, 0x0102030405060708ull, std::numeric_limits<std::uint64_t>::max()};
    for (const bool populated : {false, true}) {
        SCOPED_TRACE(populated);
        WorldStreamingState state;
        if (populated) {
            auto chunk = state.get_or_create_chunk(IVec3(0, 0, 0));
            chunk->set_state(ChunkState::Idle);
            chunk->water_depth_mm = {17, 23};
        }
        const auto legacy_bytes = LegacyWorldHashInput(state);
        const auto legacy_hash = IndependentChecksum(legacy_bytes);
        const auto legacy_sub = P::ComputeWorldStreamingStateSubHashes(state);
        EXPECT_EQ(P::ComputeWorldStreamingStateHash(state), legacy_hash);
        EXPECT_EQ(P::ComputeWorldStreamingStateHash(state, 0), legacy_hash);
        EXPECT_EQ(P::ComputeWorldStreamingStateSubHashes(state, 0).water, legacy_sub.water);
        if (!populated) {
            // Independent legacy water projection, including exact whitespace.
            const nlohmann::json water{{"section", "water"},
                                       {"order_contract", "chunk_id_ascending"},
                                       {"chunk_count", 0},
                                       {"chunks", nlohmann::json::array()}};
            EXPECT_EQ(legacy_sub.water, IndependentChecksum(water.dump(4) + "\n"));
        }
        for (const auto cursor : cursors) {
            SCOPED_TRACE(cursor);
            std::string cursor_bytes;
            if (cursor != 0) {
                cursor_bytes = "|waterSimCursor:v1:";
                auto remaining = cursor;
                for (unsigned byte = 0; byte < 8; ++byte) {
                    cursor_bytes.push_back(static_cast<char>(remaining % 256));
                    remaining /= 256;
                }
            }
            const auto hash = P::ComputeWorldStreamingStateHash(state, cursor);
            EXPECT_EQ(hash, IndependentChecksum(legacy_bytes + cursor_bytes));
            EXPECT_EQ(P::WorldSaveService{}.world_hash(state, cursor), hash);
            const auto sub = P::ComputeWorldStreamingStateSubHashes(state, "entities", cursor);
            EXPECT_EQ(sub.terrain, legacy_sub.terrain);
            EXPECT_EQ(sub.mesh, legacy_sub.mesh);
            EXPECT_EQ(sub.entities, IndependentChecksum("entities"));
            if (cursor == 0) {
                EXPECT_EQ(sub.water, legacy_sub.water);
            } else {
                EXPECT_NE(hash, legacy_hash);
                EXPECT_EQ(sub.water,
                          legacy_sub.water +
                              "|waterSimCursor:v1:" + IndependentChecksum(cursor_bytes));
                EXPECT_EQ(WaterHashDifferenceSection(sub.water, legacy_sub.water), "water/cursor");
                EXPECT_EQ(WaterHashDifferenceSection(legacy_sub.water, sub.water), "water/cursor");
                if (cursor != cursors.back()) {
                    const auto other = P::ComputeWorldStreamingStateSubHashes(state, cursor + 1);
                    EXPECT_NE(other.water, sub.water);
                    EXPECT_EQ(WaterHashDifferenceSection(other.water, sub.water), "water/cursor");
                }
            }
        }
    }
}

TEST(WaterCursorHash, HeavyAndReplayAndWireCompareTheSameWaterCursor) {
    const WorldStreamingState state;
    Server::HeavyHashes first;
    first.world_hash = P::ComputeWorldStreamingStateHash(state, 65);
    first.sub = P::ComputeWorldStreamingStateSubHashes(state, 65);
    auto second = first;
    second.world_hash = P::ComputeWorldStreamingStateHash(state, 66);
    second.sub = P::ComputeWorldStreamingStateSubHashes(state, 66);
    for (const bool active_regions : {false, true}) {
        first.active_regions = second.active_regions = active_regions;
        EXPECT_FALSE(Server::AuthoritativeStateEqual(first, second));
        EXPECT_TRUE(Server::MeshEqual(first, second));
    }
    const TestSupport::ScopedTempFile stream("water-cursor-", ".lrec1");
    Replay::ReplayHeader header;
    header.start_world_hash = first.world_hash;
    Replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(stream.Path().string(), header));
    const HashCheckpoint checkpoint{.tick = 30,
                                    .world_hash = first.world_hash,
                                    .terrain = first.sub.terrain,
                                    .water = first.sub.water,
                                    .entities = first.sub.entities};
    writer.RecordCheckpoint(checkpoint);
    ASSERT_TRUE(writer.Finalize(30));
    const auto replay = Replay::ReadReplay(stream.Path().string());
    ASSERT_TRUE(replay.has_value());
    if (!replay.has_value())
        return;
    ASSERT_FALSE(replay->truncated);
    ASSERT_EQ(replay->checkpoints.size(), 1u);
    const auto& recorded = replay->checkpoints.front();
    EXPECT_EQ(recorded.world_hash, first.world_hash);
    EXPECT_EQ(recorded.water, first.sub.water);
    EXPECT_EQ(WaterHashDifferenceSection(recorded.water, second.sub.water), "water/cursor");
    Net::HashMsg decoded;
    ASSERT_TRUE(Net::DecodeHash(Net::EncodeHash(checkpoint), decoded));
    EXPECT_EQ(decoded.world_hash, recorded.world_hash);
    EXPECT_EQ(decoded.water, recorded.water);
}

TEST(WaterCursorHash, UnknownAndPartialCheckpointComponentsRemainWaterDifferences) {
    const std::string base = "0123456789abcdef";
    const auto cursor = base + "|waterSimCursor:v1:0123456789abcdef";
    EXPECT_EQ(WaterHashDifferenceSection(base, base), "water");
    for (const std::string_view invalid :
         {"", "0", "0123456789abcdef|", "0123456789abcdef|unknown:v1:0123456789abcdef"}) {
        EXPECT_EQ(WaterHashDifferenceSection(cursor, invalid), "water");
        EXPECT_EQ(WaterHashDifferenceSection(invalid, cursor), "water");
    }
    EXPECT_EQ(WaterHashDifferenceSection(cursor, "1123456789abcdef"), "water");
    EXPECT_EQ(WaterHashDifferenceSection(cursor, cursor.substr(0, cursor.size() - 1)), "water");
}

TEST(WaterCursorHash, SavedMetadataAndLoadedChunksReproduceTheSameHash) {
    const TestSupport::ScopedTempFile workspace("water-cursor-save-", ".unused");
    const auto save = workspace.Path().parent_path() / "world";
    const P::WorldSaveService service;
    WorldStreamingState original;
    auto chunk = original.get_or_create_chunk(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Idle);
    constexpr std::size_t cursor = 65;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(original, save, &errors));
    ASSERT_TRUE(service.save_metadata(
        nlohmann::json{{"container_version", P::WorldSaveService::kContainerVersion},
                       {"waterSimCursor", cursor}}
            .dump(),
        save,
        &errors));
    WorldStreamingState loaded;
    ASSERT_TRUE(service.load_world(loaded, save, errors));
    world::WorldClock clock;
    bool active_regions = false;
    std::size_t restored_cursor = 0;
    ASSERT_TRUE(service.read_clock_metadata(
        save, clock, active_regions, &errors, nullptr, nullptr, &restored_cursor));
    ASSERT_TRUE(errors.empty());
    EXPECT_EQ(restored_cursor, cursor);
    EXPECT_EQ(service.world_hash(loaded, restored_cursor), service.world_hash(original, cursor));
    EXPECT_EQ(P::ComputeWorldStreamingStateSubHashes(loaded, restored_cursor).water,
              P::ComputeWorldStreamingStateSubHashes(original, cursor).water);
    EXPECT_NE(service.world_hash(loaded), service.world_hash(loaded, restored_cursor));

    const auto metadata = save / "world_info.json";
    for (const std::string_view invalid :
         {"{", R"({"waterSimCursor":)", R"({"waterSimCursor":"0"})"}) {
        ASSERT_TRUE(service.write_file_durably(metadata, std::string(invalid)));
        errors.clear();
        EXPECT_FALSE(service.read_clock_metadata(
            save, clock, active_regions, &errors, nullptr, nullptr, &restored_cursor));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(service.world_hash(loaded, cursor), service.world_hash(original, cursor));
    }
    ASSERT_TRUE(std::filesystem::remove(metadata));
    errors.clear();
    ASSERT_TRUE(service.read_clock_metadata(
        save, clock, active_regions, &errors, nullptr, nullptr, &restored_cursor));
    EXPECT_EQ(restored_cursor, 0u);
    EXPECT_EQ(service.world_hash(loaded, restored_cursor), service.world_hash(loaded));
    ASSERT_TRUE(std::filesystem::create_directory(metadata));
    EXPECT_FALSE(service.read_clock_metadata(
        save, clock, active_regions, &errors, nullptr, nullptr, &restored_cursor));
    EXPECT_FALSE(errors.empty());
}
} // namespace
