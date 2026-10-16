#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../support/SystemConfigVariants.h"
#include "luminumbra_common/net/ReplicationAdmission.h"
#include "luminumbra_common/net/ReplicationEndpoint.h"
#include "luminumbra_common/net/ReplicationProtocol.h"

namespace {
using namespace Luminumbra::Net;

HelloMsg Identity(std::uint32_t client_id) {
    HelloMsg hello;
    hello.client_id = client_id;
    hello.seed = 42;
    hello.preset = "default";
    Luminumbra::world::GameSession session;
    hello.system_config_identity = session.SystemConfigIdentity();
    hello.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
    return hello;
}

struct Packet {
    std::vector<std::uint8_t> bytes;
    FrameDelivery delivery;
};
struct Channel {
    std::deque<Packet> packets;
    bool open = true;
};

class ScriptedTransport final : public ILockstepTransport {
public:
    ScriptedTransport(std::shared_ptr<Channel> tx, std::shared_ptr<Channel> rx)
        : m_tx(std::move(tx))
        , m_rx(std::move(rx)) {}

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   FrameDelivery delivery = FrameDelivery::Reliable) override {
        sends.push_back({frame, delivery});
        if (fail_send || !IsPeerConnected())
            return false;
        if (!suppress_delivery)
            m_tx->packets.push_back({frame, delivery});
        return true;
    }
    std::size_t MaxFramePayloadBytes() const override {
        return max_frame_payload_bytes;
    }
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override {
        FrameDelivery delivery = FrameDelivery::Reliable;
        return TryReceiveFrameWithDelivery(out, delivery);
    }
    bool TryReceiveFrameWithDelivery(std::vector<std::uint8_t>& out,
                                     FrameDelivery& delivery) override {
        if (m_rx->packets.empty())
            return false;
        out = std::move(m_rx->packets.front().bytes);
        delivery = m_rx->packets.front().delivery;
        m_rx->packets.pop_front();
        ++receives;
        return true;
    }
    bool IsPeerConnected() const override {
        return m_tx->open && m_rx->open;
    }
    void Close() override {
        CloseWithReason("closed");
    }
    void CloseWithReason(const std::string& reason) override {
        close_reason = reason;
        m_tx->open = m_rx->open = false;
        m_tx->packets.clear();
        m_rx->packets.clear();
    }
    std::size_t max_frame_payload_bytes = kMaxFramePayloadBytes;
    bool suppress_delivery = false;
    bool fail_send = false;
    std::size_t receives = 0;
    std::vector<Packet> sends;
    std::string close_reason;

private:
    std::shared_ptr<Channel> m_tx;
    std::shared_ptr<Channel> m_rx;
};

struct Pair {
    std::shared_ptr<Channel> to_host = std::make_shared<Channel>();
    std::shared_ptr<Channel> to_join = std::make_shared<Channel>();
    ScriptedTransport host_wire{to_join, to_host};
    ScriptedTransport join_wire{to_host, to_join};
    AdmittedReplicationTransport host{host_wire, ReplicationAdmissionRole::Host, Identity(0), 1};
    AdmittedReplicationTransport join{join_wire, ReplicationAdmissionRole::Joiner, Identity(1), 0};

    void Admit() {
        host.Begin(0);
        join.Begin(0);
        host.PumpAdmission(0);
        join.PumpAdmission(0);
    }
};

TEST(ReplicationAdmission, ForwardsBackendFrameLimitBeforeAndAfterAdmission) {
    Pair pair;
    const ILockstepTransport& host = pair.host;
    const ILockstepTransport& join = pair.join;
    EXPECT_EQ(host.MaxFramePayloadBytes(), kMaxFramePayloadBytes);
    EXPECT_EQ(join.MaxFramePayloadBytes(), kMaxFramePayloadBytes);
    pair.host_wire.max_frame_payload_bytes = 512u * 1024u;
    pair.join_wire.max_frame_payload_bytes = 512u * 1024u;
    EXPECT_EQ(host.MaxFramePayloadBytes(), 512u * 1024u);
    EXPECT_EQ(join.MaxFramePayloadBytes(), 512u * 1024u);
    pair.Admit();
    ASSERT_TRUE(pair.host.admission().admitted());
    ASSERT_TRUE(pair.join.admission().admitted());
    EXPECT_EQ(host.MaxFramePayloadBytes(), 512u * 1024u);
    EXPECT_EQ(join.MaxFramePayloadBytes(), 512u * 1024u);
}

