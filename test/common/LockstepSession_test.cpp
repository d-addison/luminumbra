// LockstepSession unit tests (src/luminumbra_common/net/LockstepSession.{h,cpp}).
//
// These lock the ENGINE-GENERIC delay-based lockstep behavior over in-process transports
// (no real sockets/ports -- hermetic, no firewall/CI flakiness): handshake
// accept/reject, in-sync tick exchange, adaptive-horizon absorption of a late input,
// desync detection + LREC1 dump emission, and clean disconnect. Both session ends run in
// this one process, driven turn-by-turn so the loopback queues carry their frames.

#include "../support/SystemConfigVariants.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/NetworkSim.h"
#include "luminumbra_common/replay/ReplayStream.h"

namespace net = Luminumbra::Net;
namespace replay = Luminumbra::Replay;
namespace fs = std::filesystem;

namespace {

fs::path LockstepTempDir() {
#ifdef LUMINUMBRA_TEST_ARTIFACT_DIR
    fs::path dir = fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "lockstep-unit";
#else
    fs::path dir = fs::temp_directory_path() / "luminumbra-lockstep-unit";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// A deterministic per-peer fake "world": the hash is a function of (tick, a per-peer
// salt). With identical salts the two peers agree at every cadence (in sync); a differing
// salt at/after some tick forces a desync the oracle must catch.
struct FakeWorld {
    std::uint64_t tick = 0;
    std::string salt;               // identical across peers => agree
    std::uint64_t corrupt_from = 0; // ticks >= this use a corrupted salt (0 = never)

    std::string HashAt(std::uint64_t t, const char* section) const {
        const std::string s = (corrupt_from != 0 && t >= corrupt_from) ? (salt + "X") : salt;
        return replay::Fnv1a64Hex(s + "|" + section + "|" + std::to_string(t));
    }
};

std::vector<std::uint8_t> CollectEmpty(std::uint64_t, void*) {
    return {}; // headless: no player inputs today (empty input set must work)
}

bool ApplyStep(std::uint64_t tick, const std::vector<std::uint8_t>&, void* user) {
    auto* w = static_cast<FakeWorld*>(user);
    w->tick = tick;
    return true;
}

void CaptureHashes(std::uint64_t tick, net::HashMsg& out, void* user) {
    auto* w = static_cast<FakeWorld*>(user);
    out.tick = tick;
    out.terrain = w->HashAt(tick, "terrain");
    out.water = w->HashAt(tick, "water");
    out.entities = w->HashAt(tick, "entities");
    // The combined world_hash mixes the sub-hashes (the real server folds mesh in too;
    // for the fake world terrain/water/entities suffice to localize divergence).
    out.world_hash = replay::Fnv1a64Hex(out.terrain + out.water + out.entities);
}

net::LockstepHooks MakeHooks(FakeWorld* w) {
    net::LockstepHooks h;
    h.collect_local_input = &CollectEmpty;
    h.apply_and_step = &ApplyStep;
    h.capture_hashes = &CaptureHashes;
    h.user = w;
    return h;
}

net::LockstepConfig MakeConfig(std::uint32_t local_id, std::uint32_t peer_id) {
    net::LockstepConfig c;
    c.seed = 424242;
    c.preset = "default";
    c.tick_rate_hz = 30;
    c.local_client_id = local_id;
    c.peer_client_id = peer_id;
    c.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    c.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
    return c;
}

bool SendPeerHello(net::ILockstepTransport& peer, const net::LockstepConfig& config) {
    net::HelloMsg hello;
    hello.seed = config.seed;
    hello.preset = config.preset;
    hello.tick_rate_hz = config.tick_rate_hz;
    hello.client_id = config.peer_client_id;
    hello.system_config_identity = config.system_config_identity;
    hello.preset_content_hash = config.preset_content_hash;
    return peer.SendFrame(net::EncodeHello(hello));
}

// Reject a selected message and report a disconnected peer. Buffered receive frames
// remain readable until Close so the session can compare hashes before disconnecting.
struct RejectingTransport final : net::ILockstepTransport {
    explicit RejectingTransport(net::ILockstepTransport& transport)
        : inner(transport) {}

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   net::FrameDelivery delivery = net::FrameDelivery::Reliable) override {
        net::MessageType type;
        if (rejected && net::PeekMessageType(frame, type) && type == *rejected) {
            peer_disconnected = true;
            if (on_rejection)
                on_rejection();
            return false;
        }
        return inner.SendFrame(frame, delivery);
    }
    std::size_t MaxFramePayloadBytes() const override {
        return inner.MaxFramePayloadBytes();
    }
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override {
        if (!closed && inner.TryReceiveFrame(out))
            return true;
        if (peer_disconnected)
            drained = true;
        return false;
    }
    bool IsPeerConnected() const override {
        return !closed && (!peer_disconnected || (drain_before_disconnect && !drained)) &&
               inner.IsPeerConnected();
    }
    void Close() override {
        closed = true;
        inner.Close();
    }

    net::ILockstepTransport& inner;
    std::optional<net::MessageType> rejected;
    std::function<void()> on_rejection;
    bool closed = false;
    bool peer_disconnected = false;
    bool drain_before_disconnect = false;
    bool drained = false;
};

// Once armed by a message type, refuse the next N sends without accepting any bytes.
struct BackpressuredTransport final : net::ILockstepTransport {
    explicit BackpressuredTransport(net::ILockstepTransport& transport)
        : inner(transport) {}

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   net::FrameDelivery delivery = net::FrameDelivery::Reliable) override {
        net::MessageType type;
        const bool typed = net::PeekMessageType(frame, type);
        if (typed && type == net::MessageType::Bye)
            ++bye_attempts;
        if (start_refusing_at && typed && type == *start_refusing_at)
            start_refusing_at.reset();
        if (!start_refusing_at && refusals_remaining > 0) {
            --refusals_remaining;
            if (record_refused_frames)
                refused_frames.push_back(frame);
            return false;
        }
        return inner.SendFrame(frame, delivery);
    }
    std::size_t MaxFramePayloadBytes() const override {
        return std::min(payload_limit, inner.MaxFramePayloadBytes());
    }
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override {
        return !closed && inner.TryReceiveFrame(out);
    }
    bool IsPeerConnected() const override {
        return !closed && !peer_disconnected && inner.IsPeerConnected();
    }
    void Close() override {
        closed = true;
        inner.Close();
    }

    net::ILockstepTransport& inner;
    std::optional<net::MessageType> start_refusing_at;
    std::size_t refusals_remaining = 0;
    std::size_t payload_limit = net::kMaxFramePayloadBytes;
    std::size_t bye_attempts = 0;
    std::vector<std::vector<std::uint8_t>> refused_frames;
    bool closed = false;
    bool peer_disconnected = false;
    bool record_refused_frames = true;
};

} // namespace

