// LockstepSession unit tests (src/luminumbra_common/net/LockstepSession.{h,cpp}).
//
// These lock the ENGINE-GENERIC delay-based lockstep behavior over in-process transports
// (no real sockets/ports -- hermetic, no firewall/CI flakiness): handshake
// accept/reject, in-sync tick exchange, adaptive-horizon absorption of a late input,
// desync detection + LREC1 dump emission, and clean disconnect. Both session ends run in
// this one process, driven turn-by-turn so the loopback queues carry their frames.

#include "../support/SystemConfigVariants.h"
#include "../support/UniqueTempPath.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/NetworkSim.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"

namespace net = Luminumbra::Net;
namespace replay = Luminumbra::Replay;
namespace fs = std::filesystem;

namespace {

fs::path LockstepTempDir() {
    // CTest runs cases in separate processes. Reserve this process's namespace
    // exclusively and retain it until all local sessions/recorders have closed.
    static const Luminumbra::TestSupport::ScopedTempFile workspace("lockstep_", ".unused");
    return workspace.Path().parent_path();
}

// A deterministic per-peer fake "world": the hash is a function of (tick, a per-peer
// salt). With identical salts the two peers agree at every cadence (in sync); a differing
// salt at/after some tick forces a desync the oracle must catch.
struct FakeWorld {
    std::uint64_t tick = 0;
    std::string salt;               // identical across peers => agree
    std::uint64_t corrupt_from = 0; // ticks >= this use a corrupted salt (0 = never)
};

std::string HashAt(const FakeWorld& world, std::uint64_t tick, const char* section) {
    const std::string salt =
        (world.corrupt_from != 0 && tick >= world.corrupt_from) ? (world.salt + "X") : world.salt;
    return replay::Fnv1a64Hex(salt + "|" + section + "|" + std::to_string(tick));
}

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
    out.terrain = HashAt(*w, tick, "terrain");
    out.water = HashAt(*w, tick, "water");
    out.entities = HashAt(*w, tick, "entities");
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
    auto remote = config;
    remote.local_client_id = config.peer_client_id;
    remote.peer_client_id = config.local_client_id;
    return peer.SendFrame(net::EncodeHello(net::MakeLockstepHello(remote)));
}

// Reject a selected message and report a disconnected peer. Buffered receive frames
// remain readable until Close so the session can compare hashes before disconnecting.
struct RejectingTransportState {
    std::optional<net::MessageType> rejected;
    std::function<void()> on_rejection;
    bool closed = false;
    bool peer_disconnected = false;
    bool drain_before_disconnect = false;
    bool drained = false;
};

class RejectingTransport final : public net::ILockstepTransport {
public:
    explicit RejectingTransport(net::ILockstepTransport& transport)
        : m_inner(transport) {}

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   net::FrameDelivery delivery = net::FrameDelivery::Reliable) override {
        net::MessageType type;
        if (m_state.rejected && net::PeekMessageType(frame, type) && type == *m_state.rejected) {
            m_state.peer_disconnected = true;
            if (m_state.on_rejection)
                m_state.on_rejection();
            return false;
        }
        return m_inner.SendFrame(frame, delivery);
    }
    std::size_t MaxFramePayloadBytes() const override {
        return m_inner.MaxFramePayloadBytes();
    }
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override {
        if (!m_state.closed && m_inner.TryReceiveFrame(out))
            return true;
        if (m_state.peer_disconnected)
            m_state.drained = true;
        return false;
    }
    bool IsPeerConnected() const override {
        return !m_state.closed &&
               (!m_state.peer_disconnected ||
                (m_state.drain_before_disconnect && !m_state.drained)) &&
               m_inner.IsPeerConnected();
    }
    void Close() override {
        m_state.closed = true;
        m_inner.Close();
    }

    RejectingTransportState& State() {
        return m_state;
    }

private:
    net::ILockstepTransport& m_inner;
    RejectingTransportState m_state{};
};

// Once armed by a message type, refuse the next N sends without accepting any bytes.
struct BackpressuredTransportState {
    std::optional<net::MessageType> start_refusing_at;
    std::size_t refusals_remaining = 0;
    std::size_t payload_limit = net::kMaxFramePayloadBytes;
    std::size_t bye_attempts = 0;
    std::vector<std::vector<std::uint8_t>> refused_frames;
    bool closed = false;
    bool peer_disconnected = false;
    bool record_refused_frames = true;
};

class BackpressuredTransport final : public net::ILockstepTransport {
public:
    explicit BackpressuredTransport(net::ILockstepTransport& transport)
        : m_inner(transport) {}

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   net::FrameDelivery delivery = net::FrameDelivery::Reliable) override {
        net::MessageType type;
        const bool typed = net::PeekMessageType(frame, type);
        if (typed && type == net::MessageType::Bye)
            ++m_state.bye_attempts;
        if (m_state.start_refusing_at && typed && type == *m_state.start_refusing_at)
            m_state.start_refusing_at.reset();
        if (!m_state.start_refusing_at && m_state.refusals_remaining > 0) {
            --m_state.refusals_remaining;
            if (m_state.record_refused_frames)
                m_state.refused_frames.push_back(frame);
            return false;
        }
        return m_inner.SendFrame(frame, delivery);
    }
    std::size_t MaxFramePayloadBytes() const override {
        return std::min(m_state.payload_limit, m_inner.MaxFramePayloadBytes());
    }
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override {
        return !m_state.closed && m_inner.TryReceiveFrame(out);
    }
    bool IsPeerConnected() const override {
        return !m_state.closed && !m_state.peer_disconnected && m_inner.IsPeerConnected();
    }
    void Close() override {
        m_state.closed = true;
        m_inner.Close();
    }

    BackpressuredTransportState& State() {
        return m_state;
    }

private:
    net::ILockstepTransport& m_inner;
    BackpressuredTransportState m_state{};
};

// Refills a single receive slot on demand, without growing queue storage. The
// finite fuse makes a missing work bound fail an assertion instead of hanging.
struct ReplenishingTransportState {
    std::vector<std::uint8_t> refill_frame;
    std::optional<net::MessageType> refuse;
    std::size_t refill_left = 0;
    std::size_t polls = 0;
    std::size_t payload_bytes = 0;
    bool refill_first = false;
    bool refill_on_refusal = false;
    bool peer_gone = false;
    bool closed = false;
};

class ReplenishingTransport final : public net::ILockstepTransport {
public:
    explicit ReplenishingTransport(net::ILockstepTransport& transport)
        : m_inner(transport) {}

    bool SendFrame(const std::vector<std::uint8_t>& frame,
                   net::FrameDelivery delivery = net::FrameDelivery::Reliable) override {
        net::MessageType type;
        if (m_state.refuse && net::PeekMessageType(frame, type) && type == *m_state.refuse) {
            if (m_state.refill_on_refusal)
                m_state.refill_left = kFuse;
            return false;
        }
        return m_inner.SendFrame(frame, delivery);
    }
    bool TryReceiveFrame(std::vector<std::uint8_t>& out) override {
        ++m_state.polls;
        if (m_state.closed || m_state.polls > kFuse)
            return false;
        if (!m_state.refill_first && m_inner.TryReceiveFrame(out)) {
            m_state.payload_bytes += out.size();
            return true;
        }
        if (m_state.refill_left != 0) {
            --m_state.refill_left;
            out = m_state.refill_frame;
            m_state.payload_bytes += out.size();
            return true;
        }
        if (m_state.refill_first && m_inner.TryReceiveFrame(out)) {
            m_state.payload_bytes += out.size();
            return true;
        }
        return false;
    }
    bool IsPeerConnected() const override {
        return !m_state.closed && !m_state.peer_gone && m_inner.IsPeerConnected();
    }
    void Close() override {
        m_state.closed = true;
        m_inner.Close();
    }

    static constexpr std::size_t kFuse = 4 * net::kReceiveQueueFrameLimit;
    ReplenishingTransportState& State() {
        return m_state;
    }

private:
    net::ILockstepTransport& m_inner;
    ReplenishingTransportState m_state{};
};

} // namespace

TEST(LockstepSessionTest, ReplenishedReceiveQueueYieldsAndStillAdvancesDeterministically) {
    const std::array<std::vector<std::uint8_t>, 5> fillers{
        std::vector<std::uint8_t>{},
        std::vector<std::uint8_t>{0xFE, 0, 0, 0, 0},
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::MessageType::Input), 0, 0, 0, 0},
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::MessageType::Hash), 0, 0, 0, 0},
        net::EncodeInput({1, 1, {0xAB}})};
    for (const auto& filler : fillers) {
        auto [local, peer] = net::MakeLoopbackPair();
        ReplenishingTransport transport(*local);
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 0;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        transport.State().refill_frame = filler;
        for (std::uint64_t tick = 1; tick <= 2; ++tick) {
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {0xAB}})));
            transport.State().polls = 0;
            transport.State().refill_left = ReplenishingTransport::kFuse;
            const auto result = session.PumpTick(10);
            EXPECT_EQ(result.outcome, net::TickOutcome::Advanced);
            EXPECT_EQ(result.local_inputs, (std::vector<std::uint8_t>{0xAB}));
            EXPECT_EQ(world.tick, tick);
            EXPECT_EQ(transport.State().polls, net::kReceiveQueueFrameLimit);
            EXPECT_FALSE(transport.State().closed);
        }
    }
}

TEST(LockstepSessionTest, ReceiveByteBudgetYieldsBeforeTheNextLargeFrame) {
    for (const auto bytes : {net::kReceiveQueueByteBudget - 1,
                             net::kReceiveQueueByteBudget,
                             net::kReceiveQueueByteBudget + 1}) {
        auto [local, peer] = net::MakeLoopbackPair();
        ReplenishingTransport transport(*local);
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 0;
        net::LockstepSession session(config, &transport, {});
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        // Unknown messages still consume work. One frame may cross the byte budget.
        transport.State().refill_frame.assign(bytes, 0xFE);
        transport.State().refill_left = ReplenishingTransport::kFuse;
        transport.State().polls = 0;
        EXPECT_EQ(session.PumpTick(0).outcome, net::TickOutcome::WaitingForPeer);
        EXPECT_EQ(transport.State().polls, bytes < net::kReceiveQueueByteBudget ? 2u : 1u);
        transport.State().refill_left = 0;
        transport.State().polls = 0;
        EXPECT_EQ(session.PumpTick(0).outcome, net::TickOutcome::Finished);
        EXPECT_EQ(transport.State().polls, 1u);
    }
}

TEST(LockstepSessionTest, HandshakeSchedulingRefusalsShareTheHelloReceiveBudget) {
    auto [local, peer] = net::MakeLoopbackPair();
    ReplenishingTransport transport(*local);
    const auto config = MakeConfig(0, 1);
    net::LockstepSession session(config, &transport, {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    transport.State().refuse = net::MessageType::Input;
    transport.State().refill_on_refusal = true;
    ASSERT_TRUE(session.Handshake());
    EXPECT_EQ(transport.State().polls, net::kReceiveQueueFrameLimit);
    EXPECT_FALSE(transport.State().closed);
}

TEST(LockstepSessionTest, ReceiveBudgetIsSharedWithInputHashAndRetryRefusalDrains) {
    for (const auto refusal : {net::MessageType::Input, net::MessageType::Hash}) {
        auto [local, peer] = net::MakeLoopbackPair();
        ReplenishingTransport transport(*local);
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        transport.State().refuse = refusal;
        transport.State().refill_on_refusal = true;
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
        transport.State().polls = 0;
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        EXPECT_EQ(transport.State().polls, net::kReceiveQueueFrameLimit);
        transport.State().polls = 0;
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
        EXPECT_EQ(transport.State().polls, net::kReceiveQueueFrameLimit);
        EXPECT_FALSE(transport.State().closed);
        transport.State().refill_left = 0;
        transport.State().refill_on_refusal = false;
        transport.State().refuse.reset();
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({2, 1, {}})));
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        EXPECT_EQ(world.tick, 2u);
    }
}