TEST(ReplicationAdmission, UnreliableHelloClosesHostAndJoinerBeforeAdmission) {
    for (const bool is_host : {false, true}) {
        SCOPED_TRACE(is_host);
        Pair pair;
        auto& gate = is_host ? pair.host : pair.join;
        auto& wire = is_host ? pair.host_wire : pair.join_wire;
        auto& peer = is_host ? pair.join_wire : pair.host_wire;
        gate.Begin(0);
        ASSERT_FALSE(gate.admission().admitted());
        ASSERT_TRUE(gate.IsPeerConnected());
        const auto sends_before = wire.sends.size();
        ASSERT_TRUE(
            peer.SendFrame(EncodeHello(Identity(is_host ? 1u : 0u)), FrameDelivery::Unreliable));
        gate.PumpAdmission(1);
        EXPECT_TRUE(gate.admission().failed());
        EXPECT_FALSE(gate.admission().admitted());
        EXPECT_FALSE(gate.IsPeerConnected());
        EXPECT_FALSE(wire.IsPeerConnected());
        EXPECT_EQ(gate.admission().reason(), "unreliable admission Hello");
        EXPECT_EQ(wire.close_reason, "unreliable admission Hello");
        EXPECT_EQ(wire.sends.size(), sends_before);
    }
}

TEST(ReplicationAdmission, FailedConnectionBeforeBeginReportsDisconnectedWithoutHello) {
    for (const bool is_host : {false, true}) {
        SCOPED_TRACE(is_host);
        Pair pair;
        auto& gate = is_host ? pair.host : pair.join;
        auto& wire = is_host ? pair.host_wire : pair.join_wire;
        wire.Close();
        gate.Begin(0);
        gate.PumpAdmission(1);
        EXPECT_TRUE(gate.admission().failed());
        EXPECT_FALSE(gate.admission().admitted());
        EXPECT_FALSE(gate.IsPeerConnected());
        EXPECT_EQ(gate.admission().reason(), "admission disconnected");
        EXPECT_EQ(wire.close_reason, "admission disconnected");
        EXPECT_TRUE(wire.sends.empty());
    }
}

TEST(ReplicationAdmission, CommandsBeforeHelloAreDiscardedAndLaterCommandsReachServer) {
    for (const bool command_first : {false, true}) {
        SCOPED_TRACE(command_first);
        Pair pair;
        ReplicationServer server;
        // Premature endpoint use is safe, though modes register only after admission.
        server.AddClient(1, &pair.host);
        pair.host.Begin(10);
        UsercmdMsg early;
        early.tick = 1;
        early.player_id = 1;
        early.move_x = 123;
        if (command_first) {
            ASSERT_TRUE(pair.join_wire.SendFrame(EncodeUsercmd(early), FrameDelivery::Unreliable));
            server.PumpInbound();
            EXPECT_EQ(server.LatestUsercmd(1), nullptr);
            EXPECT_TRUE(pair.host.IsPeerConnected());
            EXPECT_FALSE(pair.host.admission().admitted());
            // Queue another command in the same drain as Hello.
            ASSERT_TRUE(pair.join_wire.SendFrame(EncodeUsercmd(early), FrameDelivery::Unreliable));
        }
        ASSERT_TRUE(pair.join_wire.SendFrame(EncodeHello(Identity(1))));
        UsercmdMsg later = early;
        later.tick = 2;
        later.move_x = 456;
        ASSERT_TRUE(pair.join_wire.SendFrame(EncodeUsercmd(later), FrameDelivery::Unreliable));
        pair.host.PumpAdmission(11);
        ASSERT_TRUE(pair.host.admission().admitted());
        EXPECT_EQ(server.LatestUsercmd(1), nullptr);
        server.PumpInbound();
        ASSERT_NE(server.LatestUsercmd(1), nullptr);
        EXPECT_EQ(*server.LatestUsercmd(1), later);
        ASSERT_EQ(pair.host_wire.sends.size(), 1u);
        EXPECT_EQ(pair.host_wire.sends[0].bytes, EncodeHello(Identity(0)));
        EXPECT_EQ(pair.host_wire.sends[0].delivery, FrameDelivery::Reliable);
    }
}