TEST(LockstepSessionTest, DisconnectFlushesQueuedFramesBeforeByeOrClosesWithoutBye) {
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        for (const bool recovered : {false, true}) {
            SCOPED_TRACE(static_cast<int>(type));
            SCOPED_TRACE(recovered);
            auto [local, peer] = net::MakeLoopbackPair();
            BackpressuredTransport transport(*local);
            transport.start_refusing_at = type;
            transport.refusals_remaining = 1;
            auto config = MakeConfig(0, 1);
            config.horizon_min = config.horizon_max = config.horizon_start = 1;
            config.hash_cadence_ticks = 1;
            FakeWorld world{0, "agree", 0};
            net::LockstepSession session(config, &transport, MakeHooks(&world));
            ASSERT_TRUE(SendPeerHello(*peer, config));
            ASSERT_TRUE(session.Handshake());
            std::vector<std::uint8_t> frame;
            ASSERT_TRUE(peer->TryReceiveFrame(frame)); // Hello.
            std::vector<std::vector<std::uint8_t>> expected;
            if (type == net::MessageType::Hash) {
                ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
                ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
                expected.push_back(net::EncodeInput({1, 0, {}}));
                expected.push_back(net::EncodeInput({2, 0, {}}));
            }
            ASSERT_EQ(transport.refused_frames.size(), 1u);
            if (recovered) {
                expected.push_back(transport.refused_frames.front());
                expected.push_back(net::EncodeBye({world.tick}));
            }
            transport.refusals_remaining = recovered ? 0 : 1;
            session.Disconnect();
            session.Disconnect();
            EXPECT_EQ(transport.bye_attempts, recovered ? 1u : 0u);
            EXPECT_TRUE(transport.closed);
            EXPECT_TRUE(session.Status().peer_disconnected);
            EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
            std::vector<std::vector<std::uint8_t>> received;
            while (peer->TryReceiveFrame(frame))
                received.push_back(frame);
            EXPECT_EQ(received, expected);
            EXPECT_FALSE(peer->IsPeerConnected());
        }
    }
}

TEST(LockstepSessionTest, TransportInputAndHashLimitsAreInclusiveAndCheckedBeforeQueueing) {
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        for (const std::size_t excess : {0u, 1u}) {
            SCOPED_TRACE(static_cast<int>(type));
            SCOPED_TRACE(excess);
            auto [local, peer] = net::MakeLoopbackPair();
            BackpressuredTransport transport(*local);
            transport.payload_limit = 128;
            transport.start_refusing_at = type;
            transport.refusals_remaining = 1;
            auto config = MakeConfig(0, 1);
            config.horizon_min = config.horizon_max = config.horizon_start = 1;
            config.hash_cadence_ticks = 1;
            struct State {
                std::size_t input_size;
                std::size_t hash_size;
            } state{type == net::MessageType::Input ? 128 - 21 + excess : 0,
                    type == net::MessageType::Hash ? 128 - 29 + excess : 0};
            net::LockstepHooks hooks;
            hooks.collect_local_input = [](std::uint64_t, void* user) {
                return std::vector<std::uint8_t>(static_cast<State*>(user)->input_size, 0xAB);
            };
            hooks.capture_hashes = [](std::uint64_t, net::HashMsg& out, void* user) {
                out.world_hash.assign(static_cast<State*>(user)->hash_size, 'h');
            };
            hooks.user = &state;
            net::LockstepSession session(config, &transport, hooks);
            ASSERT_TRUE(SendPeerHello(*peer, config));
            const bool handshaken = session.Handshake();
            if (type == net::MessageType::Input) {
                EXPECT_EQ(handshaken, excess == 0);
            } else {
                ASSERT_TRUE(handshaken);
                ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
                EXPECT_EQ(session.PumpTick(10).outcome,
                          excess == 0 ? net::TickOutcome::Advanced
                                      : net::TickOutcome::PeerDisconnected);
            }
            EXPECT_EQ(transport.refused_frames.size(), excess == 0 ? 1u : 0u);
            EXPECT_EQ(transport.closed, excess != 0);
            if (excess == 0) {
                EXPECT_EQ(transport.refused_frames.front().size(), 128u);
                session.Disconnect();
                std::vector<std::uint8_t> frame;
                bool received_exact_frame = false;
                while (peer->TryReceiveFrame(frame)) {
                    if (frame == transport.refused_frames.front())
                        received_exact_frame = true;
                }
                EXPECT_TRUE(received_exact_frame);
            } else {
                EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
            }
        }
    }
}

TEST(LockstepSessionTest, TransportLimitBelowInputOverheadIsTerminal) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    transport.payload_limit = 0;
    const auto config = MakeConfig(0, 1);
    net::LockstepSession session(config, &transport, {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    EXPECT_FALSE(session.Handshake());
    EXPECT_TRUE(transport.closed);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
}

TEST(LockstepSessionTest, PendingOutboundByteLimitAcceptsExactLimitThenDisconnectsOnOverflow) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    transport.start_refusing_at = net::MessageType::Input;
    transport.refusals_remaining = 2;
    transport.record_refused_frames = false;
    auto config = MakeConfig(0, 1);
    config.horizon_min = config.horizon_max = config.horizon_start = 1;
    net::LockstepHooks hooks;
    hooks.collect_local_input = [](std::uint64_t tick, void*) {
        return std::vector<std::uint8_t>(tick == 1 ? net::kMaxFramePayloadBytes - 21 : 0, 0xAB);
    };
    net::LockstepSession session(config, &transport, hooks);
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake()); // Exactly 64 MiB of encoded bytes retained.
    EXPECT_FALSE(transport.closed);
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
    // The final refusal keeps that frame queued; tick 2's input would exceed the cap.
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_TRUE(transport.closed);
    EXPECT_EQ(session.AgreedTick(), 1u);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
}

