#include "luminumbra_client/rendering/passes/FoliagePass.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

struct TestChunk {
    std::int32_t x;
    std::int32_t z;
};

// The prune used to walk every cache entry and, for each one, linearly rescan the
// whole chunk list while recomputing the packed key inside the inner loop. The
// key-computation count is the load-bearing assertion here: it is what fails if
// that quadratic shape ever comes back.
TEST(FoliageCachePruneTest, RetainsExactlyLiveEntriesAndComputesEachChunkKeyOnce) {
    using Luminumbra::Rendering::detail::pack_foliage_chunk_key;
    using Luminumbra::Rendering::detail::prune_foliage_cache;

    const auto retained_a = pack_foliage_chunk_key(7, -3);
    const auto retained_b = pack_foliage_chunk_key(-11, 29);
    const auto removed_a = pack_foliage_chunk_key(8, -3);
    const auto removed_b = pack_foliage_chunk_key(-11, 30);
    std::unordered_map<std::uint64_t, int> cache{
        {retained_a, 101}, {retained_b, 202}, {removed_a, 303}, {removed_b, 404}};
    const std::vector<TestChunk> chunks{{7, -3}, {-11, 29}};
    std::size_t key_computations = 0;

    prune_foliage_cache(cache, chunks, [&](const TestChunk& chunk) {
        ++key_computations;
        return pack_foliage_chunk_key(chunk.x, chunk.z);
    });

    EXPECT_EQ(key_computations, chunks.size());
    ASSERT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.at(retained_a), 101);
    EXPECT_EQ(cache.at(retained_b), 202);
    EXPECT_EQ(cache.count(removed_a), 0u);
    EXPECT_EQ(cache.count(removed_b), 0u);
}

// Negative coordinates pack through a uint32 cast; a chunk at (x, z) must never
// collide with one at (z, x).
TEST(FoliageCachePruneTest, PackedKeysAreDistinctAcrossAxisSwap) {
    using Luminumbra::Rendering::detail::pack_foliage_chunk_key;

    EXPECT_NE(pack_foliage_chunk_key(7, -3), pack_foliage_chunk_key(-3, 7));
    EXPECT_NE(pack_foliage_chunk_key(-1, 0), pack_foliage_chunk_key(0, -1));
    EXPECT_EQ(pack_foliage_chunk_key(-1, -1), pack_foliage_chunk_key(-1, -1));
}

// An empty live set prunes the cache to nothing; a fully-live set prunes nothing.
TEST(FoliageCachePruneTest, HandlesEmptyAndFullyLiveChunkSets) {
    using Luminumbra::Rendering::detail::pack_foliage_chunk_key;
    using Luminumbra::Rendering::detail::prune_foliage_cache;

    const auto key_fn = [](const TestChunk& chunk) {
        return pack_foliage_chunk_key(chunk.x, chunk.z);
    };

    std::unordered_map<std::uint64_t, int> cache{{pack_foliage_chunk_key(0, 0), 1},
                                                 {pack_foliage_chunk_key(1, 1), 2}};
    prune_foliage_cache(cache, std::vector<TestChunk>{}, key_fn);
    EXPECT_TRUE(cache.empty());

    cache = {{pack_foliage_chunk_key(0, 0), 1}, {pack_foliage_chunk_key(1, 1), 2}};
    prune_foliage_cache(cache, std::vector<TestChunk>{{0, 0}, {1, 1}}, key_fn);
    EXPECT_EQ(cache.size(), 2u);
}

} // namespace