TEST(ReplicationAdmission, SnapshotBeforeAckCannotPopulateHistoryOrSendSnapshotAck) {
    Pair pair;
    ReplicationClient client(1, &pair.join);
    pair.host.Begin(0);
    pair.join.Begin(0);
    ASSERT_EQ(pair.join_wire.sends.size(), 1u);
    EXPECT_EQ(pair.join_wire.sends[0].delivery, FrameDelivery::Reliable);
    SnapshotMsg early;
    early.snapshot_seq = 1;
    early.server_tick = 1;
    early.entities.push_back(ReplEntityState{});
    EXPECT_FALSE(pair.host.SendFrame(EncodeSnapshot(early), FrameDelivery::Unreliable));
    EXPECT_TRUE(pair.host_wire.sends.empty());
    ASSERT_TRUE(pair.host_wire.SendFrame(EncodeSnapshot(early), FrameDelivery::Unreliable));
    UsercmdMsg cmd;
    cmd.tick = 1;
    cmd.player_id = 1;
    client.SendUsercmd(cmd);
    client.PumpInbound();
    EXPECT_FALSE(client.has_snapshot());
    EXPECT_EQ(pair.join_wire.sends.size(), 1u); // Only the reliable Hello.

    pair.host.PumpAdmission(1);
    // A delta against the discarded snapshot must remain unusable after admission.
    SnapshotMsg delta = early;
    delta.snapshot_seq = 2;
    delta.delta_from_seq = 1;
    ASSERT_TRUE(pair.host_wire.SendFrame(EncodeSnapshot(delta), FrameDelivery::Unreliable));
    pair.join.PumpAdmission(1);
    ASSERT_TRUE(pair.join.admission().admitted());
    EXPECT_EQ(pair.to_join->packets.size(), 1u); // Stop precisely at acknowledgement.
    client.PumpInbound();
    EXPECT_FALSE(client.has_snapshot());

    ReplicationServer server;
    server.AddClient(1, &pair.host);
    client.SendUsercmd(cmd);
    server.PumpInbound();
    ASSERT_NE(server.LatestUsercmd(1), nullptr);
    EXPECT_EQ(*server.LatestUsercmd(1), cmd);
    ReplEntityState entity;
    entity.entity_id = 1;
    server.BroadcastSnapshot(3, {entity});
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    EXPECT_EQ(client.snapshot().entities.size(), 1u);
    server.PumpInbound();
    EXPECT_EQ(server.AckedSnapshotSeq(1), client.snapshot().snapshot_seq);
}

TEST(ReplicationAdmission, LostReliableAckExpiresAt150WithoutTrafficExtendingDeadline) {
    Pair pair;
    pair.host_wire.suppress_delivery = true;
    pair.host.Begin(20);
    pair.join.Begin(20);
    pair.host.PumpAdmission(20);
    ASSERT_TRUE(pair.host.admission().admitted());
    ASSERT_EQ(pair.host_wire.sends.size(), 1u);
    EXPECT_EQ(pair.host_wire.sends[0].delivery, FrameDelivery::Reliable);
    pair.host_wire.suppress_delivery = false;
    SnapshotMsg snapshot;
    snapshot.snapshot_seq = 1;
    for (std::uint64_t now = 21; now < 170; ++now) {
        ASSERT_TRUE(pair.host_wire.SendFrame(EncodeSnapshot(snapshot), FrameDelivery::Unreliable));
        pair.join.PumpAdmission(now);
        ASSERT_FALSE(pair.join.admission().admitted());
        ASSERT_TRUE(pair.join.IsPeerConnected());
    }
    pair.join.PumpAdmission(170);
    EXPECT_TRUE(pair.join.admission().failed());
    EXPECT_EQ(pair.join_wire.close_reason, "admission ack timeout");
}

