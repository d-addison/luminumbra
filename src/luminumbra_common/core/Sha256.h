#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Luminumbra::Core {
namespace Sha256Detail {
// SHA-256 compression uses scalar working variables so each round can stay in
// registers without shifting an indexed state array.
inline constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline void Compress(std::array<std::uint32_t, 8>& state,
                     const std::array<unsigned char, 64>& block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i)
        for (std::size_t byte = 0; byte < 4; ++byte)
            words[i] = (words[i] << 8) | block[i * 4 + byte];
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto x = words[i - 15], y = words[i - 2];
        words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + words[i - 7] +
                   (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }
    auto a = state[0], b = state[1], c = state[2], d = state[3];
    auto e = state[4], f = state[5], g = state[6], h = state[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                           ((e & f) ^ (~e & g)) + kRoundConstants[i] + words[i];
        const auto second =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace Sha256Detail

inline std::array<std::uint8_t, 32> Sha256(std::span<const std::uint8_t> bytes) noexcept {
    std::array<std::uint32_t, 8> state = {0x6a09e667,
                                          0xbb67ae85,
                                          0x3c6ef372,
                                          0xa54ff53a,
                                          0x510e527f,
                                          0x9b05688c,
                                          0x1f83d9ab,
                                          0x5be0cd19};
    std::array<unsigned char, 64> block{};
    const auto bit_count = static_cast<std::uint64_t>(bytes.size()) * 8;
    while (bytes.size() >= block.size()) {
        std::copy_n(bytes.begin(), block.size(), block.begin());
        Sha256Detail::Compress(state, block);
        bytes = bytes.subspan(block.size());
    }
    block.fill(0);
    std::copy(bytes.begin(), bytes.end(), block.begin());
    block[bytes.size()] = 0x80;
    if (bytes.size() >= 56) {
        Sha256Detail::Compress(state, block);
        block.fill(0);
    }
    for (std::size_t i = 0; i < 8; ++i)
        block[63 - i] = static_cast<unsigned char>(bit_count >> (i * 8));
    Sha256Detail::Compress(state, block);
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state.size(); ++i)
        for (std::size_t byte = 0; byte < 4; ++byte)
            digest[i * 4 + byte] = static_cast<std::uint8_t>(state[i] >> ((3 - byte) * 8));
    return digest;
}

inline std::array<std::uint8_t, 32> Sha256(std::string_view bytes) noexcept {
    return Sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                                bytes.size()));
}

} // namespace Luminumbra::Core
