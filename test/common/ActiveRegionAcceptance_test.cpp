#include "../support/ActiveRegionAcceptanceWorld.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using Luminumbra::Vec3;
using Luminumbra::TestSupport::ActiveRegionAcceptanceWorld;
using Luminumbra::TestSupport::SessionWorldHash;
using Luminumbra::world::GameSession;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionState;
namespace P = Luminumbra::Persistence;
namespace fs = std::filesystem;

constexpr RegionKey kRemote{40, -40};

struct AcceptanceFrame {
    std::string schedule, ledger, authority;
    bool operator==(const AcceptanceFrame&) const = default;
};

void ReplayInputs(unsigned workers, bool reopen, std::vector<AcceptanceFrame>& frames) {
    ActiveRegionAcceptanceWorld fixture(workers);
    GameSession original, loaded;
    fixture.Configure(original);
    ActiveRegionAcceptanceWorld::AdoptAuthority(original);
    original.NotifyGroundObjectEdit(Vec3(20488, 0, -20472));
    original.PinActiveRegion(kRemote, true);
    auto* session = &original;
    // Twelve ticks, two chunks and at most six ledger records. No procedural jobs.
    for (std::uint64_t tick = 1; tick <= 12; ++tick) {
        SCOPED_TRACE(tick);
        // Equality after tick 7 deliberately selects the under-hold path.
        session->RecordActiveRegionWork({kRemote, tick <= 7 ? 11u : 10u, 0, 0, 0});
        ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session->GetRegionSchedulerFault().has_value());
        const auto& ledger = session->GetActiveRegionLedger();
        ASSERT_LE(ledger.records().size(), 6u);
        const auto& record = ledger.records().at(kRemote);
        const auto expected = tick < 3 || tick >= 11  ? RegionState::Active
                              : tick < 5 || tick >= 9 ? RegionState::Reduced
                                                      : RegionState::Frozen;
        EXPECT_EQ(record.state, expected);
        if (expected == RegionState::Frozen) {
            EXPECT_EQ(record.frozen_at, 5u);
            EXPECT_NE(record.frozen_digest, 0u);
            EXPECT_LT(record.last_ticked, record.frozen_at);
        }
        EXPECT_FALSE(session->GetRegionSchedule().digest.empty());
        frames.push_back({session->GetRegionSchedule().digest,
                          ledger.canonical_bytes(),
                          SessionWorldHash(*session)});
        if (reopen && tick == 4) {
            // Persist an in-progress over-hold, immediately before freezing.
            EXPECT_EQ(record.over_hold, 1u);
            const auto save = fixture.path() / "enabled-save";
            ASSERT_TRUE(session->SaveWorldStateTo(save));
            fixture.Configure(loaded);
            ASSERT_TRUE(loaded.LoadWorldStateFrom(save)) << loaded.GetWorldOpenError();
            EXPECT_EQ(loaded.GetActiveRegionLedger().encode(), ledger.encode());
            EXPECT_EQ(loaded.GetActiveRegionLedger().canonical_bytes(), frames.back().ledger);
            EXPECT_EQ(SessionWorldHash(loaded), frames.back().authority);
            session = &loaded;
        }
    }
}

class ActiveRegionAcceptance : public testing::TestWithParam<unsigned> {};

TEST_P(ActiveRegionAcceptance, EnabledReplayMatchesAcrossWorkers) {
    std::vector<AcceptanceFrame> reference, replay;
    ASSERT_NO_FATAL_FAILURE(ReplayInputs(1, false, reference));
    ASSERT_NO_FATAL_FAILURE(ReplayInputs(GetParam(), true, replay));
    ASSERT_EQ(reference.size(), 12u);
    EXPECT_EQ(replay, reference);
}

INSTANTIATE_TEST_SUITE_P(Workers, ActiveRegionAcceptance, testing::Values(1u, 2u, 4u));