TEST(ReplicationAdmission, DrainIsBoundedAndHostDeadlineUsesExplicitElapsedTicks) {
    Pair pair;
    pair.host.Begin(1000);
    for (std::size_t i = 0; i < 3 * kAdmissionDrainLimit; ++i)
        ASSERT_TRUE(
            pair.join_wire.SendFrame(EncodeUsercmd(UsercmdMsg{}), FrameDelivery::Unreliable));
    pair.host.PumpAdmission(1149);
    EXPECT_EQ(pair.host_wire.receives, kAdmissionDrainLimit);
    EXPECT_FALSE(pair.host.admission().failed());
    pair.host.PumpAdmission(1150);
    EXPECT_EQ(pair.host_wire.receives, kAdmissionDrainLimit);
    EXPECT_EQ(pair.host_wire.close_reason, "admission Hello timeout");
}

TEST(ReplicationAdmission, DuplicateHelloIsConsumedAndResendsAckWithoutResettingReplication) {
    Pair pair;
    pair.Admit();
    ASSERT_TRUE(pair.host.admission().admitted());
    ASSERT_TRUE(pair.join.admission().admitted());
    ReplicationServer server;
    server.AddClient(1, &pair.host);
    UsercmdMsg cmd;
    cmd.player_id = 1;
    cmd.tick = 10;
    ASSERT_TRUE(pair.join.SendFrame(EncodeUsercmd(cmd), FrameDelivery::Unreliable));
    server.PumpInbound();
    ASSERT_NE(server.LatestUsercmd(1), nullptr);
    ASSERT_TRUE(pair.join_wire.SendFrame(EncodeHello(Identity(1))));
    server.PumpInbound();
    ASSERT_EQ(pair.host_wire.sends.size(), 2u);
    EXPECT_EQ(pair.host_wire.sends.back().bytes, EncodeHello(Identity(0)));
    EXPECT_EQ(pair.host_wire.sends.back().delivery, FrameDelivery::Reliable);
    EXPECT_EQ(*server.LatestUsercmd(1), cmd);
    std::vector<std::uint8_t> frame;
    EXPECT_FALSE(pair.join.TryReceiveFrame(frame)); // Duplicate ack is control too.
    EXPECT_TRUE(pair.join.admission().admitted());
}

TEST(ReplicationAdmission, CommandWhoseTickStartsWithHelloMagicIsGameplay) {
    Pair pair;
    pair.Admit();
    UsercmdMsg cmd;
    cmd.tick = 0x315054534cULL; // Little endian LSTP1.
    cmd.player_id = 1;
    cmd.move_x = 32767;
    ASSERT_TRUE(pair.join.SendFrame(EncodeUsercmd(cmd), FrameDelivery::Unreliable));
    ReplicationServer server;
    server.AddClient(1, &pair.host);
    server.PumpInbound();
    ASSERT_NE(server.LatestUsercmd(1), nullptr);
    EXPECT_EQ(*server.LatestUsercmd(1), cmd);
    EXPECT_TRUE(pair.host.admission().admitted());
}

ReplicationAdmissionState Waiting(ReplicationAdmissionRole role, const HelloMsg& local) {
    ReplicationAdmissionState state(role, local, role == ReplicationAdmissionRole::Host ? 1u : 0u);
    state.Begin(0);
    if (role == ReplicationAdmissionRole::Joiner)
        state.OnHelloSendResult(true, 10);
    return state;
}

