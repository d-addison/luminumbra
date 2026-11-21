#include "luminumbra_common/core/IntegerPrimitives.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace Luminumbra::Core {
namespace {

// Frozen ParticlePass/FoliagePass byte loop. Keep independent of production.
constexpr std::uint64_t
FrozenRenderFold(const unsigned char* bytes, std::size_t len, std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Frozen WaterfallDetect loop (standard basis, including its explicit widening).
constexpr std::uint64_t
FrozenWaterfallFold(const unsigned char* bytes, std::size_t size, std::uint64_t seed) {
    for (std::size_t i = 0; i < size; ++i) {
        seed ^= static_cast<std::uint64_t>(bytes[i]);
        seed *= 0x100000001b3ull;
    }
    return seed;
}

// Frozen GBufferPass string loop, including unsigned conversion of char.
constexpr std::uint64_t FrozenMeshPathFold(std::string_view text, std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

// Frozen SkinnedMeshFormat 32-bit loop, with the seed made explicit for chaining.
constexpr std::uint32_t FrozenNameFold(std::string_view text, std::uint32_t seed) {
    std::uint32_t hash = seed;
    for (const char character : text) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 16777619u;
    }
    return hash;
}

// Frozen StructurePlacement semantics, also exercised at 64-bit width.
template<typename Integer>
constexpr Integer FrozenStructureDivision(Integer value, Integer divisor) {
    const Integer quotient = value / divisor;
    const Integer remainder = value % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

// Frozen FarLodSystem variant. Its callers always use a positive divisor.
template<typename Integer>
constexpr Integer FrozenFarDivision(Integer value, Integer divisor) {
    const Integer quotient = value / divisor;
    const Integer remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

// Frozen StructureStamping test variant (positive modulus).
template<typename Integer>
constexpr Integer FrozenPositiveModulo(Integer value, Integer divisor) {
    Integer remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

constexpr bool SmallDivisionParity() {
    for (int value = -32; value <= 32; ++value) {
        for (int divisor = -8; divisor <= 8; ++divisor) {
            if (divisor == 0)
                continue;
            if (FloorDiv(value, divisor) != FrozenStructureDivision(value, divisor))
                return false;
            if (divisor > 0 && (FloorDiv(value, divisor) != FrozenFarDivision(value, divisor) ||
                                FloorMod(value, divisor) != FrozenPositiveModulo(value, divisor)))
                return false;
        }
    }
    return true;
}

static_assert(SmallDivisionParity());
static_assert(Fnv1a64("hello", 5, kFnv1a64StandardBasis) == 0xa430d84680aabd0bull);
static_assert(Fnv1a64("hello", 5, kFnv1a64HistoricalBasis) == 0x005a0d15131ec7a1ull);
static_assert(Fnv1a32("hello", 5, kFnv1a32StandardBasis) == 0x4f9f2cabu);

constexpr bool ShortByteParity() {
    for (unsigned value = 0; value < 256; ++value) {
        const std::array<unsigned char, 2> bytes = {static_cast<unsigned char>(value), 0};
        for (std::size_t size = 0; size <= bytes.size(); ++size) {
            for (const auto seed : {kFnv1a64StandardBasis, kFnv1a64HistoricalBasis}) {
                if (Fnv1a64(bytes.data(), size, seed) != FrozenRenderFold(bytes.data(), size, seed))
                    return false;
                if (Fnv1a64(bytes.data(), size, seed) !=
                    FrozenWaterfallFold(bytes.data(), size, seed))
                    return false;
            }
        }
        const char character = static_cast<char>(static_cast<unsigned char>(value));
        const std::string_view text(&character, 1);
        if (Fnv1a32(text.data(), text.size(), kFnv1a32StandardBasis) !=
            FrozenNameFold(text, kFnv1a32StandardBasis))
            return false;
        if (Fnv1a64(text.data(), text.size(), kFnv1a64HistoricalBasis) !=
            FrozenMeshPathFold(text, kFnv1a64HistoricalBasis))
            return false;
    }
    return true;
}

static_assert(ShortByteParity());

template<typename Integer>
constexpr bool BoundaryDivisionParity() {
    constexpr auto minimum = (std::numeric_limits<Integer>::min)();
    constexpr auto maximum = (std::numeric_limits<Integer>::max)();
    constexpr std::array<Integer, 9> edges = {
        minimum, minimum + 1, -16, -1, 0, 1, 16, maximum - 1, maximum};
    for (const auto value : edges) {
        for (const auto divisor : edges) {
            if (divisor == 0 || (value == minimum && divisor == -1))
                continue;
            if (FloorDiv(value, divisor) != FrozenStructureDivision(value, divisor))
                return false;
            if (divisor > 0 && (FloorDiv(value, divisor) != FrozenFarDivision(value, divisor) ||
                                FloorMod(value, divisor) != FrozenPositiveModulo(value, divisor)))
                return false;
        }
    }
    return FloorMod(maximum, minimum) == -1 && FloorMod(minimum + 1, minimum) == minimum + 1;
}

static_assert(BoundaryDivisionParity<int>());
static_assert(BoundaryDivisionParity<std::int64_t>());

constexpr auto kLongBytes = [] {
    std::array<unsigned char, 4096> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<unsigned char>(i % 256);
    return bytes;
}();
static_assert(Fnv1a64(kLongBytes.data(), kLongBytes.size(), kFnv1a64StandardBasis) ==
              FrozenRenderFold(kLongBytes.data(), kLongBytes.size(), kFnv1a64StandardBasis));
static_assert(Fnv1a64(kLongBytes.data(), kLongBytes.size(), kFnv1a64HistoricalBasis) ==
              FrozenRenderFold(kLongBytes.data(), kLongBytes.size(), kFnv1a64HistoricalBasis));

void ExpectByteParity(const std::string& bytes) {
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
    constexpr std::array seeds = {kFnv1a64StandardBasis,
                                  kFnv1a64HistoricalBasis,
                                  std::uint64_t{0},
                                  (std::numeric_limits<std::uint64_t>::max)()};
    for (const auto seed : seeds) {
        const auto expected = FrozenRenderFold(raw, bytes.size(), seed);
        EXPECT_EQ(FrozenWaterfallFold(raw, bytes.size(), seed), expected);
        EXPECT_EQ(FrozenMeshPathFold(bytes, seed), expected);
        EXPECT_EQ(Fnv1a64(raw, bytes.size(), seed), expected);
        EXPECT_EQ(Fnv1a64(bytes.data(), bytes.size(), seed), expected);
        EXPECT_EQ(Fnv1a64(static_cast<const void*>(bytes.data()), bytes.size(), seed), expected);
        const auto split = bytes.size() / 2;
        const auto prefix = Fnv1a64(raw, split, seed);
        EXPECT_EQ(Fnv1a64(raw + split, bytes.size() - split, prefix), expected);
    }
    constexpr std::array seeds32 = {
        kFnv1a32StandardBasis, std::uint32_t{0}, (std::numeric_limits<std::uint32_t>::max)()};
    for (const auto seed : seeds32) {
        const auto expected = FrozenNameFold(bytes, seed);
        EXPECT_EQ(Fnv1a32(raw, bytes.size(), seed), expected);
        EXPECT_EQ(Fnv1a32(bytes.data(), bytes.size(), seed), expected);
        EXPECT_EQ(Fnv1a32(static_cast<const void*>(bytes.data()), bytes.size(), seed), expected);
        const auto split = bytes.size() / 2;
        const auto prefix = Fnv1a32(raw, split, seed);
        EXPECT_EQ(Fnv1a32(raw + split, bytes.size() - split, prefix), expected);
    }
}

TEST(IntegerPrimitivesTest, FrozenByteFoldsMatchEveryByteAndShortString) {
    ExpectByteParity("");
    for (unsigned byte = 0; byte < 256; ++byte)
        ExpectByteParity(std::string(1, static_cast<char>(static_cast<unsigned char>(byte))));
    // All strings of length two over this alphabet, including embedded NUL and high bits.
    constexpr std::array<unsigned char, 5> alphabet = {0, 1, 127, 128, 255};
    for (const auto first : alphabet)
        for (const auto second : alphabet)
            ExpectByteParity(std::string{static_cast<char>(first), static_cast<char>(second)});
}

TEST(IntegerPrimitivesTest, EmptyAndLongDigestsStayPinned) {
    EXPECT_EQ(Fnv1a64(nullptr, 0, kFnv1a64StandardBasis), 14695981039346656037ull);
    EXPECT_EQ(Fnv1a64(nullptr, 0, kFnv1a64HistoricalBasis), 1469598103934665603ull);
    EXPECT_EQ(Fnv1a32(nullptr, 0, kFnv1a32StandardBasis), 2166136261u);
    EXPECT_EQ(Fnv1a64("a", 1, kFnv1a64StandardBasis), 0xaf63dc4c8601ec8cull);
    EXPECT_EQ(Fnv1a64("a", 1, kFnv1a64HistoricalBasis), 0x44bd8ad473cd9906ull);
    EXPECT_EQ(Fnv1a32("a", 1, kFnv1a32StandardBasis), 0xe40c292cu);
    // Fixed 64 KiB bound, with every byte value repeated 256 times.
    std::string bytes(65536, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(static_cast<unsigned char>(i % 256));
    ExpectByteParity(bytes);
    EXPECT_EQ(Fnv1a64(bytes.data(), bytes.size(), kFnv1a64StandardBasis), 0x1ec0db407f352325ull);
    EXPECT_EQ(Fnv1a64(bytes.data(), bytes.size(), kFnv1a64HistoricalBasis), 0x435795b8ed7f0383ull);
    EXPECT_EQ(Fnv1a32(bytes.data(), bytes.size(), kFnv1a32StandardBasis), 0xa2d79dc5u);
}

template<typename Integer>
void ExpectDivisionParity(Integer value, Integer divisor) {
    // Both operations are undefined in the old implementations for these inputs.
    if (divisor == 0 || (value == (std::numeric_limits<Integer>::min)() && divisor == -1))
        return;
    EXPECT_EQ(FloorDiv(value, divisor), FrozenStructureDivision(value, divisor));
    if (divisor > 0) {
        EXPECT_EQ(FloorDiv(value, divisor), FrozenFarDivision(value, divisor));
        EXPECT_EQ(FloorMod(value, divisor), FrozenPositiveModulo(value, divisor));
    }
}

template<typename Integer>
void ExpectDivisionRanges() {
    for (Integer value = -64; value <= 64; ++value) {
        for (Integer divisor = -16; divisor <= 16; ++divisor) {
            ExpectDivisionParity(value, divisor);
            if (divisor != 0) {
                // Reconstruction is safe in this small range, for either divisor sign.
                EXPECT_EQ(FloorDiv(value, divisor) * divisor + FloorMod(value, divisor), value);
                const auto remainder = FloorMod(value, divisor);
                EXPECT_TRUE(remainder == 0 || ((remainder < 0) == (divisor < 0)));
            }
        }
    }
    constexpr auto minimum = (std::numeric_limits<Integer>::min)();
    constexpr auto maximum = (std::numeric_limits<Integer>::max)();
    constexpr std::array<Integer, 13> edges = {minimum,
                                               minimum + 1,
                                               minimum + 15,
                                               -512,
                                               -16,
                                               -1,
                                               0,
                                               1,
                                               16,
                                               512,
                                               maximum - 15,
                                               maximum - 1,
                                               maximum};
    for (const auto value : edges)
        for (const auto divisor : edges)
            ExpectDivisionParity(value, divisor);
    EXPECT_EQ(FloorDiv(minimum, Integer{1}), minimum);
    EXPECT_EQ(FloorDiv(minimum, Integer{-2}), -(minimum / 2));
    EXPECT_EQ(FloorDiv(maximum, minimum), -1);
    EXPECT_EQ(FloorMod(maximum, minimum), -1);
    EXPECT_EQ(FloorMod(minimum + 1, minimum), minimum + 1);
}

TEST(IntegerPrimitivesTest, FrozenSignedDivisionMatchesSmallRangesAndEdges) {
    ExpectDivisionRanges<int>();
    ExpectDivisionRanges<std::int64_t>();
}

TEST(IntegerPrimitivesTest, PositiveDivisorVariantsRemainDistinctForNegativeDivisors) {
    EXPECT_EQ(FrozenFarDivision(-5, -2), 1);
    EXPECT_EQ(FloorDiv(-5, -2), 2);
    EXPECT_EQ(FrozenPositiveModulo(-5, -2), -3);
    EXPECT_EQ(FloorMod(-5, -2), -1);
    EXPECT_EQ(FloorDiv(5, -2), -3);
    EXPECT_EQ(FloorMod(5, -2), -1);
}

} // namespace
} // namespace Luminumbra::Core
