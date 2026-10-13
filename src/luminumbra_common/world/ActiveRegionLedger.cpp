#include "ActiveRegionLedger.h"

#include "persistence/WorldPersistenceRoundtrip.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace Luminumbra::world {
namespace {
void Append(std::string& out, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        out.push_back(static_cast<char>((value >> (i * 8)) & 255));
}
std::uint64_t Hash(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}
std::uint64_t Sum(std::uint64_t a, std::uint64_t b) {
    return a + std::min(b, std::numeric_limits<std::uint64_t>::max() - a);
}
bool ValidConfig(RegionSchedulerConfig config) {
    return config.hold_ticks > 0 && config.reduced_shift > 0 && config.reduced_shift <= 16;
}
bool ValidKey(RegionKey key) {
    // Existing packed chunk identity: [-2^20, 2^20) chunks / 32.
    return key.x >= -32768 && key.x < 32768 && key.z >= -32768 && key.z < 32768;
}
int Rank(const ActiveRegionRecord& record) {
    return record.pinned ? 0 : record.edited ? 1 : 2;
}
class Reader {
public:
    explicit Reader(std::string_view bytes)
        : m_bytes(bytes) {}
    std::uint64_t read(unsigned width) {
        if (m_offset + width > m_bytes.size())
            throw std::invalid_argument("truncated ledger");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= std::uint64_t{static_cast<unsigned char>(m_bytes[m_offset++])} << (8 * i);
        return value;
    }

private:
    std::string_view m_bytes;
    std::size_t m_offset = 0;
};
} // namespace