TEST(LockstepSessionTest, ClosureAcrossReceiveBatchesPreservesQueuedLateDesync) {
    // Exercise explicit Disconnect, Bye, transport EOF, and a send-side failure
    // that reports closure while complete received frames remain buffered.
    for (unsigned closure = 0; closure < 4; ++closure) {
        SCOPED_TRACE(closure);
        auto [local, peer] = net::MakeLoopbackPair();
        ReplenishingTransport transport(*local);
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        const auto dump =
            LockstepTempDir() / ("batched-close-" + std::to_string(closure) + ".lrec1");
        std::error_code ec;
        fs::remove(dump, ec);
        session.SetDumpPath(dump.string());
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {0xAB}})));
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        FakeWorld mismatching{0, "different", 0};
        net::HashMsg mismatch;
        CaptureHashes(1, mismatch, &mismatching);
        if (closure == 1) {
            ASSERT_TRUE(peer->SendFrame(net::EncodeBye({1})));
            for (std::size_t index = 0; index < net::kReceiveQueueFrameLimit; ++index)
                ASSERT_TRUE(peer->SendFrame({}));
        }
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(mismatch)));
        transport.State().refill_first = true;
        transport.State().refill_left = closure == 1 ? 0 : net::kReceiveQueueFrameLimit + 1;
        transport.State().polls = 0;
        if (closure == 0) {
            session.Disconnect();
        } else {
            if (closure == 2)
                peer->Close();
            if (closure == 3) {
                transport.State().peer_gone = true;
                transport.State().refuse = net::MessageType::Input;
            }
            EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
        }
        EXPECT_EQ(transport.State().polls, net::kReceiveQueueFrameLimit);
        EXPECT_FALSE(transport.State().closed);
        EXPECT_FALSE(session.Status().peer_disconnected);
        EXPECT_EQ(world.tick, 1u);
        transport.State().polls = 0;
        if (closure == 0)
            session.Disconnect();
        else
            session.PumpTick(10);
        EXPECT_LE(transport.State().polls, net::kReceiveQueueFrameLimit);
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
        EXPECT_EQ(session.Status().desync_tick, 1u);
        EXPECT_EQ(session.Status().dump_path, dump.string());
        const auto contents = replay::ReadReplay(dump.string());
        ASSERT_TRUE(contents.has_value());
        if (!contents)
            return;
        EXPECT_EQ(contents->header.version, replay::kLrec1Version);
        ASSERT_EQ(contents->inputs.size(), 1u);
        EXPECT_EQ(contents->inputs.front().inputs, (std::vector<std::uint8_t>{0xAB}));
        ASSERT_EQ(contents->checkpoints.size(), 1u);
        EXPECT_EQ(contents->checkpoints.front().terrain, HashAt(world, 1, "terrain"));
        fs::remove(dump, ec);
    }
}

TEST(LockstepSessionTest, LateDesyncAfterSendRefusalSurvivesAnExhaustedReceiveBudget) {
    for (const auto refusal : {net::MessageType::Input, net::MessageType::Hash}) {
        for (const std::uint64_t divergence : {1u, 2u}) {
            SCOPED_TRACE(static_cast<int>(refusal));
            SCOPED_TRACE(divergence);
            auto [local, peer] = net::MakeLoopbackPair();
            RejectingTransport rejecting(*local);
            ReplenishingTransport transport(rejecting);
            auto config = MakeConfig(0, 1);
            config.hash_cadence_ticks = 1;
            FakeWorld world{0, "agree", 0};
            net::LockstepSession session(config, &transport, MakeHooks(&world));
            const auto dump = LockstepTempDir() /
                              ("batched-refusal-" + std::to_string(static_cast<int>(refusal)) +
                               "-" + std::to_string(divergence) + ".lrec1");
            std::error_code ec;
            fs::remove(dump, ec);
            session.SetDumpPath(dump.string());
            ASSERT_TRUE(SendPeerHello(*peer, config));
            ASSERT_TRUE(session.Handshake());
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
            FakeWorld mismatching{0, "different", 0};
            net::HashMsg mismatch;
            CaptureHashes(divergence, mismatch, &mismatching);
            rejecting.State().rejected = refusal;
            rejecting.State().on_rejection = [&] {
                EXPECT_TRUE(peer->SendFrame(net::EncodeHash(mismatch)));
                transport.State().refill_first = true;
                transport.State().refill_left = net::kReceiveQueueFrameLimit + 1;
            };
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({2, 1, {}})));
            transport.State().polls = 0;
            EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
            EXPECT_EQ(transport.State().polls, net::kReceiveQueueFrameLimit);
            EXPECT_FALSE(transport.State().closed);
            EXPECT_FALSE(session.Status().peer_disconnected);
            EXPECT_EQ(world.tick, 2u);
            transport.State().polls = 0;
            EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
            EXPECT_LE(transport.State().polls, net::kReceiveQueueFrameLimit);
            EXPECT_EQ(session.Status().desync_tick, divergence);
            EXPECT_EQ(session.Status().dump_path, dump.string());
            const auto contents = replay::ReadReplay(dump.string());
            ASSERT_TRUE(contents.has_value());
            if (!contents)
                return;
            ASSERT_EQ(contents->checkpoints.size(), divergence);
            EXPECT_EQ(contents->checkpoints.back().tick, divergence);
            EXPECT_EQ(contents->checkpoints.back().terrain, HashAt(world, divergence, "terrain"));
            fs::remove(dump, ec);
        }
    }
}

TEST(LockstepSessionTest, SaturatedLocalDisconnectFinishesOnAnEmptyPollWithoutAnotherStep) {
    for (const bool finish_with_pump : {false, true}) {
        auto [local, peer] = net::MakeLoopbackPair();
        ReplenishingTransport transport(*local);
        const auto config = MakeConfig(0, 1);
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        transport.State().refill_left = ReplenishingTransport::kFuse;
        transport.State().polls = 0;
        session.Disconnect();
        EXPECT_EQ(transport.State().polls, net::kReceiveQueueFrameLimit);
        EXPECT_FALSE(transport.State().closed);
        EXPECT_FALSE(session.Status().peer_disconnected);
        transport.State().refill_left = 0;
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({2, 1, {}})));
        if (finish_with_pump)
            EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
        else
            session.Disconnect();
        EXPECT_TRUE(transport.State().closed);
        EXPECT_TRUE(session.Status().peer_disconnected);
        EXPECT_FALSE(session.Status().desynced);
        EXPECT_EQ(world.tick, 1u);
        EXPECT_EQ(session.Status().disconnect_tick, 1u);
        EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
        EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
        const auto polls = transport.State().polls;
        session.Disconnect();
        EXPECT_EQ(transport.State().polls, polls);
    }
}

TEST(LockstepSessionTest, ClosedTransportDeliversInputsBeyondAReceiveBatchBeforeReleasingStorage) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 0;
    net::LockstepSession session(config, local.get(), {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    for (std::size_t index = 0; index < net::kReceiveQueueFrameLimit; ++index)
        ASSERT_TRUE(peer->SendFrame({}));
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {0xAB}})));
    peer->Close();
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_FALSE(session.Status().peer_disconnected);
    const auto delivered = session.PumpTick(10);
    EXPECT_EQ(delivered.outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(delivered.local_inputs, (std::vector<std::uint8_t>{0xAB}));
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
}

TEST(LockstepSessionTest, PeerHelloMustMatchTheExpectedDistinctSlot) {
    for (const std::uint32_t local_id : {0u, 1u}) {
        for (const std::uint32_t claimed_id : {local_id, 42u}) {
            auto [local, peer] = net::MakeLoopbackPair();
            const auto config = MakeConfig(local_id, 1u - local_id);
            FakeWorld world{0, "agree", 0};
            net::LockstepSession session(config, local.get(), MakeHooks(&world));
            auto claimed = config;
            claimed.peer_client_id = claimed_id;
            ASSERT_TRUE(SendPeerHello(*peer, claimed));
            EXPECT_FALSE(session.Handshake());
            EXPECT_FALSE(session.Status().handshaken);
            EXPECT_EQ(session.Status().rejected_peer_messages, 1u);
            EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
        }
    }
}

TEST(LockstepSessionTest, BothRolesRejectSpoofedInputWithoutChangingTheAgreedInput) {
    for (const std::uint32_t local_id : {0u, 1u}) {
        auto [local, peer] = net::MakeLoopbackPair();
        const auto config = MakeConfig(local_id, 1u - local_id);
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, local.get(), MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, local_id, {0xEE}})));
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 42, {0xFF}})));
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, config.peer_client_id, {0xAB}})));
        const auto result = session.PumpTick(10);
        EXPECT_EQ(result.outcome, net::TickOutcome::Advanced);
        EXPECT_EQ(result.local_inputs, (std::vector<std::uint8_t>{0xAB}));
        EXPECT_EQ(session.Status().rejected_peer_messages, 2u);
        EXPECT_FALSE(session.Status().desynced);
        EXPECT_FALSE(session.Status().peer_disconnected);
        EXPECT_EQ(session.AdmissionStatus().peer_inputs, 0u);
    }
}

TEST(LockstepSessionTest, RejectsStaleAndFutureInputsAndHashesWithoutRetainingThem) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    net::HashMsg matching;
    CaptureHashes(1, matching, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(matching)));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    ASSERT_EQ(session.AdmissionStatus().confirmed_hash_tick, 1u);
    const auto too_far = session.AgreedTick() + std::uint64_t{2} * config.horizon_max + 1;
    for (const auto tick : {std::uint64_t{0},
                            std::uint64_t{1},
                            too_far,
                            (std::numeric_limits<std::uint64_t>::max)()}) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {0xFF}})));
        matching.tick = tick;
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(matching)));
    }
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.Status().rejected_peer_messages, 8u);
    const auto retained = session.AdmissionStatus();
    EXPECT_EQ(retained.peer_inputs, 0u);
    EXPECT_EQ(retained.peer_hashes, 0u);
    EXPECT_EQ(retained.retained_bytes, 0u);
    EXPECT_LE(retained.input_ticks, config.horizon_max);
    EXPECT_EQ(world.tick, 1u);
}

