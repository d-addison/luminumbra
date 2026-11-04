#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Luminumbra::Core {

class Crc32Accumulator {
public:
    void Update(const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        const auto* table = kTable.data();
        std::uint32_t state = state_;
        for (std::size_t i = 0; i < size; ++i) {
            state = (state >> 8u) ^ table[(state ^ bytes[i]) & 0xffu];
        }
        state_ = state;
    }

    [[nodiscard]] std::uint32_t Value() const noexcept {
        return ~state_;
    }

private:
    // Eight reflected polynomial steps for every possible low byte. The upper
    // bits shift independently, preserving the incremental IEEE CRC32 stream.
    static constexpr auto kTable = [] {
        std::array<std::uint32_t, 256> table{};
        for (std::size_t i = 0; i < table.size(); ++i) {
            auto value = static_cast<std::uint32_t>(i);
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1u) ^ ((value & 1u) != 0u ? 0xedb88320u : 0u);
            }
            table[i] = value;
        }
        return table;
    }();

    std::uint32_t state_ = 0xffffffffu;
};

} // namespace Luminumbra::Core
