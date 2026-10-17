//  delta-snapshot codec tests — round-trip, bandwidth, removals, and surviving
// the wire through the deterministic network-condition sim.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
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

} // namespace