void ExpectTerminalReceiveFloodBounded(bool check_bytes) {
    auto [local, peer] = net::MakeLoopbackPair();
    RejectingTransport transport(*local);
    auto config = MakeConfig(0, 1);
    config.horizon_min = config.horizon_start = config.horizon_max = 1;
    config.hash_cadence_ticks = 1;
    config.max_frame_payload_bytes = check_bytes ? 29 : 128;
    net::LockstepSession session(config, &transport, {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    const auto bounds = session.AdmissionStatus();
    const auto window = bounds.checkpoint_window;
    const auto budget = window + bounds.message_limit + 1;
    for (std::uint64_t tick = 1; tick <= window; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        ASSERT_EQ(session.PumpTick(budget).outcome, net::TickOutcome::Advanced);
    }
    ASSERT_EQ(session.PumpTick(budget).outcome, net::TickOutcome::WaitingForPeer);

    // A terminal send side can still yield receive frames. Withhold tick 1,
    // then flood distinct old checkpoints while continuing to supply inputs.
    transport.State().peer_disconnected = true;
    for (std::uint64_t tick = 2; tick <= window; ++tick) {
        net::HashMsg hash;
        hash.tick = tick;
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    }
    // The first batch contains only old checkpoints. Yield without closing before
    // the next batch can deliver the wanted input; retain the original final bounds.
    ASSERT_GT(window - 1, net::kReceiveQueueFrameLimit);
    ASSERT_EQ(session.PumpTick(budget).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, net::kReceiveQueueFrameLimit);
    for (std::uint64_t tick = window + 1; tick <= budget; ++tick) {
        if (tick > window + 1) {
            net::HashMsg hash;
            hash.tick = tick - 1;
            ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        }
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        const auto result = session.PumpTick(budget);
        const auto usage = session.AdmissionStatus();
        if (check_bytes) {
            EXPECT_LE(usage.retained_bytes, bounds.byte_limit);
        } else
            EXPECT_LE(usage.peer_inputs + usage.peer_hashes, bounds.message_limit);
        EXPECT_LE(usage.local_hashes, window + 2);
        if (result.outcome == net::TickOutcome::PeerDisconnected)
            break;
        ASSERT_EQ(result.outcome, net::TickOutcome::Advanced);
    }
    EXPECT_TRUE(session.Status().peer_disconnected);
    EXPECT_GT(session.Status().rejected_peer_messages, 0u);
    EXPECT_EQ(session.AgreedTick(), window + 2);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
}

TEST(LockstepSessionTest, TerminalReceiveFloodCannotExceedNegotiatedMessageCount) {
    ExpectTerminalReceiveFloodBounded(false);
}

TEST(LockstepSessionTest, TerminalReceiveFloodCannotExceedNegotiatedByteSize) {
    ExpectTerminalReceiveFloodBounded(true);
}

TEST(LockstepSessionTest, ByeAfterTransportClosureCannotReopenAdmission) {
    auto [local, peer] = net::MakeLoopbackPair();
    RejectingTransport transport(*local);
    auto config = MakeConfig(0, 1);
    config.horizon_min = config.horizon_start = config.horizon_max = 1;
    config.hash_cadence_ticks = 1;
    net::LockstepSession session(config, &transport, {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    transport.State().peer_disconnected = true;
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {1}})));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    ASSERT_TRUE(peer->SendFrame(net::EncodeBye({3})));
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({2, 1, {2}})));
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({3, 1, {3}})));
    const auto last = session.PumpTick(10);
    ASSERT_EQ(last.outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(last.local_inputs, (std::vector<std::uint8_t>{2}));
    EXPECT_EQ(session.Status().rejected_peer_messages, 1u);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(session.AgreedTick(), 2u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
}

TEST(LockstepSessionTest, NegotiatedMessageCapacityRefusesFullAndOverflowedUsage) {
    auto [local, peer] = net::MakeLoopbackPair();
    const auto config = MakeConfig(0, 1);
    net::LockstepSession session(config, local.get(), {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    auto usage = session.AdmissionStatus();
    ASSERT_GT(usage.message_limit, 1u);
    usage.peer_inputs = 1;
    usage.peer_hashes = usage.message_limit - 2;
    EXPECT_TRUE(net::HasPeerRetentionCapacity(usage, 29));
    ++usage.peer_hashes;
    EXPECT_FALSE(net::HasPeerRetentionCapacity(usage, 29));
    usage.peer_inputs = usage.message_limit;
    usage.peer_hashes = 0;
    EXPECT_FALSE(net::HasPeerRetentionCapacity(usage, 29));
    usage.peer_inputs = (std::numeric_limits<std::size_t>::max)();
    usage.peer_hashes = (std::numeric_limits<std::size_t>::max)();
    EXPECT_FALSE(net::HasPeerRetentionCapacity(usage, 29));
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
}

TEST(LockstepSessionTest, NegotiatedByteCapacityRefusesFullAndOverflowedUsage) {
    auto [local, peer] = net::MakeLoopbackPair();
    const auto config = MakeConfig(0, 1);
    net::LockstepSession session(config, local.get(), {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    auto usage = session.AdmissionStatus();
    ASSERT_GT(usage.byte_limit, 29u);
    usage.retained_bytes = usage.byte_limit - 29;
    EXPECT_TRUE(net::HasPeerRetentionCapacity(usage, 29));
    EXPECT_FALSE(net::HasPeerRetentionCapacity(usage, 30));
    EXPECT_FALSE(net::HasPeerRetentionCapacity(usage, (std::numeric_limits<std::size_t>::max)()));
    usage.retained_bytes = usage.byte_limit;
    EXPECT_FALSE(net::HasPeerRetentionCapacity(usage, 1));
    usage.retained_bytes = (std::numeric_limits<std::size_t>::max)();
    EXPECT_FALSE(net::HasPeerRetentionCapacity(usage, 1));
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
}

TEST(LockstepSessionTest, WithheldFirstCheckpointBoundsLateHashFloodAndRefusesSettledTicks) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    config.max_frame_payload_bytes = 128;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    const auto bounds = session.AdmissionStatus();
    const auto window = bounds.checkpoint_window;
    for (std::uint64_t tick = 1; tick <= window; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        ASSERT_EQ(session.PumpTick(window + 1).outcome, net::TickOutcome::Advanced);
    }
    ASSERT_EQ(session.PumpTick(window + 1).outcome, net::TickOutcome::WaitingForPeer);
    // All are distinct past ticks. The first checkpoint remains withheld while
    // late matches are reduced to bounded markers, never uncomparable strings.
    for (std::uint64_t tick = 2; tick <= window; ++tick) {
        net::HashMsg hash;
        CaptureHashes(tick, hash, &world);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    }
    ASSERT_EQ(session.PumpTick(window + 1).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, net::kReceiveQueueFrameLimit);
    ASSERT_EQ(session.PumpTick(window + 1).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, window - 1);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 29 * (window - 1));
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, 0u);
    // Settled out-of-order ticks cannot be reinserted with fresh payloads.
    for (std::uint64_t tick = 2; tick <= window; ++tick) {
        net::HashMsg hash;
        CaptureHashes(tick, hash, &world);
        hash.world_hash.assign(99, 'x');
        hash.terrain.clear();
        hash.water.clear();
        hash.entities.clear();
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    }
    ASSERT_EQ(session.PumpTick(window + 1).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.Status().rejected_peer_messages, net::kReceiveQueueFrameLimit);
    ASSERT_EQ(session.PumpTick(window + 1).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.Status().rejected_peer_messages, window - 1);
    EXPECT_LE(session.AdmissionStatus().peer_hashes, bounds.message_limit);
    EXPECT_LE(session.AdmissionStatus().retained_bytes, bounds.byte_limit);
    EXPECT_FALSE(session.Status().desynced);

    net::HashMsg first;
    CaptureHashes(1, first, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(first)));
    ASSERT_EQ(session.PumpTick(window).outcome, net::TickOutcome::Finished);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, window);
    // Every distinct old checkpoint is now outside retained admission history.
    for (std::uint64_t tick = 1; tick <= window; ++tick) {
        net::HashMsg hash;
        CaptureHashes(tick, hash, &world);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    }
    EXPECT_EQ(session.PumpTick(window).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.Status().rejected_peer_messages, window - 1 + net::kReceiveQueueFrameLimit);
    EXPECT_EQ(session.PumpTick(window).outcome, net::TickOutcome::Finished);
    EXPECT_EQ(session.Status().rejected_peer_messages, 2 * window - 1);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
}

TEST(LockstepSessionTest, RetentionBudgetsMustFitAllNegotiatedTraffic) {
    for (const bool byte_bound : {false, true}) {
        auto [local, peer] = net::MakeLoopbackPair();
        auto config = MakeConfig(0, 1);
        if (byte_bound)
            config.peer_retained_byte_limit = net::kMaxFramePayloadBytes;
        else
            config.peer_retained_message_limit = 2;
        net::LockstepSession session(config, local.get(), {});
        ASSERT_TRUE(SendPeerHello(*peer, config));
        EXPECT_FALSE(session.Handshake());
        EXPECT_EQ(session.Status().handshake_error,
                  "lockstep retention budget cannot fit negotiated traffic");
        EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
    }
}

TEST(LockstepSessionTest, ExactDerivedBudgetsFitAllPermittedInputAndHashSlots) {
    auto config = MakeConfig(0, 1);
    config.max_frame_payload_bytes = 128;
    config.hash_cadence_ticks = 1;
    auto [initial_local, initial_peer] = net::MakeLoopbackPair();
    net::LockstepSession initial(config, initial_local.get(), {});
    ASSERT_TRUE(SendPeerHello(*initial_peer, config));
    ASSERT_TRUE(initial.Handshake());
    const auto bounds = initial.AdmissionStatus();
    config.peer_retained_byte_limit = bounds.byte_limit;
    config.peer_retained_message_limit = bounds.message_limit;
    auto [local, peer] = net::MakeLoopbackPair();
    net::LockstepSession session(config, local.get(), {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    const auto lead = std::uint64_t{2} * config.horizon_max;
    // All unique future ticks in the negotiated input window, both message types
    // at the exact maximum frame size, fit concurrently before execution.
    for (std::uint64_t tick = 1; tick <= lead; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, std::vector<std::uint8_t>(107)})));
        net::HashMsg hash;
        hash.tick = tick;
        hash.terrain.assign(99, 'h'); // 29-byte overhead + 99 bytes = 128.
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    }
    ASSERT_EQ(session.PumpTick(lead).outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(session.AdmissionStatus().peer_inputs, lead - 1);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, lead - 1);
    EXPECT_LE(session.AdmissionStatus().retained_bytes, bounds.byte_limit);
    for (std::uint64_t tick = 2; tick <= lead; ++tick)
        ASSERT_EQ(session.PumpTick(lead).outcome,
                  tick == lead ? net::TickOutcome::Finished : net::TickOutcome::Advanced);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    EXPECT_EQ(session.Status().rejected_peer_messages, 0u);
}

TEST(LockstepSessionTest, FloodBoundsContainersAndHonestTrafficRecovers) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    const auto lead = std::uint64_t{2} * config.horizon_max;
    for (int batch = 0; batch < 8; ++batch) {
        for (std::uint64_t tick = 2; tick <= lead + 32; ++tick) {
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {0xAB}})));
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 42, {0xFF}})));
            net::HashMsg hash;
            CaptureHashes(tick, hash, &world);
            ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        }
        EXPECT_EQ(session.PumpTick(100).outcome, net::TickOutcome::WaitingForPeer);
        const auto retained = session.AdmissionStatus();
        EXPECT_EQ(retained.peer_inputs, lead - 1);
        EXPECT_EQ(retained.peer_hashes, lead - 1);
        EXPECT_LE(retained.peer_inputs + retained.peer_hashes, retained.message_limit);
        EXPECT_LE(retained.retained_bytes, retained.byte_limit);
        EXPECT_LE(retained.input_ticks, lead);
    }
    EXPECT_GT(session.Status().rejected_peer_messages, 0u);
    // The missing oldest tick was never displaced by later arrivals or the flood.
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {0xAB}})));
    net::HashMsg first;
    CaptureHashes(1, first, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(first)));
    for (std::uint64_t tick = 1; tick <= lead; ++tick)
        ASSERT_EQ(session.PumpTick(lead).outcome,
                  tick == lead ? net::TickOutcome::Finished : net::TickOutcome::Advanced);
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, lead);
    EXPECT_EQ(session.AdmissionStatus().peer_inputs, 0u);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    EXPECT_FALSE(session.Status().desynced);
    EXPECT_FALSE(session.Status().peer_disconnected);
}

TEST(LockstepSessionTest, ConsumedInputsAndConfirmedHashesReleaseTheirEntirePeerHistory) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    for (std::uint64_t tick = 1; tick <= 120; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {0xAB}})));
        net::HashMsg hash;
        CaptureHashes(tick, hash, &world);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        ASSERT_EQ(session.PumpTick(121).outcome, net::TickOutcome::Advanced);
        const auto retained = session.AdmissionStatus();
        EXPECT_EQ(retained.peer_inputs, 0u);
        EXPECT_EQ(retained.peer_hashes, 0u);
        EXPECT_EQ(retained.retained_bytes, 0u);
        EXPECT_EQ(retained.confirmed_hash_tick, tick);
        EXPECT_EQ(retained.input_ticks, config.horizon_start);
    }
    EXPECT_EQ(session.Status().rejected_peer_messages, 0u);
    EXPECT_FALSE(session.Status().desynced);
}

TEST(LockstepSessionTest, ReorderedHashesConfirmOnlyAfterEarlierCheckpointsArrive) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    }
    for (const std::uint64_t tick : {3u, 1u, 2u}) {
        net::HashMsg hash;
        CaptureHashes(tick, hash, &world);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
        EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick,
                  tick == 3   ? 0u
                  : tick == 1 ? 1u
                              : 3u);
    }
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    EXPECT_EQ(session.Status().rejected_peer_messages, 0u);
}

TEST(LockstepSessionTest, LookaheadIncludesBothPeerHorizonsAndRejectsTheNextTick) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    // Admission includes both peers' maximum validated scheduling horizons.
    config.horizon_start = config.horizon_min = 2;
    config.horizon_max = 4;
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    const auto last = std::uint64_t{2} * config.horizon_max;
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({last, 1, {0xAB}})));
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({last + 1, 1, {0xFF}})));
    net::HashMsg hash;
    CaptureHashes(last, hash, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    hash.tick = last + 1;
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.AdmissionStatus().peer_inputs, 1u);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 1u);
    EXPECT_EQ(session.Status().rejected_peer_messages, 2u);
}

