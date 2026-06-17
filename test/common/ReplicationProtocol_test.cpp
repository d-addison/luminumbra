// T-I6 P3.0: authoritative-server state-replication wire protocol. Proves the
// Usercmd / Snapshot / Ack messages round-trip (encode -> decode -> equal),
// including a multi-entity snapshot and truncation-robustness, and that the
// position/angle quantization round-trips within tolerance.
#include <gtest/gtest.h>

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

TEST(ReplicationProtocol, EmptySnapshotRoundTrips) {
    SnapshotMsg in;
    in.server_tick = 1;
    in.snapshot_seq = 1;
    const std::vector<std::uint8_t> frame = EncodeSnapshot(in);
    SnapshotMsg out;
    ASSERT_TRUE(DecodeSnapshot(frame, out));
    EXPECT_TRUE(out.entities.empty());
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

} // namespace
