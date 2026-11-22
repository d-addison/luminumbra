#include <gtest/gtest.h>

#include "luminumbra_common/world/ActiveRegionLedger.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::world;
using Luminumbra::Vec3;

std::uint64_t Sum(std::uint64_t left, std::uint64_t right) {
    return left + std::min(right, std::numeric_limits<std::uint64_t>::max() - left);
}
std::uint64_t Durable(RegionKey key) {
    return (std::uint64_t{static_cast<std::uint32_t>(key.x)} << 32) |
           static_cast<std::uint32_t>(key.z);
}
void Append(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index) {
        bytes.push_back(static_cast<char>((value >> (8 * index)) & 255));
    }
}
std::string Digest(const std::string& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char character : bytes) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    std::string result(16, '0');
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned index = 0; index < 16; ++index) {
        result[index] = digits[(hash >> (4 * (15 - index))) & 15];
    }
    return result;
}
struct Reference {
    std::map<RegionKey, ActiveRegionRecord> records;
    RegionSchedule schedule;
};

// Independent value-copy scheduler retaining the full-sort priority rule.
Reference SortedSchedule(const ActiveRegionLedger& before,
                         std::uint64_t tick,
                         std::optional<std::span<const Vec3>> anchors,
                         std::span<const RegionWork> work,
                         const ActiveRegionLedger::OwnershipProbe& owned = {}) {
    Reference expected{before.records(), {}};
    auto& result = expected.schedule;
    result.tick = tick;
    std::vector<RegionKey> visits;
    const auto visit = [&](const Vec3& anchor) {
        if (!ActiveRegionLedger::valid_anchor(anchor)) {
            ++result.rejected.anchors;
            return;
        }
        for (const auto key : ActiveRegionLedger::activation_regions(anchor)) {
            const auto [entry, added] = expected.records.try_emplace(key);
            if (added) {
                entry->second.first_active = tick;
            }
            visits.push_back(key);
        }
    };
    if (!anchors && before.local_anchor()) {
        visit(*before.local_anchor());
    }
    for (const auto& anchor : anchors.value_or(std::span<const Vec3>{})) {
        visit(anchor);
    }
    for (auto& [key, record] : expected.records) {
        record.pressure = 0;
        if (std::ranges::find(visits, key) != visits.end()) {
            record.visited = true;
            record.last_proximity = tick;
        }
    }
    const auto accept = [&](const RegionWork& units) {
        if (!before.records().contains(units.key)) {
            ++result.rejected.work;
            return;
        }
        const auto total = Sum(Sum(units.region_ticks, units.entity_updates),
                               Sum(units.water_cells, units.page_updates));
        result.work = Sum(result.work, total);
        auto& record = expected.records.at(units.key);
        record.pressure = Sum(record.pressure, total);
    };
    for (const auto& units : work) {
        accept(units);
    }
    if (before.tick() + 1 == tick) {
        for (const auto& [key, total] : before.completed_work()) {
            accept(RegionWork{key, total, 0, 0, 0});
        }
    }
    std::vector<RegionKey> order;
    for (const auto& [key, record] : expected.records) {
        order.push_back(key);
        const bool idle = tick - record.last_proximity >= ActiveRegionLedger::kIdleEvictionTicks;
        if (!record.pinned && !record.edited && !record.populated && record.water_cursor == 0 &&
            idle && !before.energy_ages().contains(key) && !before.water_debts().contains(key) &&
            !before.completed_work().contains(key) && owned && !owned(key)) {
            result.evicted.push_back(key);
        }
    }
    const auto priority = [&](RegionKey key) {
        const auto& record = expected.records.at(key);
        const int rank = record.pinned ? 0 : record.edited ? 1 : 2;
        return std::tuple(rank, tick - record.last_proximity, key);
    };
    std::ranges::sort(
        order, [&](RegionKey left, RegionKey right) { return priority(left) < priority(right); });
    const auto& config = before.config();
    const bool over = result.work > config.work_limit;
    std::optional<RegionKey> selected;
    const auto eligible = [&](RegionKey key) {
        const auto& record = expected.records.at(key);
        if (record.last_proximity == tick || record.wake_pending) {
            return false;
        }
        return over ? record.state != RegionState::Frozen &&
                          record.over_hold >= config.hold_ticks - 1
                    : record.state != RegionState::Active &&
                          record.under_hold >= config.hold_ticks - 1;
    };
    if (over) {
        const auto found = std::find_if(order.rbegin(), order.rend(), eligible);
        if (found != order.rend()) {
            selected = *found;
        }
    } else {
        const auto found = std::ranges::find_if(order, eligible);
        if (found != order.end()) {
            selected = *found;
        }
    }
    for (const auto key : order) {
        auto& record = expected.records.at(key);
        const auto previous = record.state;
        if (record.last_proximity == tick || record.wake_pending) {
            record.over_hold = record.under_hold = 0;
            record.state = RegionState::Active;
        } else {
            if (over) {
                record.over_hold = std::min(record.over_hold, config.hold_ticks - 1) + 1;
                record.under_hold = 0;
            } else {
                record.under_hold = std::min(record.under_hold, config.hold_ticks - 1) + 1;
                record.over_hold = 0;
            }
            if (selected == key) {
                if (over) {
                    record.state = previous == RegionState::Active ? RegionState::Reduced
                                                                   : RegionState::Frozen;
                } else {
                    record.state = previous == RegionState::Frozen ? RegionState::Reduced
                                                                   : RegionState::Active;
                }
            }
        }
        if (previous != record.state) {
            if (record.state == RegionState::Frozen) {
                record.frozen_digest = Durable(key);
                if (record.frozen_at == 0 || record.last_ticked >= record.frozen_at) {
                    record.frozen_at = tick;
                }
            }
            record.over_hold = record.under_hold = 0;
            record.cadence_shift = record.state == RegionState::Active ? 0 : config.reduced_shift;
            record.cadence_phase = ActiveRegionLedger::phase(key, record.cadence_shift);
            result.transitions.push_back({key, previous, record.state});
        }
        record.wake_pending = false;
        record.last_decision = tick;
        if (record.state != RegionState::Frozen &&
            (tick & ((std::uint64_t{1} << record.cadence_shift) - 1)) == record.cadence_phase) {
            result.due.push_back(key);
            result.prior_ticks.push_back(record.last_ticked);
            record.last_ticked = tick;
        }
    }
    std::string trace = "region_schedule:v1:";
    Append(trace, tick, 8);
    Append(trace, result.work, 8);
    Append(trace, order.size(), 4);
    for (const auto key : order) {
        const auto& record = expected.records.at(key);
        Append(trace, static_cast<std::uint32_t>(key.x), 4);
        Append(trace, static_cast<std::uint32_t>(key.z), 4);
        Append(trace, static_cast<std::uint8_t>(record.state), 1);
        Append(trace, record.last_ticked == tick, 1);
    }
    Append(trace, result.transitions.size(), 4);
    for (const auto& transition : result.transitions) {
        Append(trace, static_cast<std::uint32_t>(transition.key.x), 4);
        Append(trace, static_cast<std::uint32_t>(transition.key.z), 4);
        Append(trace, static_cast<std::uint8_t>(transition.from), 1);
        Append(trace, static_cast<std::uint8_t>(transition.to), 1);
    }
    if (result.rejected.any()) {
        Append(trace, result.rejected.anchors, 4);
        Append(trace, result.rejected.work, 4);
        Append(trace, result.rejected.capacity, 4);
    }
    result.digest = Digest(trace);
    for (const auto key : result.evicted) {
        expected.records.erase(key);
    }
    std::vector<RegionKey> surviving_due;
    std::vector<std::uint64_t> surviving_ticks;
    for (std::size_t index = 0; index < result.due.size(); ++index) {
        if (expected.records.contains(result.due[index])) {
            surviving_due.push_back(result.due[index]);
            surviving_ticks.push_back(result.prior_ticks[index]);
        }
    }
    result.due = std::move(surviving_due);
    result.prior_ticks = std::move(surviving_ticks);
    result.ledger_records = expected.records.size();
    return expected;
}

