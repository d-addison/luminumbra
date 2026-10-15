#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "luminumbra_common/net/MessageReceiveQueue.h"
#include "luminumbra_common/net/NetworkSim.h"

namespace {

using namespace Luminumbra::Net;

TEST(MessageReceiveQueue, CountsOnlyQueuedPayloadBytes) {
    MessageReceiveQueue queue;
    EXPECT_EQ(queue.QueuedBytes(), 0u);
    EXPECT_TRUE(queue.CanReceive());
    const std::vector<std::uint8_t> first{1, 2, 3};
    const std::vector<std::uint8_t> last{4, 5};
    ASSERT_TRUE(queue.Push(first.data(), first.size()));
    ASSERT_TRUE(queue.Push(nullptr, 0));
    ASSERT_TRUE(queue.Push(last.data(), last.size()));
    EXPECT_EQ(queue.QueuedBytes(), first.size() + last.size());
    queue.Close();
    EXPECT_EQ(queue.QueuedBytes(), first.size() + last.size());
    EXPECT_FALSE(queue.CanReceive());
    EXPECT_FALSE(queue.Push(last.data(), last.size()));
    std::vector<std::uint8_t> received(100, 0xAB);
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_EQ(received, first);
    EXPECT_EQ(queue.QueuedBytes(), last.size());
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(queue.QueuedBytes(), last.size());
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_EQ(received, last);
    EXPECT_EQ(queue.QueuedBytes(), 0u);
    EXPECT_FALSE(queue.IsDrained());
    EXPECT_FALSE(queue.TryReceiveFrame(received));
    EXPECT_EQ(queue.QueuedBytes(), 0u);
    EXPECT_TRUE(queue.IsDrained());
}

TEST(MessageReceiveQueue, StopsAtByteBudgetAndResumesAfterDelivery) {
    MessageReceiveQueue queue;
    const std::vector<std::uint8_t> frame(kReceiveQueueByteBudget, 0xAB);
    ASSERT_TRUE(queue.Push(frame.data(), frame.size()));
    EXPECT_EQ(queue.QueuedBytes(), kReceiveQueueByteBudget);
    EXPECT_FALSE(queue.CanReceive());
    // No payload may be inspected or copied while admission is paused.
    EXPECT_FALSE(queue.Push(nullptr, 1));
    EXPECT_FALSE(queue.Push(nullptr, 0));
    EXPECT_FALSE(queue.IsClosed());
    std::vector<std::uint8_t> received;
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_EQ(received, frame);
    EXPECT_EQ(queue.QueuedBytes(), 0u);
    EXPECT_TRUE(queue.CanReceive());
    ASSERT_TRUE(queue.Push(nullptr, 0));
    EXPECT_TRUE(queue.CanReceive());
    ASSERT_TRUE(queue.Push(frame.data(), frame.size()));
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(queue.QueuedBytes(), kReceiveQueueByteBudget);
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_EQ(received, frame);
    EXPECT_EQ(queue.QueuedBytes(), 0u);
}

TEST(MessageReceiveQueue, OneMaximumFrameMayCrossBudgetBeforeOversizedClosure) {
    MessageReceiveQueue queue;
    const std::vector<std::uint8_t> prefix(kReceiveQueueByteBudget - 1, 0xAB);
    const std::vector<std::uint8_t> maximum(kMaxFramePayloadBytes, 0xCD);
    ASSERT_TRUE(queue.Push(prefix.data(), prefix.size()));
    ASSERT_TRUE(queue.CanReceive());
    ASSERT_TRUE(queue.Push(maximum.data(), maximum.size()));
    EXPECT_EQ(queue.QueuedBytes(), kReceiveQueueByteBudget - 1 + kMaxFramePayloadBytes);
    EXPECT_FALSE(queue.CanReceive());
    EXPECT_FALSE(queue.Push(nullptr, 1));
    // Validate size before allocation/addition, even when already over budget.
    EXPECT_FALSE(queue.Push(nullptr, (std::numeric_limits<std::size_t>::max)()));
    EXPECT_TRUE(queue.IsClosed());
    EXPECT_FALSE(queue.IsDrained());
    EXPECT_EQ(queue.QueuedBytes(), prefix.size() + maximum.size());
    std::vector<std::uint8_t> received;
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_EQ(received, prefix);
    EXPECT_EQ(queue.QueuedBytes(), maximum.size());
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_EQ(received, maximum);
    EXPECT_EQ(queue.QueuedBytes(), 0u);
    EXPECT_FALSE(queue.IsDrained());
    EXPECT_FALSE(queue.TryReceiveFrame(received));
    EXPECT_TRUE(queue.IsDrained());
    EXPECT_FALSE(queue.CanReceive());
}

TEST(MessageReceiveQueue, EmptyFramesHaveBoundedAdmission) {
    MessageReceiveQueue queue;
    for (std::size_t i = 0; i < kReceiveQueueFrameLimit; ++i) {
        ASSERT_TRUE(queue.CanReceive());
        ASSERT_TRUE(queue.Push(nullptr, 0));
    }
    EXPECT_EQ(queue.QueuedBytes(), 0u);
    EXPECT_FALSE(queue.CanReceive());
    EXPECT_FALSE(queue.Push(nullptr, 0));
    EXPECT_FALSE(queue.IsClosed());
    std::vector<std::uint8_t> received{0xAB};
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_TRUE(received.empty());
    EXPECT_TRUE(queue.CanReceive());
    ASSERT_TRUE(queue.Push(nullptr, 0));
    queue.Close();
    for (std::size_t i = 0; i < kReceiveQueueFrameLimit; ++i) {
        ASSERT_TRUE(queue.TryReceiveFrame(received));
        EXPECT_TRUE(received.empty());
    }
    EXPECT_FALSE(queue.IsDrained());
    EXPECT_FALSE(queue.TryReceiveFrame(received));
    EXPECT_TRUE(queue.IsDrained());
    EXPECT_EQ(queue.QueuedBytes(), 0u);
}

TEST(MessageReceiveQueue, InProcessTransportsBoundBytesInBothDirections) {
    const auto exercise =
        [](ILockstepTransport& sender, ILockstepTransport& receiver, FrameDelivery delivery) {
            const std::vector<std::uint8_t> first(kReceiveQueueByteBudget - 1, 0xAB);
            const std::vector<std::uint8_t> second{1, 2};
            const std::vector<std::uint8_t> last{3};
            ASSERT_TRUE(sender.SendFrame(first, delivery));
            ASSERT_TRUE(sender.SendFrame(second, delivery));
            EXPECT_FALSE(sender.SendFrame(last, delivery));
            EXPECT_FALSE(sender.SendFrame({}, delivery));
            // The opposite direction has an independent budget.
            ASSERT_TRUE(receiver.SendFrame(last, delivery));
            std::vector<std::uint8_t> received;
            ASSERT_TRUE(sender.TryReceiveFrame(received));
            EXPECT_EQ(received, last);
            ASSERT_TRUE(receiver.TryReceiveFrame(received));
            EXPECT_EQ(received, first);
            ASSERT_TRUE(sender.SendFrame(last, delivery));
            sender.Close();
            EXPECT_FALSE(sender.SendFrame(last, delivery));
            EXPECT_TRUE(receiver.IsPeerConnected());
            ASSERT_TRUE(receiver.TryReceiveFrame(received));
            EXPECT_EQ(received, second);
            EXPECT_TRUE(receiver.IsPeerConnected());
            ASSERT_TRUE(receiver.TryReceiveFrame(received));
            EXPECT_EQ(received, last);
            EXPECT_FALSE(receiver.IsPeerConnected());
            EXPECT_FALSE(receiver.TryReceiveFrame(received));
            EXPECT_EQ(received, last);
        };
    for (const bool send_from_a : {false, true}) {
        SCOPED_TRACE(send_from_a);
        for (const auto delivery : {FrameDelivery::Reliable, FrameDelivery::Unreliable}) {
            SCOPED_TRACE(static_cast<int>(delivery));
            auto [a, b] = MakeLoopbackPair();
            exercise(send_from_a ? *a : *b, send_from_a ? *b : *a, delivery);
            NetworkSim sim(NetworkConditions{});
            exercise(send_from_a ? sim.A() : sim.B(), send_from_a ? sim.B() : sim.A(), delivery);
        }
    }
}

TEST(MessageReceiveQueue, InProcessTransportsBoundEmptyFramesInBothDirections) {
    const auto exercise = [](ILockstepTransport& sender, ILockstepTransport& receiver) {
        for (std::size_t i = 0; i < kReceiveQueueFrameLimit; ++i)
            ASSERT_TRUE(sender.SendFrame({}));
        EXPECT_FALSE(sender.SendFrame({}));
        std::vector<std::uint8_t> received{0xAB};
        ASSERT_TRUE(receiver.TryReceiveFrame(received));
        EXPECT_TRUE(received.empty());
        ASSERT_TRUE(sender.SendFrame({}));
        sender.Close();
        for (std::size_t i = 0; i < kReceiveQueueFrameLimit; ++i) {
            EXPECT_TRUE(receiver.IsPeerConnected());
            ASSERT_TRUE(receiver.TryReceiveFrame(received));
            EXPECT_TRUE(received.empty());
        }
        EXPECT_FALSE(receiver.IsPeerConnected());
        EXPECT_FALSE(receiver.TryReceiveFrame(received));
    };
    for (const bool send_from_a : {false, true}) {
        SCOPED_TRACE(send_from_a);
        auto [a, b] = MakeLoopbackPair();
        exercise(send_from_a ? *a : *b, send_from_a ? *b : *a);
        NetworkSim sim(NetworkConditions{});
        exercise(send_from_a ? sim.A() : sim.B(), send_from_a ? sim.B() : sim.A());
    }
}

TEST(MessageReceiveQueue, NetworkSimCountsDelayedFramesBeforeAndAfterClose) {
    for (const bool send_from_a : {false, true}) {
        SCOPED_TRACE(send_from_a);
        NetworkSim sim(NetworkConditions{3, 0, 0, 1});
        auto& sender = send_from_a ? sim.A() : sim.B();
        auto& receiver = send_from_a ? sim.B() : sim.A();
        const std::vector<std::uint8_t> frame(kReceiveQueueByteBudget, 0xAB);
        ASSERT_TRUE(sender.SendFrame(frame));
        EXPECT_FALSE(sender.SendFrame({1}));
        std::vector<std::uint8_t> received{0xCD};
        for (int tick = 0; tick < 3; ++tick) {
            EXPECT_FALSE(receiver.TryReceiveFrame(received));
            EXPECT_EQ(received, (std::vector<std::uint8_t>{0xCD}));
            EXPECT_FALSE(sender.SendFrame({1}));
            sim.Tick();
        }
        ASSERT_TRUE(receiver.TryReceiveFrame(received));
        EXPECT_EQ(received, frame);
        ASSERT_TRUE(sender.SendFrame(frame));
        EXPECT_FALSE(sender.SendFrame({1}));
        sender.Close();
        for (int tick = 0; tick < 3; ++tick) {
            EXPECT_TRUE(receiver.IsPeerConnected());
            EXPECT_FALSE(receiver.TryReceiveFrame(received));
            sim.Tick();
        }
        EXPECT_TRUE(receiver.IsPeerConnected());
        ASSERT_TRUE(receiver.TryReceiveFrame(received));
        EXPECT_EQ(received, frame);
        EXPECT_FALSE(receiver.IsPeerConnected());
        EXPECT_FALSE(receiver.TryReceiveFrame(received));
    }
}

TEST(MessageReceiveQueue, NetworkSimBackpressurePreservesDeterministicLossSequence) {
    const auto run = [](bool reject) {
        NetworkSim sim(NetworkConditions{0, 0, 2, 42});
        const std::vector<std::uint8_t> full(kReceiveQueueByteBudget, 0xAB);
        EXPECT_TRUE(sim.A().SendFrame(full));
        if (reject)
            EXPECT_FALSE(sim.A().SendFrame({0xCD}));
        std::vector<std::uint8_t> received;
        EXPECT_TRUE(sim.B().TryReceiveFrame(received));
        EXPECT_EQ(received, full);
        for (std::uint8_t id = 0; id < 32; ++id)
            EXPECT_TRUE(sim.A().SendFrame({id}, FrameDelivery::Unreliable));
        std::vector<std::vector<std::uint8_t>> frames;
        while (sim.B().TryReceiveFrame(received))
            frames.push_back(received);
        return frames;
    };
    const auto expected = run(false);
    EXPECT_FALSE(expected.empty());
    EXPECT_LT(expected.size(), 32u);
    EXPECT_EQ(run(true), expected);
}

TEST(MessageReceiveQueue, ValidFramesDrainInOrderBeforeOversizedMessageClosure) {
    // This is the admission/delivery path shared by the GNS and Steam transports.
    MessageReceiveQueue queue;
    HashMsg hash;
    hash.tick = 42;
    hash.world_hash = "1234";
    const std::vector<std::vector<std::uint8_t>> frames{EncodeHash(hash), {}, EncodeBye({42})};
    for (const auto& frame : frames)
        ASSERT_TRUE(queue.Push(frame.data(), frame.size()));

    // The size must be rejected without inspecting or allocating the payload.
    EXPECT_FALSE(queue.Push(nullptr, kMaxFramePayloadBytes + 1));
    EXPECT_TRUE(queue.IsClosed());
    EXPECT_FALSE(queue.IsDrained());
    const std::uint8_t trailing = 0xAB;
    EXPECT_FALSE(queue.Push(&trailing, 1));

    std::vector<std::uint8_t> received{0xA5};
    for (const auto& frame : frames) {
        ASSERT_TRUE(queue.TryReceiveFrame(received));
        EXPECT_EQ(received, frame);
        EXPECT_FALSE(queue.IsDrained());
    }
    const auto last = received;
    for (int poll = 0; poll < 3; ++poll) {
        EXPECT_FALSE(queue.TryReceiveFrame(received));
        EXPECT_EQ(received, last);
        EXPECT_TRUE(queue.IsDrained());
        EXPECT_FALSE(queue.Push(&trailing, 1));
    }
}

TEST(MessageReceiveQueue, ClosePreservesQueuedFrames) {
    MessageReceiveQueue queue;
    const std::vector<std::uint8_t> frame{0xAB, 0xCD};
    ASSERT_TRUE(queue.Push(frame.data(), frame.size()));
    queue.Close();
    EXPECT_TRUE(queue.IsClosed());
    EXPECT_FALSE(queue.IsDrained());
    EXPECT_FALSE(queue.Push(nullptr, 0));
    std::vector<std::uint8_t> received;
    ASSERT_TRUE(queue.TryReceiveFrame(received));
    EXPECT_EQ(received, frame);
    EXPECT_FALSE(queue.IsDrained());
    EXPECT_FALSE(queue.TryReceiveFrame(received));
    EXPECT_TRUE(queue.IsDrained());
}

} // namespace
