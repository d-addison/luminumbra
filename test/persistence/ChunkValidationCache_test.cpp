#include "luminumbra_common/core/Sha256.h"
#include "luminumbra_common/persistence/ChunkValidationCache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>

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
    Cache cache(8, 32, 0);
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
    Cache cache(4, 1024, 0);
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
    Cache cache(1, 8, 0);
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
        Cache cache(limit, Cache::kMaxPayloadBytes, 0);
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

namespace {
void CheckBackingBounds(const Cache& cache,
                        std::size_t entries,
                        std::size_t heap,
                        std::size_t spill) {
    const auto stats = cache.stats();
    EXPECT_LE(stats.entries, entries);
    EXPECT_LE(stats.payload_bytes, heap);
    EXPECT_LE(stats.spill_bytes, spill);
    EXPECT_LE(stats.spill_extent, spill);
    EXPECT_LE(stats.peak_entries, entries);
    EXPECT_LE(stats.peak_payload_bytes, heap);
    EXPECT_LE(stats.peak_spill_bytes, spill);
    EXPECT_EQ(stats.payload_bytes, stats.modes[0].payload_bytes + stats.modes[1].payload_bytes);
}

TEST(ChunkValidationCache, WholeInventoryBeyondHeapLimitSharesExactBackingBetweenModes) {
    std::filesystem::path directory;
    {
        Cache cache;
        const std::string payload(Cache::kMaxRecordBytes, 'x');
        for (std::uint64_t id = 0; id < 72; ++id) {
            for (const bool strict : {false, true}) {
                cache.remember({id, 1, 0, 0, strict}, payload, {{}, 17, true}, true);
                CheckBackingBounds(
                    cache, Cache::kMaxEntries, Cache::kMaxPayloadBytes, Cache::kMaxSpillBytes);
            }
        }
        EXPECT_EQ(cache.stats().entries, 144u);
        EXPECT_EQ(cache.stats().payload_bytes, Cache::kMaxPayloadBytes);
        EXPECT_EQ(cache.stats().spill_bytes, 8u * Cache::kMaxRecordBytes);
        EXPECT_EQ(cache.stats().evictions, 0u);
        for (const bool strict : {false, true}) {
            for (std::uint64_t id = 0; id < 72; ++id) {
                const auto facts = cache.find({id, 1, 0, 0, strict}, payload);
                ASSERT_TRUE(facts.has_value());
                EXPECT_EQ(facts->sdf_size, 17u);
                EXPECT_TRUE(facts->authoritative);
            }
            EXPECT_EQ(cache.stats().modes[strict].writer_seeded, 72u);
            EXPECT_GE(cache.stats().modes[strict].spill_hits, 8u);
        }
        directory = cache.spill_directory_for_testing();
        ASSERT_FALSE(directory.empty());
        EXPECT_TRUE(std::filesystem::exists(directory));
    }
    EXPECT_FALSE(std::filesystem::exists(directory));
}

TEST(ChunkValidationCache, SpillReplacementEvictionAndReclamationHoldAllBounds) {
    Cache cache(8, 16, 32);
    for (std::uint64_t id = 0; id < 100; ++id) {
        const std::string payload(8, static_cast<char>(id));
        const Cache::Key key{id % 5, 8, 0, 0, false};
        cache.remember(key, payload, {});
        auto strict = key;
        strict.strict_water = true;
        cache.remember(strict, payload, {});
        CheckBackingBounds(cache, 8, 16, 32);
        // Replacement and eviction may discard facts, but never accept old bytes.
        auto changed = payload;
        changed[4] = static_cast<char>(static_cast<unsigned char>(changed[4]) ^ 1u);
        EXPECT_FALSE(cache.find(key, changed));
        EXPECT_FALSE(cache.find(strict, changed));
        // A spill mismatch drops the shared backing. Revalidated bytes can be
        // retained again, preserving pressure on replacement and eviction.
        cache.remember(key, payload, {});
        cache.remember(strict, payload, {});
        CheckBackingBounds(cache, 8, 16, 32);
    }
    EXPECT_GT(cache.stats().evictions, 0u);
    EXPECT_GT(cache.stats().spill_extent, 0u);
    const auto directory = cache.spill_directory_for_testing();
    cache.clear();
    EXPECT_FALSE(std::filesystem::exists(directory));
    EXPECT_EQ(cache.stats().spill_bytes, 0u);
    EXPECT_EQ(cache.stats().spill_extent, 0u);
    EXPECT_EQ(cache.stats().payload_bytes, 0u);
}

TEST(ChunkValidationCache, SpillFaultsNeverAuthorizeReuseAndCanRecover) {
    for (const auto fault : {Cache::SpillFault::Create,
                             Cache::SpillFault::Write,
                             Cache::SpillFault::Read,
                             Cache::SpillFault::ShortRead}) {
        Cache cache(4, 0, 32);
        const Cache::Key key{1, 7, 0, 0, true};
        cache.spill_fault_for_testing(fault);
        cache.remember(key, "payload", {});
        EXPECT_FALSE(cache.find(key, "payload"));
        EXPECT_GT(cache.stats().modes[1].spill_faults, 0u);
        cache.spill_fault_for_testing(Cache::SpillFault::None);
        cache.remember(key, "payload", {});
        ASSERT_TRUE(cache.find(key, "payload"));
        cache.spill_fault_for_testing(Cache::SpillFault::Read);
        EXPECT_FALSE(cache.find(key, "payload"));
        EXPECT_EQ(cache.stats().entries, 0u);
        cache.spill_fault_for_testing(Cache::SpillFault::None);
        EXPECT_FALSE(cache.find(key, "payload"));
        cache.remember(key, "payload", {});
        EXPECT_TRUE(cache.find(key, "payload"));
        CheckBackingBounds(cache, 4, 0, 32);
    }
}

TEST(ChunkValidationCache, SpillExhaustionAndOversizeOnlyDiscardOptionalFacts) {
    Cache cache(2, 0, 8);
    cache.remember({1, 8, 0, 0, false}, "12345678", {});
    cache.remember({2, 8, 0, 0, false}, "abcdefgh", {});
    EXPECT_FALSE(cache.find({1, 8, 0, 0, false}, "12345678"));
    EXPECT_TRUE(cache.find({2, 8, 0, 0, false}, "abcdefgh"));
    cache.remember({3, 9, 0, 0, false}, "oversized", {});
    EXPECT_FALSE(cache.find({3, 9, 0, 0, false}, "oversized"));
    EXPECT_TRUE(cache.find({2, 8, 0, 0, false}, "abcdefgh"));
    CheckBackingBounds(cache, 2, 0, 8);
}

TEST(ChunkValidationCache, SaveContainingTemporaryRootDisablesAndRemovesSpill) {
    Cache cache(4, 0, 32);
    const Cache::Key key{1, 7, 0, 0, true};
    cache.remember(key, "payload", {});
    const auto directory = cache.spill_directory_for_testing();
    ASSERT_FALSE(directory.empty());
    cache.exclude_save_directory(std::filesystem::temp_directory_path());
    EXPECT_FALSE(cache.find(key, "payload"));
    EXPECT_FALSE(std::filesystem::exists(directory));
    cache.remember(key, "payload", {});
    EXPECT_FALSE(cache.find(key, "payload"));
    EXPECT_TRUE(cache.spill_directory_for_testing().empty());
}
} // namespace

