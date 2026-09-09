#pragma once

#include "WorldClock.h"
#include "luminumbra/core/Types.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::world {

struct RegionKey {
    std::int32_t x = 0;
    std::int32_t z = 0;
    auto operator<=>(const RegionKey&) const = default;
};

enum class RegionState : std::uint8_t {
    Active,
    Reduced,
    Frozen
};

struct RegionSchedulerConfig {
    std::uint64_t work_limit = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t hold_ticks = 30;
    std::uint8_t reduced_shift = 1;
    bool operator==(const RegionSchedulerConfig&) const = default;
};

struct ActiveRegionRecord {
    bool visited = false, edited = false, pinned = false, populated = false;
    std::uint64_t first_active = 0, last_proximity = 0, last_edit = 0;
    std::uint64_t last_ticked = 0, frozen_at = 0, last_save = 0;
    RegionState state = RegionState::Active;
    std::uint8_t cadence_shift = 0;
    std::uint32_t cadence_phase = 0;
    std::uint64_t pressure = 0;
    std::uint32_t over_hold = 0, under_hold = 0;
    std::uint64_t last_decision = 0, water_cursor = 0, frozen_digest = 0;
    bool wake_pending = false;
    bool operator==(const ActiveRegionRecord&) const = default;
};

struct RegionWork {
    RegionKey key;
    std::uint64_t region_ticks = 0, entity_updates = 0, water_cells = 0, page_updates = 0;
};

struct RegionTransition {
    RegionKey key;
    RegionState from, to;
    bool operator==(const RegionTransition&) const = default;
};

struct RegionSchedule {
    std::uint64_t tick = 0, work = 0;
    std::vector<RegionKey> due;
    std::vector<RegionTransition> transitions;
    std::string digest;
    bool operator==(const RegionSchedule&) const = default;
};

// Future systems explicitly opt in to this interface; no consumer is registered
// here. Elapsed ticks are bounded by both the requested cap and one calendar day.
struct RegionResumeWindow {
    RegionKey key;
    std::uint64_t frozen_at = 0, resumed_at = 0, elapsed_ticks = 0;
};
using BoundedRegionResumeHook = std::function<void(const RegionResumeWindow&)>;

class ActiveRegionLedger {
public:
    static constexpr std::uint32_t kMaxRegions = 1u << 20;
    static constexpr std::size_t kRecordBytes = 104;
    // Magic, version, reserved, canonical tick, validation ceiling, config,
    // anchor presence and anchor.
    static constexpr std::size_t kHeaderBytes = 56;
    static constexpr std::size_t kMaxFileBytes = kHeaderBytes + kMaxRegions * kRecordBytes + 8;
    static constexpr const char* kCorruptMessage = "Corrupt active-region ledger.";
    static constexpr const char* kFutureMessage =
        "Unsupported future active-region ledger version.";
    using DurableDigest = std::function<std::uint64_t(RegionKey)>;

    // An absent ledger starts at the restored clock without scheduling a tick.
    explicit ActiveRegionLedger(RegionSchedulerConfig config = {},
                                const WorldClock& clock = WorldClock{});
    [[nodiscard]] const std::map<RegionKey, ActiveRegionRecord>& records() const {
        return m_records;
    }
    [[nodiscard]] const RegionSchedulerConfig& config() const {
        return m_config;
    }
    // The canonical scheduler tick. Advances only when the scheduler runs, and
    // is part of the hash projection.
    [[nodiscard]] std::uint64_t tick() const {
        return m_tick;
    }
    // The decode validation ceiling: no persisted record stamp may exceed it.
    // Reconciliation after an interrupted save raises this and never the
    // canonical tick, so recovery cannot move the hash projection. Observational
    // bookkeeping, persisted but excluded from the hash.
    [[nodiscard]] std::uint64_t header_tick() const {
        return m_headerTick;
    }
    [[nodiscard]] const std::optional<Vec3>& local_anchor() const {
        return m_localAnchor;
    }
    void set_local_anchor(const Vec3& walking_feet);
    void mark_edited(RegionKey key, const WorldClock& clock);
    void pin(RegionKey key, bool pinned, const WorldClock& clock);
    void set_populated(RegionKey key, bool populated);
    void set_water_cursor(RegionKey key, std::uint64_t cursor);
    void mark_saved(const WorldClock& clock);
    // An accepted partial save may have an older ledger than its clock. Raise
    // only the validation ceiling on load; never run catch-up, alter any region
    // record, or move the canonical tick that the hash projection reads.
    void restore_clock(const WorldClock& clock);
    [[nodiscard]] static RegionKey region_at(const Vec3& position);
    [[nodiscard]] static std::uint32_t phase(RegionKey key, std::uint8_t shift);
    [[nodiscard]] static RegionResumeWindow resume_window(RegionKey key,
                                                          std::uint64_t frozen_at,
                                                          const WorldClock& clock,
                                                          std::uint64_t system_cap);
    // Inputs are complete for this host tick. Duplicate work entries are summed
    // before any decision; unknown regions and repeated/backward ticks refuse.
    // nullopt uses the local single-player anchor; an explicit empty span visits nothing.
    RegionSchedule schedule(const WorldClock& clock,
                            std::optional<std::span<const Vec3>> replicated_anchors = std::nullopt,
                            std::span<const RegionWork> work = {},
                            const DurableDigest& durable_digest = {});
    [[nodiscard]] std::string canonical_bytes() const;
    [[nodiscard]] std::string encode() const;
    // Transactional decode: refuses without modifying out. No IO or migration.
    static bool decode(std::string_view bytes, ActiveRegionLedger& out, std::string& error);

private:
    RegionSchedule schedule_in_place(const WorldClock& clock,
                                     std::optional<std::span<const Vec3>> replicated_anchors,
                                     std::span<const RegionWork> work,
                                     const DurableDigest& durable_digest);
    ActiveRegionRecord& activate(RegionKey key, const WorldClock& clock);
    void visit(const Vec3& anchor, const WorldClock& clock);
    std::string payload(bool observational) const;
    RegionSchedulerConfig m_config;
    std::map<RegionKey, ActiveRegionRecord> m_records;
    std::optional<Vec3> m_localAnchor;
    std::uint64_t m_tick = 0;
    std::uint64_t m_headerTick = 0;
};

} // namespace Luminumbra::world