ActiveRegionLedger::ActiveRegionLedger(RegionSchedulerConfig config, const WorldClock& clock)
    : m_config(config)
    , m_tick(clock.tick())
    , m_headerTick(clock.tick()) {
    if (!ValidConfig(config))
        throw std::invalid_argument("Invalid active-region scheduler configuration");
}
bool ActiveRegionLedger::valid_anchor(const Vec3& p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && p.x >= -16777216.0f &&
           p.x < 16777216.0f && p.z >= -16777216.0f && p.z < 16777216.0f;
}
RegionKey ActiveRegionLedger::region_at(const Vec3& position) {
    if (!valid_anchor(position))
        throw std::invalid_argument("Invalid simulation anchor");
    return {static_cast<std::int32_t>(std::floor(static_cast<double>(position.x) / 512.0)),
            static_cast<std::int32_t>(std::floor(static_cast<double>(position.z) / 512.0))};
}
void ActiveRegionLedger::set_local_anchor(const Vec3& walking_feet) {
    if (!valid_anchor(walking_feet))
        throw std::invalid_argument("Invalid simulation anchor");
    m_localAnchor = walking_feet;
    // Canonicalise signed zero, including anchor hashes and persisted bits.
    for (int axis = 0; axis < 3; ++axis)
        if ((*m_localAnchor)[axis] == 0.0f)
            (*m_localAnchor)[axis] = 0.0f;
}
bool ActiveRegionLedger::try_set_local_anchor(const Vec3& walking_feet) noexcept {
    if (!valid_anchor(walking_feet))
        return false;
    set_local_anchor(walking_feet);
    return true;
}
ActiveRegionRecord& ActiveRegionLedger::activate(RegionKey key, const WorldClock& clock) {
    if (!ValidKey(key) || clock.tick() < m_headerTick)
        throw std::invalid_argument("Invalid region activation");
    if (!m_records.contains(key) && m_records.size() == kMaxRegions)
        throw std::length_error("Active-region ledger capacity reached");
    auto [it, added] = m_records.try_emplace(key);
    if (added)
        it->second.first_active = clock.tick();
    m_headerTick = std::max(m_headerTick, clock.tick());
    return it->second;
}
void ActiveRegionLedger::mark_edited(RegionKey key, const WorldClock& clock) {
    auto& record = activate(key, clock);
    record.edited = true;
    record.last_edit = clock.tick();
    record.wake_pending = true;
}
void ActiveRegionLedger::pin(RegionKey key, bool pinned, const WorldClock& clock) {
    if (!pinned && !m_records.contains(key))
        return;
    auto& record = activate(key, clock);
    record.pinned = pinned;
    if (pinned)
        record.wake_pending = true;
}
void ActiveRegionLedger::set_populated(RegionKey key, bool populated) {
    m_records.at(key).populated = populated;
}
void ActiveRegionLedger::set_water_cursor(RegionKey key, std::uint64_t cursor) {
    m_records.at(key).water_cursor = cursor;
}
void ActiveRegionLedger::mark_saved(const WorldClock& clock) {
    for (auto& [key, record] : m_records) {
        (void)key;
        record.last_save = clock.tick();
    }
    // The ceiling is deliberately not raised here. A save whose clock runs ahead
    // of everything this ledger knows about must stamp a last_save above the
    // ceiling and fail its own decode check, which is how an inconsistent
    // metadata/ledger pair is refused rather than written.
}
void ActiveRegionLedger::restore_clock(const WorldClock& clock) {
    if (clock.tick() < m_headerTick)
        throw std::invalid_argument("Cannot restore a region ledger ahead of the world clock");
    // Only the validation ceiling moves. The canonical tick belongs to the
    // scheduler, so an accepted state keeps the hash projection it was saved
    // with and recovery cannot rewrite history.
    m_headerTick = clock.tick();
}
void ActiveRegionLedger::acknowledge_world_clock(const WorldClock& clock) noexcept {
    m_headerTick = std::max(m_headerTick, clock.tick());
}
bool ActiveRegionLedger::allocate_persistent_id(std::uint64_t& id) {
    if (m_nextPersistentId >= kPersistentIdLimit)
        return false;
    id = m_nextPersistentId++;
    return true;
}
bool ActiveRegionLedger::energy_age(RegionKey key, std::uint64_t& age) const noexcept {
    const auto found = m_energyAges.find(key);
    if (found == m_energyAges.end())
        return false;
    age = found->second;
    return true;
}
bool ActiveRegionLedger::set_energy_age(RegionKey key, std::uint64_t age) {
    if (!m_records.contains(key) || age >= WorldClock::kTickLimit)
        return false;
    m_energyAges.insert_or_assign(key, age);
    return true;
}
bool ActiveRegionLedger::set_energy_ages(const EnergyAges& ages) {
    for (const auto& [key, age] : ages)
        if (!m_records.contains(key) || age >= WorldClock::kTickLimit)
            return false;
    auto next = ages;
    m_energyAges.swap(next);
    return true;
}
std::uint32_t ActiveRegionLedger::phase(RegionKey key, std::uint8_t shift) {
    if (shift > 16)
        throw std::invalid_argument("Invalid region cadence shift");
    std::string bytes;
    Append(bytes, static_cast<std::uint32_t>(key.x), 4);
    Append(bytes, static_cast<std::uint32_t>(key.z), 4);
    return static_cast<std::uint32_t>(Hash(bytes)) & ((std::uint32_t{1} << shift) - 1);
}
RegionResumeWindow ActiveRegionLedger::resume_window(RegionKey key,
                                                     std::uint64_t frozen_at,
                                                     const WorldClock& clock,
                                                     std::uint64_t system_cap) {
    if (frozen_at > clock.tick())
        throw std::invalid_argument("Region resume precedes freeze");
    return {key,
            frozen_at,
            clock.tick(),
            std::min({clock.tick() - frozen_at,
                      system_cap,
                      std::uint64_t{clock.calendar().dayLengthTicks}})};
}
void ActiveRegionLedger::visit(const Vec3& anchor,
                               const WorldClock& clock,
                               RegionRejections& rejected) {
    const auto centre = region_at(anchor);
    // The fixed disc intersects a region's closed XZ square; corner-only
    // AABB candidates outside the circle are excluded. Streaming is unrelated.
    for (int x = centre.x - 2; x <= centre.x + 1; ++x)
        for (int z = centre.z - 2; z <= centre.z + 1; ++z) {
            const RegionKey key{x, z};
            if (!ValidKey(key))
                continue;
            const double dx = std::max({x * 512.0 - anchor.x, 0.0, anchor.x - (x + 1) * 512.0});
            const double dz = std::max({z * 512.0 - anchor.z, 0.0, anchor.z - (z + 1) * 512.0});
            if (dx * dx + dz * dz > 512.0 * 512.0)
                continue;
            if (!m_records.contains(key) && m_records.size() == kMaxRegions) {
                ++rejected.capacity;
                continue;
            }
            auto& record = activate(key, clock);
            record.visited = true;
            record.last_proximity = clock.tick();
        }
}
void ActiveRegionLedger::evict_idle(const WorldClock& clock, const OwnershipProbe& owned) {
    if (!owned)
        return;
    for (auto it = m_records.begin(); it != m_records.end();) {
        const auto& r = it->second;
        // Cheap record conditions first so the probe runs only for evictable records.
        const bool idle = clock.tick() >= r.last_proximity &&
                          clock.tick() - r.last_proximity >= kIdleEvictionTicks;
        if (!r.pinned && !r.edited && !r.populated && r.water_cursor == 0 && idle &&
            !m_completedWork.contains(it->first) && !m_energyAges.contains(it->first) &&
            !owned(it->first))
            it = m_records.erase(it);
        else
            ++it;
    }
}
RegionSchedule ActiveRegionLedger::schedule(const WorldClock& clock,
                                            std::optional<std::span<const Vec3>> replicated_anchors,
                                            std::span<const RegionWork> work,
                                            const DurableDigest& durable_digest,
                                            const OwnershipProbe& owned) {
    auto next = *this;
    std::vector<RegionWork> combined(work.begin(), work.end());
    // Restored clock gaps expire stale work only after this schedule commits successfully.
    if (m_tick + 1 == clock.tick())
        for (const auto& [key, total] : m_completedWork)
            combined.push_back(RegionWork{key, total, 0, 0, 0});
    auto result =
        next.schedule_in_place(clock, replicated_anchors, combined, durable_digest, owned);
    next.m_completedWork.clear();
    *this = std::move(next);
    return result;
}
RegionSchedule
ActiveRegionLedger::schedule_in_place(const WorldClock& clock,
                                      std::optional<std::span<const Vec3>> replicated_anchors,
                                      std::span<const RegionWork> work,
                                      const DurableDigest& durable_digest,
                                      const OwnershipProbe& owned) {
    // The ceiling, not the canonical tick, is the latest world time this ledger
    // knows about. After reconciling an interrupted save the canonical tick is
    // older, and scheduling at an already elapsed tick would re-run it.
    if (clock.tick() <= m_headerTick)
        throw std::invalid_argument("Region schedule requires a new absolute tick");
    RegionSchedule result;
    result.tick = clock.tick();
    const auto all_anchors = replicated_anchors.value_or(std::span<const Vec3>{});
    std::vector<Vec3> anchors;
    anchors.reserve(all_anchors.size());
    for (const auto& anchor : all_anchors) {
        if (valid_anchor(anchor))
            anchors.push_back(anchor);
        else
            ++result.rejected.anchors;
    }
    std::vector<RegionWork> accepted_work;
    accepted_work.reserve(work.size());
    for (const auto& units : work) {
        if (m_records.contains(units.key))
            accepted_work.push_back(units);
        else
            ++result.rejected.work;
    }
    if (m_localAnchor && !replicated_anchors)
        visit(*m_localAnchor, clock, result.rejected);
    for (const auto& anchor : anchors)
        visit(anchor, clock, result.rejected);

    // std::map nodes are pointer stable and nothing is inserted after this point.
    struct Row {
        RegionKey key;
        ActiveRegionRecord* record;
    };
    std::vector<Row> rows;
    rows.reserve(m_records.size());
    for (auto& [key, record] : m_records) {
        record.pressure = 0;
        rows.push_back({key, &record});
    }
    for (const auto& units : accepted_work) {
        const auto total = Sum(Sum(units.region_ticks, units.entity_updates),
                               Sum(units.water_cells, units.page_updates));
        auto& record = m_records.at(units.key);
        record.pressure = Sum(record.pressure, total);
        result.work = Sum(result.work, total);
    }
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const auto& ra = *a.record;
        const auto& rb = *b.record;
        return std::tuple(Rank(ra), clock.tick() - ra.last_proximity, a.key) <
               std::tuple(Rank(rb), clock.tick() - rb.last_proximity, b.key);
    });
    const bool over = result.work > m_config.work_limit;
    for (const auto& row : rows) {
        auto& r = *row.record;
        const bool near = r.visited && r.last_proximity == clock.tick();
        if (near || r.wake_pending) {
            r.over_hold = r.under_hold = 0;
        } else if (over) {
            r.over_hold = std::min(r.over_hold, m_config.hold_ticks - 1) + 1;
            r.under_hold = 0;
        } else {
            r.under_hold = std::min(r.under_hold, m_config.hold_ticks - 1) + 1;
            r.over_hold = 0;
        }
    }
    // Select at most one budget transition from the complete tick's totals.
    // Degrade the lowest priority eligible region; recover the highest. Then
    // apply all transitions (including forced wakes) in the forward total order.
    std::optional<RegionKey> selected;
    const auto eligible = [&](const ActiveRegionRecord& r) {
        if ((r.visited && r.last_proximity == clock.tick()) || r.wake_pending)
            return false;
        return over ? r.state != RegionState::Frozen && r.over_hold == m_config.hold_ticks
                    : r.state != RegionState::Active && r.under_hold == m_config.hold_ticks;
    };
    if (over) {
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            if (eligible(*it->record)) {
                selected = it->key;
                break;
            }
    } else {
        for (const auto& row : rows)
            if (eligible(*row.record)) {
                selected = row.key;
                break;
            }
    }
    for (const auto& row : rows) {
        const auto key = row.key;
        auto& r = *row.record;
        const auto previous = r.state;
        if ((r.visited && r.last_proximity == clock.tick()) || r.wake_pending) {
            r.state = RegionState::Active;
        } else if (selected == key) {
            if (over)
                r.state =
                    r.state == RegionState::Active ? RegionState::Reduced : RegionState::Frozen;
            else
                r.state =
                    r.state == RegionState::Frozen ? RegionState::Reduced : RegionState::Active;
        }
        if (previous != r.state) {
            if (r.state == RegionState::Frozen) {
                if (!durable_digest)
                    throw std::invalid_argument(
                        "Freezing requires a durable region digest provider");
                r.frozen_digest = durable_digest(key);
                r.frozen_at = clock.tick();
            }
            r.over_hold = r.under_hold = 0;
            r.cadence_shift = r.state == RegionState::Active ? 0 : m_config.reduced_shift;
            r.cadence_phase = phase(key, r.cadence_shift);
            result.transitions.push_back({key, previous, r.state});
        }
        r.wake_pending = false;
        r.last_decision = clock.tick();
        if (r.state != RegionState::Frozen &&
            (clock.tick() & ((std::uint64_t{1} << r.cadence_shift) - 1)) == r.cadence_phase) {
            r.last_ticked = clock.tick();
            result.due.push_back(key);
        }
    }
    m_tick = clock.tick();
    m_headerTick = std::max(m_headerTick, clock.tick());
    std::string trace = "region_schedule:v1:";
    Append(trace, result.tick, 8);
    Append(trace, result.work, 8);
    Append(trace, rows.size(), 4);
    for (const auto& row : rows) {
        const auto key = row.key;
        const auto& r = *row.record;
        Append(trace, static_cast<std::uint32_t>(key.x), 4);
        Append(trace, static_cast<std::uint32_t>(key.z), 4);
        Append(trace, static_cast<std::uint8_t>(r.state), 1);
        Append(trace, r.last_ticked == clock.tick(), 1);
    }
    Append(trace, result.transitions.size(), 4);
    for (const auto& transition : result.transitions) {
        Append(trace, static_cast<std::uint32_t>(transition.key.x), 4);
        Append(trace, static_cast<std::uint32_t>(transition.key.z), 4);
        Append(trace, static_cast<std::uint8_t>(transition.from), 1);
        Append(trace, static_cast<std::uint8_t>(transition.to), 1);
    }
    if (result.rejected.any()) {
        Append(trace, result.rejected.anchors, 4);
        Append(trace, result.rejected.work, 4);
        Append(trace, result.rejected.capacity, 4);
    }
    result.digest = Persistence::StableChecksum(trace);
    // Retention runs after the digest so the digest and trace describe the pre-eviction state.
    evict_idle(clock, owned);
    // Consumers may create authority and publish work only for surviving records.
    std::erase_if(result.due, [this](RegionKey key) { return !m_records.contains(key); });
    result.ledger_records = m_records.size();
    return result;
}

