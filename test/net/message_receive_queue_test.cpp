#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "luminumbra_common/net/MessageReceiveQueue.h"

namespace {

using namespace Luminumbra::Net;

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