TEST(LockstepSessionTest, DifferentPeerInputsMergeIdenticallyUnderTransientRefusal) {
    auto [ta, tb] = net::MakeLoopbackPair();
    BackpressuredTransport a_transport(*ta);
    BackpressuredTransport b_transport(*tb);
    a_transport.start_refusing_at = net::MessageType::Input;
    b_transport.start_refusing_at = net::MessageType::Hash;
    a_transport.refusals_remaining = b_transport.refusals_remaining = 3;
    struct State {
        std::uint8_t id;
        std::vector<std::vector<std::uint8_t>> applied;
    } a_state{0, {}}, b_state{1, {}};
    const auto hooks_for = [](State& state) {
        net::LockstepHooks hooks;
        hooks.collect_local_input = [](std::uint64_t tick, void* user) {
            return std::vector<std::uint8_t>{static_cast<State*>(user)->id,
                                             static_cast<std::uint8_t>(tick)};
        };
        hooks.apply_and_step =
            [](std::uint64_t, const std::vector<std::uint8_t>& merged, void* user) {
                static_cast<State*>(user)->applied.push_back(merged);
                return true;
            };
        hooks.user = &state;
        return hooks;
    };
    auto a_config = MakeConfig(0, 1);
    auto b_config = MakeConfig(1, 0);
    a_config.hash_cadence_ticks = b_config.hash_cadence_ticks = 1;
    net::LockstepSession a(a_config, &a_transport, hooks_for(a_state));
    net::LockstepSession b(b_config, &b_transport, hooks_for(b_state));
    ASSERT_TRUE(SendPeerHello(*tb, a_config));
    ASSERT_TRUE(SendPeerHello(*ta, b_config));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());
    bool a_finished = false;
    bool b_finished = false;
    for (int pump = 0; pump < 100 && !(a_finished && b_finished); ++pump) {
        const auto ar = a.PumpTick(10);
        const auto br = b.PumpTick(10);
        ASSERT_NE(ar.outcome, net::TickOutcome::Desync);
        ASSERT_NE(br.outcome, net::TickOutcome::Desync);
        ASSERT_NE(ar.outcome, net::TickOutcome::PeerDisconnected);
        ASSERT_NE(br.outcome, net::TickOutcome::PeerDisconnected);
        a_finished = ar.outcome == net::TickOutcome::Finished;
        b_finished = br.outcome == net::TickOutcome::Finished;
    }
    ASSERT_TRUE(a_finished);
    ASSERT_TRUE(b_finished);
    EXPECT_EQ(a_state.applied, b_state.applied);
    ASSERT_EQ(a_state.applied.size(), 10u);
    for (std::uint8_t tick = 1; tick <= 10; ++tick)
        EXPECT_EQ(a_state.applied[tick - 1], (std::vector<std::uint8_t>{0, tick, 1, tick}));
}

TEST(LockstepSessionTest, TransientInputAndHashRefusalsRetryExactFramesInOrder) {
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto [local, peer] = net::MakeLoopbackPair();
        BackpressuredTransport transport(*local);
        transport.start_refusing_at = type;
        transport.refusals_remaining = 3;
        auto config = MakeConfig(0, 1);
        config.horizon_min = config.horizon_max = config.horizon_start;
        config.hash_cadence_ticks = 1;
        struct State {
            FakeWorld world{0, "agree", 0};
            std::vector<std::uint64_t> collected;
            std::vector<std::uint64_t> captured;
        } state;
        net::LockstepHooks hooks;
        hooks.collect_local_input = [](std::uint64_t tick, void* user) {
            static_cast<State*>(user)->collected.push_back(tick);
            return std::vector<std::uint8_t>{static_cast<std::uint8_t>(tick), 0, 0xFF};
        };
        hooks.apply_and_step =
            [](std::uint64_t tick, const std::vector<std::uint8_t>& merged, void* user) {
                return ApplyStep(tick, merged, &static_cast<State*>(user)->world);
            };
        hooks.capture_hashes = [](std::uint64_t tick, net::HashMsg& out, void* user) {
            auto* current = static_cast<State*>(user);
            current->captured.push_back(tick);
            CaptureHashes(tick, out, &current->world);
        };
        hooks.user = &state;
        net::LockstepSession session(config, &transport, hooks);
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        std::vector<std::uint8_t> frame;
        ASSERT_TRUE(peer->TryReceiveFrame(frame));
        net::HelloMsg hello;
        ASSERT_TRUE(net::DecodeHello(frame, hello));

        std::vector<std::vector<std::uint8_t>> expected;
        for (std::uint64_t tick = 1; tick <= config.horizon_start; ++tick)
            expected.push_back(
                net::EncodeInput({tick, 0, {static_cast<std::uint8_t>(tick), 0, 0xFF}}));
        for (std::uint64_t tick = 1; tick <= 6; ++tick) {
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
            const auto scheduled = tick + config.horizon_start;
            expected.push_back(
                net::EncodeInput({scheduled, 0, {static_cast<std::uint8_t>(scheduled), 0, 0xFF}}));
            net::HashMsg hash;
            CaptureHashes(tick, hash, &state.world);
            expected.push_back(net::EncodeHash(hash));
        }
        // Also refuse the final cadence hash: reaching the tick budget must not
        // finish the driver while the last encoded frame still needs retrying.
        for (std::uint64_t tick = 1; tick <= 6; ++tick) {
            if (tick == 6) {
                transport.start_refusing_at = net::MessageType::Hash;
                transport.refusals_remaining = 2;
            }
            const auto result = session.PumpTick(6);
            ASSERT_EQ(result.outcome, net::TickOutcome::Advanced);
            EXPECT_EQ(result.tick, tick);
            EXPECT_FALSE(session.Status().peer_disconnected);
            EXPECT_TRUE(transport.IsPeerConnected());
        }
        EXPECT_EQ(session.PumpTick(6).outcome, net::TickOutcome::WaitingForPeer);
        EXPECT_EQ(session.PumpTick(6).outcome, net::TickOutcome::Finished);
        EXPECT_EQ(session.PumpTick(6).outcome, net::TickOutcome::Finished);
        EXPECT_FALSE(transport.closed);
        EXPECT_FALSE(session.Status().peer_disconnected);
        EXPECT_EQ(state.world.tick, 6u);
        EXPECT_EQ(state.collected, (std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
        EXPECT_EQ(state.captured, (std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6}));

        std::vector<std::vector<std::uint8_t>> received;
        while (peer->TryReceiveFrame(frame))
            received.push_back(frame);
        EXPECT_EQ(received, expected); // Includes exact bytes, global order and multiplicity.
        ASSERT_EQ(transport.refused_frames.size(), 5u);
        const auto& first_refused =
            type == net::MessageType::Input ? expected.front() : expected[4];
        for (std::size_t index = 0; index < 3; ++index)
            EXPECT_EQ(transport.refused_frames[index], first_refused);
        EXPECT_EQ(transport.refused_frames[3], expected.back());
        EXPECT_EQ(transport.refused_frames[4], expected.back());
    }
}

TEST(LockstepSessionTest, PendingInputRetriesWhileWaitingForRemoteInput) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    transport.start_refusing_at = net::MessageType::Input;
    transport.refusals_remaining = 3;
    auto config = MakeConfig(0, 1);
    config.horizon_max = config.horizon_start;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, &transport, MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    for (int pump = 0; pump < 3; ++pump) {
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
        EXPECT_FALSE(session.Status().peer_disconnected);
    }
    std::vector<std::uint8_t> frame;
    ASSERT_TRUE(peer->TryReceiveFrame(frame)); // Hello.
    for (std::uint64_t tick = 1; tick <= config.horizon_start; ++tick) {
        ASSERT_TRUE(peer->TryReceiveFrame(frame));
        EXPECT_EQ(frame, net::EncodeInput({tick, 0, {}}));
    }
    EXPECT_FALSE(peer->TryReceiveFrame(frame));
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(world.tick, 1u);
}

