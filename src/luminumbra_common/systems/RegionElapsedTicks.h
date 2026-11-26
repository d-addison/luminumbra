#pragma once

#include "../world/ActiveRegionLedger.h"

#include <cstdint>
#include <map>

namespace Luminumbra::world {

// Tick-local, non-owning view of existing owner windows (RegionalFoliageWindows::elapsed).
// Keep the map alive and unchanged through all consumers. No ledger scan, copying, allocation,
// persistence or hashing occurs here; lookup uses only the participating-owner index.
class RegionElapsedTicks {
public:
    RegionElapsedTicks() = default;
    explicit RegionElapsedTicks(const std::map<RegionKey, std::uint32_t>& windows)
        : m_windows(&windows) {}
    RegionElapsedTicks(std::map<RegionKey, std::uint32_t>&&) = delete;

    [[nodiscard]] std::uint32_t ForOwner(RegionKey owner) const {
        if (!m_windows) {
            return 1;
        }
        const auto found = m_windows->find(owner);
        return found == m_windows->end() ? 0 : found->second;
    }

private:
    const std::map<RegionKey, std::uint32_t>* m_windows = nullptr;
};

} // namespace Luminumbra::world
