//  authoritative-server state-replication wire protocol. Proves the
// Usercmd / Snapshot / Ack messages round-trip (encode -> decode -> equal),
// including a multi-entity snapshot and truncation-robustness, and that the
// position/angle quantization round-trips within tolerance.
#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "luminumbra_common/net/ReplicationProtocol.h"

namespace {

using namespace Luminumbra::Net;

TEST(ReplicationProtocol, UsercmdRoundTrip) {
    UsercmdMsg in;
    in.tick = 12345;
    in.player_id = 7;
    in.move_x = -32000;
    in.move_z = 31000;
    in.yaw_mrad = -1570; // ~ -pi/2
    in.action_bits = 0b00000101;

    const std::vector<std::uint8_t> frame = EncodeUsercmd(in);
    ReplMessageType type;
    ASSERT_TRUE(PeekReplMessageType(frame, type));
    EXPECT_EQ(type, ReplMessageType::Usercmd);

    UsercmdMsg out;
    ASSERT_TRUE(DecodeUsercmd(frame, out));
    EXPECT_EQ(in, out);
}

TEST(ReplicationProtocol, AckRoundTrip) {
    AckMsg in;
    in.snapshot_seq = 999;
    in.usercmd_tick = 4242;
    const std::vector<std::uint8_t> frame = EncodeAck(in);
    AckMsg out;
    ASSERT_TRUE(DecodeAck(frame, out));
    EXPECT_EQ(in.snapshot_seq, out.snapshot_seq);
    EXPECT_EQ(in.usercmd_tick, out.usercmd_tick);
}

TEST(ReplicationProtocol, SnapshotMultiEntityRoundTrip) {
    SnapshotMsg in;
    in.server_tick = 5000;
    in.snapshot_seq = 42;
    in.acked_usercmd_tick = 4990;
    for (std::uint32_t i = 0; i < 20; ++i) {
        ReplEntityState e;
        e.entity_id = i;
        e.px_mm = static_cast<std::int32_t>(i) * 1000 - 5000;
        e.py_mm = 34000 + static_cast<std::int32_t>(i);
        e.pz_mm = -2000 * static_cast<std::int32_t>(i);
        e.yaw_mrad = static_cast<std::int16_t>(i * 100);
        e.flags = static_cast<std::uint8_t>(i & 1);
        in.entities.push_back(e);
    }

    const std::vector<std::uint8_t> frame = EncodeSnapshot(in);
    SnapshotMsg out;
    ASSERT_TRUE(DecodeSnapshot(frame, out));
    EXPECT_EQ(out.server_tick, in.server_tick);
    EXPECT_EQ(out.snapshot_seq, in.snapshot_seq);
    EXPECT_EQ(out.acked_usercmd_tick, in.acked_usercmd_tick);
    ASSERT_EQ(out.entities.size(), in.entities.size());
    for (std::size_t i = 0; i < in.entities.size(); ++i) {
        EXPECT_EQ(out.entities[i], in.entities[i]) << "entity " << i;
    }
}

TEST(ReplicationProtocol, EntityTypeAnimAndRemovedIdsRoundTrip) {
    SnapshotMsg in;
    in.server_tick = 77;
    in.snapshot_seq = 5;
    ReplEntityState player;
    player.entity_id = 1;
    player.type_id = 0;
    player.anim_state = 2;
    player.anim_phase = 128;
    ReplEntityState deer;
    deer.entity_id = 50;
    deer.type_id = 7;
    deer.px_mm = 4000;
    deer.anim_state = 1;
    deer.anim_phase = 200;
    deer.flags = 1;
    ReplEntityState arrow;
    arrow.entity_id = 900;
    arrow.type_id = 42;
    arrow.px_mm = -1234;
    arrow.yaw_mrad = 1570;
    arrow.flags = 2;
    in.entities = {player, deer, arrow};
    in.removed_ids = {901, 902, 17};

    SnapshotMsg out;
    ASSERT_TRUE(DecodeSnapshot(EncodeSnapshot(in), out));
    ASSERT_EQ(out.entities.size(), 3u);
    EXPECT_EQ(out.entities[1].type_id, 7u);
    EXPECT_EQ(out.entities[1].anim_state, 1u);
    EXPECT_EQ(out.entities[1].anim_phase, 200u);
    EXPECT_EQ(out.entities[2].type_id, 42u);
    EXPECT_EQ(out.entities[2].flags, 2u);
    // Every field round-trips (operator== covers type/anim too).
    for (std::size_t i = 0; i < in.entities.size(); ++i) {
        EXPECT_EQ(out.entities[i], in.entities[i]) << "entity " << i;
    }
    EXPECT_EQ(out.removed_ids, (std::vector<std::uint32_t>{901, 902, 17}));
}

TEST(ReplicationProtocol, EmptySnapshotRoundTrips) {
    SnapshotMsg in;
    in.server_tick = 1;
    in.snapshot_seq = 1;
    const std::vector<std::uint8_t> frame = EncodeSnapshot(in);
    SnapshotMsg out;
    ASSERT_TRUE(DecodeSnapshot(frame, out));
    EXPECT_TRUE(out.entities.empty());
}

TEST(ReplicationProtocol, BorrowedSnapshotEntitiesPreserveHeaderAndRemovals) {
    SnapshotMsg header;
    header.server_tick = 77;
    header.snapshot_seq = 5;
    header.delta_from_seq = 3;
    header.acked_usercmd_tick = 70;
    header.entities.push_back(ReplEntityState{}); // the supplied set must take precedence
    header.removed_ids = {10, 20};
    ReplEntityState entity;
    entity.entity_id = 42;
    entity.px_mm = -1234;
    entity.type_id = 7;
    entity.anim_phase = 200;
    const std::vector<ReplEntityState> entities{entity};

    const auto frame = EncodeSnapshot(header, entities);
    EXPECT_EQ(frame.size(), 37u + 23u + 8u);
    SnapshotMsg decoded;
    ASSERT_TRUE(DecodeSnapshot(frame, decoded));
    EXPECT_EQ(decoded.server_tick, header.server_tick);
    EXPECT_EQ(decoded.snapshot_seq, header.snapshot_seq);
    EXPECT_EQ(decoded.delta_from_seq, header.delta_from_seq);
    EXPECT_EQ(decoded.acked_usercmd_tick, header.acked_usercmd_tick);
    EXPECT_EQ(decoded.entities, entities);
    EXPECT_EQ(decoded.removed_ids, header.removed_ids);
    EXPECT_EQ(header.entities.front().entity_id, 0u);
    EXPECT_EQ(entities.front(), entity);
}

TEST(ReplicationProtocol, TruncatedFrameRejected) {
    SnapshotMsg in;
    in.server_tick = 7;
    in.snapshot_seq = 3;
    in.entities.push_back(ReplEntityState{1, 100, 200, 300, 0, 0});
    std::vector<std::uint8_t> frame = EncodeSnapshot(in);
    // Lop off the last few bytes -> the declared length no longer matches.
    frame.resize(frame.size() - 4);
    SnapshotMsg out;
    EXPECT_FALSE(DecodeSnapshot(frame, out));
}

TEST(ReplicationProtocol, HugeSnapshotCountsAreRejectedBeforeAllocation) {
    // Empty snapshot: 5-byte envelope, 24-byte fixed fields, then two u32 counts.
    for (const std::size_t count_offset : {29u, 33u}) {
        SCOPED_TRACE(count_offset);
        auto frame = EncodeSnapshot({});
        ASSERT_EQ(frame.size(), 37u);
        const auto count = std::numeric_limits<std::uint32_t>::max();
        for (int i = 0; i < 4; ++i)
            frame[count_offset + i] = static_cast<std::uint8_t>(count >> (8 * i));

        SnapshotMsg out;
        const auto entity_capacity = out.entities.capacity();
        const auto removed_capacity = out.removed_ids.capacity();
        EXPECT_FALSE(DecodeSnapshot(frame, out));
        EXPECT_TRUE(out.entities.empty());
        EXPECT_TRUE(out.removed_ids.empty());
        EXPECT_EQ(out.entities.capacity(), entity_capacity);
        EXPECT_EQ(out.removed_ids.capacity(), removed_capacity);
    }
}

TEST(ReplicationProtocol, WrongTypeRejected) {
    const std::vector<std::uint8_t> frame = EncodeAck(AckMsg{1, 2});
    UsercmdMsg out;
    EXPECT_FALSE(DecodeUsercmd(frame, out)); // type tag mismatch
}

TEST(ReplicationProtocol, QuantizationRoundTripsWithinTolerance) {
    for (float m : {0.0f, 1.234f, -56.789f, 1500.0f}) {
        EXPECT_NEAR(ReplDequantPos(ReplQuantPos(m)), m, 0.001f) << "pos " << m;
    }
    for (float a : {0.0f, 1.5707f, -3.1415f, 2.5f}) {
        EXPECT_NEAR(ReplDequantAngle(ReplQuantAngle(a)), a, 0.001f) << "angle " << a;
    }
}

// --- : unreliable-delivery reliability layer ---

SnapshotMsg MakeSnap(std::uint32_t seq, std::uint64_t tick) {
    SnapshotMsg s;
    s.snapshot_seq = seq;
    s.server_tick = tick;
    return s;
}

TEST(ReplicationReliability, SnapshotReceiverKeepsMostRecent) {
    SnapshotReceiver rx;
    EXPECT_FALSE(rx.has_snapshot());
    EXPECT_TRUE(rx.Receive(MakeSnap(1, 100)));
    EXPECT_TRUE(rx.Receive(MakeSnap(2, 130)));
    EXPECT_EQ(rx.current().snapshot_seq, 2u);

    // Out-of-order / late datagram (older seq) is discarded; current unchanged.
    EXPECT_FALSE(rx.Receive(MakeSnap(1, 100)));
    EXPECT_EQ(rx.current().snapshot_seq, 2u);
    EXPECT_EQ(rx.current().server_tick, 130u);

    // Duplicate of the current seq is also discarded.
    EXPECT_FALSE(rx.Receive(MakeSnap(2, 130)));

    // A genuinely newer datagram wins.
    EXPECT_TRUE(rx.Receive(MakeSnap(5, 220)));
    EXPECT_EQ(rx.current().snapshot_seq, 5u);
}

TEST(ReplicationReliability, AckReflectsNewestSnapshotAndUsercmd) {
    SnapshotReceiver rx;
    AckMsg none = rx.make_ack(0);
    EXPECT_EQ(none.snapshot_seq, 0u); // nothing applied yet
    rx.Receive(MakeSnap(7, 300));
    AckMsg ack = rx.make_ack(295);
    EXPECT_EQ(ack.snapshot_seq, 7u);
    EXPECT_EQ(ack.usercmd_tick, 295u);
}

TEST(ReplicationReliability, UsercmdReceiverKeepsNewestTick) {
    UsercmdReceiver rx;
    UsercmdMsg a;
    a.tick = 10;
    a.move_x = 100;
    UsercmdMsg b;
    b.tick = 12;
    b.move_x = 200;
    EXPECT_TRUE(rx.Receive(a));
    EXPECT_TRUE(rx.Receive(b));
    EXPECT_EQ(rx.latest().tick, 12u);
    // Reordered older command discarded.
    EXPECT_FALSE(rx.Receive(a));
    EXPECT_EQ(rx.latest().move_x, 200);
}

TEST(ReplicationReliability, AckTrackingIsMonotonic) {
    UsercmdReceiver rx;
    rx.ApplyAck(AckMsg{5, 0});
    EXPECT_EQ(rx.acked_snapshot_seq(), 5u);
    rx.ApplyAck(AckMsg{3, 0}); // stale ack must not lower it
    EXPECT_EQ(rx.acked_snapshot_seq(), 5u);
    rx.ApplyAck(AckMsg{9, 0});
    EXPECT_EQ(rx.acked_snapshot_seq(), 9u);
}

} // namespace