TEST(LockstepSessionTest, PendingRetryRefusalWithDisconnectedPeerDisconnects) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    transport.start_refusing_at = net::MessageType::Input;
    transport.refusals_remaining = 2;
    const auto config = MakeConfig(0, 1);
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, &transport, MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    transport.peer_disconnected = true;
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_TRUE(transport.closed);
    EXPECT_TRUE(session.Status().peer_disconnected);
    EXPECT_EQ(world.tick, 0u);
}

TEST(LockstepSessionTest, PendingOutboundFrameLimitDisconnectsConnectedPeer) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    transport.start_refusing_at = net::MessageType::Input;
    transport.refusals_remaining = 1024;
    auto config = MakeConfig(0, 1);
    config.horizon_min = config.horizon_max = config.horizon_start;
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, &transport, MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    // Three initial inputs plus two frames per tick fill 1024 entries at tick 511;
    // that tick's hash would be entry 1025. All encoded frames are far below 64 MiB.
    for (std::uint64_t tick = 1; tick <= 511; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        ASSERT_TRUE(transport.IsPeerConnected());
        const auto result = session.PumpTick(1000);
        EXPECT_EQ(result.outcome,
                  tick < 511 ? net::TickOutcome::Advanced : net::TickOutcome::PeerDisconnected);
    }
    EXPECT_TRUE(transport.closed);
    EXPECT_TRUE(session.Status().peer_disconnected);
    EXPECT_FALSE(session.Status().desynced);
    EXPECT_EQ(world.tick, 511u);
    EXPECT_EQ(session.PumpTick(1000).outcome, net::TickOutcome::PeerDisconnected);
}

TEST(LockstepSessionTest, TransportCloseDrainsBufferedInputsThenDisconnectsPermanently) {
    auto [local, peer] = net::MakeLoopbackPair();
    const auto config = MakeConfig(0, 1);
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({2, 1, {}})));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    peer->Close();
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(world.tick, 2u);
    for (int pump = 0; pump < 3; ++pump) {
        const auto result = session.PumpTick(10);
        EXPECT_EQ(result.outcome, net::TickOutcome::PeerDisconnected);
        EXPECT_EQ(result.tick, 2u);
    }
    EXPECT_TRUE(session.Status().peer_disconnected);
    EXPECT_EQ(session.Status().disconnect_tick, 2u);
    EXPECT_EQ(session.Status().late_input_events, 0u);
    EXPECT_FALSE(session.Status().desynced);
    EXPECT_EQ(world.tick, 2u);
}

TEST(LockstepSessionTest, HandshakeSendFailuresDisconnect) {
    for (const auto type : {net::MessageType::Hello, net::MessageType::Input}) {
        auto [local, peer] = net::MakeLoopbackPair();
        RejectingTransport transport(*local);
        transport.rejected = type;
        const auto config = MakeConfig(0, 1);
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        EXPECT_FALSE(session.Handshake());
        EXPECT_TRUE(transport.closed);
        EXPECT_FALSE(session.Status().handshaken);
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
        EXPECT_EQ(world.tick, 0u);
    }
}

TEST(LockstepSessionTest, InputAndHashSendFailuresDisconnectPermanently) {
    // A permanent refusal reports closure only after the receive drain.
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        auto [local, peer] = net::MakeLoopbackPair();
        RejectingTransport transport(*local);
        transport.drain_before_disconnect = true;
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
        transport.rejected = type;
        for (int pump = 0; pump < 3; ++pump) {
            const auto result = session.PumpTick(10);
            EXPECT_EQ(result.outcome, net::TickOutcome::PeerDisconnected);
            EXPECT_EQ(result.tick, 1u);
        }
        EXPECT_TRUE(transport.closed);
        EXPECT_TRUE(session.Status().peer_disconnected);
        EXPECT_FALSE(session.Status().desynced);
        EXPECT_EQ(world.tick, 1u);
    }
}

TEST(LockstepSessionTest, BufferedDesyncTakesPrecedenceOverInputAndHashSendFailures) {
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto [local, peer] = net::MakeLoopbackPair();
        RejectingTransport transport(*local);
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        const auto dump =
            LockstepTempDir() / (type == net::MessageType::Input ? "desync-input-send.lrec1"
                                                                 : "desync-hash-send.lrec1");
        std::error_code ec;
        fs::remove(dump, ec);
        session.SetDumpPath(dump.string());
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {0xAB}})));
        FakeWorld mismatching{0, "different", 0};
        net::HashMsg peer_hash;
        CaptureHashes(1, peer_hash, &mismatching);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(peer_hash)));
        transport.rejected = type;

        const auto result = session.PumpTick(10);
        EXPECT_EQ(result.outcome, net::TickOutcome::Desync);
        EXPECT_EQ(result.tick, 1u);
        EXPECT_EQ(result.local_inputs, (std::vector<std::uint8_t>{0xAB}));
        EXPECT_EQ(world.tick, 1u);
        EXPECT_TRUE(transport.closed);
        EXPECT_TRUE(session.Status().peer_disconnected);
        EXPECT_TRUE(session.Status().desynced);
        EXPECT_EQ(session.Status().desync_tick, 1u);
        EXPECT_EQ(session.Status().desync_section, "terrain");
        EXPECT_EQ(session.Status().dump_path, dump.string());
        const auto contents = replay::ReadReplay(dump.string());
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value())
            return;
        EXPECT_FALSE(contents->truncated);
        ASSERT_EQ(contents->checkpoints.size(), 1u);
        EXPECT_EQ(contents->checkpoints.front().tick, 1u);
        EXPECT_EQ(contents->checkpoints.front().terrain, world.HashAt(1, "terrain"));
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
        fs::remove(dump, ec);
    }
}