TEST(ActiveRegionAcceptance, SessionInputBoundsPreserveValidAuthority) {
    ActiveRegionAcceptanceWorld fixture(1);
    GameSession session;
    fixture.Configure(session);
    constexpr float limit = 16777216.0f;
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    // Both accepted endpoints and their adjacent interior binary32 values.
    struct AcceptedPosition {
        float coordinate;
        std::int32_t region;
    };
    const std::array accepted{AcceptedPosition{-limit, -32768},
                              AcceptedPosition{std::nextafter(-limit, 0.0f), -32768},
                              AcceptedPosition{std::nextafter(limit, 0.0f), 32767}};
    for (const int axis : {0, 2}) {
        for (const auto& input : accepted) {
            Vec3 position(256, 0, 256);
            position[axis] = input.coordinate;
            session.SetLocalPlayerSimulationPosition(position, true);
            EXPECT_EQ(session.GetActiveRegionLedger().local_anchor(), std::optional(position));
            session.NotifyGroundObjectEdit(position);
            const RegionKey key =
                axis == 0 ? RegionKey{input.region, 0} : RegionKey{0, input.region};
            ASSERT_TRUE(session.GetActiveRegionLedger().records().contains(key));
            EXPECT_TRUE(session.GetActiveRegionLedger().records().at(key).edited);
            session.PinActiveRegion(key, true);
            EXPECT_TRUE(session.GetActiveRegionLedger().records().at(key).pinned);
            session.PinActiveRegion(key, false);
            EXPECT_FALSE(session.GetActiveRegionLedger().records().at(key).pinned);
        }
    }
    const Vec3 walking(256, 0, 256);
    session.SetLocalPlayerSimulationPosition(walking, true);
    const auto before = session.GetActiveRegionLedger().encode();
    const auto authority = SessionWorldHash(session);
    // Noclip cannot replace the walking anchor, even with otherwise valid input.
    session.SetLocalPlayerSimulationPosition(Vec3(1024, 0, 1024), false);
    session.SetLocalPlayerSimulationPosition(Vec3(nan, 0, 0), false);
    std::uint32_t rejected = 0;
    for (const int axis : {0, 2}) {
        for (const float coordinate : {limit,
                                       std::nextafter(limit, infinity),
                                       std::nextafter(-limit, -infinity),
                                       nan,
                                       infinity,
                                       -infinity}) {
            Vec3 position(256, 0, 256);
            position[axis] = coordinate;
            EXPECT_NO_THROW(session.SetLocalPlayerSimulationPosition(position, true));
            EXPECT_NO_THROW(session.NotifyGroundObjectEdit(position));
            ++rejected;
            EXPECT_EQ(session.GetActiveRegionLedger().local_anchor(), std::optional(walking));
            EXPECT_EQ(session.GetActiveRegionLedger().encode(), before);
        }
    }
    for (const float coordinate : {nan, infinity, -infinity}) {
        const Vec3 position(256, coordinate, 256);
        session.SetLocalPlayerSimulationPosition(position, true);
        session.NotifyGroundObjectEdit(position);
        ++rejected;
        EXPECT_EQ(session.GetActiveRegionLedger().local_anchor(), std::optional(walking));
        EXPECT_EQ(session.GetActiveRegionLedger().encode(), before);
    }
    std::uint32_t rejected_pins = 0;
    for (const bool x_axis : {true, false}) {
        for (const int coordinate :
             {-32769, 32768, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
            const RegionKey key = x_axis ? RegionKey{coordinate, 0} : RegionKey{0, coordinate};
            for (const bool pinned : {false, true}) {
                EXPECT_NO_THROW(session.PinActiveRegion(key, pinned));
                ++rejected_pins;
            }
        }
    }
    EXPECT_EQ(session.GetRegionInputRejections().anchors, rejected);
    EXPECT_EQ(session.GetRegionInputRejections().edits, rejected);
    EXPECT_EQ(session.GetRegionInputRejections().pins, rejected_pins);
    EXPECT_EQ(session.GetActiveRegionLedger().local_anchor(), std::optional(walking));
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), before);
    EXPECT_EQ(SessionWorldHash(session), authority);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
}

std::map<std::string, std::string> SaveMembers(const fs::path& save) {
    std::map<std::string, std::string> members;
    // This fixture writes at most eight members, each at most 1 MiB.
    std::size_t entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (++entries > 16)
            throw std::runtime_error("acceptance save tree exceeds bound");
        const auto relative = entry.path().lexically_relative(save);
        // Only the two known container directories are omitted. Every other
        // entry must open as a member; size and bytes use that same handle.
        if (relative == "chunks" || relative == "chunks/region")
            continue;
        Luminumbra::Filesystem::ReplacementSafeInputFile input;
        if (!input.open(entry.path()))
            throw std::runtime_error("cannot open acceptance save member");
        std::error_code error;
        const auto size = input.size(error);
        if (error || size > (1u << 20) || members.size() == 8)
            throw std::runtime_error("acceptance save member exceeds bound");
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!input.read(bytes.data(), bytes.size()))
            throw std::runtime_error("incomplete acceptance save member");
        members.emplace(Luminumbra::Filesystem::GenericPathString(relative), std::move(bytes));
    }
    return members;
}

TEST(ActiveRegionAcceptance, DisabledInputsLeaveHashesAndSaveMembersUnchanged) {
    ActiveRegionAcceptanceWorld fixture(1);
    GameSession control, inputs;
    for (auto* session : {&control, &inputs}) {
        fixture.Configure(*session, false);
        ActiveRegionAcceptanceWorld::AdoptAuthority(*session);
    }
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        inputs.SetLocalPlayerSimulationPosition(Vec3(20488, 0, -20472), true);
        inputs.SetLocalPlayerSimulationPosition(Vec3(nan, 0, 0), true);
        inputs.SetReplicatedSimulationAnchors({Vec3(256, 0, 256), Vec3(nan, 0, 0)});
        inputs.NotifyGroundObjectEdit(Vec3(20488, 0, -20472));
        inputs.NotifyGroundObjectEdit(Vec3(16777216.0f, 0, 0));
        inputs.PinActiveRegion(kRemote, true);
        inputs.PinActiveRegion({32768, 0}, false);
        inputs.RecordActiveRegionWork({kRemote, UINT64_MAX, 0, 0, 0});
        for (auto* session : {&control, &inputs})
            ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
        EXPECT_EQ(SessionWorldHash(inputs), SessionWorldHash(control));
        EXPECT_EQ(inputs.GetActiveRegionLedger().canonical_bytes(),
                  control.GetActiveRegionLedger().canonical_bytes());
    }
    EXPECT_TRUE(inputs.GetActiveRegionLedger().records().empty());
    EXPECT_EQ(inputs.GetRegionInputRejections().anchors, 0u);
    EXPECT_EQ(inputs.GetRegionInputRejections().edits, 0u);
    EXPECT_EQ(inputs.GetRegionInputRejections().pins, 0u);
    const auto baseline = fixture.path() / "disabled-control";
    const auto submitted = fixture.path() / "disabled-inputs";
    ASSERT_TRUE(control.SaveWorldStateTo(baseline));
    ASSERT_TRUE(inputs.SaveWorldStateTo(submitted));
    const auto members = SaveMembers(baseline);
    ASSERT_FALSE(members.empty());
    EXPECT_EQ(SaveMembers(submitted), members);
    EXPECT_FALSE(fs::exists(P::WorldSaveService::active_regions_path(submitted)));
}
} // namespace
