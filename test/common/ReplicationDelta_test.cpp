//  delta-snapshot codec tests — round-trip, bandwidth, removals, and surviving
// the wire through the deterministic network-condition sim.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "net/LockstepSession.h"
#include "net/NetworkSim.h"
#include "net/ReplicationDelta.h"
#include "net/ReplicationProtocol.h"

namespace {

using Luminumbra::Net::ApplySnapshotDelta;
using Luminumbra::Net::DecodeSnapshot;
using Luminumbra::Net::EncodeSnapshot;
using Luminumbra::Net::FrameDelivery;
using Luminumbra::Net::MakeSnapshotDelta;
using Luminumbra::Net::NetworkConditions;
using Luminumbra::Net::NetworkSim;
using Luminumbra::Net::ReplEntityState;
using Luminumbra::Net::SnapshotEncodedSize;
using Luminumbra::Net::SnapshotMsg;

ReplEntityState ent(std::uint32_t id, std::int32_t x) {
    ReplEntityState e;
    e.entity_id = id;
    e.px_mm = x;
    return e;
}
SnapshotMsg snap(std::uint32_t seq, std::vector<ReplEntityState> es) {
    SnapshotMsg m;
    m.snapshot_seq = seq;
    m.server_tick = seq;
    m.entities = std::move(es);
    return m;
}
std::vector<ReplEntityState> sortedEntities(SnapshotMsg m) {
    std::sort(m.entities.begin(),
              m.entities.end(),
              [](const ReplEntityState& a, const ReplEntityState& b) {
                  return a.entity_id < b.entity_id;
              });
    return m.entities;
}

// baseline + delta(baseline, current) reconstructs current exactly (changed + added + removed).
TEST(ReplicationDelta, RoundTripReconstructsCurrent) {
    const SnapshotMsg base = snap(1, {ent(1, 100), ent(2, 200), ent(3, 300)});
    const SnapshotMsg cur =
        snap(2, {ent(1, 100), ent(2, 250), ent(4, 400)}); // 2 changed, 3 gone, 4 new
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    const SnapshotMsg recon = ApplySnapshotDelta(base, delta);
    EXPECT_EQ(sortedEntities(recon), sortedEntities(cur));
}

// A near-identical snapshot deltas down to just the changed entity (the bandwidth win).
TEST(ReplicationDelta, DeltaCarriesOnlyChanges) {
    const SnapshotMsg base = snap(1, {ent(1, 100), ent(2, 200), ent(3, 300)});
    const SnapshotMsg cur = snap(2, {ent(1, 100), ent(2, 999), ent(3, 300)}); // only 2 moved
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    EXPECT_EQ(delta.entities.size(), 1u);
    EXPECT_EQ(delta.entities[0].entity_id, 2u);
    EXPECT_TRUE(delta.removed_ids.empty());
    EXPECT_LT(delta.entities.size(), cur.entities.size());
}

TEST(ReplicationDelta, RemovedEntitiesDropped) {
    const SnapshotMsg base = snap(1, {ent(1, 100), ent(2, 200), ent(3, 300)});
    const SnapshotMsg cur = snap(2, {ent(1, 100), ent(2, 200)}); // 3 despawned
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    EXPECT_EQ(delta.removed_ids, (std::vector<std::uint32_t>{3}));
    EXPECT_TRUE(delta.entities.empty());
    EXPECT_EQ(sortedEntities(ApplySnapshotDelta(base, delta)), sortedEntities(cur));
}

// A delta survives encoding + the network-condition sim and reconstructs the full state.
TEST(ReplicationDelta, SurvivesWireThroughNetworkSim) {
    const SnapshotMsg base = snap(1, {ent(1, 100), ent(2, 200), ent(3, 300)});
    const SnapshotMsg cur = snap(2, {ent(1, 150), ent(2, 200), ent(4, 400)});
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);

    NetworkSim sim(NetworkConditions{/*latency*/ 1, 0, 0, 1});
    sim.A().SendFrame(EncodeSnapshot(delta), FrameDelivery::Unreliable);
    sim.Tick();
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(sim.B().TryReceiveFrame(out));
    SnapshotMsg decoded;
    ASSERT_TRUE(DecodeSnapshot(out, decoded));
    EXPECT_EQ(sortedEntities(ApplySnapshotDelta(base, decoded)), sortedEntities(cur));
}

