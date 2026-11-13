#pragma once

// Test-support code only: never include from src/.

#include "UniqueTempPath.h"
#include "WorldgenTestData.h"

#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace Luminumbra::TestSupport {

namespace fs = std::filesystem;
using world::GameSession;

inline fs::path SourcePresetPath() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds" / "atlas" / "presets" / "default.json";
}

// Temp root containing the preset and worldgen data, with no client assets.
// The root is unique per fixture instance so
// concurrent common_tests processes never share (or clobber) a directory; each
// TEST builds ONE instance and reuses it, so within-run path stability still
// holds. The destructor removes the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        root_ =
            Luminumbra::TestSupport::CreateUniqueTempDirectory("luminumbra_headless_world_test_");
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(SourcePresetPath(), root_ / "worlds" / "atlas" / "presets" / "default.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root_);
    }
    ~HeadlessRoot() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    [[nodiscard]] const fs::path& path() const {
        return root_;
    }
    // GameSession::SetRootPath expects a trailing separator (paths are
    // concatenated, not joined).
    [[nodiscard]] std::string root_string() const {
        return root_.string() + static_cast<char>(fs::path::preferred_separator);
    }

private:
    fs::path root_;
};

inline std::string ReadClockTestFile(const fs::path& path) {
    // Consuming rdbuf() mutates the buffer; keep its owning stream mutable.
    std::ifstream input;
    input.open(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

inline nlohmann::json SessionWorldSubHashes(GameSession& session) {
    Luminumbra::WorldStreamingState state;
    for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks())
        state.insert_chunk(chunk);
    std::string aether = session.GetAetherFieldSystem()->ComputeAetherSubHash();
    const auto energy = session.ComputeAetherStateSubHash();
    if (!energy.empty())
        aether = Luminumbra::Persistence::StableChecksum(aether + "|state:" + energy);
    return {{"chunk",
             session.ActiveRegionsEnabled()
                 ? session.ComputeChunkStateHash()
                 : Luminumbra::Persistence::ComputeWorldStreamingStateHash(
                       state, session.GetWorldSystem()->GetWaterSimWindowCursor())},
            {"wind", session.GetWindFieldSystem()->ComputeWindSubHash()},
            {"weather", session.GetWeatherSystem()->ComputeWeatherSubHash()},
            {"aether", aether},
            {"scents", session.ComputeScentSubHash()},
            {"ecology",
             session.FoldClockIntoEcologyHash(
                 luminumbra::ai::ComputeEcologySubHash(session.GetRegistry()))},
            {"plants", session.ComputePlantSubHash()}};
}

inline std::string SessionWorldHash(GameSession& session) {
    const auto hashes = SessionWorldSubHashes(session);
    return Luminumbra::Persistence::ComposeWorldHash(hashes.at("chunk"),
                                                     hashes.at("wind"),
                                                     hashes.at("weather"),
                                                     hashes.at("aether"),
                                                     hashes.at("scents"),
                                                     hashes.at("ecology"),
                                                     hashes.at("plants"));
}

inline entt::entity ClockTestPlant(entt::registry& registry, const Luminumbra::Vec3& position) {
    const auto plant = registry.create();
    registry.emplace<Luminumbra::Components::PlantTag>(plant);
    registry.emplace<Luminumbra::Components::PlantGenomeComponent>(plant).genes.fill(0.5f);
    registry.emplace<Luminumbra::Components::PlantGrowthComponent>(plant);
    registry.emplace<Luminumbra::Components::TransformComponent>(plant).position = position;
    return plant;
}

inline void AdoptRegionGoldenChunk(GameSession& session, int x) {
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {
        8.0f + static_cast<float>(x), 8.5f + static_cast<float>(x), 9.0f + static_cast<float>(x)};
    if (x == 0)
        chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
}

inline void ExpectSameWorldHash(GameSession& actual, GameSession& expected) {
    EXPECT_EQ(SessionWorldHash(actual), SessionWorldHash(expected))
        << "actual: " << SessionWorldSubHashes(actual).dump()
        << "\nexpected: " << SessionWorldSubHashes(expected).dump();
}

} // namespace Luminumbra::TestSupport