TEST(LockstepSessionTest, DuplicateInputsCannotReplaceTheFirstAdmittedBlob) {
    auto [local, peer] = net::MakeLoopbackPair();
    const auto config = MakeConfig(0, 1);
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    const auto original = net::EncodeInput({2, 1, {0xAB}});
    ASSERT_TRUE(peer->SendFrame(original));
    ASSERT_TRUE(peer->SendFrame(original));
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({2, 1, {0xFF, 0xFF}})));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.AdmissionStatus().peer_inputs, 1u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, original.size());
    EXPECT_EQ(session.Status().rejected_peer_messages, 1u);
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    const auto result = session.PumpTick(10);
    EXPECT_EQ(result.outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(result.local_inputs, (std::vector<std::uint8_t>{0xAB}));
    EXPECT_EQ(session.AdmissionStatus().peer_inputs, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
}

TEST(LockstepSessionTest, AdmissionRejectsMalformedLengthsTrailingDataAndOffCadenceHashes) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 2;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    auto bad_input = net::EncodeInput({1, 1, {}});
    bad_input.back() = 0xFF; // Blob length exceeds the complete frame.
    ASSERT_TRUE(peer->SendFrame(bad_input));
    auto trailing_input = net::EncodeInput({1, 1, {}});
    trailing_input.push_back(0xFF);
    ASSERT_TRUE(peer->SendFrame(trailing_input));
    net::HashMsg hash;
    CaptureHashes(2, hash, &world);
    auto bad_hash = net::EncodeHash(hash);
    bad_hash[13] = 0xFF; // First string length exceeds the complete frame.
    ASSERT_TRUE(peer->SendFrame(bad_hash));
    auto trailing_hash = net::EncodeHash(hash);
    trailing_hash.push_back(0xFF);
    ASSERT_TRUE(peer->SendFrame(trailing_hash));
    for (const std::uint64_t tick : {0u, 1u}) {
        hash.tick = tick;
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    }
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.Status().rejected_peer_messages, 6u);
    EXPECT_EQ(session.AdmissionStatus().peer_inputs, 0u);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
}

TEST(LockstepSessionTest, ReorderedLaterMatchCannotHideAnEarlierDelayedDesync) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    const auto dump = LockstepTempDir() / "desync-reordered-checkpoints.lrec1";
    session.SetDumpPath(dump.string());
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    }
    net::HashMsg hash;
    CaptureHashes(3, hash, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
    ASSERT_EQ(session.AdmissionStatus().confirmed_hash_tick, 0u);
    FakeWorld mismatching{0, "different", 0};
    CaptureHashes(1, hash, &mismatching);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
    EXPECT_EQ(session.Status().desync_tick, 1u);
    const auto contents = replay::ReadReplay(dump.string());
    if (!contents.has_value()) {
        FAIL() << "desync replay could not be read";
    }
    const auto& replay_contents = *contents;
    EXPECT_EQ(replay_contents.inputs.size(), 3u);
    ASSERT_EQ(replay_contents.checkpoints.size(), 1u);
    EXPECT_EQ(replay_contents.checkpoints.front().tick, 1u);
    std::error_code ec;
    fs::remove(dump, ec);
}

TEST(LockstepSessionTest, ClosureReleasesRetainedMessagesAfterDrainingBufferedInputs) {
    for (const bool send_bye : {false, true}) {
        SCOPED_TRACE(send_bye);
        auto [local, peer] = net::MakeLoopbackPair();
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, local.get(), MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        constexpr std::uint64_t ticks = 5; // Beyond the three pre-scheduled local slots.
        for (std::uint64_t tick = 1; tick <= ticks; ++tick)
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        net::HashMsg hash;
        CaptureHashes(ticks, hash, &world);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        if (send_bye) {
            ASSERT_TRUE(peer->SendFrame(net::EncodeBye({ticks})));
        }
        peer->Close();
        for (std::uint64_t tick = 1; tick <= ticks; ++tick)
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        EXPECT_EQ(world.tick, ticks);
        std::vector<std::uint8_t> frame;
        std::size_t frames_sent = 0;
        while (peer->TryReceiveFrame(frame))
            ++frames_sent;
        EXPECT_EQ(frames_sent, 1u + config.horizon_start); // Hello and initial inputs only.
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
        EXPECT_FALSE(session.Status().desynced);
        const auto retained = session.AdmissionStatus();
        EXPECT_EQ(retained.input_ticks, 0u);
        EXPECT_EQ(retained.peer_inputs, 0u);
        EXPECT_EQ(retained.peer_hashes, 0u);
        EXPECT_EQ(retained.retained_bytes, 0u);
    }
}

TEST(LockstepSessionTest, ByeStopsAtExecutedTickDespitePrescheduledPeerInputs) {
    for (const std::uint64_t stop_tick : {0u, 1u, 5u}) {
        SCOPED_TRACE(stop_tick);
        auto [local, peer] = net::MakeLoopbackPair();
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, local.get(), MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        for (std::uint64_t tick = 1; tick <= stop_tick + config.horizon_start; ++tick)
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        ASSERT_TRUE(peer->SendFrame(net::EncodeBye({stop_tick})));
        // Queued oracle messages after Bye must still compare through T.
        if (stop_tick != 0) {
            net::HashMsg hash;
            CaptureHashes(stop_tick, hash, &world);
            ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        }
        peer->Close();
        for (std::uint64_t tick = 1; tick <= stop_tick; ++tick)
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        EXPECT_EQ(world.tick, stop_tick);
        EXPECT_EQ(session.AgreedTick(), stop_tick);
        if (stop_tick != 0) {
            EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, stop_tick == 1 ? 1u : 0u);
            EXPECT_EQ(session.AdmissionStatus().peer_hashes, stop_tick == 1 ? 0u : 1u);
        }
        const auto result = session.PumpTick(10);
        EXPECT_EQ(result.outcome, net::TickOutcome::PeerDisconnected);
        EXPECT_EQ(result.tick, stop_tick);
        EXPECT_EQ(session.Status().disconnect_tick, stop_tick);
        EXPECT_FALSE(session.Status().desynced);
        EXPECT_EQ(world.tick, stop_tick);
        EXPECT_EQ(session.AgreedTick(), stop_tick);
        const auto retained = session.AdmissionStatus();
        EXPECT_EQ(retained.input_ticks, 0u);
        EXPECT_EQ(retained.peer_inputs, 0u);
        EXPECT_EQ(retained.peer_hashes, 0u);
        EXPECT_EQ(retained.retained_bytes, 0u);
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
        EXPECT_EQ(world.tick, stop_tick);
    }
}

TEST(LockstepSessionTest, DelayedHashAfterByeStillDetectsDesyncBeforeBufferedTicks) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    const auto dump = LockstepTempDir() / "desync-delayed-after-bye.lrec1";
    session.SetDumpPath(dump.string());
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    for (std::uint64_t tick = 1; tick <= 5; ++tick)
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
    ASSERT_TRUE(peer->SendFrame(net::EncodeBye({5})));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    FakeWorld mismatching{0, "different", 0};
    net::HashMsg hash;
    CaptureHashes(1, hash, &mismatching);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
    EXPECT_EQ(session.Status().desync_tick, 1u);
    EXPECT_EQ(world.tick, 1u);
    EXPECT_EQ(session.Status().rejected_peer_messages, 0u);
    std::error_code ec;
    fs::remove(dump, ec);
}

TEST(LockstepSessionTest, ByeComparesQueuedHashAtFinalTickBeforeDiscardingLaterInputs) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    FakeWorld mismatch{0, "different", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    const auto dump = LockstepTempDir() / "bye-final-tick-desync.lrec1";
    session.SetDumpPath(dump.string());
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    constexpr std::uint64_t stop_tick = 5;
    for (std::uint64_t tick = 1; tick <= stop_tick + config.horizon_start; ++tick)
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
    ASSERT_TRUE(peer->SendFrame(net::EncodeBye({stop_tick})));
    net::HashMsg hash;
    CaptureHashes(stop_tick, hash, &mismatch);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    peer->Close();
    for (std::uint64_t tick = 1; tick < stop_tick; ++tick)
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
    EXPECT_EQ(world.tick, stop_tick);
    EXPECT_EQ(session.AgreedTick(), stop_tick);
    EXPECT_EQ(session.Status().desync_tick, stop_tick);
    EXPECT_EQ(session.Status().dump_path, dump.string());
    const auto parsed = replay::ReadReplay(dump.string());
    if (!parsed.has_value()) {
        FAIL() << "final-tick mismatch lost its replay";
    }
    const auto& contents = *parsed;
    EXPECT_FALSE(contents.truncated);
    EXPECT_EQ(contents.tick_count, stop_tick);
    ASSERT_EQ(contents.inputs.size(), stop_tick);
    EXPECT_EQ(contents.inputs.back().tick, stop_tick);
    ASSERT_EQ(contents.checkpoints.size(), stop_tick);
    EXPECT_EQ(contents.checkpoints.back().tick, stop_tick);
    EXPECT_EQ(contents.checkpoints.back().terrain, HashAt(world, stop_tick, "terrain"));
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
    EXPECT_EQ(world.tick, stop_tick);
    fs::remove(dump);
}

TEST(LockstepSessionTest, ByeDrainsThroughFinalTickWithPendingOutboundFrames) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, &transport, MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    constexpr std::uint64_t stop_tick = 5;
    for (std::uint64_t tick = 1; tick <= stop_tick + config.horizon_start; ++tick)
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
    transport.State().start_refusing_at = net::MessageType::Input;
    transport.State().refusals_remaining = 2;
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    ASSERT_EQ(transport.State().refused_frames.size(), 1u);
    ASSERT_TRUE(peer->SendFrame(net::EncodeBye({stop_tick})));
    net::HashMsg hash;
    CaptureHashes(stop_tick, hash, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    peer->Close();
    for (std::uint64_t tick = 2; tick <= stop_tick; ++tick)
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(session.AgreedTick(), stop_tick);
    EXPECT_EQ(session.Status().disconnect_tick, stop_tick);
    EXPECT_EQ(world.tick, stop_tick);
    EXPECT_FALSE(session.Status().desynced);
    EXPECT_EQ(transport.State().refused_frames.size(), 1u);
    EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
}

TEST(LockstepSessionTest, ByeAndFinalHashAreDrainedAtTheTickBudget) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    for (std::uint64_t tick = 1; tick <= config.horizon_start; ++tick)
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
    ASSERT_EQ(session.PumpTick(1).outcome, net::TickOutcome::Finished);
    ASSERT_TRUE(peer->SendFrame(net::EncodeBye({1})));
    net::HashMsg hash;
    CaptureHashes(1, hash, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    peer->Close();
    const auto result = session.PumpTick(1);
    EXPECT_EQ(result.outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(result.tick, 1u);
    EXPECT_EQ(session.Status().disconnect_tick, 1u);
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, 1u);
    EXPECT_EQ(world.tick, 1u);
    EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
}

TEST(LockstepSessionTest, ByeDiscardsRetriesAtSmallerBudgetAndStillDrainsReceivedInputs) {
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto [local, peer] = net::MakeLoopbackPair();
        BackpressuredTransport transport(*local);
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        constexpr std::uint64_t stop_tick = 5;
        for (std::uint64_t tick = 1; tick <= stop_tick; ++tick)
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        transport.State().start_refusing_at = type;
        transport.State().refusals_remaining = 3;
        ASSERT_EQ(session.PumpTick(1).outcome, net::TickOutcome::Advanced);
        ASSERT_EQ(transport.State().refused_frames.size(), 1u);

        // Partial Bye frames must not disable retries or close the session.
        const auto bye = net::EncodeBye({stop_tick});
        for (std::size_t size = 1; size < bye.size(); ++size) {
            auto partial = bye;
            partial.resize(size);
            ASSERT_TRUE(peer->SendFrame(partial));
        }
        ASSERT_TRUE(peer->SendFrame({255})); // Unknown metadata is ignored.
        ASSERT_EQ(session.PumpTick(1).outcome, net::TickOutcome::WaitingForPeer);
        EXPECT_EQ(session.Status().rejected_peer_messages, bye.size() - 1);
        ASSERT_EQ(transport.State().refused_frames.size(), 2u);
        EXPECT_FALSE(session.Status().peer_disconnected);

        ASSERT_TRUE(peer->SendFrame(bye));
        ASSERT_TRUE(peer->SendFrame(net::EncodeBye({0}))); // First stop tick is binding.
        peer->Close();
        for (const std::uint64_t budget : {0u, 1u, 1u})
            EXPECT_EQ(session.PumpTick(budget).outcome, net::TickOutcome::Finished);
        EXPECT_EQ(world.tick, 1u);
        EXPECT_EQ(transport.State().refused_frames.size(), 2u);
        for (std::uint64_t tick = 2; tick <= stop_tick; ++tick)
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
        EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
        EXPECT_EQ(session.Status().disconnect_tick, stop_tick);
        EXPECT_EQ(world.tick, stop_tick);
        EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
        EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    }
}