TEST(LockstepSessionTest, QueuedDesyncTakesPrecedenceOverLocalDisconnect) {
    for (const bool reject_bye : {false, true}) {
        SCOPED_TRACE(reject_bye);
        auto [local, peer] = net::MakeLoopbackPair();
        RejectingTransport transport(*local);
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        const auto dump = LockstepTempDir() /
                          (reject_bye ? "desync-rejected-bye.lrec1" : "desync-sent-bye.lrec1");
        std::error_code ec;
        fs::remove(dump, ec);
        session.SetDumpPath(dump.string());
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {0xAB}})));
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        ASSERT_EQ(world.tick, 1u);

        FakeWorld mismatching{0, "different", 0};
        net::HashMsg peer_hash;
        CaptureHashes(1, peer_hash, &mismatching);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(peer_hash)));
        if (reject_bye)
            transport.rejected = net::MessageType::Bye;
        session.Disconnect();
        unsigned bye_count = 0;
        std::vector<std::uint8_t> sent;
        while (peer->TryReceiveFrame(sent)) {
            net::MessageType type;
            if (net::PeekMessageType(sent, type) && type == net::MessageType::Bye)
                ++bye_count;
        }
        EXPECT_EQ(bye_count, reject_bye ? 0u : 1u);

        const auto result = session.PumpTick(10);
        EXPECT_EQ(result.outcome, net::TickOutcome::Desync);
        EXPECT_EQ(result.tick, 1u);
        EXPECT_TRUE(transport.closed);
        EXPECT_TRUE(session.Status().peer_disconnected);
        EXPECT_TRUE(session.Status().desynced);
        EXPECT_EQ(session.Status().desync_section, "terrain");
        EXPECT_EQ(session.Status().dump_path, dump.string());
        const auto contents = replay::ReadReplay(dump.string());
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value())
            return;
        EXPECT_FALSE(contents->truncated);
        ASSERT_EQ(contents->inputs.size(), 1u);
        EXPECT_EQ(contents->inputs.front().tick, 1u);
        EXPECT_EQ(contents->inputs.front().inputs, (std::vector<std::uint8_t>{0xAB}));
        ASSERT_EQ(contents->checkpoints.size(), 1u);
        EXPECT_EQ(contents->checkpoints.front().tick, 1u);
        EXPECT_EQ(contents->checkpoints.front().terrain, world.HashAt(1, "terrain"));
        session.Disconnect();
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
        EXPECT_EQ(world.tick, 1u);
        fs::remove(dump, ec);
    }
}

TEST(LockstepSessionTest, QueuedDesyncTakesPrecedenceOverPeerByeAndTransportClose) {
    for (const bool send_bye : {false, true}) {
        SCOPED_TRACE(send_bye);
        auto [local, peer] = net::MakeLoopbackPair();
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, local.get(), MakeHooks(&world));
        const auto dump =
            LockstepTempDir() / (send_bye ? "desync-peer-bye.lrec1" : "desync-peer-close.lrec1");
        std::error_code ec;
        fs::remove(dump, ec);
        session.SetDumpPath(dump.string());
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        for (std::uint64_t tick = 1; tick <= 2; ++tick) {
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        }
        if (send_bye) {
            ASSERT_TRUE(peer->SendFrame(net::EncodeBye({2})));
        }
        net::HashMsg matching;
        CaptureHashes(1, matching, &world);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(matching)));
        FakeWorld mismatching{0, "different", 0};
        net::HashMsg mismatch;
        CaptureHashes(2, mismatch, &mismatching);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(mismatch)));
        peer->Close();

        const auto result = session.PumpTick(10);
        EXPECT_EQ(result.outcome, net::TickOutcome::Desync);
        EXPECT_EQ(result.tick, 2u);
        EXPECT_EQ(world.tick, 2u);
        EXPECT_EQ(session.Status().dump_path, dump.string());
        const auto contents = replay::ReadReplay(dump.string());
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value())
            return;
        EXPECT_FALSE(contents->truncated);
        ASSERT_EQ(contents->checkpoints.size(), 2u);
        EXPECT_EQ(contents->checkpoints.back().tick, 2u);
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
        fs::remove(dump, ec);
    }
}

TEST(LockstepSessionTest, DesyncArrivingDuringSendFailureTakesPrecedence) {
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        for (const std::uint64_t mismatch_tick : {1u, 2u}) {
            SCOPED_TRACE(static_cast<int>(type));
            SCOPED_TRACE(mismatch_tick);
            auto [local, peer] = net::MakeLoopbackPair();
            RejectingTransport transport(*local);
            auto config = MakeConfig(0, 1);
            config.hash_cadence_ticks = 1;
            FakeWorld world{0, "agree", 0};
            net::LockstepSession session(config, &transport, MakeHooks(&world));
            const auto dump = LockstepTempDir() /
                              ((type == net::MessageType::Input ? "desync-during-input-send-"
                                                                : "desync-during-hash-send-") +
                               std::to_string(mismatch_tick) + ".lrec1");
            std::error_code ec;
            fs::remove(dump, ec);
            session.SetDumpPath(dump.string());
            ASSERT_TRUE(SendPeerHello(*peer, config));
            ASSERT_TRUE(session.Handshake());
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
            FakeWorld mismatching{0, "different", 0};
            net::HashMsg mismatch;
            CaptureHashes(mismatch_tick, mismatch, &mismatching);
            net::HashMsg matching;
            CaptureHashes(mismatch_tick, matching, &world);
            transport.rejected = type;
            transport.on_rejection = [&] {
                EXPECT_TRUE(peer->SendFrame(net::EncodeHash(mismatch)));
                // A later matching hash must not overwrite an available mismatch.
                EXPECT_TRUE(peer->SendFrame(net::EncodeHash(matching)));
                // TCP can report send-side disconnect with complete receive frames
                // still buffered. They must be compared before destructive Close.
                transport.peer_disconnected = true;
            };
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({2, 1, {}})));

            const auto result = session.PumpTick(10);
            EXPECT_EQ(result.outcome, net::TickOutcome::Desync);
            EXPECT_EQ(result.tick, mismatch_tick);
            EXPECT_TRUE(transport.closed);
            EXPECT_TRUE(session.Status().peer_disconnected);
            EXPECT_EQ(session.Status().dump_path, dump.string());
            const auto contents = replay::ReadReplay(dump.string());
            ASSERT_TRUE(contents.has_value());
            if (!contents.has_value())
                return;
            EXPECT_FALSE(contents->truncated);
            ASSERT_EQ(contents->checkpoints.size(), mismatch_tick);
            EXPECT_EQ(contents->checkpoints.back().tick, mismatch_tick);
            EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
            EXPECT_EQ(world.tick, 2u);
            fs::remove(dump, ec);
        }
    }
}