namespace {
TEST(ChunkValidationCache, LargerReplacementReclaimsFreeTailTogetherWithUnusedCapacity) {
    Cache cache(4, 0, 24);
    const Cache::Key key{1, 24, 0, 0, false};
    cache.remember(key, "12345678", {});
    ASSERT_TRUE(cache.find(key, "12345678"));
    const std::string larger(24, 'x');
    cache.remember(key, larger, {});
    EXPECT_TRUE(cache.find(key, larger));
    EXPECT_FALSE(cache.find(key, "12345678"));
    EXPECT_EQ(cache.stats().entries, 0u);
    cache.remember(key, larger, {});
    EXPECT_TRUE(cache.find(key, larger));
    EXPECT_EQ(cache.stats().spill_bytes, 24u);
    EXPECT_EQ(cache.stats().spill_extent, 24u);
    CheckBackingBounds(cache, 4, 0, 24);
}
} // namespace

namespace {
TEST(ChunkValidationCache, Sha256MatchesKnownAnswersIncludingPaddingAndBinaryBytes) {
    const auto hex_digest = [](std::string_view bytes) {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string hex;
        for (const auto byte : Luminumbra::Core::Sha256(bytes)) {
            hex.push_back(digits[byte >> 4]);
            hex.push_back(digits[byte & 15u]);
        }
        return hex;
    };
    EXPECT_EQ(hex_digest(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex_digest("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(hex_digest(std::string(1000000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    EXPECT_EQ(hex_digest(std::string(1, '\0')),
              "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d");
}

TEST(ChunkValidationCache, RewrittenSpillCannotReturnEitherModesOriginallyValidatedFacts) {
    for (const bool strict_first : {false, true}) {
        for (const bool query_replacement : {false, true}) {
            SCOPED_TRACE(strict_first);
            SCOPED_TRACE(query_replacement);
            Cache cache(4, 0, 32);
            const Cache::Key permissive{1, 7, 0, 0, false}, strict{1, 7, 0, 0, true};
            for (const auto& key : {permissive, strict}) {
                cache.remember(key, "payload", {{}, 17, true});
                ASSERT_TRUE(cache.find(key, "payload"));
            }
            EXPECT_EQ(cache.stats().spill_bytes, 7u); // One shared extent.
            const auto spill = cache.spill_directory_for_testing() / "payloads";
            ASSERT_EQ(std::filesystem::file_size(spill), 7u);
            {
                std::fstream file(spill, std::ios::in | std::ios::out | std::ios::binary);
                ASSERT_TRUE(file.is_open());
                file.write("changed", 7);
                file.flush();
                ASSERT_TRUE(file.good());
            }
            ASSERT_EQ(std::filesystem::file_size(spill), 7u);
            const auto& first = strict_first ? strict : permissive;
            const auto& second = strict_first ? permissive : strict;
            const std::string_view fresh = query_replacement ? "changed" : "payload";
            // Either a digest mismatch after exact equality or a byte mismatch
            // drops both entries. No stale success or failure can survive.
            EXPECT_FALSE(cache.find(first, fresh));
            EXPECT_EQ(cache.stats().entries, 0u);
            EXPECT_EQ(cache.stats().spill_bytes, 0u);
            EXPECT_FALSE(cache.find(second, fresh));
            EXPECT_FALSE(cache.find(first, "payload"));
            for (const auto& key : {permissive, strict}) {
                cache.remember(key, "changed", {{}, 29, false});
                const auto facts = cache.find(key, "changed");
                ASSERT_TRUE(facts.has_value());
                EXPECT_EQ(facts->sdf_size, 29u);
                EXPECT_FALSE(facts->authoritative);
            }
            EXPECT_EQ(cache.stats().spill_bytes, 7u);
            CheckBackingBounds(cache, 4, 0, 32);
        }
    }
}

TEST(ChunkValidationCache, CrossModeRetentionDropsRewrittenBackingBeforeSharing) {
    for (const bool strict_first : {false, true}) {
        SCOPED_TRACE(strict_first);
        Cache cache(4, 0, 32);
        const Cache::Key first{1, 7, 0, 0, strict_first}, second{1, 7, 0, 0, !strict_first};
        cache.remember(first, "payload", {{}, 17, true});
        {
            std::fstream file(cache.spill_directory_for_testing() / "payloads",
                              std::ios::in | std::ios::out | std::ios::binary);
            ASSERT_TRUE(file.is_open());
            file.write("changed", 7);
            file.flush();
            ASSERT_TRUE(file.good());
        }
        cache.remember(second, "changed", {{}, 29, false});
        EXPECT_EQ(cache.stats().entries, 1u);
        EXPECT_EQ(cache.stats().spill_bytes, 7u);
        EXPECT_FALSE(cache.find(first, "changed"));
        const auto facts = cache.find(second, "changed");
        ASSERT_TRUE(facts.has_value());
        EXPECT_EQ(facts->sdf_size, 29u);
        EXPECT_FALSE(facts->authoritative);
    }
}
} // namespace