TEST(ReplicationDelta, BoundedReconstructionChecksCountsBeforeRetainingTheResult) {
    using Luminumbra::Net::TryApplySnapshotDelta;
    const SnapshotMsg base = snap(1, {ent(1, 100), ent(2, 200)});
    SnapshotMsg delta = snap(2, {ent(3, 300)});
    EXPECT_FALSE(TryApplySnapshotDelta(base, delta, 83).has_value());
    EXPECT_FALSE(TryApplySnapshotDelta(base, delta, 36).has_value());
    EXPECT_FALSE(TryApplySnapshotDelta(base, delta, 60).has_value());

    // Duplicate ids and overwrites consume no new slots at the exact bound.
    delta = snap(2, {ent(2, 250), ent(2, 260)});
    const auto overwritten = TryApplySnapshotDelta(base, delta, 83);
    ASSERT_TRUE(overwritten.has_value());
    EXPECT_EQ(sortedEntities(overwritten.value_or(SnapshotMsg{})),
              sortedEntities(snap(2, {ent(1, 100), ent(2, 260)})));

    // Removal metadata consumes bytes even when its id was never in the baseline.
    delta = snap(2, {});
    delta.removed_ids = {99};
    EXPECT_FALSE(TryApplySnapshotDelta(base, delta, 83).has_value());
    EXPECT_FALSE(TryApplySnapshotDelta(snap(1, {}), delta, 37).has_value());
    delta.removed_ids = {1, 2};
    const auto empty = TryApplySnapshotDelta(base, delta, 83);
    ASSERT_TRUE(empty.has_value());
    const auto cleared = empty.value_or(SnapshotMsg{});
    EXPECT_TRUE(cleared.entities.empty());
    EXPECT_EQ(cleared.removed_ids, delta.removed_ids);
    EXPECT_TRUE(TryApplySnapshotDelta(snap(1, {}), snap(2, {}), 37).has_value());
}

// The node-map implementation is retained as a byte oracle, independent of the
// flat indices. In particular, creation keeps duplicate current records while
// reconstruction uses the last record for each id.
// Diff `current` against `baseline`: include an entity iff it is NEW or CHANGED; list the
// ids present in `baseline` but absent from `current` as removed. Header fields come from
// `current`. Pure + deterministic (id-ordered).
[[nodiscard]] SnapshotMsg LegacyMakeSnapshotDelta(const SnapshotMsg& baseline,
                                                  const SnapshotMsg& current) {
    std::map<std::uint32_t, const ReplEntityState*> base;
    for (const ReplEntityState& e : baseline.entities)
        base[e.entity_id] = &e;

    SnapshotMsg delta;
    delta.server_tick = current.server_tick;
    delta.snapshot_seq = current.snapshot_seq;
    delta.acked_usercmd_tick = current.acked_usercmd_tick;

    std::map<std::uint32_t, bool> seen;
    for (const ReplEntityState& e : current.entities) {
        seen[e.entity_id] = true;
        const auto it = base.find(e.entity_id);
        if (it == base.end() || !(*it->second == e)) {
            delta.entities.push_back(e); // new or changed
        }
    }
    for (const ReplEntityState& e : baseline.entities) {
        if (seen.find(e.entity_id) == seen.end())
            delta.removed_ids.push_back(e.entity_id);
    }
    // ALSO carry the caller-stamped despawns on `current` (explicit/pending removals -- e.g. a
    // spent transient or a disconnected client's avatar whose id is not in the acked baseline's
    // replicated set). Without this, delta mode silently drops those removals that full mode
    // delivers, leaving a stale ghost on the client. Union + dedup keeps full==delta parity.
    for (std::uint32_t id : current.removed_ids)
        delta.removed_ids.push_back(id);
    std::sort(delta.entities.begin(),
              delta.entities.end(),
              [](const ReplEntityState& a, const ReplEntityState& b) {
                  return a.entity_id < b.entity_id;
              });
    std::sort(delta.removed_ids.begin(), delta.removed_ids.end());
    delta.removed_ids.erase(std::unique(delta.removed_ids.begin(), delta.removed_ids.end()),
                            delta.removed_ids.end());
    return delta;
}