TEST(LockstepSessionTest, LoopbackAcceptsMaximumAndRejectsMaximumPlusOne) {
    auto [local, peer] = net::MakeLoopbackPair();
    for (const auto delivery : {net::FrameDelivery::Reliable, net::FrameDelivery::Unreliable}) {
        std::vector<std::uint8_t> frame(net::kMaxFramePayloadBytes + 1, 0xAB);
        EXPECT_FALSE(local->SendFrame(frame, delivery));
        std::vector<std::uint8_t> received{0xA5};
        EXPECT_FALSE(peer->TryReceiveFrame(received));
        EXPECT_EQ(received, (std::vector<std::uint8_t>{0xA5}));
        EXPECT_TRUE(local->IsPeerConnected());
        frame.resize(net::kMaxFramePayloadBytes);
        ASSERT_TRUE(local->SendFrame(frame, delivery));
        ASSERT_TRUE(peer->TryReceiveFrame(frame));
        ASSERT_EQ(frame.size(), net::kMaxFramePayloadBytes);
        EXPECT_TRUE(std::all_of(
            frame.begin(), frame.end(), [](std::uint8_t byte) { return byte == 0xAB; }));
        EXPECT_FALSE(peer->TryReceiveFrame(frame));
    }
}

TEST(LockstepSessionTest, WaitingInputSendFailureDisconnects) {
    auto [local, peer] = net::MakeLoopbackPair();
    RejectingTransport transport(*local);
    const auto config = MakeConfig(0, 1);
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, &transport, MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    transport.rejected = net::MessageType::Input;
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_TRUE(transport.closed);
    EXPECT_EQ(world.tick, 0u);
}

TEST(LockstepSessionTest, ByeSendFailureStillDisconnects) {
    auto [local, peer] = net::MakeLoopbackPair();
    RejectingTransport transport(*local);
    const auto config = MakeConfig(0, 1);
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, &transport, MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    transport.rejected = net::MessageType::Bye;
    session.Disconnect();
    session.Disconnect();
    EXPECT_TRUE(transport.closed);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
}

TEST(LockstepSessionTest, LargeHandshakeInputsStayConnectedAndArriveInOrder) {
    const auto exercise = [](net::ILockstepTransport& ta,
                             net::ILockstepTransport& tb,
                             net::NetworkSim* sim) {
        const auto advance_delay = [sim] {
            if (sim) {
                for (int tick = 0; tick < 3; ++tick)
                    sim->Tick();
            }
        };
        FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
        auto ha = MakeHooks(&wa);
        auto hb = MakeHooks(&wb);
        ha.collect_local_input = hb.collect_local_input = [](std::uint64_t tick, void*) {
            return std::vector<std::uint8_t>(128u * 1024u, static_cast<std::uint8_t>(tick));
        };
        const auto ca = MakeConfig(0, 1);
        const auto cb = MakeConfig(1, 0);
        net::LockstepSession a(ca, &ta, ha);
        net::LockstepSession b(cb, &tb, hb);

        // Prime the peer Hello. Each handshake queues all three 128 KiB inputs
        // within the default horizon before the receiving session drains any.
        ASSERT_TRUE(SendPeerHello(tb, ca));
        advance_delay();
        ASSERT_TRUE(a.Handshake());
        advance_delay();
        ASSERT_TRUE(b.Handshake());
        advance_delay();
        ASSERT_EQ(a.Status().horizon, 3u);
        ASSERT_EQ(b.Status().horizon, 3u);
        for (std::uint64_t tick = 1; tick <= 3; ++tick) {
            const std::vector<std::uint8_t> expected(256u * 1024u, static_cast<std::uint8_t>(tick));
            const auto ra = a.PumpTick(10);
            const auto rb = b.PumpTick(10);
            ASSERT_EQ(ra.outcome, net::TickOutcome::Advanced);
            ASSERT_EQ(rb.outcome, net::TickOutcome::Advanced);
            EXPECT_EQ(ra.tick, tick);
            EXPECT_EQ(rb.tick, tick);
            EXPECT_EQ(ra.local_inputs, expected);
            EXPECT_EQ(rb.local_inputs, expected);
            EXPECT_FALSE(a.Status().peer_disconnected);
            EXPECT_FALSE(b.Status().peer_disconnected);
            EXPECT_TRUE(ta.IsPeerConnected());
            EXPECT_TRUE(tb.IsPeerConnected());
        }
        EXPECT_EQ(wa.tick, 3u);
        EXPECT_EQ(wb.tick, 3u);
    };
    auto [ta, tb] = net::MakeLoopbackPair();
    exercise(*ta, *tb, nullptr);
    net::NetworkSim sim(net::NetworkConditions{3, 0, 0, 1});
    exercise(sim.A(), sim.B(), &sim);
}

// Both peers handshake successfully on matching params.
TEST(LockstepSessionTest, HandshakeAccepts) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    // Both must send Hello before either can complete (single-process driver order):
    // a's Handshake sends a's Hello but cannot yet see b's, so prime b's send first.
    // Drive: send each Hello, then complete each handshake.
    // Easiest: have each send its Hello by attempting Handshake; since the peer Hello is
    // not yet queued, we send b's Hello first via a manual encode, then handshake a, then b.
    // Simpler and faithful: call b.Handshake AFTER a has sent -- but Handshake is atomic.
    // Use the documented pattern: send both Hellos, then handshake. We achieve that by
    // calling each Handshake twice if needed; instead we send Hellos explicitly:
    // (Handshake sends its own Hello, so calling a then b leaves a's Hello waiting for b.)
    // a.Handshake will fail to find b's Hello (not sent yet) -> so we send b's Hello first.
    EXPECT_TRUE(tb->SendFrame(net::EncodeHello([&] {
        net::HelloMsg m;
        m.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
        m.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
        m.seed = 424242;
        m.preset = "default";
        m.tick_rate_hz = 30;
        m.client_id = 1;
        return m;
    }())));
    EXPECT_TRUE(a.Handshake());
    // Now a has sent its Hello (into a->b queue); b can handshake against it.
    EXPECT_TRUE(b.Handshake());
    EXPECT_TRUE(a.Status().handshaken);
    EXPECT_TRUE(b.Status().handshaken);
}

TEST(LockstepSessionTest, EffectiveConfigIdentityAcceptsEqualAndRejectsDifferentRules) {
    const auto baseline = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    auto variants = Luminumbra::TestSupport::EffectiveIdentityMismatches();
    variants.emplace_back("identical", baseline);
    for (const auto& [name, identity] : variants) {
        SCOPED_TRACE(name);
        auto [host_transport, peer_transport] = net::MakeLoopbackPair();
        FakeWorld world{0, "same-initial-state", 0};
        auto local = MakeConfig(0, 1);
        local.system_config_identity = baseline;
        local.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
        net::LockstepSession host(local, host_transport.get(), MakeHooks(&world));
        net::HelloMsg hello;
        hello.seed = local.seed;
        hello.preset = local.preset;
        hello.client_id = 1;
        hello.system_config_identity = identity;
        hello.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
        ASSERT_TRUE(peer_transport->SendFrame(net::EncodeHello(hello)));
        const bool equal = name == "identical";
        EXPECT_EQ(hello.system_config_identity == baseline, equal);
        auto expected_hello = hello;
        expected_hello.system_config_identity = baseline;
        expected_hello.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
        EXPECT_EQ(net::HelloMatches(expected_hello, hello), equal);
        EXPECT_EQ(host.Handshake(), equal);
        EXPECT_EQ(host.Status().handshaken, equal);
        EXPECT_EQ(world.tick, 0u);
    }
}