TEST(LockstepSessionTest, ByeDuringRefusedSendDoesNotRecreateRetriesAtSmallerBudget) {
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        for (const bool during_retry : {false, true}) {
            SCOPED_TRACE(static_cast<int>(type));
            SCOPED_TRACE(during_retry);
            auto [local, peer] = net::MakeLoopbackPair();
            RejectingTransport transport(*local);
            auto config = MakeConfig(0, 1);
            config.hash_cadence_ticks = 1;
            FakeWorld world{0, "agree", 0};
            net::LockstepSession session(config, &transport, MakeHooks(&world));
            ASSERT_TRUE(SendPeerHello(*peer, config));
            ASSERT_TRUE(session.Handshake());
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
            transport.State().rejected = type;
            // Keep the first refusal transient when testing the later retry drain.
            transport.State().drain_before_disconnect = true;
            std::size_t refusals = 0;
            transport.State().on_rejection = [&] {
                ++refusals;
                if (!during_retry || refusals == 2) {
                    ASSERT_TRUE(peer->SendFrame(net::EncodeBye({5})));
                    peer->Close();
                } else {
                    transport.State().peer_disconnected = false;
                }
            };
            EXPECT_EQ(session.PumpTick(1).outcome,
                      during_retry ? net::TickOutcome::Advanced : net::TickOutcome::Finished);
            EXPECT_EQ(session.PumpTick(1).outcome, net::TickOutcome::Finished);
            EXPECT_EQ(session.PumpTick(1).outcome, net::TickOutcome::Finished);
            EXPECT_EQ(refusals, during_retry ? 2u : 1u);
            EXPECT_EQ(world.tick, 1u);
            EXPECT_FALSE(session.Status().peer_disconnected);
            // No input for tick 2: closure must still terminate a larger budget.
            EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
            EXPECT_EQ(session.Status().disconnect_tick, 5u);
        }
    }
}

TEST(LockstepSessionTest, ByeReceivedDuringSendStillDrainsThroughFinalTick) {
    auto [local, peer] = net::MakeLoopbackPair();
    RejectingTransport transport(*local);
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, &transport, MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    constexpr std::uint64_t stop_tick = 5;
    for (std::uint64_t tick = 1; tick <= stop_tick + config.horizon_start; ++tick)
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
    transport.State().rejected = net::MessageType::Input;
    transport.State().on_rejection = [&] {
        ASSERT_TRUE(peer->SendFrame(net::EncodeBye({stop_tick})));
        net::HashMsg hash;
        CaptureHashes(stop_tick, hash, &world);
        ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        peer->Close();
    };
    for (std::uint64_t tick = 1; tick <= stop_tick; ++tick)
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(session.AgreedTick(), stop_tick);
    EXPECT_EQ(session.Status().disconnect_tick, stop_tick);
    EXPECT_EQ(world.tick, stop_tick);
    EXPECT_FALSE(session.Status().desynced);
    EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
}

TEST(LockstepSessionTest, DisconnectFlushesQueuedFramesBeforeByeOrClosesWithoutBye) {
    for (const auto type : {net::MessageType::Input, net::MessageType::Hash}) {
        for (const bool recovered : {false, true}) {
            SCOPED_TRACE(static_cast<int>(type));
            SCOPED_TRACE(recovered);
            auto [local, peer] = net::MakeLoopbackPair();
            BackpressuredTransport transport(*local);
            transport.State().start_refusing_at = type;
            transport.State().refusals_remaining = 1;
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
            ASSERT_EQ(transport.State().refused_frames.size(), 1u);
            if (recovered) {
                expected.push_back(transport.State().refused_frames.front());
                expected.push_back(net::EncodeBye({world.tick}));
            }
            transport.State().refusals_remaining = recovered ? 0 : 1;
            session.Disconnect();
            session.Disconnect();
            EXPECT_EQ(transport.State().bye_attempts, recovered ? 1u : 0u);
            EXPECT_TRUE(transport.State().closed);
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
            transport.State().payload_limit = 128;
            transport.State().start_refusing_at = type;
            transport.State().refusals_remaining = 1;
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
            EXPECT_EQ(transport.State().refused_frames.size(), excess == 0 ? 1u : 0u);
            EXPECT_EQ(transport.State().closed, excess != 0);
            if (excess == 0) {
                EXPECT_EQ(transport.State().refused_frames.front().size(), 128u);
                session.Disconnect();
                std::vector<std::uint8_t> frame;
                bool received_exact_frame = false;
                while (peer->TryReceiveFrame(frame)) {
                    if (frame == transport.State().refused_frames.front())
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
    transport.State().payload_limit = 0;
    const auto config = MakeConfig(0, 1);
    net::LockstepSession session(config, &transport, {});
    ASSERT_TRUE(SendPeerHello(*peer, config));
    EXPECT_FALSE(session.Handshake());
    EXPECT_TRUE(transport.State().closed);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
}

TEST(LockstepSessionTest, PendingOutboundByteLimitAcceptsExactLimitThenDisconnectsOnOverflow) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    transport.State().start_refusing_at = net::MessageType::Input;
    transport.State().refusals_remaining = 2;
    transport.State().record_refused_frames = false;
    auto config = MakeConfig(0, 1);
    config.horizon_min = config.horizon_max = config.horizon_start = 1;
    net::LockstepHooks hooks;
    hooks.collect_local_input = [](std::uint64_t tick, void*) {
        return std::vector<std::uint8_t>(tick == 1 ? net::kMaxFramePayloadBytes - 21 : 0, 0xAB);
    };
    net::LockstepSession session(config, &transport, hooks);
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake()); // Exactly 64 MiB of encoded bytes retained.
    EXPECT_FALSE(transport.State().closed);
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
    // The final refusal keeps that frame queued; tick 2's input would exceed the cap.
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_TRUE(transport.State().closed);
    EXPECT_EQ(session.AgreedTick(), 1u);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
}

TEST(LockstepSessionTest, DifferentPeerInputsMergeIdenticallyUnderTransientRefusal) {
    auto [ta, tb] = net::MakeLoopbackPair();
    BackpressuredTransport a_transport(*ta);
    BackpressuredTransport b_transport(*tb);
    a_transport.State().start_refusing_at = net::MessageType::Input;
    b_transport.State().start_refusing_at = net::MessageType::Hash;
    a_transport.State().refusals_remaining = b_transport.State().refusals_remaining = 3;
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
        transport.State().start_refusing_at = type;
        transport.State().refusals_remaining = 3;
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
                transport.State().start_refusing_at = net::MessageType::Hash;
                transport.State().refusals_remaining = 2;
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
        EXPECT_FALSE(transport.State().closed);
        EXPECT_FALSE(session.Status().peer_disconnected);
        EXPECT_EQ(state.world.tick, 6u);
        EXPECT_EQ(state.collected, (std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
        EXPECT_EQ(state.captured, (std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5, 6}));

        std::vector<std::vector<std::uint8_t>> received;
        while (peer->TryReceiveFrame(frame))
            received.push_back(frame);
        EXPECT_EQ(received, expected); // Includes exact bytes, global order and multiplicity.
        ASSERT_EQ(transport.State().refused_frames.size(), 5u);
        const auto& first_refused =
            type == net::MessageType::Input ? expected.front() : expected[4];
        for (std::size_t index = 0; index < 3; ++index)
            EXPECT_EQ(transport.State().refused_frames[index], first_refused);
        EXPECT_EQ(transport.State().refused_frames[3], expected.back());
        EXPECT_EQ(transport.State().refused_frames[4], expected.back());
    }
}

TEST(LockstepSessionTest, PendingInputRetriesWhileWaitingForRemoteInput) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    transport.State().start_refusing_at = net::MessageType::Input;
    transport.State().refusals_remaining = 3;
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
    transport.State().start_refusing_at = net::MessageType::Input;
    transport.State().refusals_remaining = 2;
    const auto config = MakeConfig(0, 1);
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, &transport, MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    transport.State().peer_disconnected = true;
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_TRUE(transport.State().closed);
    EXPECT_TRUE(session.Status().peer_disconnected);
    EXPECT_EQ(world.tick, 0u);
}

TEST(LockstepSessionTest, PendingOutboundFrameLimitDisconnectsConnectedPeer) {
    auto [local, peer] = net::MakeLoopbackPair();
    BackpressuredTransport transport(*local);
    transport.State().start_refusing_at = net::MessageType::Input;
    transport.State().refusals_remaining = 1024;
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
    EXPECT_TRUE(transport.State().closed);
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
        transport.State().rejected = type;
        const auto config = MakeConfig(0, 1);
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        EXPECT_FALSE(session.Handshake());
        EXPECT_TRUE(transport.State().closed);
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
        transport.State().drain_before_disconnect = true;
        auto config = MakeConfig(0, 1);
        config.hash_cadence_ticks = 1;
        FakeWorld world{0, "agree", 0};
        net::LockstepSession session(config, &transport, MakeHooks(&world));
        ASSERT_TRUE(SendPeerHello(*peer, config));
        ASSERT_TRUE(session.Handshake());
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {}})));
        transport.State().rejected = type;
        for (int pump = 0; pump < 3; ++pump) {
            const auto result = session.PumpTick(10);
            EXPECT_EQ(result.outcome, net::TickOutcome::PeerDisconnected);
            EXPECT_EQ(result.tick, 1u);
        }
        EXPECT_TRUE(transport.State().closed);
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
        transport.State().rejected = type;

        const auto result = session.PumpTick(10);
        EXPECT_EQ(result.outcome, net::TickOutcome::Desync);
        EXPECT_EQ(result.tick, 1u);
        EXPECT_EQ(result.local_inputs, (std::vector<std::uint8_t>{0xAB}));
        EXPECT_EQ(world.tick, 1u);
        EXPECT_TRUE(transport.State().closed);
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
        EXPECT_EQ(contents->checkpoints.front().terrain, HashAt(world, 1, "terrain"));
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
            transport.State().rejected = net::MessageType::Bye;
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
        EXPECT_TRUE(transport.State().closed);
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
        EXPECT_EQ(contents->checkpoints.front().terrain, HashAt(world, 1, "terrain"));
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
            transport.State().rejected = type;
            transport.State().on_rejection = [&] {
                EXPECT_TRUE(peer->SendFrame(net::EncodeHash(mismatch)));
                // A later matching hash must not overwrite an available mismatch.
                EXPECT_TRUE(peer->SendFrame(net::EncodeHash(matching)));
                // TCP can report send-side disconnect with complete receive frames
                // still buffered. They must be compared before destructive Close.
                transport.State().peer_disconnected = true;
            };
            ASSERT_TRUE(peer->SendFrame(net::EncodeInput({2, 1, {}})));

            const auto result = session.PumpTick(10);
            EXPECT_EQ(result.outcome, net::TickOutcome::Desync);
            EXPECT_EQ(result.tick, mismatch_tick);
            EXPECT_TRUE(transport.State().closed);
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
    transport.State().rejected = net::MessageType::Input;
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
    EXPECT_TRUE(transport.State().closed);
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
    transport.State().rejected = net::MessageType::Bye;
    session.Disconnect();
    session.Disconnect();
    EXPECT_TRUE(transport.State().closed);
    EXPECT_EQ(session.PumpTick(10).outcome, net::TickOutcome::PeerDisconnected);
}

