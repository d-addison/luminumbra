#include "../support/RegionFixtureWorld.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/WorldStreamingState.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {
namespace fs = std::filesystem;
namespace P = Luminumbra::Persistence;
using Luminumbra::TestSupport::RegionFixtureWorld;
using Luminumbra::world::GameSession;

nlohmann::json SubHashes(GameSession& session) {
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

std::string WorldHash(const nlohmann::json& hashes) {
    return P::ComposeWorldHash(hashes.at("chunk"),
                               hashes.at("wind"),
                               hashes.at("weather"),
                               hashes.at("aether"),
                               hashes.at("scents"),
                               hashes.at("ecology"),
                               hashes.at("plants"));
}

nlohmann::json Member(const fs::path& path) {
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

nlohmann::json Capture(bool plant) {
    RegionFixtureWorld root;
    GameSession session, loaded;
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

bool IsWithinSourceTree(const fs::path& path) {
    const auto root = fs::canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
    // Compare filesystem identities, including Windows case and short-name aliases.
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        if (fs::equivalent(ancestor, root))
            return true;
        if (ancestor == ancestor.parent_path())
            break;
    }
    return false;
}

bool RequestedCapture(fs::path& destination) {
#if defined(_MSC_VER)
    wchar_t* value = nullptr;
    std::size_t size = 0;
    const auto result = _wdupenv_s(&value, &size, L"LUMINUMBRA_REGION_FIXTURE_CAPTURE");
    const std::unique_ptr<wchar_t, decltype(&std::free)> requested(value, &std::free);
    if (result != 0)
        throw std::runtime_error("cannot read capture destination");
    if (requested == nullptr)
        return false;
    destination = fs::path(requested.get());
#elif defined(_WIN32)
    const wchar_t* requested = _wgetenv(L"LUMINUMBRA_REGION_FIXTURE_CAPTURE");
    if (requested == nullptr)
        return false;
    destination = fs::path(requested);
#else
    const char* requested = std::getenv("LUMINUMBRA_REGION_FIXTURE_CAPTURE");
    if (requested == nullptr)
        return false;
    destination = Luminumbra::Filesystem::PathFromUtf8(requested);
#endif
    return true;
}

TEST(RegionFixtureCapture, Emit) {
    // Dormant during normal discovery/CI: no world, jobs or files are created.
    fs::path destination;
    if (!RequestedCapture(destination))
        return;
    ASSERT_TRUE(Luminumbra::Filesystem::IsAbsolutePath(destination));
    const auto parent = fs::canonical(destination.parent_path());
    destination = parent / destination.filename();
    ASSERT_FALSE(IsWithinSourceTree(parent)) << "capture destination must be outside source tree";
    ASSERT_FALSE(fs::exists(destination)) << "capture destination must be new";
    const nlohmann::json capture{
        {"schema", "luminumbra.region_fixture_capture.v1"},
        {"scenarios", {{"devel-off", Capture(false)}, {"devel-off-plant-edit", Capture(true)}}}};
    const auto payload = capture.dump() + '\n';
    // Match the regenerator's 16 MiB total input limit before staging any output.
    ASSERT_LE(payload.size(), 16U << 20);
    // Stage on the destination filesystem, close and check before atomic rename.
    std::error_code error;
    const auto staging = Luminumbra::TestSupport::detail::ReserveTempDirectory(
        parent, Luminumbra::TestSupport::detail::UniqueTempStem("region_capture_"), error);
    ASSERT_FALSE(staging.empty()) << error.message();
    try {
        const auto temporary = staging / "capture.json";
        std::ofstream output(temporary, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << payload;
        output.close();
        fs::rename(temporary, destination);
    } catch (...) {
        fs::remove_all(staging, error);
        throw;
    }
    fs::remove_all(staging, error);
    ASSERT_FALSE(error) << error.message();
}
} // namespace
