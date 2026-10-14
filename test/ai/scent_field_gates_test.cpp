#include "gtest/gtest.h"

#include "luminumbra_common/ai/ScentFieldGates.h"
#include "luminumbra_common/fields/FieldPageState.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace {
using luminumbra::ai::ScentField;
using luminumbra::ai::detail::CacheNearScentGates;
using luminumbra::ai::detail::ScentStepGates;
using namespace Luminumbra::world;

constexpr int kCells = 128;
constexpr int kChannels = 4;
constexpr double kDiffusionRate = 0.25;
constexpr std::size_t kDiffusionIterations = 4;
constexpr double kEvaporation = 0.05;
constexpr double kTauMin = 1.0e-9;
constexpr double kTauMax = 1.0e6;
constexpr std::array<std::pair<double, double>, 6> kWinds{
    std::pair{0.0, 0.0}, {0.375, -1.25}, {-2.625, 0.875}, {4.25, -3.5}, {0.0, 2.125}, {-1.75, 0.0}};

// Reference owner/due/edge lookups mirror GameSession's original live callbacks.
// They deliberately do not use tile splits, corner decisions or cached gates.
struct LiveSchedule {
    ScentCellKey origin;
    std::map<RegionKey, ActiveRegionRecord> records;
    std::vector<RegionKey> due;
    std::uint64_t tick = 0;
    bool interior = true;

    explicit LiveSchedule(ScentCellKey domain_origin)
        : origin(domain_origin) {
        for (const int z : {0, kCells - 1})
            for (const int x : {0, kCells - 1})
                Set(Owner(x, z), RegionState::Active, std::uint8_t{0});
    }

    RegionKey Owner(int x, int z) const {
        return FieldPageState::region_of_scent_cell({origin.x + x, origin.z + z});
    }

    void Set(RegionKey owner, RegionState state, std::uint8_t shift) {
        auto& record = records[owner];
        record.state = state;
        record.cadence_shift = shift;
        record.cadence_phase = ActiveRegionLedger::phase(owner, shift);
    }

    void Reschedule(std::uint64_t next_tick) {
        tick = next_tick;
        due.clear();
        for (const auto& [owner, record] : records)
            if (record.state != RegionState::Frozen &&
                (tick & ((std::uint64_t{1} << record.cadence_shift) - 1)) == record.cadence_phase)
                due.push_back(owner);
    }

    bool CellDue(int x, int z) const {
        return std::find(due.begin(), due.end(), Owner(x, z)) != due.end();
    }

    bool EdgeOpen(int x, int z, int nx, int nz) const {
        const auto source = Owner(x, z), destination = Owner(nx, nz);
        if (source == destination)
            return CellDue(x, z);
        const auto a = records.find(source), b = records.find(destination);
        if (a == records.end() || b == records.end() || a->second.state == RegionState::Frozen ||
            b->second.state == RegionState::Frozen)
            return false;
        const auto shift = std::max(a->second.cadence_shift, b->second.cadence_shift);
        return shift < 63 && (tick & ((std::uint64_t{1} << shift) - 1)) == 0;
    }

    ScentStepGates LiveGates() const {
        return {[this](int x, int z) { return interior && CellDue(x, z); },
                [this](int x, int z, int nx, int nz) {
                    return EdgeOpen(x, z, nx, nz);
                }};
    }

    ScentStepGates CachedGates() const {
        return CacheNearScentGates(
            kCells,
            [this](int x, int z) { return Owner(x, z); },
            [this](int x, int z) { return interior && CellDue(x, z); },
            [this](int x, int z, int nx, int nz) { return EdgeOpen(x, z, nx, nz); });
    }
};

ScentField InitialField() {
    ScentField field(kCells, kCells, kChannels);
    for (int channel = 0; channel < kChannels; ++channel)
        for (int z = 0; z < kCells; ++z)
            for (int x = 0; x < kCells; ++x) {
                // Nonuniform binary64 inputs distinguish bilinear sample/path choices.
                // Zeros and spikes also exercise both ends of the clamp.
                const int pattern = (x * 17 + z * 31 + channel * 43) % 101;
                const double value = pattern == 0   ? 0.0
                                     : pattern == 1 ? 2.0e6
                                                    : static_cast<double>(pattern) / 37.0;
                field.RestoreCell(channel, x, z, value);
            }
    return field;
}

testing::AssertionResult SameBits(const ScentField& reference, const ScentField& optimized) {
    for (int channel = 0; channel < kChannels; ++channel)
        for (int z = 0; z < kCells; ++z)
            for (int x = 0; x < kCells; ++x) {
                const auto expected = std::bit_cast<std::uint64_t>(reference.Sample(channel, x, z));
                const auto actual = std::bit_cast<std::uint64_t>(optimized.Sample(channel, x, z));
                if (expected != actual)
                    return testing::AssertionFailure()
                           << "channel=" << channel << " cell=(" << x << ',' << z
                           << ") expected bits=" << expected << " actual bits=" << actual;
            }
    return testing::AssertionSuccess();
}

void AdvanceAndCompare(ScentField& reference,
                       ScentField& optimized,
                       const LiveSchedule& schedule,
                       const ScentStepGates& cached,
                       std::pair<double, double> wind) {
    const auto live = schedule.LiveGates();
    ASSERT_TRUE(live.participants);
    ASSERT_TRUE(live.exchange);
    // Supplying a nonempty live EdgeGate keeps the pre-optimization BilinearSealed
    // path reachable, even when every lookup returns true. ScentField is unchanged.
    reference.Step(kDiffusionRate,
                   kDiffusionIterations,
                   kEvaporation,
                   wind.first,
                   wind.second,
                   live.participants,
                   live.exchange);
    optimized.Step(kDiffusionRate,
                   kDiffusionIterations,
                   kEvaporation,
                   wind.first,
                   wind.second,
                   cached.participants,
                   cached.exchange);
    ASSERT_TRUE(SameBits(reference, optimized)) << "after Step, before Clamp";
    reference.Clamp(kTauMin, kTauMax, live.participants);
    optimized.Clamp(kTauMin, kTauMax, cached.participants);
    ASSERT_TRUE(SameBits(reference, optimized)) << "after Clamp";
}
} // namespace

