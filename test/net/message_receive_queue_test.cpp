#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "luminumbra_common/net/MessageReceiveQueue.h"

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