TEST(LockstepSessionTest, LargeHandshakeInputsStayConnectedAndArriveInOrder) {
    const auto exercise =
        [](net::ILockstepTransport& ta, net::ILockstepTransport& tb, net::NetworkSim* sim) {
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
                return std::vector<std::uint8_t>(std::size_t{128} * 1024,
                                                 static_cast<std::uint8_t>(tick));
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
                const std::vector<std::uint8_t> expected(std::size_t{256} * 1024,
                                                         static_cast<std::uint8_t>(tick));
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

TEST(LockstepSessionTest, DefaultHorizonsDeliverTwentyFourMiBFramesWithoutLoss) {
    auto [ta, tb] = net::MakeLoopbackPair();
    auto ca = MakeConfig(0, 1);
    auto cb = MakeConfig(1, 0);
    auto hooks = net::LockstepHooks{};
    hooks.collect_local_input = [](std::uint64_t tick, void*) {
        return std::vector<std::uint8_t>(std::size_t{24} * 1024 * 1024,
                                         static_cast<std::uint8_t>(tick));
    };
    net::LockstepSession a(ca, ta.get(), hooks);
    net::LockstepSession b(cb, tb.get(), hooks);
    ASSERT_TRUE(SendPeerHello(*tb, ca));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        const auto ra = a.PumpTick(4);
        const auto rb = b.PumpTick(4);
        ASSERT_EQ(ra.outcome, net::TickOutcome::Advanced);
        ASSERT_EQ(rb.outcome, ra.outcome);
        ASSERT_EQ(ra.local_inputs.size(), 48u * 1024u * 1024u);
        EXPECT_EQ(ra.local_inputs, rb.local_inputs);
        EXPECT_EQ(ra.local_inputs.front(), tick);
        EXPECT_EQ(ra.local_inputs.back(), tick);
        EXPECT_LE(a.AdmissionStatus().applied_input_bytes, net::kMaxFramePayloadBytes);
        EXPECT_LE(b.AdmissionStatus().applied_input_bytes, net::kMaxFramePayloadBytes);
    }
    // Each large frame consumes a receive batch, including pre-scheduled inputs
    // beyond the tick budget. Finish only after those frames and an empty poll.
    for (std::uint32_t remaining = 0; remaining < ca.horizon_start; ++remaining) {
        ASSERT_EQ(a.PumpTick(4).outcome, net::TickOutcome::WaitingForPeer);
        ASSERT_EQ(b.PumpTick(4).outcome, net::TickOutcome::WaitingForPeer);
    }
    EXPECT_EQ(a.PumpTick(4).outcome, net::TickOutcome::Finished);
    EXPECT_EQ(b.PumpTick(4).outcome, net::TickOutcome::Finished);
    EXPECT_EQ(a.AgreedTick(), 4u);
    EXPECT_EQ(b.AgreedTick(), 4u);
    EXPECT_EQ(a.Status().rejected_peer_messages, 0u);
    EXPECT_EQ(b.Status().rejected_peer_messages, 0u);
}

TEST(LockstepSessionTest, ThousandAndTwentyFiveTickHorizonCompletesWithoutLoss) {
    auto [ta, tb] = net::MakeLoopbackPair();
    auto ca = MakeConfig(0, 1);
    auto cb = MakeConfig(1, 0);
    ca.horizon_start = ca.horizon_min = ca.horizon_max = 1025;
    cb.horizon_start = cb.horizon_min = cb.horizon_max = 1025;
    net::LockstepSession a(ca, ta.get(), {});
    net::LockstepSession b(cb, tb.get(), {});
    ASSERT_TRUE(SendPeerHello(*tb, ca));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());
    for (std::uint64_t tick = 1; tick <= 1026; ++tick) {
        const auto ra = a.PumpTick(1026);
        const auto rb = b.PumpTick(1026);
        ASSERT_EQ(ra.outcome,
                  tick == 1026 ? net::TickOutcome::Finished : net::TickOutcome::Advanced);
        ASSERT_EQ(rb.outcome, ra.outcome);
        EXPECT_LE(a.AdmissionStatus().peer_inputs, 2050u);
        EXPECT_LE(b.AdmissionStatus().peer_inputs, 2050u);
    }
    EXPECT_EQ(a.AgreedTick(), 1026u);
    EXPECT_EQ(b.AgreedTick(), 1026u);
    EXPECT_EQ(a.Status().rejected_peer_messages, 0u);
    EXPECT_EQ(b.Status().rejected_peer_messages, 0u);
}

TEST(LockstepSessionTest, DelayedFirstCheckpointAlwaysFitsAfterThousandLaterCheckpoints) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    for (std::uint64_t tick = 1; tick <= 1025; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        if (tick > 1) {
            net::HashMsg hash;
            CaptureHashes(tick, hash, &world);
            ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        }
        ASSERT_EQ(session.PumpTick(1026).outcome, net::TickOutcome::Advanced);
    }
    ASSERT_EQ(session.AdmissionStatus().peer_hashes, 1024u);
    ASSERT_EQ(session.AdmissionStatus().confirmed_hash_tick, 0u);
    net::HashMsg first;
    CaptureHashes(1, first, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(first)));
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1026, 1, {}})));
    ASSERT_EQ(session.PumpTick(1026).outcome, net::TickOutcome::Finished);
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, 1025u);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.Status().rejected_peer_messages, 0u);
}

TEST(LockstepSessionTest, ConfirmationWindowPausesWithoutLosingTheNextInputAndResumes) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    const auto window = session.AdmissionStatus().checkpoint_window;
    for (std::uint64_t tick = 1; tick <= window; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        if (tick != 1) {
            net::HashMsg hash;
            CaptureHashes(tick, hash, &world);
            ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        }
        ASSERT_EQ(session.PumpTick(window + 1).outcome, net::TickOutcome::Advanced);
    }
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({window + 1, 1, {0xAB}})));
    for (int pump = 0; pump < 3; ++pump) {
        ASSERT_EQ(session.PumpTick(window + 1).outcome, net::TickOutcome::WaitingForPeer);
        EXPECT_EQ(session.AgreedTick(), window);
        EXPECT_EQ(session.AdmissionStatus().peer_inputs, 1u);
        EXPECT_EQ(session.AdmissionStatus().local_hashes, window);
        EXPECT_EQ(session.AdmissionStatus().peer_hashes, window - 1);
        EXPECT_EQ(session.AdmissionStatus().retained_bytes, (window - 1) * 29 + 22);
    }
    net::HashMsg first;
    CaptureHashes(1, first, &world);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(first)));
    const auto resumed = session.PumpTick(window + 1);
    EXPECT_EQ(resumed.outcome, net::TickOutcome::Finished);
    EXPECT_EQ(resumed.local_inputs, (std::vector<std::uint8_t>{0xAB}));
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, window);
    EXPECT_EQ(session.AdmissionStatus().peer_hashes, 0u);
    EXPECT_EQ(session.Status().rejected_peer_messages, 0u);
}

TEST(LockstepSessionTest, NegotiatedHelloRoundtripsAllAdmissionSettings) {
    auto config = MakeConfig(1, 0);
    config.horizon_start = 7;
    config.horizon_min = 4;
    config.horizon_max = 21;
    config.hash_cadence_ticks = 17;
    config.peer_retained_byte_limit = 123456;
    config.peer_retained_message_limit = 789;
    const auto hello = net::MakeLockstepHello(config, 1024);
    net::HelloMsg decoded;
    ASSERT_TRUE(net::DecodeHello(net::EncodeHello(hello), decoded));
    EXPECT_EQ(decoded.horizon_start, config.horizon_start);
    EXPECT_EQ(decoded.horizon_min, config.horizon_min);
    EXPECT_EQ(decoded.horizon_max, config.horizon_max);
    EXPECT_EQ(decoded.hash_cadence_ticks, config.hash_cadence_ticks);
    EXPECT_EQ(decoded.max_frame_bytes, 1024u);
    EXPECT_EQ(decoded.retained_byte_limit, config.peer_retained_byte_limit);
    EXPECT_EQ(decoded.retained_message_limit, config.peer_retained_message_limit);
    EXPECT_EQ(decoded.system_config_identity, config.system_config_identity);
}

TEST(LockstepSessionTest, IncompatibleHorizonsAndLimitsFailAtHandshake) {
    const auto config = MakeConfig(0, 1);
    std::array<net::LockstepConfig, 6> remotes;
    remotes.fill(config);
    remotes[0].horizon_max = 21;
    remotes[1].horizon_start = 4;
    remotes[2].horizon_min = 2;
    remotes[3].hash_cadence_ticks = 1;
    remotes[4].peer_retained_byte_limit = 1;
    remotes[5].peer_retained_message_limit = 1;
    for (const auto& remote : remotes) {
        auto [local, peer] = net::MakeLoopbackPair();
        net::LockstepSession session(config, local.get(), {});
        ASSERT_TRUE(SendPeerHello(*peer, remote));
        EXPECT_FALSE(session.Handshake());
        EXPECT_EQ(session.Status().handshake_error,
                  "incompatible lockstep horizons, cadence or retention limits");
        EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
    }
}

namespace {

struct HorizonSettings {
    std::uint32_t minimum;
    std::uint32_t start;
    std::uint32_t maximum;
};

constexpr HorizonSettings kInvalidHorizons[]{
    {.minimum = 0, .start = 3, .maximum = 10},
    {.minimum = 1, .start = 0, .maximum = 10},
    {.minimum = 1, .start = 3, .maximum = 0},
    {.minimum = 4, .start = 3, .maximum = 10},
    {.minimum = 3, .start = 11, .maximum = 10},
    {.minimum = 11, .start = 11, .maximum = 10},
};

void SetHorizons(net::LockstepConfig& config, HorizonSettings horizons) {
    config.horizon_min = horizons.minimum;
    config.horizon_start = horizons.start;
    config.horizon_max = horizons.maximum;
}

void ExpectHorizonRefusal(const net::LockstepConfig& config,
                          const net::LockstepConfig& remote,
                          const char* expected_error) {
    auto [local, peer] = net::MakeLoopbackPair();
    struct Calls {
        std::size_t collected = 0;
        std::size_t stepped = 0;
    } calls;
    net::LockstepHooks hooks;
    hooks.user = &calls;
    hooks.collect_local_input = [](std::uint64_t, void* user) {
        ++static_cast<Calls*>(user)->collected;
        return std::vector<std::uint8_t>{};
    };
    hooks.apply_and_step = [](std::uint64_t, const std::vector<std::uint8_t>&, void* user) {
        ++static_cast<Calls*>(user)->stepped;
        return true;
    };
    net::LockstepSession session(config, local.get(), hooks);
    ASSERT_TRUE(SendPeerHello(*peer, remote));
    EXPECT_FALSE(session.Handshake());
    EXPECT_FALSE(session.Status().handshaken);
    EXPECT_EQ(session.Status().handshake_error, expected_error);
    EXPECT_EQ(session.AgreedTick(), 0u);
    EXPECT_EQ(session.AdmissionStatus().input_ticks, 0u);
    EXPECT_EQ(calls.collected, 0u);
    EXPECT_EQ(calls.stepped, 0u);
    std::vector<std::uint8_t> frame;
    while (peer->TryReceiveFrame(frame)) {
        net::MessageType type{};
        ASSERT_TRUE(net::PeekMessageType(frame, type));
        EXPECT_EQ(type, net::MessageType::Hello);
    }
}

} // namespace

TEST(LockstepSessionTest, InvalidLocalHorizonsAreRefusedEvenWhenPeerMatches) {
    for (const auto horizons : kInvalidHorizons) {
        auto config = MakeConfig(0, 1);
        SetHorizons(config, horizons);
        ExpectHorizonRefusal(
            config, config, "invalid local lockstep horizons: require 0 < min <= start <= max");
    }
}

TEST(LockstepSessionTest, InvalidPeerHorizonsAreRefusedBeforeInputScheduling) {
    const auto config = MakeConfig(0, 1);
    for (const auto horizons : kInvalidHorizons) {
        auto remote = config;
        SetHorizons(remote, horizons);
        ExpectHorizonRefusal(
            config, remote, "invalid peer lockstep horizons: require 0 < min <= start <= max");
    }
}