// Reconstruct the full snapshot from (baseline + delta): keep baseline entities, drop the
// delta's removed ids, then overwrite/insert the delta's entities. Header fields come from
// `delta`. Pure + deterministic (id-ordered output).
// Bound reconstructed state as well as individual delta frames. The map holds
// at most one frame's entities; reject new ids before allocating beyond that cap.
[[nodiscard]] std::optional<SnapshotMsg> LegacyTryApplySnapshotDelta(const SnapshotMsg& baseline,
                                                                     const SnapshotMsg& delta,
                                                                     std::size_t max_frame_bytes) {
    const auto base_bytes = SnapshotEncodedSize(baseline.entities.size(), 0);
    const auto overhead = SnapshotEncodedSize(0, delta.removed_ids.size());
    if (!base_bytes || *base_bytes > max_frame_bytes || !overhead || *overhead > max_frame_bytes)
        return std::nullopt;
    const auto entity_limit = (max_frame_bytes - *overhead) / 23u;
    std::map<std::uint32_t, ReplEntityState> set;
    for (const ReplEntityState& e : baseline.entities)
        set[e.entity_id] = e;
    for (std::uint32_t id : delta.removed_ids)
        set.erase(id);
    for (const ReplEntityState& e : delta.entities) {
        const auto existing = set.find(e.entity_id);
        if (existing != set.end()) {
            existing->second = e;
        } else {
            if (set.size() >= entity_limit)
                return std::nullopt;
            set.emplace(e.entity_id, e);
        }
    }
    if (set.size() > entity_limit)
        return std::nullopt;

    SnapshotMsg full;
    full.server_tick = delta.server_tick;
    full.snapshot_seq = delta.snapshot_seq;
    full.acked_usercmd_tick = delta.acked_usercmd_tick;
    // Surface this frame's despawns on the reconstructed snapshot, exactly as full-snapshot mode
    // does (ReplicationEndpoint stamps snap.removed_ids). A TRANSIENT removal -- an id never in
    // the entity set (a spent projectile, a just-joined leaver) -- has no entity to drop, so
    // removed_ids is the ONLY signal the consumer gets; dropping it here would leave a stale
    // ghost and break full==delta parity.
    full.removed_ids = delta.removed_ids;
    full.entities.reserve(set.size());
    for (const auto& [id, e] : set)
        full.entities.push_back(e); // map -> ascending id
    return full;
}

void ExpectSameSnapshot(const SnapshotMsg& actual, const SnapshotMsg& expected) {
    EXPECT_EQ(actual.entities, expected.entities);
    EXPECT_EQ(actual.removed_ids, expected.removed_ids);
    EXPECT_EQ(EncodeSnapshot(actual), EncodeSnapshot(expected));
}

void ExpectLegacyEquivalent(const SnapshotMsg& baseline, const SnapshotMsg& current) {
    using Luminumbra::Net::TryApplySnapshotDelta;
    const SnapshotMsg expected_delta = LegacyMakeSnapshotDelta(baseline, current);
    const SnapshotMsg actual_delta = MakeSnapshotDelta(baseline, current);
    ExpectSameSnapshot(actual_delta, expected_delta);
    const std::vector<std::size_t> limits = {
        0, 36, 37, 38, 60, 83, 106, 128, 256, 4096, (std::numeric_limits<std::size_t>::max)()};
    // Also treat current as an arbitrary incoming delta, including unsorted
    // removals, duplicate updates and remove-then-reinsert of the same id.
    for (const auto* incoming : {&current, &expected_delta}) {
        for (const std::size_t limit : limits) {
            SCOPED_TRACE(limit);
            const auto expected = LegacyTryApplySnapshotDelta(baseline, *incoming, limit);
            const auto actual = TryApplySnapshotDelta(baseline, *incoming, limit);
            ASSERT_EQ(actual.has_value(), expected.has_value());
            if (actual && expected)
                ExpectSameSnapshot(*actual, *expected);
        }
        const auto unbounded = LegacyTryApplySnapshotDelta(
            baseline, *incoming, (std::numeric_limits<std::size_t>::max)());
        if (!unbounded) {
            FAIL() << "legacy reconstruction unexpectedly refused unbounded input";
            return;
        }
        const auto exact_bytes = Luminumbra::Net::SnapshotEncodedSize(
            unbounded->entities.size(), unbounded->removed_ids.size());
        if (!exact_bytes) {
            FAIL() << "legacy reconstruction size overflowed";
            return;
        }
        for (const std::size_t limit : {*exact_bytes - 1, *exact_bytes, *exact_bytes + 1}) {
            const auto expected = LegacyTryApplySnapshotDelta(baseline, *incoming, limit);
            const auto actual = TryApplySnapshotDelta(baseline, *incoming, limit);
            ASSERT_EQ(actual.has_value(), expected.has_value());
            if (actual && expected)
                ExpectSameSnapshot(*actual, *expected);
        }
    }
    const auto expected_full = LegacyTryApplySnapshotDelta(
        baseline, expected_delta, (std::numeric_limits<std::size_t>::max)());
    if (!expected_full) {
        FAIL() << "legacy round trip unexpectedly refused unbounded input";
        return;
    }
    ExpectSameSnapshot(ApplySnapshotDelta(baseline, actual_delta), *expected_full);
}

