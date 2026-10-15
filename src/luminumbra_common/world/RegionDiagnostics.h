#pragma once

#include "ActiveRegionLedger.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Luminumbra::world {
class GameSession;

// Observational, owner-thread data. Never persisted or included in authority captures.
struct RegionTransitionObservation {
    std::uint64_t tick = 0;
    RegionTransition transition{{}, RegionState::Active, RegionState::Active};
    bool operator==(const RegionTransitionObservation&) const = default;
};

class RegionTransitionRing {
public:
    static constexpr std::size_t kCapacity = 64;
    // Called once after a scheduler commit, including while the panel is closed.
    void Append(const RegionSchedule& schedule) noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return m_size;
    }
    [[nodiscard]] RegionTransitionObservation at(std::size_t index) const noexcept;

private:
    std::array<RegionTransitionObservation, kCapacity> m_entries{};
    std::size_t m_next = 0, m_size = 0;
};

struct CachedRegionHashes {
    bool valid = false;
    std::uint64_t tick = 0;
    std::string world, regions, creatures, items, pages;
    bool operator==(const CachedRegionHashes&) const = default;
};

struct RegionPanelQuery {
    std::size_t page = 0;
    std::size_t rows_per_page = 64;
    bool descending = false;
};

struct RegionPanelRow {
    RegionKey key;
    ActiveRegionRecord record;
    bool due = false;
};

struct RegionWaterDiagnostics {
    // Reserved/used windows include paired border reservations; cells include preparation.
    std::uint64_t reserved = 0, used = 0, unused = 0, unallocated = 0, cells = 0;
};

// An owned display value with no authority pointers. Rows are signed-key ordered;
// pagination/order are display selection only. Capture on the owner thread at a boundary.
// Building never hashes, performs IO, samples controllers or advances simulation.
struct RegionPanelSnapshot {
    static constexpr std::size_t kMaxRows = 256;
    static constexpr std::size_t kMaxFaultText = 512;
    bool enabled = false;
    std::uint64_t tick = 0, schedule_tick = 0;
    std::size_t total_rows = 0, page = 0, page_count = 1, rows_per_page = 64;
    bool descending = false;
    std::vector<RegionPanelRow> rows;
    std::uint64_t active = 0, reduced = 0, frozen = 0;
    std::uint64_t full_creatures = 0, coarse_creatures = 0, frozen_creatures = 0;
    // Aggregate pressure saturates at the same uint64 ceiling as scheduler work.
    std::uint64_t work = 0, work_limit = 0, pressure = 0, due = 0;
    RegionRejections rejected;
    std::array<RegionTransitionObservation, RegionTransitionRing::kCapacity> transitions{};
    std::size_t transition_count = 0;
    std::size_t parked_bytes = 0, parked_count = 0;
    std::int64_t oldest_parked_tick = -1, last_save_tick = -1;
    double last_save_ms = 0;
    std::uint32_t save_failures = 0;
    bool save_requested = false;
    std::optional<std::uint64_t> fault_tick;
    std::string fault;
    std::uint64_t water_reserved = 0, water_used = 0, water_unused = 0;
    std::uint64_t water_unallocated = 0, water_cells = 0;
    // No dynamic service theorem is available. Absence must be displayed as unknown.
    std::optional<std::uint64_t> water_bound_ticks;
    CachedRegionHashes hashes;
    std::optional<std::uint64_t> hash_age_ticks;
};

[[nodiscard]] RegionPanelSnapshot BuildRegionPanelSnapshot(const GameSession& session,
                                                           RegionPanelQuery query = {});
} // namespace Luminumbra::world
