#pragma once

#include <cstddef>
#include <cstdint>

namespace Luminumbra::Core {

class Crc32Accumulator {
public:
    void Update(const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            state_ ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit) {
                const std::uint32_t low_bit_mask = (state_ & 1u) != 0u ? 0xffffffffu : 0u;
                state_ = (state_ >> 1u) ^ (0xedb88320u & low_bit_mask);
            }
        }
    }

    [[nodiscard]] std::uint32_t Value() const noexcept {
        return ~state_;
    }

private:
    std::uint32_t state_ = 0xffffffffu;
};

} // namespace Luminumbra::Core