TEST(ActiveRegionPriorityTest, UnchangedPrioritiesPerformZeroComparisons) {
    ActiveRegionLedger ledger;
    EXPECT_EQ(ledger.priority_comparisons(), 0u);
    for (std::int32_t index = 0; index < 1536; ++index) {
        ledger.activate_owned({index - 768, index % 19}, WorldClock{});
    }
    const std::array anchors{Vec3{100, 0, 100}};
    ledger.schedule(WorldClock(1), anchors);
    const auto initial = ledger.priority_comparisons();
    ASSERT_GT(initial, 0u);
    ledger.schedule(WorldClock(2), anchors);
    EXPECT_EQ(ledger.priority_comparisons(), initial);
    const RegionKey changed{0, 8};
    ledger.pin(changed, true, WorldClock(2));
    ledger.schedule(WorldClock(3), anchors);
    const auto pinned = ledger.priority_comparisons();
    EXPECT_GT(pinned, initial);
    ledger.mark_edited(changed, WorldClock(3));
    ledger.pin(changed, true, WorldClock(3));
    ledger.schedule(WorldClock(4), anchors);
    EXPECT_EQ(ledger.priority_comparisons(), pinned);
    ledger.pin(changed, false, WorldClock(4));
    ledger.schedule(WorldClock(5), anchors);
    EXPECT_GT(ledger.priority_comparisons(), pinned);
    const auto unpinned = ledger.priority_comparisons();
    EXPECT_THROW(ledger.schedule(WorldClock(5), anchors), std::invalid_argument);
    ledger.schedule(WorldClock(6), anchors);
    EXPECT_EQ(ledger.priority_comparisons(), unpinned);
}