TEST(ReplicationAdmission, HostAndJoinerRejectAllEffectiveConfigAndContentMismatches) {
    const auto variants = Luminumbra::TestSupport::EffectiveIdentityMismatches();
    ASSERT_FALSE(variants.empty());
    for (const auto role : {ReplicationAdmissionRole::Host, ReplicationAdmissionRole::Joiner}) {
        const auto local = Identity(role == ReplicationAdmissionRole::Host ? 0u : 1u);
        const auto compatible = Identity(role == ReplicationAdmissionRole::Host ? 1u : 0u);
        for (const auto& [name, identity] : variants) {
            SCOPED_TRACE(name);
            auto peer = compatible;
            peer.system_config_identity = identity;
            auto state = Waiting(role, local);
            state.OnFrame(EncodeHello(peer), FrameDelivery::Reliable);
            EXPECT_TRUE(state.failed());
            EXPECT_EQ(state.reason(), "SystemConfig identity mismatch");
        }
        for (const bool missing_local : {false, true}) {
            for (const bool config : {false, true}) {
                auto mine = local;
                auto peer = compatible;
                auto& changed = missing_local ? mine : peer;
                if (config)
                    changed.system_config_identity.clear();
                else
                    changed.preset_content_hash.clear();
                auto state = Waiting(role, mine);
                state.OnFrame(EncodeHello(peer), FrameDelivery::Reliable);
                EXPECT_TRUE(state.failed());
                EXPECT_EQ(state.reason(),
                          config ? "SystemConfig identity mismatch"
                                 : "preset content hash mismatch");
            }
        }
        auto changed_preset = Luminumbra::TestSupport::PresetIdentityFixture();
        changed_preset["generation_params"]["terrain"]["base_amplitude"] = 13.0;
        auto peer = compatible;
        peer.preset_content_hash = Luminumbra::world::TerrainPresetContentHash(changed_preset);
        auto state = Waiting(role, local);
        state.OnFrame(EncodeHello(peer), FrameDelivery::Reliable);
        EXPECT_TRUE(state.failed());
        EXPECT_EQ(state.reason(), "preset content hash mismatch");
    }
}

TEST(ReplicationAdmission, BothDirectionsRejectWrongVersionIdAndOtherIdentityFields) {
    for (const auto role : {ReplicationAdmissionRole::Host, ReplicationAdmissionRole::Joiner}) {
        const auto local = Identity(role == ReplicationAdmissionRole::Host ? 0u : 1u);
        const auto compatible = Identity(role == ReplicationAdmissionRole::Host ? 1u : 0u);
        for (const std::uint16_t version : {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{4}}) {
            auto peer = compatible;
            peer.protocol_version = version;
            auto state = Waiting(role, local);
            state.OnFrame(EncodeHello(peer), FrameDelivery::Reliable);
            EXPECT_EQ(state.reason(), "protocol version mismatch");
        }
        for (int field = 0; field < 4; ++field) {
            auto peer = compatible;
            if (field == 0)
                peer.client_id = 9;
            if (field == 1)
                ++peer.seed;
            if (field == 2)
                peer.preset = "different";
            if (field == 3)
                ++peer.tick_rate_hz;
            auto state = Waiting(role, local);
            state.OnFrame(EncodeHello(peer), FrameDelivery::Reliable);
            EXPECT_TRUE(state.failed());
            EXPECT_EQ(state.reason(),
                      field == 0 ? "admission client id mismatch"
                                 : "admission seed, preset or tick rate mismatch");
        }
    }
}

