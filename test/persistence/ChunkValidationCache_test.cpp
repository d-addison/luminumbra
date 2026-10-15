#include "luminumbra_common/persistence/ChunkValidationCache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace {
using Cache = Luminumbra::Persistence::ChunkValidationCache;

void Scan(Cache& cache, bool strict) {
    const Cache::Facts facts{};
    for (std::uint64_t id = 0; id < 12; ++id) {
        const Cache::Key key{id, 8, 0, 0, strict};
        const std::string bytes(8, static_cast<char>('a' + id));
        if (!cache.find(key, bytes))
            cache.remember(key, bytes, facts);
        const auto stats = cache.stats();
        EXPECT_LE(stats.entries, 8u);
        EXPECT_LE(stats.payload_bytes, 32u);
        EXPECT_EQ(stats.entries, stats.modes[0].entries + stats.modes[1].entries);
        EXPECT_EQ(stats.payload_bytes, stats.modes[0].payload_bytes + stats.modes[1].payload_bytes);
    }
}

TEST(ChunkValidationCache, OversizedOrderedScansRetainStrictHitsWithinBothBounds) {
    Cache cache(8, 32);
    Scan(cache, false); // A one-shot permissive inventory must not crowd out strict reuse.
    Scan(cache, true);
    const auto cold = cache.stats();
    EXPECT_EQ(cold.modes[1].hits, 0u);
    Scan(cache, true);
    const auto warm = cache.stats();
    EXPECT_GT(warm.modes[1].hits, cold.modes[1].hits);
    EXPECT_GT(warm.modes[1].evictions, 0u);
    // A later permissive scan cannot evict the reusable strict subset.
    Scan(cache, false);
    const auto before = cache.stats().modes[1].hits;
    Scan(cache, true);
    EXPECT_GT(cache.stats().modes[1].hits, before);
}

TEST(ChunkValidationCache, OrderedScansAlsoRetainHitsAtTheEntryBound) {
    Cache cache(4, 1024);
    const Cache::Facts facts{};
    for (int pass = 0; pass < 2; ++pass) {
        for (std::uint64_t id = 0; id < 12; ++id) {
            const Cache::Key key{id, 1, 0, 0, true};
            if (!cache.find(key, "x"))
                cache.remember(key, "x", facts);
            EXPECT_LE(cache.stats().entries, 4u);
            EXPECT_LE(cache.stats().payload_bytes, 1024u);
        }
    }
    EXPECT_GT(cache.stats().modes[1].hits, 0u);
}

TEST(ChunkValidationCache, ModeStatisticsClassifyMissesAndRetainedBytes) {
    Cache cache(1, 8);
    const Cache::Key key{1, 7, 0, 0, false}, other{2, 7, 0, 0, false};
    const Cache::Facts facts{};
    EXPECT_FALSE(cache.find(key, "payload"));
    cache.remember(key, "payload", facts);
    EXPECT_TRUE(cache.find(key, "payload"));
    EXPECT_FALSE(cache.find(key, "changed"));
    EXPECT_FALSE(cache.find(key, "oversized"));
    cache.remember(other, "payload", facts);
    EXPECT_FALSE(cache.find(key, "payload"));
    auto strict = other;
    strict.strict_water = true;
    EXPECT_FALSE(cache.find(strict, "payload"));
    cache.record_decode(false);
    cache.record_decode(true);
    const auto stats = cache.stats();
    const auto& mode = stats.modes[0];
    EXPECT_EQ(mode.hits, 1u);
    EXPECT_EQ(mode.decodes, 1u);
    EXPECT_EQ(stats.modes[1].decodes, 1u);
    EXPECT_EQ(mode.misses, 4u);
    EXPECT_EQ(mode.insertions, 2u);
    EXPECT_EQ(mode.evictions, 1u);
    EXPECT_EQ(mode.absent, 1u);
    EXPECT_EQ(mode.byte_mismatch, 1u);
    EXPECT_EQ(mode.oversize, 1u);
    EXPECT_EQ(mode.evicted, 1u);
    EXPECT_EQ(mode.entries, 1u);
    EXPECT_EQ(mode.payload_bytes, 7u);
    EXPECT_EQ(stats.modes[1].absent, 1u);
    cache.clear();
    EXPECT_EQ(cache.stats().modes[0].insertions, 0u);
    EXPECT_EQ(cache.stats().modes[0].payload_bytes, 0u);
}

TEST(ChunkValidationCache, EvictionHistoryNeverExceedsCapacityDuringOrderedScans) {
    for (const std::size_t limit :
         {std::size_t{0}, std::size_t{1}, std::size_t{4}, Cache::kMaxEntries}) {
        SCOPED_TRACE(limit);
        Cache cache(limit, Cache::kMaxPayloadBytes);
        const auto check_bounds = [&] {
            const auto stats = cache.stats();
            EXPECT_LE(stats.entries, limit);
            EXPECT_LE(stats.payload_bytes, Cache::kMaxPayloadBytes);
            EXPECT_LE(stats.eviction_history_entries, limit);
            EXPECT_LE(stats.eviction_history_order_entries, limit);
            // The high-water mark also catches overflow between public operations.
            EXPECT_LE(stats.eviction_history_peak_entries, limit);
            EXPECT_EQ(stats.eviction_history_entries, stats.eviction_history_order_entries);
        };
        for (std::uint64_t id = 0; id < 2 * limit + 3; ++id) {
            SCOPED_TRACE(id);
            const Cache::Key key{id, 1, 0, 0, false};
            EXPECT_FALSE(cache.find(key, "x"));
            check_bounds();
            cache.remember(key, "x", {});
            check_bounds();
            const std::size_t evictions = limit == 0 || id < limit ? 0 : id - limit + 1;
            EXPECT_EQ(cache.stats().evictions, evictions);
            EXPECT_EQ(cache.stats().eviction_history_entries, std::min(evictions, limit));
        }
        EXPECT_EQ(cache.stats().evictions, limit == 0 ? 0u : limit + 3);
        EXPECT_EQ(cache.stats().eviction_history_peak_entries, limit);
        cache.clear();
        check_bounds();
        EXPECT_EQ(cache.stats().eviction_history_entries, 0u);
        EXPECT_EQ(cache.stats().eviction_history_order_entries, 0u);
        EXPECT_EQ(cache.stats().eviction_history_peak_entries, 0u);
    }
}

TEST(ChunkValidationCache, FirstStrictInventoryIncludesHitsAndStopsAfterFirstWalk) {
    Cache cache;
    const Cache::Key key{1, 7, 0, 0, true};
    cache.remember(key, "payload", {});
    {
        const Cache::InventoryScope inventory(&cache);
        EXPECT_TRUE(cache.find(key, "payload"));
        EXPECT_FALSE(cache.find({2, 3, 0, 0, true}, "new"));
        EXPECT_FALSE(cache.find({3, 4, 0, 0, false}, "skip"));
    }
    {
        const Cache::InventoryScope inventory(&cache);
        EXPECT_TRUE(cache.find(key, "payload"));
    }
    EXPECT_EQ(cache.stats().first_strict_records, 2u);
    EXPECT_EQ(cache.stats().first_strict_bytes, 10u);
}
} // namespace