TEST(LockstepSessionTest, NegotiatesSmallerFramesAndRejectsOversizedTrafficBeforeRecovery) {
    auto [ta, tb] = net::MakeLoopbackPair();
    auto ca = MakeConfig(0, 1);
    auto cb = MakeConfig(1, 0);
    ca.max_frame_payload_bytes = 128;
    cb.max_frame_payload_bytes = 1024;
    ca.hash_cadence_ticks = cb.hash_cadence_ticks = 1;
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", 0};
    net::LockstepSession a(ca, ta.get(), MakeHooks(&wa));
    net::LockstepSession b(cb, tb.get(), MakeHooks(&wb));
    ASSERT_TRUE(tb->SendFrame(net::EncodeHello(net::MakeLockstepHello(cb))));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());
    const auto bounds = a.AdmissionStatus();
    EXPECT_EQ(bounds.frame_limit, 128u);
    EXPECT_EQ(b.AdmissionStatus().frame_limit, 128u);
    EXPECT_EQ(bounds.byte_limit, bounds.message_limit * 128);
    EXPECT_EQ(b.AdmissionStatus().byte_limit, bounds.byte_limit);
    ASSERT_TRUE(tb->SendFrame(net::EncodeInput({1, 1, std::vector<std::uint8_t>(129 - 21)})));
    net::HashMsg oversized;
    oversized.tick = 1;
    oversized.world_hash.assign(129 - 29, 'h');
    ASSERT_TRUE(tb->SendFrame(net::EncodeHash(oversized)));
    for (std::uint64_t tick = 1; tick <= 4; ++tick) {
        const auto ra = a.PumpTick(4);
        const auto rb = b.PumpTick(4);
        ASSERT_EQ(ra.outcome, tick == 4 ? net::TickOutcome::Finished : net::TickOutcome::Advanced);
        ASSERT_EQ(rb.outcome, ra.outcome);
    }
    EXPECT_EQ(a.Status().rejected_peer_messages, 2u);
    EXPECT_EQ(b.Status().rejected_peer_messages, 0u);
    EXPECT_FALSE(a.Status().desynced);
    EXPECT_FALSE(b.Status().desynced);
}

TEST(LockstepSessionTest, HundredThousandTicksBoundAllHistoryAndStreamEveryAppliedTick) {
    auto [ta, tb] = net::MakeLoopbackPair();
    auto ca = MakeConfig(0, 1);
    auto cb = MakeConfig(1, 0);
    ca.hash_cadence_ticks = cb.hash_cadence_ticks = 1;
    struct Counter {
        std::uint64_t records = 0;
        std::uint64_t checkpoints = 0;
        std::uint64_t digest = 0;
    } a_counter, b_counter;
    const auto hooks_for = [](Counter& counter) {
        net::LockstepHooks hooks;
        hooks.collect_local_input = [](std::uint64_t tick, void*) {
            return std::vector<std::uint8_t>{static_cast<std::uint8_t>(tick)};
        };
        hooks.record_replay_tick = [](std::uint64_t tick,
                                      const std::vector<std::uint8_t>& merged,
                                      const net::HashMsg* hash,
                                      void* user) {
            auto& output = *static_cast<Counter*>(user);
            ++output.records;
            output.checkpoints += hash != nullptr;
            output.digest = output.digest * 31 + tick;
            for (const auto byte : merged)
                output.digest = output.digest * 31 + byte;
        };
        hooks.user = &counter;
        return hooks;
    };
    net::LockstepSession a(ca, ta.get(), hooks_for(a_counter));
    net::LockstepSession b(cb, tb.get(), hooks_for(b_counter));
    ASSERT_TRUE(SendPeerHello(*tb, ca));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());
    constexpr std::uint64_t ticks = 100000;
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        ASSERT_EQ(a.PumpTick(ticks).outcome,
                  tick == ticks ? net::TickOutcome::Finished : net::TickOutcome::Advanced);
        ASSERT_EQ(b.PumpTick(ticks).outcome,
                  tick == ticks ? net::TickOutcome::Finished : net::TickOutcome::Advanced);
        // Explicit persistence boundaries keep this accelerated simulation test
        // independent of how the runner schedules recorder workers.
        if (tick % 256 == 0) {
            ASSERT_TRUE(a.FlushReplayRecorder());
            ASSERT_TRUE(b.FlushReplayRecorder());
        }
        for (const auto& retained : {a.AdmissionStatus(), b.AdmissionStatus()}) {
            ASSERT_LE(retained.applied_inputs, net::kReceiveQueueFrameLimit);
            ASSERT_LE(retained.applied_input_bytes, net::kMaxFramePayloadBytes);
            ASSERT_LE(retained.local_hashes, net::kReceiveQueueFrameLimit + 1);
            ASSERT_LE(retained.peer_inputs + retained.peer_hashes, retained.message_limit);
            ASSERT_LE(retained.retained_bytes, retained.byte_limit);
        }
    }
    EXPECT_EQ(a_counter.records, ticks);
    EXPECT_EQ(b_counter.records, ticks);
    EXPECT_EQ(a_counter.checkpoints, ticks);
    EXPECT_EQ(b_counter.checkpoints, ticks);
    // Whole-session identity belongs to the streaming consumer, which receives
    // identical bytes on both peers without retaining the history in memory.
    EXPECT_EQ(a_counter.digest, b_counter.digest);
    EXPECT_EQ(a.Status().rejected_peer_messages, 0u);
    EXPECT_EQ(b.Status().rejected_peer_messages, 0u);
}

TEST(LockstepSessionTest, MatchedOutOfOrderHashReleasesItsStringAllocation) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    net::LockstepHooks hooks;
    hooks.capture_hashes = [](std::uint64_t, net::HashMsg& hash, void*) {
        hash.world_hash.assign(std::size_t{1024} * 1024, 'h');
    };
    net::LockstepSession session(config, local.get(), hooks);
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    // Checkpoint 1 is deliberately missing, keeping checkpoint 2's marker alive.
    for (std::uint64_t tick = 1; tick <= 2; ++tick) {
        ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
        ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
    }
    net::HashMsg hash;
    hash.tick = 2;
    hash.world_hash.assign(std::size_t{1024} * 1024, 'h');
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
    ASSERT_EQ(session.AdmissionStatus().peer_hashes, 1u);
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, 0u);
    const auto capacity = session.PeerHashStringCapacity(2);
    if (!capacity.has_value()) {
        FAIL() << "out-of-order confirmation marker was lost";
    }
    const net::HashMsg empty;
    EXPECT_EQ(*capacity,
              empty.world_hash.capacity() + empty.terrain.capacity() + empty.water.capacity() +
                  empty.entities.capacity());
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 29u); // encoded empty marker
    hash.tick = 1;
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::WaitingForPeer);
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, 2u);
    EXPECT_EQ(session.AdmissionStatus().retained_bytes, 0u);
    EXPECT_FALSE(session.PeerHashStringCapacity(2).has_value());
}

TEST(LockstepSessionTest, CompleteDumpPreservesBootHashWithOrWithoutCheckpoints) {
    for (const bool capture : {false, true}) {
        for (const std::uint64_t cadence : {0u, 2u, 30u}) {
            SCOPED_TRACE(capture);
            SCOPED_TRACE(cadence);
            auto [local, peer] = net::MakeLoopbackPair();
            auto config = MakeConfig(0, 1);
            config.hash_cadence_ticks = cadence;
            FakeWorld world{0, "agree", 0};
            auto hooks = MakeHooks(&world);
            if (!capture)
                hooks.capture_hashes = nullptr;
            hooks.apply_and_step =
                [](std::uint64_t tick, const std::vector<std::uint8_t>& merged, void* user) {
                    return tick < 3 && ApplyStep(tick, merged, user);
                };
            net::HashMsg boot;
            if (capture)
                CaptureHashes(0, boot, &world);
            const auto dump = LockstepTempDir() / "boot-hash.lrec1";
            net::LockstepSession session(config, local.get(), hooks);
            session.SetDumpPath(dump.string());
            ASSERT_TRUE(SendPeerHello(*peer, config));
            ASSERT_TRUE(session.Handshake());
            for (std::uint64_t tick = 1; tick <= 3; ++tick)
                ASSERT_TRUE(peer->SendFrame(net::EncodeInput({tick, 1, {}})));
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Advanced);
            ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
            ASSERT_EQ(session.Status().dump_path, dump.string());
            const auto parsed = replay::ReadReplay(dump.string());
            if (!parsed.has_value()) {
                FAIL() << "complete recording could not be read";
            }
            EXPECT_FALSE(parsed->truncated);
            EXPECT_EQ(parsed->elided_through_tick, 0u);
            EXPECT_EQ(parsed->header.start_world_hash, boot.world_hash);
            EXPECT_EQ(parsed->checkpoints.size(), cadence == 2 ? 1u : 0u);
            if (capture && cadence == 2) {
                ASSERT_FALSE(parsed->checkpoints.empty());
                net::HashMsg checkpoint;
                CaptureHashes(2, checkpoint, &world);
                EXPECT_EQ(parsed->checkpoints.front().world_hash, checkpoint.world_hash);
                EXPECT_NE(parsed->checkpoints.front().world_hash, boot.world_hash);
            }
            fs::remove(dump);
        }
    }
}

TEST(LockstepSessionTest, DumpDestinationCanBeSetAfterHandshakeBeforeFirstTick) {
    auto [local, peer] = net::MakeLoopbackPair();
    const auto config = MakeConfig(0, 1);
    net::LockstepHooks hooks;
    hooks.apply_and_step = [](std::uint64_t, const std::vector<std::uint8_t>&, void*) {
        return false;
    };
    net::LockstepSession session(config, local.get(), hooks);
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    const auto directory = LockstepTempDir() / "dump-after-handshake";
    const auto dump = directory / "desync.lrec1";
    session.SetDumpPath(dump.string());
    ASSERT_TRUE(peer->SendFrame(net::EncodeInput({1, 1, {0xab}})));
    ASSERT_EQ(session.PumpTick(10).outcome, net::TickOutcome::Desync);
    ASSERT_EQ(session.Status().dump_path, dump.string());
    const auto parsed = replay::ReadReplay(dump.string());
    if (!parsed.has_value()) {
        FAIL() << "destination set after handshake lost its artifact";
    }
    const auto& contents = *parsed;
    EXPECT_FALSE(contents.truncated);
    ASSERT_EQ(contents.inputs.size(), 1u);
    EXPECT_EQ(contents.inputs.front().inputs, (std::vector<std::uint8_t>{0xab}));
    fs::remove(dump);
    EXPECT_TRUE(fs::is_empty(directory));
    fs::remove(directory);
}