TEST(ActiveRegionPriorityTest, RandomizedChangesMatchSortingTraceRecordsAndSaveBytes) {
    ActiveRegionLedger ledger({20, 2, 3});
    for (std::int32_t index = 0; index < 1280; ++index) {
        ledger.activate_owned({index - 640, index % 23 - 11}, WorldClock{});
    }
    std::uint32_t random = 0x71c398ab;
    const auto next = [&random]() {
        random = random * 1664525u + 1013904223u;
        return random;
    };
    std::uint64_t tick = 0;
    Vec3 anchor{100, 0, 100};
    for (unsigned step = 0; step < 192; ++step) {
        SCOPED_TRACE(step);
        for (unsigned change = 0; change < 7; ++change) {
            const auto index = static_cast<std::int32_t>(next() % 1280);
            const RegionKey key{index - 640, index % 23 - 11};
            switch (next() % 5) {
                case 0:
                    ledger.pin(key, true, WorldClock(tick));
                    break;
                case 1:
                    ledger.pin(key, false, WorldClock(tick));
                    break;
                case 2:
                    ledger.mark_edited(key, WorldClock(tick));
                    break;
                case 3:
                    ledger.activate_owned(key, WorldClock(tick));
                    break;
                default:
                    ledger.wake_frozen(key);
                    break;
            }
        }
        const auto x = static_cast<float>(static_cast<std::int32_t>(next() % 1600) - 800) * 512.0f;
        if (step % 4 == 0) {
            anchor = Vec3{x + 256, 0, 256};
        }
        const std::array visits{anchor, anchor, Vec3{std::numeric_limits<float>::infinity(), 0, 0}};
        ledger.set_local_anchor(anchor);
        const auto mode = (step / 4) % 3;
        const auto anchors = mode == 0   ? std::optional<std::span<const Vec3>>{}
                             : mode == 1 ? std::span<const Vec3>(visits)
                                         : std::span<const Vec3>{};
        const auto owner = ledger.records().begin()->first;
        const auto units = step % 16 < 12 ? std::numeric_limits<std::uint64_t>::max() : 0u;
        const std::array work{RegionWork{owner, units, 1, 0, 0},
                              RegionWork{owner, units, 0, 0, 0},
                              RegionWork{{32767, 32767}, 5, 0, 0, 0}};
        const std::array completed{RegionWork{owner, step % 7, 0, 0, 0}};
        ledger.publish_completed_work(completed);
        tick += step == 180 ? ActiveRegionLedger::kIdleEvictionTicks : 1;
        const ActiveRegionLedger::OwnershipProbe owned =
            step < 180 ? ActiveRegionLedger::OwnershipProbe{}
                       : ActiveRegionLedger::OwnershipProbe{[](RegionKey key) {
                             return key.x % 3 == 0;
                         }};
        auto fresh = ledger;
        const auto expected = SortedSchedule(ledger, tick, anchors, work, owned);
        const auto actual = ledger.schedule(WorldClock(tick), anchors, work, Durable, owned);
        EXPECT_EQ(actual, expected.schedule);
        EXPECT_EQ(ledger.records(), expected.records);
        EXPECT_EQ(actual, fresh.schedule(WorldClock(tick), anchors, work, Durable, owned));
        EXPECT_EQ(ledger.canonical_bytes(), fresh.canonical_bytes());
        EXPECT_EQ(ledger.encode(), fresh.encode());
        if (step == 60) {
            ActiveRegionLedger decoded;
            std::string error;
            ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), decoded, error)) << error;
            EXPECT_EQ(decoded.encode(), ledger.encode());
            ledger = std::move(decoded);
        } else if (step == 90) {
            ActiveRegionLedger assigned;
            assigned = ledger;
            ledger = std::move(assigned);
        } else if (step == 120) {
            auto moved = std::move(ledger);
            ledger = std::move(moved);
        }
    }
}

