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
bool ValidAnchor(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && p.x >= -16777216.0f &&
           p.x < 16777216.0f && p.z >= -16777216.0f && p.z < 16777216.0f;
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

ActiveRegionLedger::ActiveRegionLedger(RegionSchedulerConfig config)
    : m_config(config) {
    if (!ValidConfig(config))
        throw std::invalid_argument("Invalid active-region scheduler configuration");
}
RegionKey ActiveRegionLedger::region_at(const Vec3& position) {
    if (!ValidAnchor(position))
        throw std::invalid_argument("Invalid simulation anchor");
    return {static_cast<std::int32_t>(std::floor(static_cast<double>(position.x) / 512.0)),
            static_cast<std::int32_t>(std::floor(static_cast<double>(position.z) / 512.0))};
}
void ActiveRegionLedger::set_local_anchor(const Vec3& walking_feet) {
    if (!ValidAnchor(walking_feet))
        throw std::invalid_argument("Invalid simulation anchor");
    m_localAnchor = walking_feet;
    // Canonicalise signed zero, including anchor hashes and persisted bits.
    for (int axis = 0; axis < 3; ++axis)
        if ((*m_localAnchor)[axis] == 0.0f)
            (*m_localAnchor)[axis] = 0.0f;
}
ActiveRegionRecord& ActiveRegionLedger::activate(RegionKey key, const WorldClock& clock) {
    if (!ValidKey(key) || clock.tick() < m_tick)
        throw std::invalid_argument("Invalid region activation");
    if (!m_records.contains(key) && m_records.size() == kMaxRegions)
        throw std::length_error("Active-region ledger capacity reached");
    auto [it, added] = m_records.try_emplace(key);
    if (added)
        it->second.first_active = clock.tick();
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
void ActiveRegionLedger::visit(const Vec3& anchor, const WorldClock& clock) {
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
            auto& record = activate(key, clock);
            record.visited = true;
            record.last_proximity = clock.tick();
        }
}
RegionSchedule ActiveRegionLedger::schedule(const WorldClock& clock,
                                            std::span<const Vec3> replicated_anchors,
                                            std::span<const RegionWork> work,
                                            const DurableDigest& durable_digest) {
    auto next = *this;
    auto result = next.schedule_in_place(clock, replicated_anchors, work, durable_digest);
    *this = std::move(next);
    return result;
}
RegionSchedule ActiveRegionLedger::schedule_in_place(const WorldClock& clock,
                                                     std::span<const Vec3> replicated_anchors,
                                                     std::span<const RegionWork> work,
                                                     const DurableDigest& durable_digest) {
    if (clock.tick() <= m_tick)
        throw std::invalid_argument("Region schedule requires a new absolute tick");
    for (const auto& anchor : replicated_anchors)
        if (!ValidAnchor(anchor))
            throw std::invalid_argument("Invalid simulation anchor");
    for (const auto& units : work)
        if (!m_records.contains(units.key))
            throw std::invalid_argument("Work refers to an inactive region");
    if (m_localAnchor && replicated_anchors.empty())
        visit(*m_localAnchor, clock);
    for (const auto& anchor : replicated_anchors)
        visit(anchor, clock);

    RegionSchedule result;
    result.tick = clock.tick();
    std::vector<RegionKey> order;
    for (auto& [key, record] : m_records) {
        record.pressure = 0;
        order.push_back(key);
    }
    for (const auto& units : work) {
        const auto total = Sum(Sum(units.region_ticks, units.entity_updates),
                               Sum(units.water_cells, units.page_updates));
        auto& record = m_records.at(units.key);
        record.pressure = Sum(record.pressure, total);
        result.work = Sum(result.work, total);
    }
    std::sort(order.begin(), order.end(), [&](RegionKey a, RegionKey b) {
        const auto& ra = m_records.at(a);
        const auto& rb = m_records.at(b);
        return std::tuple(Rank(ra), clock.tick() - ra.last_proximity, a) <
               std::tuple(Rank(rb), clock.tick() - rb.last_proximity, b);
    });
    const bool over = result.work > m_config.work_limit;
    for (const auto key : order) {
        auto& r = m_records.at(key);
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
    const auto eligible = [&](RegionKey key) {
        const auto& r = m_records.at(key);
        if ((r.visited && r.last_proximity == clock.tick()) || r.wake_pending)
            return false;
        return over ? r.state != RegionState::Frozen && r.over_hold == m_config.hold_ticks
                    : r.state != RegionState::Active && r.under_hold == m_config.hold_ticks;
    };
    if (over) {
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            if (eligible(*it)) {
                selected = *it;
                break;
            }
    } else {
        for (const auto key : order)
            if (eligible(key)) {
                selected = key;
                break;
            }
    }
    for (const auto key : order) {
        auto& r = m_records.at(key);
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
    std::string trace = "region_schedule:v1:";
    Append(trace, result.tick, 8);
    Append(trace, result.work, 8);
    Append(trace, order.size(), 4);
    for (const auto key : order) {
        const auto& r = m_records.at(key);
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
    result.digest = Persistence::StableChecksum(trace);
    return result;
}

std::string ActiveRegionLedger::payload(bool observational) const {
    std::string bytes;
    Append(bytes, m_tick, 8);
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
        Append(bytes, r.visited | (r.edited << 1) | (r.pinned << 2) | (r.populated << 3), 1);
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
    return m_records.empty() ? std::string{} : "active_regions:v1:" + payload(false);
}
std::string ActiveRegionLedger::encode() const {
    std::string bytes = "ARL1";
    Append(bytes, 1, 2);
    Append(bytes, 0, 2);
    bytes += payload(true);
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
    if (version > 1) {
        error = kFutureMessage;
        return false;
    }
    if (version != 1 || bytes.size() < kHeaderBytes + 8 || bytes.size() > kMaxFileBytes)
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
        result.m_config.work_limit = in.read(8);
        result.m_config.hold_ticks = static_cast<std::uint32_t>(in.read(4));
        result.m_config.reduced_shift = static_cast<std::uint8_t>(in.read(1));
        const auto has_anchor = in.read(1);
        if (in.read(2) != 0 || has_anchor > 1 || !ValidConfig(result.m_config) ||
            result.m_tick >= WorldClock::kTickLimit)
            return false;
        Vec3 anchor{};
        for (int axis = 0; axis < 3; ++axis)
            anchor[axis] = std::bit_cast<float>(static_cast<std::uint32_t>(in.read(4)));
        if (!ValidAnchor(anchor) || (!has_anchor && anchor != Vec3{}))
            return false;
        if (has_anchor)
            result.set_local_anchor(anchor);
        const auto count = in.read(4);
        if (count > kMaxRegions || bytes.size() != kHeaderBytes + count * kRecordBytes + 8)
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
                if (tick > result.m_tick)
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
