// shared deterministic seeded PRNG coverage (sim-path mutation noise).
// Validates reproducibility, uniform bounds, Gaussian shape, and stream separation.

#include "gtest/gtest.h"

#include "luminumbra_common/core/DeterministicRng.h"

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace {
using luminumbra::core::DeterministicRng;
} // namespace

TEST(DeterministicRng, SameSeedSameSequence) {
    DeterministicRng a(12345), b(12345);
    for (int i = 0; i < 64; ++i)
        EXPECT_EQ(a.next_u64(), b.next_u64());
}

TEST(DeterministicRng, DifferentSeedsDiverge) {
    DeterministicRng a(1), b(2);
    int same = 0;
    for (int i = 0; i < 64; ++i)
        if (a.next_u64() == b.next_u64())
            ++same;
    EXPECT_EQ(same, 0); // independent streams should not collide over 64 draws
}

TEST(DeterministicRng, UnitIsInHalfOpenZeroOne) {
    DeterministicRng r(99);
    for (int i = 0; i < 10000; ++i) {
        const float u = r.next_unit();
        EXPECT_GE(u, 0.0f);
        EXPECT_LT(u, 1.0f);
    }
}

TEST(DeterministicRng, RangeStaysWithinBounds) {
    DeterministicRng r(7);
    for (int i = 0; i < 10000; ++i) {
        const float v = r.next_range(-3.0f, 5.0f);
        EXPECT_GE(v, -3.0f);
        EXPECT_LT(v, 5.0f);
    }
}

TEST(DeterministicRng, GaussianIsZeroMeanBounded) {
    DeterministicRng r(2024);
    double sum = 0.0;
    const int n = 50000;
    for (int i = 0; i < n; ++i) {
        const float g = r.next_gaussian();
        EXPECT_GE(g, -6.0f); // Irwin-Hall(12) support is [-6, 6]
        EXPECT_LE(g, 6.0f);
        sum += g;
    }
    const double mean = sum / n;
    EXPECT_NEAR(mean, 0.0, 0.05); // ~zero mean over many samples
}

TEST(DeterministicRng, SeededComposesInputsDeterministicallyAndDistinctly) {
    // Same (a,b,c) -> same stream; different inputs -> different streams.
    auto first = [](std::uint64_t a, std::uint64_t b, std::uint64_t c) {
        return DeterministicRng::seeded(a, b, c).next_u64();
    };
    EXPECT_EQ(first(16, 100, 5), first(16, 100, 5));
    EXPECT_NE(first(16, 100, 5), first(16, 100, 6)); // generation differs
    EXPECT_NE(first(16, 100, 5), first(16, 101, 5)); // entity id differs
    EXPECT_NE(first(16, 100, 5), first(17, 100, 5)); // subsystem offset differs
}

TEST(DeterministicRng, ModuleStreamsHaveGoldenSequences) {
    struct Case {
        std::uint64_t world_seed;
        std::string_view mod_id;
        std::string_view stream_name;
        std::uint64_t entity_id;
        std::uint64_t tick;
        std::array<std::uint64_t, 4> expected;
    };
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    constexpr std::array cases{
        Case{42,
             "acme.mod",
             "spawn",
             100,
             7,
             {0xCFD0B3BBAD35181Bull,
              0xC09B142D9DBEB890ull,
              0x4DEBE3D1F8EC932Dull,
              0x1AF0ED81EFF4F2DEull}},
        Case{42,
             "other.mod",
             "spawn",
             100,
             7,
             {0x44499508B98AEDF6ull,
              0x6A4BFB668CDF3332ull,
              0x5B193B9C4DC993B9ull,
              0x25BDF5B6F13CDB62ull}},
        Case{42,
             "acme.mod",
             "loot",
             100,
             7,
             {0x6282EFE996784128ull,
              0xEEEF9890EEB540CDull,
              0xB7582A286790A7B7ull,
              0xAC34F5FF5E2B83AAull}},
        Case{0,
             {},
             {},
             0,
             0,
             {0xAC6E169784097957ull,
              0x8E790B1A44CD7593ull,
              0x6683EA15B946679Full,
              0xB953F30C9A7FF43Full}},
        Case{max,
             "\xC3\xA9.mod",
             "\x80\xFF",
             max,
             max,
             {0xB17805231843F9CEull,
              0xFB23267100488082ull,
              0x579121513D810FBAull,
              0x8C16A5857FCB096Eull}},
        Case{42,
             "a:b",
             "c",
             100,
             7,
             {0xABFAC03BAF4E452Full,
              0xF455EE07EB73CE62ull,
              0x942EA61890BE1B77ull,
              0x71A47D6E36C7C0D0ull}},
        Case{42,
             "a",
             "b:c",
             100,
             7,
             {0x4339C7095BBA2F1Aull,
              0x74D618C56FF2F0C3ull,
              0x3BDEDCC5B9D21EBCull,
              0xE0B3FD6B64B5ADFEull}},
        Case{42,
             std::string_view{"a\0b", 3},
             "c",
             100,
             7,
             {0xE835CDA3BBB257B9ull,
              0xE581C6F2D2F1444Bull,
              0xD459F0572CD74859ull,
              0x8D10472E7916C6BEull}},
        Case{42,
             "a",
             std::string_view{"\0bc", 3},
             100,
             7,
             {0x26BBBAD93A6BF2BDull,
              0x9E940206AE1A6BFFull,
              0xDC41507F6DE4B4EBull,
              0x90B1AC5E59A92F71ull}},
    };
    for (const auto& entry : cases) {
        auto rng = DeterministicRng::for_module(
            entry.world_seed, entry.mod_id, entry.stream_name, entry.entity_id, entry.tick);
        auto repeated = DeterministicRng::for_module(
            entry.world_seed, entry.mod_id, entry.stream_name, entry.entity_id, entry.tick);
        for (const auto expected : entry.expected) {
            EXPECT_EQ(rng.next_u64(), expected);
            EXPECT_EQ(repeated.next_u64(), expected);
        }
    }
}

