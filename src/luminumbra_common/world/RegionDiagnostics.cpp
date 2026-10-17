#include "RegionDiagnostics.h"

#include "GameSession.h"

#include <algorithm>
#include <limits>

namespace Luminumbra::world {
void RegionTransitionRing::Append(const RegionSchedule& schedule) noexcept {
    // Scheduler transitions are actual state changes. Earlier entries cannot survive
    // a batch of 64, so even a mass wake costs at most 64 observations.
    const auto first =
        schedule.transitions.size() > kCapacity ? schedule.transitions.size() - kCapacity : 0;
    for (std::size_t i = first; i < schedule.transitions.size(); ++i) {
        const auto& transition = schedule.transitions[i];
        if (transition.from == transition.to)
            continue;
        m_entries[m_next] = {schedule.tick, transition};
        m_next = (m_next + 1) % kCapacity;
        m_size = std::min(m_size + 1, kCapacity);
    }
}

RegionTransitionObservation RegionTransitionRing::at(std::size_t index) const noexcept {
    if (index >= m_size)
        return {};
    return m_entries[(m_next + kCapacity - m_size + index) % kCapacity];
}

RegionPanelSnapshot BuildRegionPanelSnapshot(const GameSession& session, RegionPanelQuery query) {
    RegionPanelSnapshot snapshot;
    snapshot.enabled = session.ActiveRegionsEnabled();
    snapshot.tick = session.GetWorldClock().tick();
    snapshot.rows_per_page =
        std::clamp(query.rows_per_page, std::size_t{1}, RegionPanelSnapshot::kMaxRows);
    snapshot.descending = query.descending;
    if (!snapshot.enabled)
        return snapshot;
    const auto& ledger = session.GetActiveRegionLedger();
    const auto& records = ledger.records();
    const auto& schedule = session.GetRegionSchedule();
    snapshot.schedule_tick = schedule.tick;
    snapshot.total_rows = records.size();
    snapshot.page_count = records.empty() ? 1 : 1 + (records.size() - 1) / snapshot.rows_per_page;
    snapshot.page = std::min(query.page, snapshot.page_count - 1);
    const auto first = snapshot.page * snapshot.rows_per_page;
    const auto count = std::min(snapshot.rows_per_page, records.size() - first);
    snapshot.rows.reserve(count);
    std::size_t index = 0;
    // One bounded-ledger traversal; no full copied/sorted row inventory and no offset overflow.
    for (const auto& [key, record] : records) {
        snapshot.pressure += std::min(
            record.pressure, std::numeric_limits<std::uint64_t>::max() - snapshot.pressure);
        switch (record.state) {
            case RegionState::Active:
                ++snapshot.active;
                break;
            case RegionState::Reduced:
                ++snapshot.reduced;
                break;
            case RegionState::Frozen:
                ++snapshot.frozen;
                break;
        }
        const auto display_index = query.descending ? records.size() - 1 - index : index;
        if (display_index >= first && display_index - first < count)
            snapshot.rows.push_back({key, record, false});
        ++index;
    }
    // Resume acknowledgements can stamp last_ticked without a scheduled due update.
    // Match the actual due list against only the bounded, still-ascending page.
    for (const auto key : schedule.due) {
        const auto row = std::lower_bound(
            snapshot.rows.begin(),
            snapshot.rows.end(),
            key,
            [](const RegionPanelRow& entry, RegionKey owner) { return entry.key < owner; });
        if (row != snapshot.rows.end() && row->key == key)
            row->due = true;
    }
    if (query.descending)
        std::reverse(snapshot.rows.begin(), snapshot.rows.end());
    const auto creatures = session.GetCreatureActivity();
    snapshot.full_creatures = creatures.full;
    snapshot.coarse_creatures = creatures.coarse;
    snapshot.frozen_creatures = creatures.frozen;
    snapshot.work = schedule.work;
    snapshot.work_limit = ledger.config().work_limit;
    snapshot.due = schedule.due.size();
    snapshot.rejected = schedule.rejected;
    const auto& ring = session.GetRegionTransitionRing();
    snapshot.transition_count = ring.size();
    for (std::size_t i = 0; i < ring.size(); ++i)
        snapshot.transitions[i] = ring.at(i);
    const auto persistence = session.GetPersistenceStatus();
    snapshot.parked_bytes = persistence.parked_bytes;
    snapshot.parked_count = persistence.parked_count;
    snapshot.oldest_parked_tick = persistence.oldest_parked_tick;
    snapshot.last_save_tick = persistence.last_save_tick;
    snapshot.last_save_ms = persistence.last_save_ms;
    snapshot.save_failures = persistence.failures;
    snapshot.save_requested = persistence.save_requested;
    if (const auto& fault = session.GetRegionSchedulerFault(); fault) {
        snapshot.fault_tick = fault->tick;
        snapshot.fault = fault->message.substr(0, RegionPanelSnapshot::kMaxFaultText);
    }
    const auto water = session.ObserveRegionalWaterWork();
    snapshot.water_reserved = water.reserved;
    snapshot.water_used = water.used;
    snapshot.water_unused = water.unused;
    snapshot.water_unallocated = water.unallocated;
    snapshot.water_cells = water.cells;
    snapshot.hashes = session.GetCachedRegionHashes();
    if (snapshot.hashes.valid && snapshot.tick >= snapshot.hashes.tick)
        snapshot.hash_age_ticks = snapshot.tick - snapshot.hashes.tick;
    return snapshot;
}
} // namespace Luminumbra::world
