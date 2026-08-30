#include "luminumbra_common/core/Crc32.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace Luminumbra::Core {
namespace {

TEST(Crc32AccumulatorTest, MatchesStandardCheckValue) {
    constexpr std::string_view input = "123456789";
    Crc32Accumulator crc;

    crc.Update(input.data(), input.size());

    EXPECT_EQ(crc.Value(), 0xcbf43926u);
}

TEST(Crc32AccumulatorTest, IncrementalUpdatesMatchContiguousInput) {
    constexpr std::array<std::uint8_t, 8> input = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    Crc32Accumulator contiguous;
    contiguous.Update(input.data(), input.size());

    Crc32Accumulator incremental;
    incremental.Update(input.data(), 3u);
    incremental.Update(input.data() + 3u, input.size() - 3u);

    EXPECT_EQ(incremental.Value(), contiguous.Value());
}

} // namespace
} // namespace Luminumbra::Core