TEST(DeterministicRng, ModuleStreamInputsAreSeparated) {
    const std::array first_draws{
        DeterministicRng::for_module(42, "acme.mod", "spawn", 100, 7).next_u64(),
        DeterministicRng::for_module(43, "acme.mod", "spawn", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "other.mod", "spawn", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "acme.mod", "loot", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "acme.mod", "spawn", 101, 7).next_u64(),
        DeterministicRng::for_module(42, "acme.mod", "spawn", 100, 8).next_u64(),
        DeterministicRng::for_module(42, "acme.mod", "spawn", 7, 100).next_u64(),
        DeterministicRng::for_module(100, "acme.mod", "spawn", 42, 7).next_u64(),
        DeterministicRng::for_module(42, "ab", "c", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "a", "bc", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "a:b", "c", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "a", "b:c", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "", "a", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "a", "", 100, 7).next_u64(),
        DeterministicRng::for_module(42, "spawn", "acme.mod", 100, 7).next_u64(),
        DeterministicRng::seeded(42, 100, 7).next_u64(),
    };
    for (std::size_t i = 0; i < first_draws.size(); ++i) {
        for (std::size_t j = i + 1; j < first_draws.size(); ++j) {
            EXPECT_NE(first_draws[i], first_draws[j]) << i << ", " << j;
        }
    }
}

TEST(DeterministicRng, ModuleStreamsAreIndependentOfOtherDraws) {
    auto expected = DeterministicRng::for_module(42, "acme.mod", "spawn", 100, 7);
    auto actual = DeterministicRng::for_module(42, "acme.mod", "spawn", 100, 7);
    auto other_mod = DeterministicRng::for_module(42, "other.mod", "spawn", 100, 7);
    auto other_stream = DeterministicRng::for_module(42, "acme.mod", "loot", 100, 7);
    for (int i = 0; i < 32; ++i) {
        other_mod.next_u64();
        other_stream.next_u64();
        other_stream.next_u64();
        EXPECT_EQ(actual.next_u64(), expected.next_u64());
    }
}

TEST(DeterministicRng, LegacyStreamsHaveGoldenSequences) {
    struct Case {
        std::uint64_t a;
        std::uint64_t b;
        std::uint64_t c;
        std::array<std::uint64_t, 4> expected;
    };
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    constexpr std::array cases{
        Case{16,
             100,
             5,
             {0x8F94DEE865FD148Bull,
              0xD3F6D5D524716292ull,
              0x55885EF760C3584Full,
              0xE8E467BC3B76D663ull}},
        Case{0,
             0,
             0,
             {0x0CC78FB979CE5090ull,
              0xAB9AA3DAFBA6B4ACull,
              0xB0C750A86B3B1DD2ull,
              0x4B04A99A393A7F6Bull}},
        Case{max,
             max,
             max,
             {0x796E7B97C35024E0ull,
              0x30C94F527437DAB0ull,
              0x01850F892BAFFAE9ull,
              0x74C21BF8979A52DCull}},
    };
    for (const auto& entry : cases) {
        auto rng = DeterministicRng::seeded(entry.a, entry.b, entry.c);
        for (const auto expected : entry.expected) {
            EXPECT_EQ(rng.next_u64(), expected);
        }
    }
    DeterministicRng zero_seed(0);
    constexpr std::array zero_expected{
        0x06C45D188009454Full, 0xF88BB8A8724C81ECull, 0x1B39896A51A8749Bull, 0x53CB9F0C747EA2EAull};
    for (const auto expected : zero_expected) {
        EXPECT_EQ(zero_seed.next_u64(), expected);
    }
}