namespace {

void VerifyLateDesyncArtifact(const net::LockstepSession& session,
                              const FakeWorld& world,
                              const fs::path& path,
                              const net::LockstepConfig& config,
                              std::uint64_t divergence) {
    const auto status = session.Status();
    EXPECT_TRUE(status.desynced);
    EXPECT_EQ(status.desync_tick, divergence);
    EXPECT_EQ(status.desync_section, "terrain");
    ASSERT_EQ(status.dump_path, path.string());
    ASSERT_TRUE(fs::exists(path)); // server's required artifact contract
    const auto parsed = replay::ReadReplay(status.dump_path);
    if (!parsed.has_value()) {
        FAIL() << "late desync artifact could not be read";
    }
    const auto& contents = *parsed;
    EXPECT_FALSE(contents.truncated);
    EXPECT_TRUE(contents.trailer_present);
    EXPECT_EQ(contents.tick_count, divergence);
    EXPECT_EQ(contents.header.seed, config.seed);
    EXPECT_EQ(contents.header.preset, config.preset);
    EXPECT_EQ(contents.header.system_config_identity, config.system_config_identity);
    EXPECT_EQ(contents.header.preset_content_hash, config.preset_content_hash);
    ASSERT_FALSE(contents.inputs.empty());
    EXPECT_EQ(contents.inputs.front().tick, contents.elided_through_tick + 1);
    EXPECT_EQ(contents.inputs.back().tick, divergence);
    for (const auto& input : contents.inputs) {
        const auto byte = static_cast<std::uint8_t>(input.tick);
        EXPECT_EQ(input.inputs, (std::vector<std::uint8_t>{byte, byte}));
    }
    ASSERT_FALSE(contents.checkpoints.empty());
    EXPECT_EQ(contents.checkpoints.back().tick, divergence);
    for (const auto& checkpoint : contents.checkpoints) {
        EXPECT_EQ(checkpoint.terrain, HashAt(world, checkpoint.tick, "terrain"));
        EXPECT_EQ(checkpoint.water, HashAt(world, checkpoint.tick, "water"));
        EXPECT_EQ(checkpoint.entities, HashAt(world, checkpoint.tick, "entities"));
        EXPECT_EQ(checkpoint.world_hash,
                  replay::Fnv1a64Hex(checkpoint.terrain + checkpoint.water + checkpoint.entities));
    }
    if (divergence == 1050) {
        EXPECT_EQ(contents.elided_through_tick, 0u);
    } else {
        EXPECT_GT(contents.elided_through_tick, 0u);
        EXPECT_NE(contents.elided_prefix_digest, 0u);
        EXPECT_LE(contents.inputs.size(), 2 * replay::BoundedReplayRecorder::kSegmentTicks);
    }
    EXPECT_EQ(session.AdmissionStatus().replay_bytes, 0u);
}

void CheckAutomaticReplayAfterLateDesync(std::uint64_t divergence) {
    SCOPED_TRACE(divergence);
    auto [ta, tb] = net::MakeLoopbackPair();
    FakeWorld wa{0, "agree", 0}, wb{0, "agree", divergence};
    auto ca = MakeConfig(0, 1);
    auto cb = MakeConfig(1, 0);
    ca.hash_cadence_ticks = cb.hash_cadence_ticks = 30;
    auto ha = MakeHooks(&wa);
    auto hb = MakeHooks(&wb);
    ha.collect_local_input = hb.collect_local_input = [](std::uint64_t tick, void*) {
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(tick)};
    };
    const auto dump = LockstepTempDir() / ("late-desync-" + std::to_string(divergence) + ".lrec1");
    const auto peer_dump = fs::path(dump.string() + ".peer");
    std::error_code ec;
    fs::remove(dump, ec);
    fs::remove(peer_dump, ec);
    net::LockstepSession a(ca, ta.get(), ha);
    net::LockstepSession b(cb, tb.get(), hb);
    a.SetDumpPath(dump.string());
    b.SetDumpPath(peer_dump.string());
    // No record_replay_tick hook: this is exactly how both drivers construct sessions.
    ASSERT_TRUE(SendPeerHello(*tb, ca));
    ASSERT_TRUE(a.Handshake());
    ASSERT_TRUE(b.Handshake());
    for (std::uint64_t tick = 1; tick <= divergence + 1; ++tick) {
        const auto ra = a.PumpTick(divergence + 30);
        const auto rb = b.PumpTick(divergence + 30);
        if (tick <= divergence) {
            ASSERT_EQ(ra.outcome, net::TickOutcome::Advanced);
        } else {
            ASSERT_EQ(ra.outcome, net::TickOutcome::Desync);
        }
        if (tick < divergence) {
            ASSERT_EQ(rb.outcome, net::TickOutcome::Advanced);
        } else
            ASSERT_EQ(rb.outcome, net::TickOutcome::Desync);
        if (tick % 256 == 0 && tick < divergence) {
            ASSERT_TRUE(a.FlushReplayRecorder());
            ASSERT_TRUE(b.FlushReplayRecorder());
        }
    }
    VerifyLateDesyncArtifact(a, wa, dump, ca, divergence);
    VerifyLateDesyncArtifact(b, wb, peer_dump, ca, divergence);
    fs::remove(dump, ec);
    fs::remove(peer_dump, ec);
}

} // namespace

TEST(LockstepSessionTest, AutomaticReplaySurvivesDesyncJustPastDiagnosticPruning) {
    CheckAutomaticReplayAfterLateDesync(1050);
}

TEST(LockstepSessionTest, AutomaticReplaySurvivesLateDesyncWithElidedPrefix) {
    CheckAutomaticReplayAfterLateDesync(100050);
}

TEST(LockstepSessionTest, AutomaticReplayPinsInputsForDelayedDivergencePastDiagnosticPruning) {
    auto [local, peer] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.hash_cadence_ticks = 1;
    FakeWorld world{0, "agree", 0};
    net::LockstepSession session(config, local.get(), MakeHooks(&world));
    const auto dump = LockstepTempDir() / "delayed-oldest-desync.lrec1";
    session.SetDumpPath(dump.string());
    ASSERT_TRUE(SendPeerHello(*peer, config));
    ASSERT_TRUE(session.Handshake());
    std::vector<std::uint8_t> frame;
    constexpr std::uint64_t ticks = 1025;
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        ASSERT_TRUE(
            peer->SendFrame(net::EncodeInput({tick, 1, {static_cast<std::uint8_t>(tick)}})));
        if (tick > 1) {
            net::HashMsg hash;
            CaptureHashes(tick, hash, &world);
            ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
        }
        ASSERT_EQ(session.PumpTick(ticks + 1).outcome, net::TickOutcome::Advanced);
        while (peer->TryReceiveFrame(frame)) {}
    }
    EXPECT_EQ(session.AdmissionStatus().confirmed_hash_tick, 0u);
    EXPECT_EQ(session.AdmissionStatus().applied_inputs, 1024u);
    FakeWorld mismatch{0, "different", 0};
    net::HashMsg hash;
    CaptureHashes(1, hash, &mismatch);
    ASSERT_TRUE(peer->SendFrame(net::EncodeHash(hash)));
    ASSERT_EQ(session.PumpTick(ticks + 1).outcome, net::TickOutcome::Desync);
    ASSERT_EQ(session.Status().dump_path, dump.string());
    const auto parsed = replay::ReadReplay(dump.string());
    if (!parsed.has_value()) {
        FAIL() << "delayed mismatch lost its replay";
    }
    const auto& contents = *parsed;
    EXPECT_FALSE(contents.truncated);
    EXPECT_EQ(contents.elided_through_tick, 0u);
    ASSERT_EQ(contents.inputs.size(), ticks);
    EXPECT_EQ(contents.inputs.front().tick, 1u);
    EXPECT_EQ(contents.inputs.front().inputs, (std::vector<std::uint8_t>{1}));
    ASSERT_EQ(contents.checkpoints.size(), 1u);
    EXPECT_EQ(contents.checkpoints.front().tick, 1u);
    std::error_code ec;
    fs::remove(dump, ec);
}

TEST(LockstepSessionTest, CleanSessionsBoundDiskGrowthAndRemoveTheirWorkingSegments) {
    const auto directory = LockstepTempDir() / "clean-recording";
    ASSERT_TRUE(fs::create_directory(directory) || fs::is_empty(directory));
    const auto dump = directory / "clean.lrec1";
    {
        auto [ta, tb] = net::MakeLoopbackPair();
        auto ca = MakeConfig(0, 1);
        auto cb = MakeConfig(1, 0);
        ca.hash_cadence_ticks = cb.hash_cadence_ticks = 1;
        net::LockstepSession a(ca, ta.get(), {});
        net::LockstepSession b(cb, tb.get(), {});
        a.SetDumpPath(dump.string());
        b.SetDumpPath(dump.string() + ".peer");
        ASSERT_TRUE(SendPeerHello(*tb, ca));
        ASSERT_TRUE(a.Handshake());
        ASSERT_TRUE(b.Handshake());
        constexpr std::uint64_t ticks = 8192;
        for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
            const auto expected =
                tick == ticks ? net::TickOutcome::Finished : net::TickOutcome::Advanced;
            ASSERT_EQ(a.PumpTick(ticks).outcome, expected);
            ASSERT_EQ(b.PumpTick(ticks).outcome, expected);
            if (tick % 256 == 0) {
                ASSERT_TRUE(a.FlushReplayRecorder());
                ASSERT_TRUE(b.FlushReplayRecorder());
            }
            for (const auto& retained : {a.AdmissionStatus(), b.AdmissionStatus()}) {
                ASSERT_LE(retained.replay_segments, 2u);
                ASSERT_LE(retained.replay_bytes,
                          2 * replay::BoundedReplayRecorder::kSegmentTicks * (17 + 29));
                ASSERT_LE(retained.replay_bytes, retained.replay_byte_limit);
            }
        }
        ASSERT_TRUE(a.FlushReplayRecorder());
        ASSERT_TRUE(b.FlushReplayRecorder());
        std::uintmax_t actual_bytes = 0;
        std::size_t actual_segments = 0;
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                actual_bytes += entry.file_size();
                ++actual_segments;
            }
        }
        EXPECT_LE(actual_segments, 4u);
        EXPECT_LE(actual_bytes,
                  a.AdmissionStatus().replay_bytes + b.AdmissionStatus().replay_bytes);
        EXPECT_FALSE(fs::exists(dump));
        EXPECT_FALSE(fs::exists(dump.string() + ".peer"));
        EXPECT_TRUE(a.Status().dump_path.empty());
        EXPECT_TRUE(b.Status().dump_path.empty());
    }
    EXPECT_TRUE(fs::is_empty(directory));
    fs::remove(directory);
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
    // Version 2 ended after system_config_identity, without a preset hash field
    // or the version 4 admission fields (three u32s and four u64s).
    frame.resize(frame.size() - (3 * 4 + 4 * 8) - 4);
    const auto size = static_cast<std::uint32_t>(frame.size() - 5);
    for (unsigned i = 0; i < 4; ++i)
        frame[1u + i] = static_cast<std::uint8_t>((size >> (8u * i)) & 0xffu);
    net::HelloMsg decoded;
    EXPECT_FALSE(net::DecodeHello(frame, decoded));
}

TEST(LockstepSessionTest, LegacyVersionThreeHelloIsRefusedBeforeInputScheduling) {
    // Genuine v3 wire layout: all three strings, ending at preset_content_hash.
    // This fixture is independent of the v4 encoder and has no admission fields.
    const std::vector<std::uint8_t> frame{
        0x01, 42,  0,   0,   0,                // Hello tag and exact payload length
        'L',  'S', 'T', 'P', '1',              // magic
        3,    0,                               // protocol version
        42,   0,   0,   0,   0,   0,   0,   0, // seed
        30,   0,   1,   0,   0,   0,           // tick rate and client id
        7,    0,   0,   0,   'd', 'e', 'f', 'a', 'u', 'l', 't', // preset
        1,    0,   0,   0,   'i',                               // system_config_identity
        1,    0,   0,   0,   'p'                                // preset_content_hash
    };
    ASSERT_EQ(frame.size(), 47u);
    net::HelloMsg decoded;
    EXPECT_FALSE(net::DecodeHello(frame, decoded));
    auto [host_transport, peer_transport] = net::MakeLoopbackPair();
    auto config = MakeConfig(0, 1);
    config.seed = 42;
    config.system_config_identity = "i";
    config.preset_content_hash = "p";
    struct Calls {
        std::size_t collected = 0;
        std::size_t stepped = 0;
    } calls;
    net::LockstepHooks hooks;
    hooks.user = &calls;
    hooks.collect_local_input = [](std::uint64_t, void* user) {
        ++static_cast<Calls*>(user)->collected;
        return std::vector<std::uint8_t>{};
    };
    hooks.apply_and_step = [](std::uint64_t, const std::vector<std::uint8_t>&, void* user) {
        ++static_cast<Calls*>(user)->stepped;
        return true;
    };
    net::LockstepSession host(config, host_transport.get(), hooks);
    ASSERT_TRUE(peer_transport->SendFrame(frame));
    EXPECT_FALSE(host.Handshake());
    EXPECT_FALSE(host.Status().handshaken);
    EXPECT_EQ(host.AgreedTick(), 0u);
    EXPECT_EQ(calls.collected, 0u);
    EXPECT_EQ(calls.stepped, 0u);
    std::vector<std::uint8_t> outgoing;
    ASSERT_TRUE(peer_transport->TryReceiveFrame(outgoing));
    net::MessageType type{};
    ASSERT_TRUE(net::PeekMessageType(outgoing, type));
    EXPECT_EQ(type, net::MessageType::Hello);
    EXPECT_FALSE(peer_transport->TryReceiveFrame(outgoing));
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
    // Truncate at the identity fields, before version 4's admission settings.
    auto frame = net::EncodeHello(peer);
    ASSERT_GE(frame.size(), 5u + (3 * 4 + 4 * 8) + 8u);
    frame.resize(frame.size() - (3 * 4 + 4 * 8) - 8);
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

    const auto r = DriveBoth(a, b, 90);
    EXPECT_TRUE(r.a_outcome == net::TickOutcome::Desync || r.b_outcome == net::TickOutcome::Desync);
    // At least one side must report the desync (the one that holds both hashes at tick 30).
    const bool a_desync = a.Status().desynced;
    const bool b_desync = b.Status().desynced;
    EXPECT_TRUE(a_desync || b_desync) << "oracle is vacuous: a real divergence was not caught";

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