TEST(ScentFieldGates, FullyOpenDomainsMatchLiveSolverBitExactly) {
    // One owner, either axis split, four owners, negative coordinates, and splits
    // leaving only a single cell on one side of an owner boundary.
    const std::array<ScentCellKey, 7> origins{ScentCellKey{100, 100},
                                              {480, 100},
                                              {100, 480},
                                              {480, 500},
                                              {-13, -67},
                                              {385, 511},
                                              {512, 512}};
    for (const auto origin : origins) {
        SCOPED_TRACE(testing::Message() << "origin=(" << origin.x << ',' << origin.z << ')');
        LiveSchedule schedule(origin);
        auto reference = InitialField();
        auto optimized = reference;
        for (std::size_t step = 0; step < kWinds.size(); ++step) {
            SCOPED_TRACE(step);
            schedule.Reschedule(static_cast<std::uint64_t>(step) + 1);
            const auto cached = schedule.CachedGates();
            ASSERT_FALSE(cached.participants);
            ASSERT_FALSE(cached.exchange); // Must actually exercise the ungated solver.
            AdvanceAndCompare(reference, optimized, schedule, cached, kWinds[step]);
        }
    }
}

TEST(ScentFieldGates, ReducedFrozenAbsentAndMixedOwnersMatchLiveSolverBitExactly) {
    const std::array<ScentCellKey, 4> origins{
        ScentCellKey{100, 100}, {480, 100}, {100, 480}, {-13, -67}};
    for (const auto origin : origins) {
        for (int regime = 0; regime < 4; ++regime) {
            SCOPED_TRACE(testing::Message()
                         << "origin=(" << origin.x << ',' << origin.z << ") regime=" << regime);
            LiveSchedule schedule(origin);
            std::vector<RegionKey> owners;
            for (const auto& [owner, record] : schedule.records) {
                (void)record;
                owners.push_back(owner);
            }
            for (std::size_t index = 0; index < owners.size(); ++index) {
                const int mode = regime == 3 ? static_cast<int>(index % 4) : regime;
                if (mode == 0)
                    schedule.Set(owners[index],
                                 RegionState::Reduced,
                                 static_cast<std::uint8_t>(1 + index % 2));
                else if (mode == 1)
                    schedule.Set(owners[index], RegionState::Frozen, std::uint8_t{0});
                else if (mode == 2)
                    schedule.records.erase(owners[index]);
                else
                    schedule.Set(owners[index], RegionState::Active, std::uint8_t{0});
            }
            auto reference = InitialField();
            auto optimized = reference;
            for (std::uint64_t tick = 1; tick <= 8; ++tick) {
                SCOPED_TRACE(tick);
                schedule.Reschedule(tick);
                const auto cached = schedule.CachedGates();
                AdvanceAndCompare(reference,
                                  optimized,
                                  schedule,
                                  cached,
                                  kWinds[static_cast<std::size_t>(tick % kWinds.size())]);
            }
        }
    }
}

TEST(ScentFieldGates, RebuiltGatesMatchLiveSolverAcrossScheduleChangesBitExactly) {
    LiveSchedule schedule({-31, -19});
    const std::array<RegionKey, 4> owners{schedule.Owner(0, 0),
                                          schedule.Owner(kCells - 1, 0),
                                          schedule.Owner(0, kCells - 1),
                                          schedule.Owner(kCells - 1, kCells - 1)};
    auto reference = InitialField();
    auto optimized = reference;
    for (std::size_t step = 0; step < 10; ++step) {
        SCOPED_TRACE(step);
        for (std::size_t index = 0; index < owners.size(); ++index) {
            const auto owner = owners[index];
            const std::size_t mode = step == 0 || step == 9 ? 0 : (step + index) % 4;
            if (mode == 0)
                schedule.Set(owner, RegionState::Active, std::uint8_t{0});
            else if (mode == 1)
                schedule.Set(owner, RegionState::Reduced, static_cast<std::uint8_t>(1 + step % 2));
            else if (mode == 2)
                schedule.Set(owner, RegionState::Frozen, std::uint8_t{0});
            else
                schedule.records.erase(owner);
        }
        schedule.interior = step != 3 && step != 7;
        schedule.Reschedule(static_cast<std::uint64_t>(step) + 1);
        const auto cached = schedule.CachedGates(); // No cache may survive a schedule change.
        if (step == 0 || step == 9) {
            ASSERT_FALSE(cached.participants);
            ASSERT_FALSE(cached.exchange);
        } else {
            ASSERT_TRUE(cached.participants);
            ASSERT_TRUE(cached.exchange);
        }
        AdvanceAndCompare(reference, optimized, schedule, cached, kWinds[step % kWinds.size()]);
    }
}

TEST(ScentFieldGates, BorderOnlyOpenDomainMatchesLiveSolverBitExactly) {
    LiveSchedule schedule({480, 500});
    schedule.interior = false;
    schedule.Reschedule(4);
    const auto cached = schedule.CachedGates();
    ASSERT_TRUE(cached.participants);
    ASSERT_FALSE(cached.participants(0, 0));
    ASSERT_FALSE(cached.exchange); // Open edges are independent of interior participation.
    auto reference = InitialField();
    auto optimized = reference;
    AdvanceAndCompare(reference, optimized, schedule, cached, {0.375, -1.25});
}