TEST(LockstepSessionTest, SamePresetNameBindsResolvedOverrideContent) {
    const auto preset = Luminumbra::TestSupport::PresetIdentityFixture();
    auto different = preset;
    different["generation_params"]["terrain"]["base_amplitude"] = 80.0;
    for (const auto& peer_hash : {Luminumbra::world::TerrainPresetContentHash(preset),
                                  Luminumbra::world::TerrainPresetContentHash(different),
                                  std::string{}}) {
        auto [host_transport, peer_transport] = net::MakeLoopbackPair();
        FakeWorld world{0, "same-initial-state", 0};
        auto local = MakeConfig(0, 1);
        net::HelloMsg hello;
        hello.seed = local.seed;
        hello.preset = local.preset;
        hello.client_id = 1;
        hello.system_config_identity = local.system_config_identity;
        hello.preset_content_hash = peer_hash;
        net::HelloMsg decoded;
        ASSERT_TRUE(net::DecodeHello(net::EncodeHello(hello), decoded));
        EXPECT_EQ(decoded.preset_content_hash, peer_hash);
        auto expected = hello;
        expected.preset_content_hash = local.preset_content_hash;
        const bool matches = peer_hash == local.preset_content_hash;
        EXPECT_EQ(net::HelloMatches(expected, decoded), matches);
        ASSERT_TRUE(peer_transport->SendFrame(net::EncodeHello(hello)));
        net::LockstepSession host(local, host_transport.get(), MakeHooks(&world));
        EXPECT_EQ(host.Handshake(), matches);
        EXPECT_EQ(world.tick, 0u);
    }
}

TEST(LockstepSessionTest, LegacyHelloWithoutPresetIdentityIsRefused) {
    net::HelloMsg hello;
    hello.protocol_version = 2;
    hello.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    auto frame = net::EncodeHello(hello);
    // Version 2 ended after system_config_identity, without a preset hash field.
    frame.resize(frame.size() - 4);
    const auto size = static_cast<std::uint32_t>(frame.size() - 5);
    for (unsigned i = 0; i < 4; ++i)
        frame[1u + i] = static_cast<std::uint8_t>((size >> (8u * i)) & 0xffu);
    net::HelloMsg decoded;
    EXPECT_FALSE(net::DecodeHello(frame, decoded));
}

TEST(LockstepSessionTest, HandshakeRefusesMissingIdentityOnEitherSide) {
    for (const bool missing_local : {false, true}) {
        for (const bool missing_peer : {false, true}) {
            auto [host_transport, peer_transport] = net::MakeLoopbackPair();
            FakeWorld world{0, "same-state", 0};
            auto local = MakeConfig(0, 1);
            net::HelloMsg peer;
            peer.seed = local.seed;
            peer.preset = local.preset;
            peer.client_id = 1;
            peer.system_config_identity = local.system_config_identity;
            peer.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
            if (missing_local)
                local.system_config_identity.clear();
            if (missing_peer)
                peer.system_config_identity.clear();
            net::LockstepSession host(local, host_transport.get(), MakeHooks(&world));
            ASSERT_TRUE(peer_transport->SendFrame(net::EncodeHello(peer)));
            EXPECT_EQ(host.Handshake(), !missing_local && !missing_peer);
            EXPECT_EQ(world.tick, 0u);
        }
    }
}

TEST(LockstepSessionTest, HandshakeRefusesHelloWithoutIdentityField) {
    auto [host_transport, peer_transport] = net::MakeLoopbackPair();
    FakeWorld world{0, "same-state", 0};
    const auto local = MakeConfig(0, 1);
    net::LockstepSession host(local, host_transport.get(), MakeHooks(&world));
    net::HelloMsg peer;
    peer.seed = local.seed;
    peer.preset = local.preset;
    peer.client_id = 1;
    // Remove the trailing empty-string length to reproduce an absent wire field.
    auto frame = net::EncodeHello(peer);
    ASSERT_GE(frame.size(), 9u);
    frame.resize(frame.size() - 4);
    const auto payload_size = static_cast<std::uint32_t>(frame.size() - 5);
    for (unsigned i = 0; i < 4; ++i)
        frame[1u + i] = static_cast<std::uint8_t>((payload_size >> (8u * i)) & 0xffu);
    ASSERT_TRUE(peer_transport->SendFrame(frame));
    EXPECT_FALSE(host.Handshake());
    EXPECT_EQ(world.tick, 0u);
}

// A seed mismatch is rejected loudly (a mis-paired session must not silently desync).
TEST(LockstepSessionTest, HandshakeRejectsSeedMismatch) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));

    // Queue a peer Hello with a DIFFERENT seed.
    net::HelloMsg bad;
    bad.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    bad.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
    bad.seed = 999;
    bad.preset = "default";
    bad.tick_rate_hz = 30;
    bad.client_id = 1;
    EXPECT_TRUE(tb->SendFrame(net::EncodeHello(bad)));
    EXPECT_FALSE(a.Handshake());
    EXPECT_FALSE(a.Status().handshaken);
}

// A preset mismatch is likewise rejected.
TEST(LockstepSessionTest, HandshakeRejectsPresetMismatch) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::HelloMsg bad;
    bad.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    bad.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
    bad.seed = 424242;
    bad.preset = "mountains";
    bad.tick_rate_hz = 30;
    bad.client_id = 1;
    EXPECT_TRUE(tb->SendFrame(net::EncodeHello(bad)));
    EXPECT_FALSE(a.Handshake());
}

// Drives two in-sync sessions to N ticks over the loopback pair, alternating pumps so the
// queues carry frames. Returns the final outcomes.
namespace {
struct DriveResult {
    net::TickOutcome a_outcome;
    net::TickOutcome b_outcome;
    std::uint64_t a_tick;
    std::uint64_t b_tick;
};

DriveResult DriveBoth(net::LockstepSession& a,
                      net::LockstepSession& b,
                      std::uint64_t budget,
                      int max_pumps = 100000) {
    net::TickResult ra, rb;
    ra.outcome = net::TickOutcome::WaitingForPeer;
    rb.outcome = net::TickOutcome::WaitingForPeer;
    int pumps = 0;
    auto finished = [](net::TickOutcome o) {
        return o == net::TickOutcome::Finished;
    };
    auto fatal = [](net::TickOutcome o) {
        return o == net::TickOutcome::Desync || o == net::TickOutcome::PeerDisconnected;
    };
    while (pumps++ < max_pumps) {
        ra = a.PumpTick(budget);
        rb = b.PumpTick(budget);
        // A desync/disconnect on EITHER side is terminal for the whole session (the other
        // side will stall waiting for inputs that never come -- stop driving immediately).
        if (fatal(ra.outcome) || fatal(rb.outcome))
            break;
        if (finished(ra.outcome) && finished(rb.outcome))
            break;
    }
    return {ra.outcome, rb.outcome, a.AgreedTick(), b.AgreedTick()};
}
} // namespace