void ActiveRegionLedger::publish_completed_work(std::span<const RegionWork> work) {
    std::map<RegionKey, std::uint64_t> completed;
    for (const auto& units : work) {
        if (!m_records.contains(units.key))
            throw std::invalid_argument("Completed work requires an owned ledger record");
        const auto total = Sum(Sum(units.region_ticks, units.entity_updates),
                               Sum(units.water_cells, units.page_updates));
        if (total != 0)
            completed[units.key] = Sum(completed[units.key], total);
    }
    m_completedWork = std::move(completed);
}

std::string ActiveRegionLedger::payload(bool observational) const {
    std::string bytes;
    Append(bytes, m_tick, 8);
    if (observational)
        Append(bytes, m_headerTick, 8);
    Append(bytes, m_config.work_limit, 8);
    Append(bytes, m_config.hold_ticks, 4);
    Append(bytes, m_config.reduced_shift, 1);
    Append(bytes, m_localAnchor.has_value(), 1);
    Append(bytes, 0, 2);
    const auto anchor = m_localAnchor.value_or(Vec3{});
    for (int axis = 0; axis < 3; ++axis)
        Append(bytes, std::bit_cast<std::uint32_t>(anchor[axis]), 4);
    Append(bytes, m_records.size(), 4);
    for (const auto& [key, r] : m_records) {
        Append(bytes, static_cast<std::uint32_t>(key.x), 4);
        Append(bytes, static_cast<std::uint32_t>(key.z), 4);
        const auto flags = static_cast<std::uint8_t>(r.visited) |
                           (static_cast<std::uint8_t>(r.edited) << 1) |
                           (static_cast<std::uint8_t>(r.pinned) << 2) |
                           (static_cast<std::uint8_t>(r.populated) << 3);
        Append(bytes, flags, 1);
        Append(bytes, static_cast<std::uint8_t>(r.state), 1);
        Append(bytes, r.cadence_shift, 1);
        Append(bytes, r.wake_pending, 1);
        Append(bytes, r.cadence_phase, 4);
        for (const auto stamp :
             {r.first_active, r.last_proximity, r.last_edit, r.last_ticked, r.frozen_at})
            Append(bytes, stamp, 8);
        if (observational)
            Append(bytes, r.last_save, 8);
        Append(bytes, r.last_decision, 8);
        Append(bytes, r.pressure, 8);
        Append(bytes, r.over_hold, 4);
        Append(bytes, r.under_hold, 4);
        Append(bytes, r.water_cursor, 8);
        Append(bytes, r.frozen_digest, 8);
    }
    return bytes;
}
std::string ActiveRegionLedger::canonical_bytes() const {
    if (m_records.empty() && m_nextPersistentId == 1)
        return {};
    std::string bytes = "active_regions:v1:" + payload(false);
    if (m_nextPersistentId != 1)
        bytes += "|next_id:" + std::to_string(m_nextPersistentId);
    if (!m_energyAges.empty()) {
        bytes += "|energy_age:";
        Append(bytes, m_energyAges.size(), 4);
        for (const auto& [key, age] : m_energyAges) {
            Append(bytes, static_cast<std::uint32_t>(key.x), 4);
            Append(bytes, static_cast<std::uint32_t>(key.z), 4);
            Append(bytes, age, 8);
        }
    }
    if (!m_completedWork.empty()) {
        bytes += "|completed_work:";
        Append(bytes, m_completedWork.size(), 4);
        for (const auto& [key, total] : m_completedWork) {
            Append(bytes, static_cast<std::uint32_t>(key.x), 4);
            Append(bytes, static_cast<std::uint32_t>(key.z), 4);
            Append(bytes, total, 8);
        }
    }
    return bytes;
}
bool ActiveRegionLedger::any_frozen_history() const {
    for (const auto& [key, r] : m_records) {
        (void)key;
        if (r.state == RegionState::Frozen || r.frozen_at != 0 || r.frozen_digest != 0)
            return true;
    }
    return false;
}
std::string ActiveRegionLedger::encode() const {
    constexpr std::uint8_t kDigestProjection = 2;
    const bool frozen_history = any_frozen_history();
    const bool allocator_advanced = m_nextPersistentId != 1;
    const bool extension =
        frozen_history || allocator_advanced || !m_energyAges.empty() || !m_completedWork.empty();
    std::string bytes = "ARL1";
    Append(bytes, extension ? 2 : 1, 2);
    Append(bytes, 0, 2);
    bytes += payload(true);
    if (extension) {
        // Extension block: u32 length, then ascending TLVs (u16 type, u32 length, value).
        std::string tlvs;
        if (allocator_advanced) {
            Append(tlvs, 1, 2);
            Append(tlvs, 8, 4);
            Append(tlvs, m_nextPersistentId, 8);
        }
        if (frozen_history) {
            Append(tlvs, 2, 2);
            Append(tlvs, 1, 4);
            Append(tlvs, kDigestProjection, 1);
        }
        if (!m_energyAges.empty()) {
            Append(tlvs, 4, 2);
            Append(tlvs, 4 + 16 * m_energyAges.size(), 4);
            Append(tlvs, m_energyAges.size(), 4);
            for (const auto& [key, age] : m_energyAges) {
                Append(tlvs, static_cast<std::uint32_t>(key.x), 4);
                Append(tlvs, static_cast<std::uint32_t>(key.z), 4);
                Append(tlvs, age, 8);
            }
        }
        if (!m_completedWork.empty()) {
            Append(tlvs, 6, 2);
            Append(tlvs, 4 + 16 * m_completedWork.size(), 4);
            Append(tlvs, m_completedWork.size(), 4);
            for (const auto& [key, total] : m_completedWork) {
                Append(tlvs, static_cast<std::uint32_t>(key.x), 4);
                Append(tlvs, static_cast<std::uint32_t>(key.z), 4);
                Append(tlvs, total, 8);
            }
        }
        Append(bytes, tlvs.size(), 4);
        bytes += tlvs;
    }
    Append(bytes, Hash(bytes), 8);
    return bytes;
}
bool ActiveRegionLedger::decode(std::string_view bytes,
                                ActiveRegionLedger& out,
                                std::string& error) {
    error = kCorruptMessage;
    if (bytes.size() < 6 || bytes.substr(0, 4) != "ARL1")
        return false;
    Reader version_reader(bytes.substr(4));
    const auto version = version_reader.read(2);
    if (version > 2) {
        error = kFutureMessage;
        return false;
    }
    // Version 2 carries a u32 extension length between the records and the checksum.
    const std::size_t extension_prefix = version == 2 ? 4 : 0;
    if (version == 0 || bytes.size() < kHeaderBytes + extension_prefix + 8 ||
        bytes.size() > kMaxFileBytes)
        return false;
    try {
        Reader checksum(bytes.substr(bytes.size() - 8));
        if (checksum.read(8) != Hash(bytes.substr(0, bytes.size() - 8)))
            return false;
        Reader in(bytes.substr(6));
        if (in.read(2) != 0)
            return false;
        ActiveRegionLedger result;
        result.m_tick = in.read(8);
        result.m_headerTick = in.read(8);
        result.m_config.work_limit = in.read(8);
        result.m_config.hold_ticks = static_cast<std::uint32_t>(in.read(4));
        result.m_config.reduced_shift = static_cast<std::uint8_t>(in.read(1));
        const auto has_anchor = in.read(1);
        if (in.read(2) != 0 || has_anchor > 1 || !ValidConfig(result.m_config) ||
            result.m_tick >= WorldClock::kTickLimit ||
            result.m_headerTick >= WorldClock::kTickLimit || result.m_headerTick < result.m_tick)
            return false;
        Vec3 anchor{};
        for (int axis = 0; axis < 3; ++axis)
            anchor[axis] = std::bit_cast<float>(static_cast<std::uint32_t>(in.read(4)));
        if (!valid_anchor(anchor) || (!has_anchor && anchor != Vec3{}))
            return false;
        if (has_anchor)
            result.set_local_anchor(anchor);
        const auto count = in.read(4);
        if (count > kMaxRegions)
            return false;
        // Records end here; a version 2 extension length (checked below) follows them.
        const std::uint64_t records_end = kHeaderBytes + count * kRecordBytes;
        if (bytes.size() < records_end + extension_prefix + 8 ||
            (version == 1 && bytes.size() != records_end + 8))
            return false;
        RegionKey previous{};
        for (std::uint64_t i = 0; i < count; ++i) {
            RegionKey key;
            key.x = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(in.read(4)));
            key.z = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(in.read(4)));
            const auto flags = in.read(1), state = in.read(1), shift = in.read(1),
                       wake = in.read(1);
            if (!ValidKey(key) || (i > 0 && key <= previous) || flags > 15 || state > 2 ||
                shift > 16 || wake > 1)
                return false;
            previous = key;
            ActiveRegionRecord r;
            r.visited = (flags & 1) != 0;
            r.edited = (flags & 2) != 0;
            r.pinned = (flags & 4) != 0;
            r.populated = (flags & 8) != 0;
            r.state = static_cast<RegionState>(state);
            r.cadence_shift = static_cast<std::uint8_t>(shift);
            r.wake_pending = wake != 0;
            r.cadence_phase = static_cast<std::uint32_t>(in.read(4));
            r.first_active = in.read(8);
            r.last_proximity = in.read(8);
            r.last_edit = in.read(8);
            r.last_ticked = in.read(8);
            r.frozen_at = in.read(8);
            r.last_save = in.read(8);
            r.last_decision = in.read(8);
            r.pressure = in.read(8);
            r.over_hold = static_cast<std::uint32_t>(in.read(4));
            r.under_hold = static_cast<std::uint32_t>(in.read(4));
            r.water_cursor = in.read(8);
            r.frozen_digest = in.read(8);
            for (const auto tick : {r.first_active,
                                    r.last_proximity,
                                    r.last_edit,
                                    r.last_ticked,
                                    r.frozen_at,
                                    r.last_save,
                                    r.last_decision})
                if (tick > result.m_headerTick)
                    return false;
            if (r.over_hold > result.m_config.hold_ticks ||
                r.under_hold > result.m_config.hold_ticks || (r.over_hold && r.under_hold) ||
                r.cadence_shift !=
                    (r.state == RegionState::Active ? 0 : result.m_config.reduced_shift) ||
                r.cadence_phase != phase(key, r.cadence_shift) ||
                (r.state == RegionState::Frozen && r.last_ticked >= r.frozen_at))
                return false;
            result.m_records.emplace(key, r);
        }
        bool digest_projection = false;
        if (version == 2) {
            const auto extension_length = in.read(4);
            const auto extension_begin = records_end + 4;
            if (extension_length == 0 || bytes.size() != extension_begin + extension_length + 8)
                return false;
            const std::string_view extension = bytes.substr(extension_begin, extension_length);
            // Each TLV is u16 type, u32 length, value. The TLVs must exactly fill the block.
            std::size_t at = 0;
            bool have_previous = false;
            std::uint64_t previous_type = 0;
            while (at < extension.size()) {
                if (extension.size() - at < 6)
                    return false;
                Reader tlv(extension.substr(at, 6));
                const auto type = tlv.read(2);
                const auto length = tlv.read(4);
                at += 6;
                if (length > extension.size() - at)
                    return false;
                const auto value = extension.substr(at, length);
                at += length;
                // Types 3/5 remain reserved until their consumer decoders exist.
                if ((have_previous && type <= previous_type) ||
                    (type != 1 && type != 2 && type != 4 && type != 6))
                    return false;
                have_previous = true;
                previous_type = type;
                if (type == 1) {
                    // Persistent id allocator: the next id, in [2, 2^63].
                    if (length != 8)
                        return false;
                    Reader next_id(value);
                    const auto next = next_id.read(8);
                    if (next < 2 || next > kPersistentIdLimit)
                        return false;
                    result.m_nextPersistentId = next;
                    continue;
                }
                if (type == 4) {
                    if (length < 4)
                        return false;
                    Reader ages(value);
                    const auto owners = ages.read(4);
                    if (owners == 0 || owners > result.m_records.size() ||
                        length != 4 + 16 * owners)
                        return false;
                    std::optional<RegionKey> prior;
                    for (std::uint64_t entry = 0; entry < owners; ++entry) {
                        const RegionKey key{
                            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(ages.read(4))),
                            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(ages.read(4)))};
                        const auto age = ages.read(8);
                        if (!result.m_records.contains(key) || (prior && key <= *prior) ||
                            age >= WorldClock::kTickLimit)
                            return false;
                        result.m_energyAges.emplace(key, age);
                        prior = key;
                    }
                    continue;
                }
                if (type == 6) {
                    if (length < 4)
                        return false;
                    Reader completed(value);
                    const auto completed_count = completed.read(4);
                    if (completed_count == 0 || completed_count > result.m_records.size() ||
                        length != 4 + 16 * completed_count)
                        return false;
                    std::optional<RegionKey> prior;
                    for (std::uint64_t entry = 0; entry < completed_count; ++entry) {
                        const RegionKey key{std::bit_cast<std::int32_t>(
                                                static_cast<std::uint32_t>(completed.read(4))),
                                            std::bit_cast<std::int32_t>(
                                                static_cast<std::uint32_t>(completed.read(4)))};
                        const auto total = completed.read(8);
                        if (!result.m_records.contains(key) || (prior && key <= *prior) ||
                            total == 0)
                            return false;
                        result.m_completedWork.emplace(key, total);
                        prior = key;
                    }
                    continue;
                }
                if (length != 1)
                    return false;
                const auto projection = static_cast<unsigned char>(value[0]);
                if (projection > 2) {
                    error = kProjectionMessage;
                    return false;
                }
                if (projection < 2)
                    return false;
                digest_projection = true;
            }
        }
        // Frozen history cannot be read without the digest projection declaration.
        if (!digest_projection && result.any_frozen_history()) {
            error = kProjectionMessage;
            return false;
        }
        // Also refuses alternate encodings (negative zero, reserved bytes).
        if (result.encode() != bytes)
            return false;
        out = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace Luminumbra::world
