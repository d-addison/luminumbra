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

// Observational counts of inputs that the scheduler skipped deterministically.
struct RegionRejections {
    std::uint32_t anchors = 0, work = 0, capacity = 0;
    bool any() const {
        return anchors != 0 || work != 0 || capacity != 0;
    }
    bool operator==(const RegionRejections&) const = default;
};

struct RegionSchedule {
    std::uint64_t tick = 0, work = 0;
    std::vector<RegionKey> due;
    std::vector<RegionTransition> transitions;
    RegionRejections rejected;
    std::string digest;
    // Measurement hook: ledger record count after retention ran this tick.
    std::uint64_t ledger_records = 0;
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
    // Version 2 adds a u32 extension length and ascending TLVs: type 1 allocator (14 bytes),
    // type 2 digest projection (7 bytes), type 3 water debt, type 4 energy ages and type 6
    // completed work (each 10 + 16 bytes per owned key). Type 5 remains reserved and refuses.
    static constexpr std::size_t kMaxFileBytes =
        kHeaderBytes + kMaxRegions * kRecordBytes + 4 + 7 + 14 + 3 * (6 + 4 + 16 * kMaxRegions) + 8;
    static constexpr const char* kCorruptMessage = "Corrupt active-region ledger.";
    static constexpr const char* kProjectionMessage = "Unsupported frozen digest projection.";
    static constexpr const char* kFutureMessage =
        "Unsupported future active-region ledger version.";
    using DurableDigest = std::function<std::uint64_t(RegionKey)>;
    // Reports whether a region is still owned by a consumer, so it must not be evicted.
    using OwnershipProbe = std::function<bool(RegionKey)>;
    // Idle ticks before an unowned record is dropped (one hour at 30 Hz).
    static constexpr std::uint64_t kIdleEvictionTicks = 108000;
    // Persistent ids are allocated from 1 and never reach 2^63.
    static constexpr std::uint64_t kPersistentIdLimit = 1ull << 63;

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
    [[nodiscard]] std::uint64_t next_persistent_id() const {
        return m_nextPersistentId;
    }
    // Returns false and changes nothing once next_persistent_id() reaches kPersistentIdLimit.
    bool allocate_persistent_id(std::uint64_t& id);
    using WaterDebts = std::map<RegionKey, std::uint64_t>;
    static constexpr std::uint64_t kMaxWaterDebt = std::uint64_t{1} << 62;
    [[nodiscard]] const WaterDebts& water_debts() const noexcept {
        return m_waterDebts;
    }
    // Sparse positive Q32 debt owned by known records; zero is represented by absence.
    // Validation/cap failure changes nothing. Explicitly retain absent due claims.
    bool set_water_debts(const WaterDebts& debts);
    // Apply only after every resume consumer succeeds. Refuses frozen/unknown or stale
    // stamps, so a saved frozen_at cannot cause catch-up twice.
    bool acknowledge_resume(RegionKey key, std::uint64_t frozen_at);
    using EnergyAges = std::map<RegionKey, std::uint64_t>;
    [[nodiscard]] const EnergyAges& energy_ages() const noexcept {
        return m_energyAges;
    }
    // Missing register returns false without changing age.
    bool energy_age(RegionKey key, std::uint64_t& age) const noexcept;
    // Ages are durable even at zero energy. Both setters refuse unknown owners
    // and ages >= WorldClock::kTickLimit without changing any register.
    bool set_energy_age(RegionKey key, std::uint64_t age);
    bool set_energy_ages(const EnergyAges& ages);
    void reset_energy_ages() noexcept {
        m_energyAges.clear();
    }
    // Finite, and -2^24 <= x,z < 2^24 (exactly -2^24 is valid). y must be finite.
    [[nodiscard]] static bool valid_anchor(const Vec3& position) noexcept;
    // Non-throwing form: returns false and leaves the anchor untouched when invalid.
    bool try_set_local_anchor(const Vec3& walking_feet) noexcept;
    // Raises only the validation ceiling to the clock tick (never lowers it, never
    // touches the canonical tick or any record). Used after a scheduler fault.
    void acknowledge_world_clock(const WorldClock& clock) noexcept;
    void set_local_anchor(const Vec3& walking_feet);
    void mark_edited(RegionKey key, const WorldClock& clock);
    void pin(RegionKey key, bool pinned, const WorldClock& clock);
    void set_populated(RegionKey key, bool populated);
    void set_water_cursor(RegionKey key, std::uint64_t cursor);
    void mark_saved(const WorldClock& clock);
    // Completed consumer work produced at tick(), consumed once by schedule(tick()+1).
    // Every successful consumer tick replaces this register, including an empty tick.
    void publish_completed_work(std::span<const RegionWork> work);
    void clear_completed_work() noexcept {
        m_completedWork.clear();
    }
    [[nodiscard]] const std::map<RegionKey, std::uint64_t>& completed_work() const {
        return m_completedWork;
    }
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
    // due lists only records that survive this tick's retention, in scheduler order.
    RegionSchedule schedule(const WorldClock& clock,
                            std::optional<std::span<const Vec3>> replicated_anchors = std::nullopt,
                            std::span<const RegionWork> work = {},
                            const DurableDigest& durable_digest = {},
                            const OwnershipProbe& owned = {});
    [[nodiscard]] std::string canonical_bytes() const;
    [[nodiscard]] std::string encode() const;
    // Transactional decode: refuses without modifying out. No IO or migration.
    static bool decode(std::string_view bytes, ActiveRegionLedger& out, std::string& error);

private:
    RegionSchedule schedule_in_place(const WorldClock& clock,
                                     std::optional<std::span<const Vec3>> replicated_anchors,
                                     std::span<const RegionWork> work,
                                     const DurableDigest& durable_digest,
                                     const OwnershipProbe& owned);
    // Drops idle, unowned, non-pinned, unedited, unpopulated records with no water cursor,
    // water debt, completed work or energy age, including records due in the current schedule.
    void evict_idle(const WorldClock& clock, const OwnershipProbe& owned);
    ActiveRegionRecord& activate(RegionKey key, const WorldClock& clock);
    void visit(const Vec3& anchor, const WorldClock& clock, RegionRejections& rejected);
    std::string payload(bool observational) const;
    // True when any record has been frozen (now or in the past), which requires version 2.
    bool any_frozen_history() const;
    std::map<RegionKey, std::uint64_t> m_completedWork;
    EnergyAges m_energyAges;
    WaterDebts m_waterDebts;
    RegionSchedulerConfig m_config;
    std::map<RegionKey, ActiveRegionRecord> m_records;
    std::optional<Vec3> m_localAnchor;
    std::uint64_t m_tick = 0;
    std::uint64_t m_headerTick = 0;
    std::uint64_t m_nextPersistentId = 1;
};

} // namespace Luminumbra::world
