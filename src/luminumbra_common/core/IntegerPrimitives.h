#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Luminumbra::Core {

inline constexpr std::uint64_t kFnv1a64StandardBasis = 14695981039346656037ull;
// Particle/foliage rendering and several authority domains use this older basis.
// It is a distinct algorithm contract, not an alias of the standard basis.
inline constexpr std::uint64_t kFnv1a64HistoricalBasis = 1469598103934665603ull;
inline constexpr std::uint32_t kFnv1a32StandardBasis = 2166136261u;

namespace Detail {
template<typename Byte>
concept FnvByte = std::is_same_v<Byte, char> || std::is_same_v<Byte, unsigned char> ||
                  std::is_same_v<Byte, signed char>;

template<typename Hash, FnvByte Byte>
constexpr Hash Fnv1aFold(const Byte* bytes, std::size_t size, Hash seed, Hash prime) noexcept {
    // Exactly size bytes, no allocation. A null pointer is valid when size is zero.
    for (std::size_t i = 0; i < size; ++i) {
        seed ^= static_cast<unsigned char>(bytes[i]);
        seed *= prime;
    }
    return seed;
}
} // namespace Detail

// Seeds are always explicit; callers retain their byte encoding and domain seed.
// Whole-word XOR/multiply mixers and multiply-before-XOR FNV variants are distinct
// contracts and must not be replaced with these byte folds.
template<Detail::FnvByte Byte>
constexpr std::uint64_t Fnv1a64(const Byte* bytes, std::size_t size, std::uint64_t seed) noexcept {
    return Detail::Fnv1aFold(bytes, size, seed, std::uint64_t{1099511628211ull});
}

inline std::uint64_t Fnv1a64(const void* data, std::size_t size, std::uint64_t seed) noexcept {
    return Fnv1a64(static_cast<const unsigned char*>(data), size, seed);
}

template<Detail::FnvByte Byte>
constexpr std::uint32_t Fnv1a32(const Byte* bytes, std::size_t size, std::uint32_t seed) noexcept {
    return Detail::Fnv1aFold(bytes, size, seed, std::uint32_t{16777619u});
}

inline std::uint32_t Fnv1a32(const void* data, std::size_t size, std::uint32_t seed) noexcept {
    return Fnv1a32(static_cast<const unsigned char*>(data), size, seed);
}

// StructurePlacement's sign-aware quotient: divisor must be nonzero and the
// quotient representable (in particular, min / -1 is outside the contract).
// No negation of value, so min is supported for every other nonzero divisor.
// Positive-divisor copies that subtract on a negative remainder agree only in
// that domain. The (value - 15) / 16 copies can overflow near min; leave those
// distinct until their callers' coordinate bounds have been proved.
template<typename Integer>
constexpr Integer FloorDiv(Integer value, Integer divisor) noexcept {
    static_assert(std::is_integral_v<Integer> && std::is_signed_v<Integer>);
    const Integer quotient = value / divisor;
    const Integer remainder = value % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0)))
               ? static_cast<Integer>(quotient - 1)
               : quotient;
}

// Same preconditions as FloorDiv. The remainder has the divisor's sign. The
// positive-modulus WaterSystem and StructureStamping test helpers agree for
// positive divisors; their negative-divisor variant remains local.
template<typename Integer>
constexpr Integer FloorMod(Integer value, Integer divisor) noexcept {
    static_assert(std::is_integral_v<Integer> && std::is_signed_v<Integer>);
    const Integer remainder = value % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0)))
               ? static_cast<Integer>(remainder + divisor)
               : remainder;
}

} // namespace Luminumbra::Core