TEST(ReplicationDelta, SparseChangesKeepOutputStorageProportionalToDelta) {
    SnapshotMsg baseline = snap(1, {});
    // A fixed 4096-record input makes snapshot-sized output reserves observable
    // without depending on the standard library's exact vector growth factor.
    for (std::uint32_t id = 4096; id > 0; --id)
        baseline.entities.push_back(ent(id, static_cast<std::int32_t>(id)));
    for (const std::size_t changed_count : {0u, 1u, 3u}) {
        for (const std::size_t removed_count : {0u, 2u}) {
            for (const bool explicit_removals : {false, true}) {
                SCOPED_TRACE(::testing::Message()
                             << changed_count << ',' << removed_count << ',' << explicit_removals);
                auto current = baseline;
                current.server_tick = 7;
                current.snapshot_seq = 9;
                current.acked_usercmd_tick = 5;
                for (std::size_t index = 0; index < changed_count; ++index)
                    ++current.entities[index].px_mm;
                current.entities.resize(current.entities.size() - removed_count);
                if (explicit_removals)
                    current.removed_ids = {7000, 1, 7000};
                const auto expected = LegacyMakeSnapshotDelta(baseline, current);
                const auto actual = MakeSnapshotDelta(baseline, current);
                ExpectSameSnapshot(actual, expected);
                EXPECT_LE(actual.entities.capacity(), 64u);
                EXPECT_LE(actual.removed_ids.capacity(), 64u);
            }
        }
    }
}

TEST(ReplicationDelta, FlatIndicesMatchLegacyEdgeRecordBytesAndAdmission) {
    ExpectLegacyEquivalent(snap(1, {}), snap(2, {}));
    ExpectLegacyEquivalent(snap(1, {ent(3, 1), ent(3, 2)}), snap(2, {}));
    ExpectLegacyEquivalent(snap(1, {}), snap(2, {ent(0, 1), ent(0, 2), ent(0, 1)}));
    ExpectLegacyEquivalent(snap(1, {ent(1, 1), ent(2, 2), ent(3, 3)}),
                           snap(2, {ent(3, 30), ent(1, 10), ent(2, 20)}));
    // The last baseline duplicate is the comparison target. Changed duplicates
    // in current remain separate and keep the legacy sort's equal-key ordering.
    const auto high_id = (std::numeric_limits<std::uint32_t>::max)();
    const SnapshotMsg baseline = snap(1, {ent(high_id, 1), ent(7, 2), ent(0, 3), ent(7, 4)});
    SnapshotMsg current = snap(2, {ent(7, 2), ent(7, 4), ent(0, 8), ent(7, 9), ent(7, 2)});
    current.server_tick = (std::numeric_limits<std::uint64_t>::max)();
    current.acked_usercmd_tick = current.server_tick - 1;
    current.delta_from_seq = 17;
    current.removed_ids = {high_id, 99, 7, 0, 99, 7};
    ExpectLegacyEquivalent(baseline, current);
    ExpectLegacyEquivalent(current, baseline);
    // More equal keys than a small sort partition; the oracle pins the exact
    // duplicate order, including differing payloads for the same id.
    current.entities.clear();
    for (std::int32_t i = 0; i < 128; ++i)
        current.entities.push_back(ent(7, i));
    ExpectLegacyEquivalent(baseline, current);
}

SnapshotMsg RandomSnapshot(std::mt19937& random) {
    SnapshotMsg result;
    result.server_tick = random();
    result.snapshot_seq = static_cast<std::uint32_t>(random());
    result.acked_usercmd_tick = random();
    result.delta_from_seq = static_cast<std::uint32_t>(random());
    const auto count = random() % 97;
    for (std::size_t i = 0; i < count; ++i) {
        ReplEntityState entity;
        entity.entity_id = static_cast<std::uint32_t>(random() % 32);
        entity.px_mm = static_cast<std::int32_t>(random() % 200001) - 100000;
        entity.py_mm = static_cast<std::int32_t>(random() % 200001) - 100000;
        entity.pz_mm = static_cast<std::int32_t>(random() % 200001) - 100000;
        entity.yaw_mrad =
            static_cast<std::int16_t>(static_cast<std::int32_t>(random() % 6001) - 3000);
        entity.flags = static_cast<std::uint8_t>(random() % 256);
        entity.type_id = static_cast<std::uint16_t>(random() % 65536);
        entity.anim_state = static_cast<std::uint8_t>(random() % 256);
        entity.anim_phase = static_cast<std::uint8_t>(random() % 256);
        result.entities.push_back(entity);
    }
    const auto removals = random() % 33;
    for (std::size_t i = 0; i < removals; ++i)
        result.removed_ids.push_back(static_cast<std::uint32_t>(random() % 48));
    return result;
}

TEST(ReplicationDelta, FlatIndicesMatchLegacyFixedSeedRandomizedBytesAndAdmission) {
    std::mt19937 random(0x5EED1234u);
    for (int iteration = 0; iteration < 512; ++iteration) {
        SCOPED_TRACE(iteration);
        const auto baseline = RandomSnapshot(random);
        auto current = RandomSnapshot(random);
        // Include unchanged records as well as random changed/new records.
        for (std::size_t i = 0; i < baseline.entities.size(); i += 5)
            current.entities.push_back(baseline.entities[i]);
        ExpectLegacyEquivalent(baseline, current);
    }
}

} // namespace