// Two in-sync peers run 90 ticks, exchange hashes every 30, and BOTH finish at the same
// agreed tick with no desync.
TEST(LockstepSessionTest, InSyncTickExchange) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    // Handshake (send b's Hello, a handshakes, then b handshakes against a's Hello).
    net::HelloMsg bh;
    bh.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    bh.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());

    const auto r = DriveBoth(a, b, 90);
    EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.b_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.a_tick, 90u);
    EXPECT_EQ(r.b_tick, 90u);
    EXPECT_FALSE(a.Status().desynced);
    EXPECT_FALSE(b.Status().desynced);
}

// The adaptive horizon ABSORBS a delayed peer input: peer A withholds its inputs for a
// burst of ticks, then releases them. The session must stay in sync (no desync), the
// horizon must have grown, and both must still finish at the same tick.
TEST(LockstepSessionTest, HorizonAbsorbsLateInput) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    net::HelloMsg bh;
    bh.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    bh.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());

    // Stall b: pump ONLY a for several rounds. a will WaitForPeer (b's inputs absent) and
    // grow its horizon. Then resume both -- the buffered-ahead inputs absorb the jitter.
    for (int i = 0; i < 15; ++i) {
        a.PumpTick(90); // b's input for the wanted tick is missing -> a grows horizon
    }
    EXPECT_GT(a.Status().horizon, 3u) << "horizon did not grow on a late peer";
    EXPECT_GT(a.Status().late_input_events, 0u);

    const auto r = DriveBoth(a, b, 90);
    EXPECT_EQ(r.a_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.b_outcome, net::TickOutcome::Finished);
    EXPECT_EQ(r.a_tick, 90u);
    EXPECT_EQ(r.b_tick, 90u);
    EXPECT_FALSE(a.Status().desynced) << "horizon failed to absorb the jitter (false desync)";
    EXPECT_FALSE(b.Status().desynced);
    EXPECT_GE(a.Status().max_horizon_reached, 4u);
}

// A real state divergence is CAUGHT by the oracle: peer B's world corrupts from tick 30,
// so at the first cadence (tick 30) the hashes mismatch. The session must HALT with a
// desync localized to tick 30 (terrain section) and emit an LREC1 dump that re-reads.
TEST(LockstepSessionTest, DesyncDetectedAndDumpEmitted) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0};
    FakeWorld wb{0, "agree", 30}; // b diverges from tick 30
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    const std::string dumpA = (LockstepTempDir() / "desync-a.lrec1").string();
    const std::string dumpB = (LockstepTempDir() / "desync-b.lrec1").string();
    std::error_code ec;
    fs::remove(dumpA, ec);
    fs::remove(dumpB, ec);
    a.SetDumpPath(dumpA);
    b.SetDumpPath(dumpB);

    net::HelloMsg bh;
    bh.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    bh.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());

    DriveBoth(a, b, 90);
    // At least one side must report the desync (the one that holds both hashes at tick 30).
    const bool a_desync = a.Status().desynced;
    const bool b_desync = b.Status().desynced;
    EXPECT_TRUE(a_desync || b_desync)
        << "oracle is vacuous: a real divergence was not caught at ticks " << r.a_tick << ", "
        << r.b_tick;

    // The catching side localizes to tick 30, section terrain, and wrote a dump.
    if (a_desync) {
        EXPECT_EQ(a.Status().desync_tick, 30u);
        EXPECT_EQ(a.Status().desync_section, "terrain");
        ASSERT_FALSE(a.Status().dump_path.empty());
        const auto contents = replay::ReadReplay(a.Status().dump_path);
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value())
            return;
        EXPECT_FALSE(contents->truncated);
        EXPECT_FALSE(contents->checkpoints.empty());
        EXPECT_EQ(contents->header.preset_content_hash, MakeConfig(0, 1).preset_content_hash);
    }
    if (b_desync) {
        EXPECT_EQ(b.Status().desync_tick, 30u);
        EXPECT_EQ(b.Status().desync_section, "terrain");
        ASSERT_FALSE(b.Status().dump_path.empty());
        const auto contents = replay::ReadReplay(b.Status().dump_path);
        ASSERT_TRUE(contents.has_value());
        if (!contents.has_value())
            return;
        EXPECT_FALSE(contents->truncated);
        EXPECT_EQ(contents->header.preset_content_hash, MakeConfig(1, 0).preset_content_hash);
    }

    std::error_code rmec;
    fs::remove(dumpA, rmec);
    fs::remove(dumpB, rmec);
}

// A clean Bye from one peer ENDS the session cleanly on the other (regression review): the
// surviving side reports PeerDisconnected with the disconnect tick -- NOT a desync, NOT a
// hang.
TEST(LockstepSessionTest, CleanDisconnect) {
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(MakeConfig(0, 1), ta.get(), MakeHooks(&wa));
    net::LockstepSession b(MakeConfig(1, 0), tb.get(), MakeHooks(&wb));

    net::HelloMsg bh;
    bh.system_config_identity = Luminumbra::world::GameSession{}.SystemConfigIdentity();
    bh.preset_content_hash = Luminumbra::TestSupport::PresetIdentityFixtureHash();
    bh.seed = 424242;
    bh.preset = "default";
    bh.tick_rate_hz = 30;
    bh.client_id = 1;
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(bh)));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());

    // Run a handful of ticks in sync, then b disconnects cleanly.
    for (int i = 0; i < 50; ++i) {
        a.PumpTick(90);
        b.PumpTick(90);
        if (a.AgreedTick() >= 10)
            break;
    }
    b.Disconnect();
    EXPECT_EQ(b.PumpTick(90).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(b.PumpTick(90).outcome, net::TickOutcome::PeerDisconnected);

    // a keeps pumping: it must observe the clean disconnect and stop (no hang).
    net::TickResult ra;
    for (int i = 0; i < 1000; ++i) {
        ra = a.PumpTick(90);
        if (ra.outcome == net::TickOutcome::PeerDisconnected)
            break;
    }
    EXPECT_EQ(ra.outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_TRUE(a.Status().peer_disconnected);
    EXPECT_FALSE(a.Status().desynced) << "a clean disconnect must NOT be reported as a desync";
}