TEST(ActiveRegionPriorityTest, FailedPreparationDiscardsIndexedAdmissionsBeforeRetry) {
    ActiveRegionLedger ledger({0, 1, 2});
    ledger.activate_owned({10, 10}, WorldClock{});
    const std::array work{RegionWork{{10, 10}, 1, 0, 0, 0}};
    ledger.schedule(WorldClock(1), {}, work, Durable);
    ledger.schedule(WorldClock(2), {}, work, Durable);
    const auto before = ledger.encode();
    const auto canonical = ledger.canonical_bytes();
    const std::array first{Vec3{100, 0, 100}};
    EXPECT_THROW(
        ledger.schedule(WorldClock(3),
                        first,
                        work,
                        [](RegionKey) -> std::uint64_t { throw std::runtime_error("digest"); }),
        std::runtime_error);
    EXPECT_EQ(ledger.encode(), before);
    EXPECT_EQ(ledger.canonical_bytes(), canonical);
    const std::array second{Vec3{-10240, 0, -10240}};
    const auto expected = SortedSchedule(ledger, 3, second, work);
    EXPECT_EQ(ledger.schedule(WorldClock(3), second, work, Durable), expected.schedule);
    EXPECT_EQ(ledger.records(), expected.records);
    auto fresh = ledger;
    const auto tick = ActiveRegionLedger::kIdleEvictionTicks + 4;
    unsigned calls = 0;
    EXPECT_THROW(ledger.schedule(WorldClock(tick),
                                 first,
                                 {},
                                 Durable,
                                 [&calls](RegionKey) -> bool {
                                     if (++calls == 2) {
                                         throw std::runtime_error("ownership");
                                     }
                                     return false;
                                 }),
                 std::runtime_error);
    EXPECT_EQ(ledger.encode(), fresh.encode());
    EXPECT_EQ(ledger.canonical_bytes(), fresh.canonical_bytes());
    const auto nobody = [](RegionKey) {
        return false;
    };
    const auto retry = SortedSchedule(ledger, tick, std::span<const Vec3>{}, {}, nobody);
    EXPECT_EQ(ledger.schedule(WorldClock(tick), std::span<const Vec3>{}, {}, Durable, nobody),
              retry.schedule);
    EXPECT_EQ(ledger.records(), retry.records);
    fresh.schedule(WorldClock(tick), std::span<const Vec3>{}, {}, Durable, nobody);
    EXPECT_EQ(ledger.encode(), fresh.encode());
}

TEST(ActiveRegionPriorityTest, EmptyAndFullyEvictedIndexesCanBeReused) {
    ActiveRegionLedger ledger;
    ledger.schedule(WorldClock(1));
    EXPECT_EQ(ledger.priority_comparisons(), 0u);
    const std::array anchors{Vec3{100, 0, 100}};
    ledger.schedule(WorldClock(2), anchors);
    const auto tick = ActiveRegionLedger::kIdleEvictionTicks + 2;
    const auto nobody = [](RegionKey) {
        return false;
    };
    const auto expected = SortedSchedule(ledger, tick, std::span<const Vec3>{}, {}, nobody);
    EXPECT_EQ(ledger.schedule(WorldClock(tick), std::span<const Vec3>{}, {}, Durable, nobody),
              expected.schedule);
    ASSERT_TRUE(ledger.records().empty());
    const auto empty = ledger.priority_comparisons();
    ledger.schedule(WorldClock(tick + 1));
    EXPECT_EQ(ledger.priority_comparisons(), empty);
    const auto revisited = SortedSchedule(ledger, tick + 2, anchors, {});
    EXPECT_EQ(ledger.schedule(WorldClock(tick + 2), anchors), revisited.schedule);
    EXPECT_EQ(ledger.records(), revisited.records);
}
} // namespace
