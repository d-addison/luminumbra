#pragma once

#include "ActiveRegionLedger.h"
#include "core/DeterministicRng.h"

#include <cstdint>

namespace Luminumbra::world {

// Version 1: stateless, indexed streams. Population and reproduction never share
// the needs (+41) or travel (+42) domains. All arithmetic wraps modulo 2^64.
enum class RegionRngTag : std::uint64_t {
    CreatureNeeds = 41,
    CreatureTravel = 42,
    Population = 43,
    CreatureReproduction = 44
};

class RegionRng {
public:
    static constexpr std::uint64_t kVersion = 1;

    RegionRng(std::uint64_t world_seed,
              RegionKey key,
              RegionRngTag tag,
              std::uint64_t absolute_tick,
              std::uint64_t persistent_id = 0) noexcept {
        m_key = Mix(world_seed + static_cast<std::uint64_t>(tag));
        m_key = Mix(m_key ^ kVersion);
        m_key = Mix(m_key ^ static_cast<std::uint32_t>(key.x));
        m_key = Mix(m_key ^ static_cast<std::uint32_t>(key.z));
        m_key = Mix(m_key ^ absolute_tick);
        m_key = Mix(m_key ^ persistent_id);
    }

    [[nodiscard]] std::uint64_t draw(std::uint64_t index) const noexcept {
        return Mix(m_key ^ index);
    }

private:
    static std::uint64_t Mix(std::uint64_t value) noexcept {
        return luminumbra::core::DeterministicRng::mix(value);
    }
    std::uint64_t m_key = 0;
};

} // namespace Luminumbra::world
