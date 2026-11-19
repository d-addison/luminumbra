#pragma once

// Shared capture body for the standalone tool and registered runtime regressions.
// Test support only; never include from src/.

#include "RegionFixtureWorld.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/WorldStreamingState.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace Luminumbra::TestSupport {
namespace region_fixture_capture_detail {
namespace fs = std::filesystem;
namespace P = Persistence;
using world::GameSession;

inline nlohmann::json SubHashes(GameSession& session) {
    Luminumbra::WorldStreamingState state;
    // These scenarios contain at most two chunks; no simulation-tick traversal.
    for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks())
        state.insert_chunk(chunk);
    auto aether = session.GetAetherFieldSystem()->ComputeAetherSubHash();
    const auto energy = session.ComputeAetherStateSubHash();
    if (!energy.empty())
        aether = P::StableChecksum(aether + "|state:" + energy);
    return {{"chunk",
             P::ComputeWorldStreamingStateHash(
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

inline std::string WorldHash(const nlohmann::json& hashes) {
    return P::ComposeWorldHash(hashes.at("chunk"),
                               hashes.at("wind"),
                               hashes.at("weather"),
                               hashes.at("aether"),
                               hashes.at("scents"),
                               hashes.at("ecology"),
                               hashes.at("plants"));
}

inline nlohmann::json Member(const fs::path& path) {
    Luminumbra::Filesystem::ReplacementSafeInputFile input;
    if (!input.open(path))
        throw std::runtime_error("cannot open capture member");
    std::error_code error;
    const auto size = input.size(error);
    // Each fixture member is capped at 1 MiB before allocation or hex expansion.
    constexpr std::uint64_t kMaxMemberBytes = 1U << 20;
    if (error || size == 0 || size > kMaxMemberBytes)
        throw std::runtime_error("capture member size outside bound");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input.read(bytes.data(), bytes.size()))
        throw std::runtime_error("incomplete capture member");
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const char byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        encoded.push_back(digits[value >> 4]);
        encoded.push_back(digits[value & 15]);
    }
    return {{"bytes", size}, {"hex", encoded}, {"fnv64", P::StableChecksum(bytes)}};
}

} // namespace region_fixture_capture_detail

// Fresh sessions and their root remain owned by the caller so runtime regressions
// can compare the capture with the exact world that was saved and reopened.
inline nlohmann::json CaptureRegionFixture(RegionFixtureWorld& root,
                                           world::GameSession& session,
                                           world::GameSession& loaded,
                                           bool plant) {
    namespace fs = std::filesystem;
    namespace P = Persistence;
    using namespace region_fixture_capture_detail;
    root.Configure(session, plant);
    root.Configure(loaded, plant);
    const auto save = root.path() / "capture-save";
    if (plant) {
        RegionFixtureWorld::AddPlant(session);
        if (!session.SaveWorldStateTo(save))
            throw std::runtime_error("cannot save initial plant roster");
        RegionFixtureWorld::AddChunk(session, 0);
        RegionFixtureWorld::AddChunk(session, 32);
    } else {
        RegionFixtureWorld::AddChunk(session, 0);
        // Match the existing empty-world golden's seven fixed simulation steps.
        for (int tick = 0; tick < 7; ++tick) {
            if (session.TickSimulation(session.GetSimulationClock().fixed_dt()) != 1U)
                throw std::runtime_error("cannot advance capture world");
        }
    }
    if (!session.SaveWorldStateTo(save) || !loaded.LoadWorldStateFrom(save))
        throw std::runtime_error("cannot save or reopen capture");
    if (fs::exists(P::WorldSaveService::region_file_path(save, 1, 0)))
        throw std::runtime_error("clean second region was persisted");
    P::SnapshotIndex index;
    if (!P::read_snapshot_index(save, index, nullptr))
        throw std::runtime_error("invalid production snapshot index");
    nlohmann::json members = nlohmann::json::object();
    // Explicit inventory; never discover artifacts by globbing build output.
    constexpr std::array<std::string_view, 4> names{"chunks/region/plant-entities.json",
                                                    "chunks/region/r.0.0.lmr",
                                                    "chunks/region/snapshot.json",
                                                    "chunks/region/world-manifest.json"};
    for (const auto name : names) {
        if (name == "chunks/region/plant-entities.json" && !plant && index.generation == 0)
            continue;
        if (name == "chunks/region/snapshot.json" && index.generation == 0)
            continue;
        members[std::string(name)] = Member(save / name);
    }
    const auto live = SubHashes(session);
    const auto reopened = SubHashes(loaded);
    return {{"members", members},
            {"hashes",
             {{"live_world_hash", WorldHash(live)},
              {"loaded_world_hash", WorldHash(reopened)},
              {"live_subhashes", live},
              {"loaded_subhashes", reopened}}}};
}

} // namespace Luminumbra::TestSupport