TEST(ReplicationAdmission, BothDirectionsRejectUnreliableMalformedAndNoncanonicalHello) {
    for (const auto role : {ReplicationAdmissionRole::Host, ReplicationAdmissionRole::Joiner}) {
        const auto local = Identity(role == ReplicationAdmissionRole::Host ? 0u : 1u);
        const auto peer = Identity(role == ReplicationAdmissionRole::Host ? 1u : 0u);
        auto state = Waiting(role, local);
        state.OnFrame(EncodeHello(peer), FrameDelivery::Unreliable);
        EXPECT_EQ(state.reason(), "unreliable admission Hello");
        auto valid = EncodeHello(peer);
        for (std::size_t size = 0; size < valid.size(); ++size) {
            auto truncated = valid;
            truncated.resize(size);
            auto truncated_state = Waiting(role, local);
            truncated_state.OnFrame(truncated, FrameDelivery::Reliable);
            EXPECT_TRUE(truncated_state.failed()) << size;
        }
        for (const bool update_length : {false, true}) {
            auto trailing = valid;
            trailing.push_back(0);
            if (update_length)
                ++trailing[1];
            auto trailing_state = Waiting(role, local);
            trailing_state.OnFrame(trailing, FrameDelivery::Reliable);
            EXPECT_TRUE(trailing_state.failed());
        }
        auto wrong_magic = valid;
        wrong_magic[5] = 'X';
        auto magic_state = Waiting(role, local);
        magic_state.OnFrame(wrong_magic, FrameDelivery::Reliable);
        EXPECT_EQ(magic_state.reason(), "malformed admission Hello");
        auto extended_command = EncodeUsercmd(UsercmdMsg{});
        extended_command.push_back(0);
        ++extended_command[1];
        auto command_state = Waiting(role, local);
        command_state.OnFrame(extended_command, FrameDelivery::Reliable);
        EXPECT_EQ(command_state.reason(), "malformed admission Hello");
    }
}

TEST(ReplicationAdmission, AckEnqueueIsRequiredAndFailureIsTerminal) {
    auto state = Waiting(ReplicationAdmissionRole::Host, Identity(0));
    EXPECT_EQ(state.OnFrame(EncodeHello(Identity(1)), FrameDelivery::Reliable),
              ReplicationAdmissionAction::SendAck);
    EXPECT_EQ(state.phase(), ReplicationAdmissionPhase::SendingAck);
    EXPECT_FALSE(state.admitted());
    state.OnAckSendResult(false);
    EXPECT_EQ(state.reason(), "admission ack send failed");
    state.OnAckSendResult(true);
    state.Begin(100);
    EXPECT_TRUE(state.failed());
    EXPECT_EQ(state.OnFrame(EncodeHello(Identity(1)), FrameDelivery::Reliable),
              ReplicationAdmissionAction::Discard);

    Pair pair;
    pair.host_wire.fail_send = true;
    pair.host.Begin(0);
    pair.join.Begin(0);
    pair.host.PumpAdmission(0);
    EXPECT_FALSE(pair.host.admission().admitted());
    EXPECT_EQ(pair.host_wire.close_reason, "admission ack send failed");
}

TEST(ReplicationAdmission, HelloSendFailureAndDisconnectCloseWithSpecificReasons) {
    Pair failed_send;
    failed_send.join_wire.fail_send = true;
    failed_send.join.Begin(0);
    EXPECT_EQ(failed_send.join_wire.close_reason, "admission Hello send failed");
    for (const bool host : {false, true}) {
        Pair pair;
        auto& gate = host ? pair.host : pair.join;
        auto& wire = host ? pair.host_wire : pair.join_wire;
        gate.Begin(0);
        wire.Close();
        gate.PumpAdmission(1);
        EXPECT_EQ(wire.close_reason, "admission disconnected");
    }
    Pair admitted;
    admitted.Admit();
    admitted.host_wire.Close();
    std::vector<std::uint8_t> frame;
    EXPECT_FALSE(admitted.join.TryReceiveFrame(frame));
    EXPECT_TRUE(admitted.join.admission().failed());
    EXPECT_EQ(admitted.join_wire.close_reason, "admission disconnected");
}

TEST(ReplicationAdmission, JoinDeadlineStartsAtSuccessfulHelloEnqueue) {
    ReplicationAdmissionState state(ReplicationAdmissionRole::Joiner, Identity(1), 0);
    EXPECT_EQ(state.Begin(0), ReplicationAdmissionAction::SendHello);
    state.CheckDeadline(1000);
    EXPECT_FALSE(state.failed());
    state.OnHelloSendResult(true, 1000);
    for (int pump = 0; pump < 1000; ++pump)
        state.CheckDeadline(1149);
    EXPECT_FALSE(state.failed());
    state.CheckDeadline(1150);
    EXPECT_EQ(state.reason(), "admission ack timeout");
}

} // namespace
